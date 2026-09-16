#!/usr/bin/env python3
"""
Chitta MCP Server - Python bridge to chittad daemon.

Uses the official MCP SDK to expose chitta tools to Claude Code.
Tools are defined statically (like chitta-bridge) for proper discovery.

Includes composite tools for token-efficient code intelligence:
- read_symbol: Read just a symbol's code, not entire file
- read_function: Convenience wrapper for read_symbol
- symbol_callers: Find all callers via triplet queries
- smart_context: Task-aware context assembly
"""

from __future__ import annotations

import asyncio
import hashlib
import json
import logging
import os
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from typing import Any

_executor = ThreadPoolExecutor(max_workers=4)

# Suppress MCP SDK validation warnings (Claude Code sends incomplete initialize requests)
logging.getLogger("root").setLevel(logging.ERROR)
logging.getLogger("mcp").setLevel(logging.ERROR)

# Logger for chitta-mcp (allow warnings for session mismatch detection)
logger = logging.getLogger("chitta-mcp")
logger.setLevel(logging.WARNING)

from mcp.server import InitializationOptions, Server  # noqa: E402, F401
from mcp.server.stdio import stdio_server  # noqa: E402, F401
from mcp.types import (  # noqa: E402, F401
    ServerCapabilities,
    TextContent,
    Tool,
    ToolsCapability,
)

# Sibling modules must resolve regardless of launcher: the ~/.local/bin
# console script is a setuptools *editable* install whose finder only maps
# modules that existed at install time, so a newly added sibling is invisible
# to it (incident 2026-09-08: chitta-mcp-http crash-looped on recall_gateway).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import code_tools  # noqa: E402
import daemon_bridge  # noqa: E402
import evolve_helpers  # noqa: E402
import mcp_health  # noqa: E402
import mcp_transport  # noqa: E402
import memory_tools  # noqa: E402
import recall_handlers  # noqa: E402
import session_ops  # noqa: E402
import tool_policy  # noqa: E402
from daemon_client import (  # noqa: E402, F401
    ChittaClient,
    ChittaHttpClient,
    djb2_hash,
    get_socket_dir,
    get_socket_path,
)
from mcp_transport import HttpSessionTable  # noqa: E402, F401
from recall_gateway import (  # noqa: E402, F401
    RERANK_FETCH_MUL,
    get_reranker,
    profile_async,
    profile_stage,
    rrf_merge,
    run_reranker,
)
from tool_policy import ADVANCED_TOOLS, HIDDEN_TOOLS, INTERNAL_TOOLS  # noqa: E402, F401
from tools_static import COMPOSITE_TOOLS, TOOLS  # noqa: E402

# Shared runtime is this module, including when launched as __main__.
_runtime = sys.modules[__name__]

_loop_lag_max_ms = 0.0
_loop_lag_over_count = 0


def _lag_setting_ms(name: str, legacy_name: str, default: float) -> float:
    return mcp_health._lag_setting_ms(_runtime, name, legacy_name, default)


def loop_lag_stats() -> dict[str, float | int]:
    return mcp_health.loop_lag_stats(_runtime)


async def monitor_loop_lag(interval_ms: float | None = None, warn_ms: float | None = None) -> None:
    return await mcp_health.monitor_loop_lag(_runtime, interval_ms, warn_ms)


async def _stop_loop_lag_monitor(task: asyncio.Task) -> None:
    return await mcp_health._stop_loop_lag_monitor(_runtime, task)


# HttpSessionTable remains re-exported for existing callers.


_http_sessions: HttpSessionTable | None = None


def _session_setting(name, default, kind):
    return mcp_transport._session_setting(_runtime, name, default, kind)


def http_session_stats():
    return mcp_transport.http_session_stats(_runtime)


def _with_loop_lag_health(result: str) -> str:
    return mcp_health._with_loop_lag_health(_runtime, result)


def inject_message_session(tool: str, args: dict) -> str | None:
    return session_ops.inject_message_session(_runtime, tool, args)


def handle_advanced(arguments: dict) -> str:
    return tool_policy.handle_advanced(_runtime, arguments)


def strip_null_values(obj):
    return tool_policy.strip_null_values(_runtime, obj)


def clean_tool_schema(tool: Tool) -> Tool:
    return tool_policy.clean_tool_schema(_runtime, tool)


def to_toon(obj: Any, indent: int = 0) -> str:
    return daemon_bridge.to_toon(_runtime, obj, indent)


# Global client and server
client: ChittaClient | None = None
server = Server("chitta-mcp")
current_session_id: str | None = None  # Track current session for auto-defaults
current_realm: str | None = None  # Track current realm for auto-defaults

# P0: Internal realm classification — prefixes that must land in soul:meta
_INTERNAL_REALM_PREFIXES = (
    "[thought]",
    "[impl]",
    "[thinking-block:",
    "[thinking.block:",
)


def _classify_internal_realm(content: str) -> str | None:
    """Return 'soul:meta' if content is an internal synthesis artifact, else None."""
    stripped = content.lstrip()
    for prefix in _INTERNAL_REALM_PREFIXES:
        if stripped.startswith(prefix):
            return "soul:meta"
    return None


# P3: Hook idempotency dedup cache — (source_tool, content_hash) -> epoch_s
_hook_dedup_cache: dict[tuple[str, str], float] = {}
_HOOK_DEDUP_WINDOW_S = 60.0


def ensure_daemon() -> bool:
    return daemon_bridge.ensure_daemon(_runtime)


def _normalize_args(arguments: dict) -> dict:
    return daemon_bridge._normalize_args(_runtime, arguments)


def daemon_call(tool_name: str, arguments: dict, structured: bool = False) -> str:
    return daemon_bridge.daemon_call(_runtime, tool_name, arguments, structured)


@server.list_tools()
async def list_tools():
    """Return only essential tools (hide internal/advanced to save tokens).

    Hidden tools are still callable - just not listed in tools/list.
    This reduces context from ~16k tokens to ~4k tokens.
    """
    composite_names = {t.name for t in COMPOSITE_TOOLS}
    # Filter out: composites (replaced by COMPOSITE_TOOLS), internal tools, advanced tools
    filtered = [t for t in TOOLS if t.name not in composite_names and t.name not in HIDDEN_TOOLS]
    # Also filter composite tools if they're in HIDDEN_TOOLS
    filtered_composites = [t for t in COMPOSITE_TOOLS if t.name not in HIDDEN_TOOLS]
    # Strip null values from inputSchema (required: null breaks Zod validation)
    all_tools = filtered + filtered_composites
    return [clean_tool_schema(t) for t in all_tools]


# Composite tool handlers for token-efficient code intelligence
def read_file_lines(file_path: str, start: int, end: int) -> str:
    return code_tools.read_file_lines(_runtime, file_path, start, end)


def handle_read_symbol(arguments: dict) -> str:
    return code_tools.handle_read_symbol(_runtime, arguments)


def handle_read_function(arguments: dict) -> str:
    return code_tools.handle_read_function(_runtime, arguments)


def handle_symbol_callers(arguments: dict) -> str:
    return code_tools.handle_symbol_callers(_runtime, arguments)


def handle_symbol_callees(arguments: dict) -> str:
    return code_tools.handle_symbol_callees(_runtime, arguments)


def handle_verify_correction(arguments: dict) -> str:
    return memory_tools.handle_verify_correction(_runtime, arguments)


def handle_ack_memory(arguments: dict) -> str:
    return memory_tools.handle_ack_memory(_runtime, arguments)


def handle_nack_memory(arguments: dict) -> str:
    return memory_tools.handle_nack_memory(_runtime, arguments)


def handle_memory_outcome(arguments: dict) -> str:
    return memory_tools.handle_memory_outcome(_runtime, arguments)


_TYPED_NODE_TYPES = frozenset(
    {"digest-node", "symbol-summary", "decision", "open-question", "rollup", "working-brief"}
)
_LINK_PREDICATES = frozenset({"supersedes", "invalidated-by", "anchors-to"})


def handle_remember_typed(arguments: dict) -> str:
    return memory_tools.handle_remember_typed(_runtime, arguments)


def handle_smart_context(arguments: dict) -> str:
    return code_tools.handle_smart_context(_runtime, arguments)


def handle_lookup(arguments: dict) -> str:
    return code_tools.handle_lookup(_runtime, arguments)


def _slug(text: str, width: int = 40) -> str:
    return memory_tools._slug(_runtime, text, width)


def _store_learning(
    content: str,
    tags: list[str],
    mem_type: str,
    visibility: int,
    subject: str,
    predicate: str,
    obj: str,
) -> str:
    return memory_tools._store_learning(
        _runtime, content, tags, mem_type, visibility, subject, predicate, obj
    )


def handle_learn_correction(arguments: dict) -> str:
    return memory_tools.handle_learn_correction(_runtime, arguments)


def handle_learn_preference(arguments: dict) -> str:
    return memory_tools.handle_learn_preference(_runtime, arguments)


def handle_learn_insight(arguments: dict) -> str:
    return memory_tools.handle_learn_insight(_runtime, arguments)


def handle_learn_approach(arguments: dict) -> str:
    return memory_tools.handle_learn_approach(_runtime, arguments)


def handle_learn_outcome(arguments: dict) -> str:
    return memory_tools.handle_learn_outcome(_runtime, arguments)


def handle_learn_milestone(arguments: dict) -> str:
    return memory_tools.handle_learn_milestone(_runtime, arguments)


def handle_learn_analysis(arguments: dict) -> str:
    return memory_tools.handle_learn_analysis(_runtime, arguments)


# ============================================================================
# Curiosity-driven research (background learning agent)
# ============================================================================


def handle_research_topics(arguments: dict) -> str:
    return evolve_helpers.handle_research_topics(_runtime, arguments)


def handle_research_store(arguments: dict) -> str:
    return evolve_helpers.handle_research_store(_runtime, arguments)


def handle_research_cycle(arguments: dict) -> str:
    return evolve_helpers.handle_research_cycle(_runtime, arguments)


def handle_transcript_search(arguments: dict) -> str:
    return session_ops.handle_transcript_search(_runtime, arguments)


def handle_soul_repl(arguments: dict) -> str:
    return evolve_helpers.handle_soul_repl(_runtime, arguments)


# ============================================================================
# Consolidated gateway handlers — reduce tool count for token efficiency
# ============================================================================


def handle_recall_smart(arguments: dict) -> str:
    return recall_handlers.handle_recall_smart(_runtime, arguments)


# Mirrors kind_multiplier in chitta-field/src/scoring/{mod,config}.rs, applied as
# the daemon's bounded envelope 0.7 + 0.3 * kind so reranking cannot flip a
# strongly relevant hit on kind alone.
_KIND_MULTIPLIER = {
    "correction": 1.3,
    "preference": 1.3,
    "wisdom": 1.1,
    "insight": 1.05,
    "episode": 0.7,
    "operational": 0.8,
}


def _kind_envelope(hit: dict) -> float:
    return recall_handlers._kind_envelope(_runtime, hit)


@profile_async("recall_gateway")
async def handle_recall_gateway(arguments: dict) -> str:
    return await recall_handlers.handle_recall_gateway(_runtime, arguments)


def handle_sadhana_gateway(arguments: dict) -> str:
    return evolve_helpers.handle_sadhana_gateway(_runtime, arguments)


def handle_learn_gateway(arguments: dict) -> str:
    return memory_tools.handle_learn_gateway(_runtime, arguments)


def handle_research_gateway(arguments: dict) -> str:
    return evolve_helpers.handle_research_gateway(_runtime, arguments)


def handle_triplets_gateway(arguments: dict) -> str:
    return memory_tools.handle_triplets_gateway(_runtime, arguments)


def handle_memory_edit_gateway(arguments: dict) -> str:
    return memory_tools.handle_memory_edit_gateway(_runtime, arguments)


# Map composite tool names to handlers
def handle_run_hint_enricher(arguments: dict) -> str:
    return evolve_helpers.handle_run_hint_enricher(_runtime, arguments)


COMPOSITE_HANDLERS = {
    "advanced": handle_advanced,
    "soul_repl": handle_soul_repl,
    "read_symbol": handle_read_symbol,
    "read_function": handle_read_function,
    "symbol_callers": handle_symbol_callers,
    "symbol_callees": handle_symbol_callees,
    "smart_context": handle_smart_context,
    "lookup": handle_lookup,
    # Consolidated gateways (replace individual tools)
    "recall": handle_recall_gateway,
    "recall_smart": handle_recall_smart,
    "recall_spreading": lambda a: daemon_call("recall_spreading", a),
    "sadhana": handle_sadhana_gateway,
    "learn": handle_learn_gateway,
    "research": handle_research_gateway,
    "triplets": handle_triplets_gateway,
    "memory_edit": handle_memory_edit_gateway,
    # Individual learn_* still callable via advanced gateway or direct daemon
    "learn_correction": handle_learn_correction,
    "learn_preference": handle_learn_preference,
    "learn_insight": handle_learn_insight,
    "learn_approach": handle_learn_approach,
    "learn_outcome": handle_learn_outcome,
    "learn_milestone": handle_learn_milestone,
    "learn_analysis": handle_learn_analysis,
    # Individual research_* still callable
    "research_topics": handle_research_topics,
    "research_store": handle_research_store,
    "research_cycle": handle_research_cycle,
    # Transcript search (fast, local)
    "transcript_search": handle_transcript_search,
    # Typed memory and signal tools
    "verify_correction": handle_verify_correction,
    "ack_memory": handle_ack_memory,
    "nack_memory": handle_nack_memory,
    "memory_outcome": handle_memory_outcome,
    "remember_typed": handle_remember_typed,
    "run_hint_enricher": handle_run_hint_enricher,
}

# Messaging tools that need session_id auto-injection
MSG_TOOLS = {"msg_inbox", "msg_send", "msg_respond", "msg_ack", "msg_ack_all", "msg_history"}

# Tools that need session_id injection
SESSION_TOOLS = {
    "ledger_save",
    "narrative_log",
    "narrative_history",
    "anticipation_filter",
    "anticipation_gate_status",
    "transcript_register",
    "transcript_get",
    "transcript_update",
    "transcript_remove",
    "transcript_parse",
    "msg_inbox",
    "msg_send",
    "msg_ack",
    "msg_ack_all",
    "msg_history",
    "session_register",
    "session_heartbeat",
    "session_deregister",
}

# Tools that store memories - need realm auto-injection
REALM_STORE_TOOLS = {
    "remember",
    "grow",
    "observe",
    "long_task_start",
    "checkpoint",
    "goal_set",
    "habit_observe",
    "anticipation_observe",
    "suggestion_track",
    "curiosity_note_gap",
    "background_schedule",
    "narrative_log",
    "transcript_register",
}

# Tools that filter/query by realm
REALM_FILTER_TOOLS = {"long_task_active", "smart_context", "lookup"}


def get_current_session_id(use_cache: bool = True) -> str | None:
    return session_ops.get_current_session_id(_runtime, use_cache)


def get_current_realm() -> str | None:
    return session_ops.get_current_realm(_runtime)


_SQZ_BIN = os.path.expanduser("~/.claude/bin/sqz")
_SQZ_THRESHOLD = 500


def _sqz_compress(text: str, tool_name: str = "mcp") -> str:
    return daemon_bridge._sqz_compress(_runtime, text, tool_name)


@server.call_tool()
@profile_async("tools_call")
async def call_tool(name: str, arguments: dict):
    """Handle tool calls - composite tools handled locally, others forwarded to daemon."""
    global current_session_id

    # Coerce string integers to int: LLMs sometimes generate "19" instead of 19,
    # which fails schema validation on strict integer params.
    for k, v in list(arguments.items()):
        if isinstance(v, str) and v.lstrip("-").isdigit():
            arguments[k] = int(v)

    # Track session_id from session_register and transcript_register for auto-defaults
    if name == "session_register" and "session_id" in arguments:
        current_session_id = arguments["session_id"]
    if name == "transcript_register" and "session_id" in arguments:
        current_session_id = arguments["session_id"]

    messaging_error = inject_message_session(name, arguments)
    if messaging_error:
        return messaging_error

    # Auto-inject source_session for memory writes (substrate coverage lever:
    # feeds recall_session grouping + densify_backfill SameSession edges).
    # Best-effort: skip silently if the session can't be resolved — an
    # untagged memory is fine, a failed write is not.
    if name in ("remember", "remember_batch"):
        sid = get_current_session_id(use_cache=False)
        if sid:
            if name == "remember":
                arguments.setdefault("source_session", sid)
            else:
                for _item in arguments.get("items") or []:
                    if isinstance(_item, dict):
                        _item.setdefault("source_session", sid)

    # Auto-inject session_id for transcript_search if not provided
    # Pass session_id="*" to explicitly search all transcripts
    if name == "transcript_search":
        if arguments.get("session_id") == "*":
            arguments["session_id"] = ""  # Empty = search all
        elif current_session_id and not arguments.get("session_id"):
            arguments["session_id"] = current_session_id

    # P0: Classify internal realm FIRST — overrides any auto-injection.
    # [thought], [impl], [thinking-block:] are synthesis artifacts that must
    # never land in domain recall regardless of what realm is currently active.
    if name == "remember":
        internal = _classify_internal_realm(arguments.get("content", ""))
        if internal:
            arguments["realm"] = internal
        # kind is a user-friendly alias for type (daemon field name is "type")
        if "kind" in arguments and "type" not in arguments:
            arguments["type"] = arguments.pop("kind")

    # Genome 'process' memories are internal cognitive-config state: default
    # them to realm-private visibility (0) unless the caller overrides.
    if name == "remember" and arguments.get("type") == "process":
        arguments.setdefault("visibility", 0)

    # P3: Idempotency guard for compliance hook memories within a 60s window.
    if name == "remember":
        source_tool = arguments.get("source_tool", "")
        if source_tool and "compliance" in source_tool:
            content_key = hashlib.md5(arguments.get("content", "")[:128].encode()).hexdigest()
            _cache_key = (source_tool, content_key)
            _now = time.time()
            if _now - _hook_dedup_cache.get(_cache_key, 0.0) < _HOOK_DEDUP_WINDOW_S:
                return [
                    TextContent(type="text", text="[dedup] skipped duplicate compliance memory")
                ]
            _hook_dedup_cache[_cache_key] = _now
            if len(_hook_dedup_cache) > 2000:
                cutoff = _now - _HOOK_DEDUP_WINDOW_S
                for k in [k for k, v in _hook_dedup_cache.items() if v < cutoff]:
                    del _hook_dedup_cache[k]

    # Auto-inject realm for store operations (only if not already set by P0)
    if name in REALM_STORE_TOOLS and not arguments.get("realm"):
        realm = get_current_realm()
        if realm:
            arguments["realm"] = realm

    # Auto-inject realm for filter operations
    if name in REALM_FILTER_TOOLS and not arguments.get("realm"):
        realm = get_current_realm()
        if realm:
            arguments["realm"] = realm

    # Check if this is a composite tool
    loop = asyncio.get_event_loop()

    # Merge-aware write policy (remember / grow only; never for hook-triggered observe)
    MERGE_WRITE_TOOLS = {"remember", "grow"}
    write_policy = arguments.pop("write_policy", None)
    merge_env = os.environ.get("CHITTA_MERGE_POLICY", "off")
    if name in MERGE_WRITE_TOOLS and (write_policy == "merge_aware" or merge_env == "merge_aware"):
        content = arguments.get("content", "")
        realm = arguments.get("realm", "")
        try:
            import merge_judge

            recall_raw = await loop.run_in_executor(
                _executor,
                daemon_call,
                "recall",
                {
                    "query": content,
                    "limit": 5,
                    "strategy": "hybrid",
                    **({"realm": realm} if realm else {}),
                },
            )
            candidates = json.loads(recall_raw).get("results", []) if recall_raw else []
            decision = merge_judge.judge(content, candidates)
            action = decision.get("action", "add")
            if action == "discard":
                return [
                    TextContent(
                        type="text",
                        text=f"[merge_aware] skipped: {decision.get('reason', 'duplicate')}",
                    )
                ]
            if action == "update":
                target_id = decision.get("target_id")
                if target_id:
                    await loop.run_in_executor(
                        _executor, daemon_call, "update", {"id": str(target_id), "content": content}
                    )
                    return [
                        TextContent(
                            type="text",
                            text=f"[merge_aware] updated #{target_id}: {decision.get('reason', '')}",
                        )
                    ]
        except Exception as exc:  # noqa: BLE001
            # The merge judge is an opt-in write filter spanning an optional
            # import, a recall round-trip, and the judge's own heuristics. Its
            # only job is to suppress a redundant write; if any part of it
            # fails the correct outcome is the plain write below, never a lost
            # memory. Deliberately catches everything.
            logger.info("merge-aware judge failed, writing normally: %s", exc)
    if name in COMPOSITE_HANDLERS:
        handler = COMPOSITE_HANDLERS[name]
        if asyncio.iscoroutinefunction(handler):
            result = await handler(arguments)
        else:
            result = await loop.run_in_executor(_executor, handler, arguments)
    else:
        # Forward to daemon
        result = await loop.run_in_executor(_executor, daemon_call, name, arguments)

    with profile_stage("compression", tool=name):
        result = _sqz_compress(result, name)
    if name == "health_check":
        result = _with_loop_lag_health(result)

    return [TextContent(type="text", text=result)]


def main():
    import sys

    # Check for --http mode (streamable HTTP MCP for Codex/Cursor/Copilot)
    http_mode = "--http" in sys.argv or os.environ.get("CHITTA_MCP_HTTP")
    port = int(os.environ.get("CHITTA_MCP_PORT", "9481"))

    # Parse --port / --rpc-port from argv
    for i, arg in enumerate(sys.argv):
        if arg == "--port" and i + 1 < len(sys.argv):
            port = int(sys.argv[i + 1])
        if arg == "--rpc-port" and i + 1 < len(sys.argv):
            os.environ["CHITTA_RPC_PORT"] = sys.argv[i + 1]

    if http_mode:
        _run_http(port)
    else:
        _run_stdio()


def _run_stdio():
    return mcp_transport._run_stdio(_runtime)


def _mcp_token() -> str:
    return mcp_transport._mcp_token(_runtime)


def create_http_session_manager():
    return mcp_transport.create_http_session_manager(_runtime)


def _run_http(port: int):
    return mcp_transport._run_http(_runtime, port)


if __name__ == "__main__":
    main()
