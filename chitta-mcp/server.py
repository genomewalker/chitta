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
import datetime
import hashlib
import json
import logging
import math
import os
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Any

_executor = ThreadPoolExecutor(max_workers=4)

# Suppress MCP SDK validation warnings (Claude Code sends incomplete initialize requests)
logging.getLogger("root").setLevel(logging.ERROR)
logging.getLogger("mcp").setLevel(logging.ERROR)

# Logger for chitta-mcp (allow warnings for session mismatch detection)
logger = logging.getLogger("chitta-mcp")
logger.setLevel(logging.WARNING)

from mcp.server import InitializationOptions, Server  # noqa: E402
from mcp.server.stdio import stdio_server  # noqa: E402
from mcp.types import (  # noqa: E402
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
from daemon_client import (  # noqa: E402, F401
    ChittaClient,
    ChittaHttpClient,
    djb2_hash,
    get_socket_dir,
    get_socket_path,
)
from recall_gateway import (  # noqa: E402
    RERANK_FETCH_MUL,
    get_reranker,
    profile_async,
    profile_stage,
    rrf_merge,
    run_reranker,
)
from tools_static import COMPOSITE_TOOLS, TOOLS  # noqa: E402

_loop_lag_max_ms = 0.0
_loop_lag_over_count = 0


def _lag_setting_ms(name: str, legacy_name: str, default: float) -> float:
    raw = os.environ.get(name, os.environ.get(legacy_name, str(default)))
    try:
        return max(1.0, float(raw))
    except ValueError:
        logger.warning("invalid %s=%r; using %.0fms", name, raw, default)
        return default


def loop_lag_stats() -> dict[str, float | int]:
    """Return a stable snapshot for health output and diagnostics."""
    return {
        "max_lag_ms": round(_loop_lag_max_ms, 1),
        "over_count": _loop_lag_over_count,
    }


async def monitor_loop_lag(interval_ms: float | None = None, warn_ms: float | None = None) -> None:
    """Measure asyncio scheduling delay and warn when it crosses the threshold."""
    global _loop_lag_max_ms, _loop_lag_over_count
    if interval_ms is None:
        interval_ms = _lag_setting_ms(
            "CHITTA_MCP_LAG_INTERVAL_MS", "CC_SOUL_MCP_LAG_INTERVAL_MS", 250.0
        )
    if warn_ms is None:
        warn_ms = _lag_setting_ms("CHITTA_MCP_LAG_WARN_MS", "CC_SOUL_MCP_LAG_WARN_MS", 200.0)
    interval_s = interval_ms / 1000.0
    loop = asyncio.get_running_loop()
    while True:
        expected = loop.time() + interval_s
        await asyncio.sleep(interval_s)
        lag_ms = max(0.0, (loop.time() - expected) * 1000.0)
        _loop_lag_max_ms = max(_loop_lag_max_ms, lag_ms)
        if lag_ms >= warn_ms:
            _loop_lag_over_count += 1
            logger.warning(
                "asyncio scheduling delay %.1fms exceeds %.1fms threshold",
                lag_ms,
                warn_ms,
            )


async def _stop_loop_lag_monitor(task: asyncio.Task) -> None:
    task.cancel()
    try:
        await task
    except asyncio.CancelledError:
        pass


class HttpSessionTable(dict):
    """SDK transport table with monotonic activity, bounded admission and reaping.

    The SDK inserts through __setitem__ and looks up existing sessions through
    __getitem__. Its creation lock is replaced by this async context manager so
    capacity is reserved atomically before a transport (and server task) exists.
    """

    def __init__(self, owners, idle_s=1800.0, max_sessions=64):
        super().__init__()
        self.owners = owners
        self.idle_s = idle_s
        self.max_sessions = max_sessions
        self.last_seen = {}
        self.created = self.expired = self.evicted = 0
        self.lock = asyncio.Lock()

    def __setitem__(self, key, value):
        super().__setitem__(key, value)
        self.last_seen[key] = time.monotonic()
        self.created += 1

    def __getitem__(self, key):
        value = super().__getitem__(key)
        self.last_seen[key] = time.monotonic()
        return value

    def stats(self):
        return {
            "active": len(self),
            "created": self.created,
            "expired": self.expired,
            "evicted": self.evicted,
            "max_sessions": self.max_sessions,
            "idle_s": self.idle_s,
        }

    async def _remove(self, key, reason):
        transport = self.pop(key, None)
        self.last_seen.pop(key, None)
        self.owners.pop(key, None)
        if transport is not None:
            if reason == "expired":
                self.expired += 1
            elif reason == "evicted":
                self.evicted += 1
            await transport.terminate()

    async def _reap(self):
        now = time.monotonic()
        for key in list(self.last_seen):
            if key not in self:
                self.last_seen.pop(key, None)
                self.owners.pop(key, None)
            elif self.get(key).is_terminated:
                await self._remove(key, "closed")
            elif now - self.last_seen[key] > self.idle_s:
                await self._remove(key, "expired")

    async def reap(self):
        async with self.lock:
            await self._reap()

    async def __aenter__(self):
        await self.lock.acquire()
        try:
            await self._reap()
            while len(self) >= self.max_sessions:
                oldest = min(self, key=lambda key: self.last_seen[key])
                await self._remove(oldest, "evicted")
        except BaseException:
            self.lock.release()
            raise
        return self

    async def __aexit__(self, *exc):
        self.lock.release()

    async def monitor(self):
        while True:
            await asyncio.sleep(min(30.0, self.idle_s / 2))
            await self.reap()


_http_sessions: HttpSessionTable | None = None


def _session_setting(name, default, kind):
    try:
        value = kind(os.environ.get(name, str(default)))
        if not math.isfinite(value) or value <= 0:
            raise ValueError("must be finite and positive")
        return value
    except ValueError:
        logger.warning("invalid %s; using %s", name, default)
        return default


def http_session_stats():
    return _http_sessions.stats() if _http_sessions is not None else {"active": 0}


def _with_loop_lag_health(result: str) -> str:
    """Attach MCP loop counters without assuming the daemon health format."""
    stats = loop_lag_stats()
    sessions = http_session_stats()
    try:
        payload = json.loads(result)
    except (json.JSONDecodeError, TypeError):
        suffix = json.dumps(
            {"mcp_loop_lag": stats, "mcp_sessions": sessions}, separators=(",", ":")
        )
        return f"{result.rstrip()}\n{suffix}" if result else suffix
    if isinstance(payload, dict):
        payload["mcp_loop_lag"] = stats
        payload["mcp_sessions"] = sessions
        return json.dumps(payload)
    suffix = json.dumps({"mcp_loop_lag": stats, "mcp_sessions": sessions}, separators=(",", ":"))
    return f"{result.rstrip()}\n{suffix}"


# Tools to HIDE from tools/list (still callable, just not listed)
# Goal: Expose only ~30 essential tools to save context tokens
#
# INTERNAL_TOOLS: Maintenance/hooks only - completely hidden
# ADVANCED_TOOLS: Available but not listed (use via direct call or ToolSearch)
# SPECIALIZED_TOOLS: Domain-specific tools that most sessions don't need

INTERNAL_TOOLS = {
    # Maintenance
    "cleanup",
    "cleanup_code_wisdom",
    "hygiene_run",
    "hygiene_stats",
    "consolidation_scan",
    "consolidation_merge",
    "consolidation_auto",
    "batch_forget",
    "sql_query",
    "migrate_vss",
    "reembed_memories",
    "dedupe_symbols",
    "background_run_cycle",
    "background_schedule",
    "background_status",
    # Metacognition internals
    "metacognition_corrections",
    "metacognition_outcomes",
    "metacognition_evaluate",
    "distill_status",
    "enrichment_status",
    "epiplexity_check",
    # Code intel internals
    "clear_codebase",
    "clear_triplets",
    "describe_symbol",
    "extract_symbols",
    "file_dependents",
    "file_imports",
    "resolve_callsites",
    "embed_symbols",
    "restore_code_intel_confidence",
    "ssl_convert",
    "subconscious_stats",
    # Suggestions
    "suggestion_count",
    "suggestion_pending",
    "suggestion_resolve",
    "suggestion_track",
    # Transcripts
    "transcript_get",
    "transcript_list",
    "transcript_parse",
    "transcript_register",
    "transcript_remove",
    "transcript_search",
    "transcript_update",
    # Import/export
    "type_hierarchy",
    "version_check",
    "export_soul",
    "import_soul",
    # Research internals
    "connect_batch",
    "research_cycle",
    "research_store",
    "research_topics",
    # Write-gate
    "write_gate_stats",
    # Symbol event log
    "symbol_event_log",
    "mark_memory_invalidated",
    # Daemon internals
    "cycle",
    "anticipation_gate_status",
    "anticipation_record_outcome",
    "session_register",
    "session_heartbeat",
    "session_deregister",
    "msg_ack",
    "msg_ack_all",
    # SUS metrics (hook-facing, not Claude-facing)
    "log_exposure",
    # Feedback loop health diagnostics
    "chitta_health",
    # File indexing
    "file_index_all",
    # Dream management
    "dream_cancel",
}

ADVANCED_TOOLS = {
    # Memory manipulation
    "strengthen",
    "weaken",
    "tag",
    "update",
    "get",
    "query_graph",
    "expand_memory",
    # Realms
    "realm_add",
    "realm_detect",
    "realm_get",
    "realm_list",
    "realm_remove",
    "realm_set",
    "realm_visibility",
    # Goals
    "goal_set",
    "goal_get",
    "goal_list",
    "goal_complete",
    "goal_progress",
    # Habits
    "habit_observe",
    "habit_match",
    "habit_list",
    "habit_strengthen",
    "habit_weaken",
    # Anticipation
    "anticipation_predict",
    "anticipation_observe",
    "anticipation_list",
    "anticipation_success",
    "anticipation_filter",
    # Calibration
    "calibration_record",
    "calibration_score",
    # User profile
    "profile_get",
    "profile_observe",
    "profile_update",
    # Curiosity
    "curiosity_gaps",
    "curiosity_note_gap",
    "curiosity_resolve",
    # Narrative
    "narrative_history",
    "narrative_log",
    "narrative_status",
    # Context Repository (Letta-inspired)
    "memory_history",
    "memory_revert",
    "pin_memory",
    "unpin_memory",
    "list_pinned",
    "memory_lock",
    "memory_unlock",
    "memory_lock_status",
    "propose_change",
    "list_merge_queue",
    "resolve_merge",
    # Ledger (session checkpoints)
    "ledger_save",
    "ledger_get",
    "ledger_list",
    "ledger_load",
    "ledger_delete",
    # Episodes
    "create_episode",
    "episode_cluster_status",
    "get_turns",
    # Themes
    "theme_assign_orphans",
    "theme_get",
    "theme_list",
    "theme_maintain",
    "theme_recall",
    "theme_stats",
    # Long tasks
    "long_task_active",
    "long_task_complete",
    "long_task_evaluate",
    "long_task_event",
    "long_task_get",
    "long_task_snapshot",
    "long_task_start",
    "long_task_update",
    # Messaging
    "msg_history",
    "msg_inbox",
    "msg_send",
    "msg_respond",
    "msg_ack",
    "msg_ack_all",
    "session_list",
    "session_sync",
    # Explore tools (RLM-style)
    "explore_expand",
    "explore_neighbors",
    "explore_peek",
    "explore_recall",
    # Claims/entities
    "get_entities",
    "get_policies",
    "get_relationship_events",
    "query_claims",
    # Learning — individual learn_* tools replaced by unified `learn` gateway
    "learn_analysis",
    "learn_approach",
    "learn_codebase",
    "learn_correction",
    "learn_insight",
    "learn_milestone",
    "learn_outcome",
    "learn_preference",
    # Research — individual research_* tools replaced by unified `research` gateway
    "research_cycle",
    "research_store",
    "research_topics",
    # Recall variants — replaced by unified `recall` with strategy param
    "recall_by_priority",
    "recall_temporal",
    "recall_temporal_events",
    "hybrid_recall",
    "smart_recall",
    # Sadhana — individual sadhana_* tools replaced by unified `sadhana` gateway
    "sadhana_checkpoint",
    "sadhana_list",
    "sadhana_pause",
    "sadhana_resume",
    "sadhana_set_goal",
    "sadhana_set_interval",
    "sadhana_set_model",
    "sadhana_start",
    "sadhana_status",
    "sadhana_stop",
    # Triplets — individual tools replaced by unified `triplets` gateway
    "connect_temporal",
    "query_triplets_temporal",
    "triplet_history",
    # Memory edit — individual tools replaced by unified `memory_edit` gateway
    "set_memory_type",
    "set_priority_tier",
    # Maintenance — move to hidden
    "rebuild_fts_index",
    "compact_wal",
    "health_check",
    "memory_type_stats",
    "expand_query",
    "distill_set_model",
    "cooccurrence_graph",
    "find_near_duplicates",
    "labile_memories_top",
    "consolidate_similar",
    "queue_status",
    "resonance_stats",
    "route_stats",
    # Dream management (start/wander/list/status stay accessible via dream skill)
    "dream_start",
    "dream_wander",
    "dream_list",
    "dream_status",
    "dream_force_woke",
    # Probe / calibration
    "probe_calibrate",
    "probe_seed",
    "probe_status",
    "behavioral_probe",
    # Sadhana (use sadhana gateway)
    "sadhana_set_max_turns",
    # Trajectory compaction (Latent Briefing)
    "trajectory_compact",
    # Misc advanced
    "insight_global",
    "insight_promote",
    "list_aspects",
    "list_by_aspect",
    "full_resonate",
    "grow",
    "connect",
    "query",
    # File Time Machine
    "file_timeline",
    "file_at_time",
    "file_restore",
    "file_index_session",
    # SUS metrics
    "get_sus_metrics",
    # Ingest, Wiki, Training export
    "ingest_source",
    "wiki_export",
    "health_check_start",
    "export_training_pairs",
    # Skill registry
    "skill_upload",
    "skill_read",
    "skill_list",
    "skill_search",
    "skill_deprecate",
    # Agent registry
    "agent_upsert",
    "agent_get",
    "agent_list",
    "agent_disable",
    # Layer 1: Executable Constraints
    "assert_fact",
    "retract_fact",
    "query_unify",
    "query_chain",
    "explain_fact",
    "branch_create",
    "branch_resolve",
    # Layer 2: Trigger Tissue
    "trigger_add",
    "trigger_list",
    "trigger_fire",
    "trigger_dismiss",
    # Layer 3: Predictive Memory
    "predict_needed",
    # Layer 4: Surprise Memory
    "record_surprise",
    "query_surprises",
    "get_blind_spots",
    "surprise_stats",
    # Layer 5: Epistemic Debt
    "register_debt",
    "resolve_debt",
    "defer_debt",
    "query_debts",
    "get_fragile_decisions",
    "debt_stats",
    # Layer 6: Integration Kernel
    "record_feedback",
    "get_source_weights",
    "update_source_weight",
    "integration_stats",
    # Autonomous Learning (Moves 1-6)
    "surprise_learning_stats",
    "upsert_wisdom_candidate",
    "update_wisdom_lifecycle",
    "query_wisdom_candidates",
    "wisdom_promotion_stats",
    "attach_debt_evidence",
    "update_scorer_model",
    "learned_scorer_stats",
    "effective_scorer_weights",
    # Layer 7: Intervention Ledger
    "start_intervention",
    "add_observation",
    "close_intervention",
    "record_attribution",
    "query_interventions",
    "get_intervention",
    "intervention_stats",
    "list_open_interventions",
    # Layer 8: Agent Protocol Memory
    "register_task",
    "update_task",
    "add_delegation",
    "link_evidence",
    "add_probe",
    "resolve_probe",
    "set_criterion",
    "get_task",
    "query_tasks",
    "agent_protocol_stats",
    # Layer 9: Wisdom Homeostasis
    "enroll_wisdom_lineage",
    "transition_wisdom_lineage",
    "close_rederive",
    "query_wisdom_lineages",
    "get_wisdom_lineage",
    "wisdom_lineage_stats",
    "tick_lineage_staleness",
    "lineage_expiry_check",
    # Contradiction detection (legacy query tools)
    "why_active",
    "what_superseded",
    "show_conflicts",
    # CEC: Event tape + CDAWG + Sequitur (Phase 1-6)
    "log_event",
    "recall_last_action",
    "recall_failure_pattern",
    "recall_causal_antecedent",
    "recall_hdcbind",
    "consolidation_pass",
    "recall_counterfactual",
    "refutation_stats",
    "recall_motif_value",
    "executor_flush",
    "list_policies",
    "recall_true_counterfactual",
    "hypothesis_probes",
    "turiya_status",
    "tape_stats",
    "verbalize_rules",
    "queue_experiments",
    "fep_status",
    "routed_recall",
    "witness_memory",
    "reconcile_pass",
    "harvest_scope",
    "seed_hdc_geometry",
    # Hint enricher
    "run_hint_enricher",
    # Interaction ledger (v6.0)
    "ledger_append",
    "ledger_query",
    "ledger_compile",
    "ledger_contradictions",
    "ledger_health",
    # Falsifiable memories / predicate store (v6.1)
    "predicate_attach",
    "predicate_run",
    "predicate_list",
    # Span lane maintenance (span_query stays listed; these are backfill/diagnostics)
    "span_backfill",
    "span_backfill_memories",
    "span_stats",
}

# Combined set of tools to hide from listing (but still callable)
HIDDEN_TOOLS = INTERNAL_TOOLS | ADVANCED_TOOLS


def inject_message_session(tool: str, args: dict) -> str | None:
    """Stamp the calling session onto a messaging call, mutating `args` in place.

    Returns an error string when the session cannot be resolved, otherwise None.
    Fail closed rather than forwarding: the daemon would fall back to its own
    get_session_id(), which reads chittad's environment and not the caller's, and
    would silently store and read messages under session_id="".

    Both entry points to the daemon — the `advanced` gateway and call_tool —
    need this, so it lives in one place.
    """
    if tool not in MSG_TOOLS or args.get("session_id"):
        return None
    # use_cache=False forces a fresh PPID lookup, which is what makes this
    # correct across a session resume.
    sid = get_current_session_id(use_cache=False)
    if not sid:
        return (
            "Error: could not determine this session's ID for messaging. "
            "Pass session_id explicitly, or call session_register first."
        )
    args["session_id"] = sid
    # msg_send names the same value sender_session_id.
    if tool == "msg_send" and not args.get("sender_session_id"):
        args["sender_session_id"] = sid
        if not args.get("sender_realm"):
            realm = get_current_realm()
            if realm:
                args["sender_realm"] = realm
    return None


def handle_advanced(arguments: dict) -> str:
    """
    Gateway to hidden/advanced tools.

    Actions:
    - list: Show all available hidden tools by category
    - call: Call a hidden tool by name with arguments

    Examples:
    - {"action": "list"} - List all hidden tools
    - {"action": "list", "category": "internal"} - List internal tools only
    - {"tool": "pin_memory", "arguments": {"id": 123, "reason": "important"}}
    """
    action = arguments.get("action", "")
    tool = arguments.get("tool", "")
    tool_args = arguments.get("arguments", {})
    category = arguments.get("category", "")

    # If tool specified, call it
    if tool:
        if tool not in HIDDEN_TOOLS:
            # Check if it's a valid daemon tool at all
            return f"Unknown tool: {tool}\nUse action='list' to see available hidden tools."

        tool_args = dict(tool_args)
        error = inject_message_session(tool, tool_args)
        if error:
            return error

        # Call the hidden tool via daemon
        result = daemon_call(tool, tool_args)
        return f"[{tool}]\n{result}"

    # List hidden tools
    if action == "list" or not action:
        output = "Hidden Tools (callable via advanced gateway)\n"
        output += "=" * 50 + "\n\n"

        if not category or category == "advanced":
            output += "ADVANCED TOOLS (user-facing but hidden to save context):\n"
            output += "-" * 50 + "\n"
            for name in sorted(ADVANCED_TOOLS):
                output += f"  • {name}\n"
            output += f"\n  Total: {len(ADVANCED_TOOLS)} tools\n\n"

        if not category or category == "internal":
            output += "INTERNAL TOOLS (maintenance/hooks only):\n"
            output += "-" * 50 + "\n"
            for name in sorted(INTERNAL_TOOLS):
                output += f"  • {name}\n"
            output += f"\n  Total: {len(INTERNAL_TOOLS)} tools\n\n"

        output += "Usage:\n"
        output += '  {"tool": "<name>", "arguments": {...}}\n'
        output += "\nExample:\n"
        output += '  {"tool": "pin_memory", "arguments": {"id": 123, "reason": "hot context"}}\n'
        return output

    return "Unknown action. Use action='list' or specify tool='<name>'."


def strip_null_values(obj):
    """Recursively remove keys with null values from dicts."""
    if isinstance(obj, dict):
        return {k: strip_null_values(v) for k, v in obj.items() if v is not None}
    elif isinstance(obj, list):
        return [strip_null_values(item) for item in obj]
    return obj


def clean_tool_schema(tool: Tool) -> Tool:
    """Return a Tool with null values stripped from inputSchema."""
    clean_schema = strip_null_values(tool.inputSchema)
    return Tool(name=tool.name, description=tool.description, inputSchema=clean_schema)


def to_toon(obj: Any, indent: int = 0) -> str:
    """Convert JSON to TOON format (~40% fewer tokens).

    TOON format:
    - Scalars: key: value
    - Arrays: key[n]{fields}: val1|val2 | val1|val2
    - Objects: key.subkey: value
    """
    if obj is None:
        return ""

    if isinstance(obj, (int, float, bool)):
        return str(obj)

    if isinstance(obj, str):
        # Escape newlines and pipes for TOON rows
        return obj.replace("\n", "\\n").replace("|", "\\|")

    if isinstance(obj, list):
        if not obj:
            return "[]"
        # Check if list of uniform dicts
        if all(isinstance(x, dict) for x in obj):
            # Get common keys
            keys = list(obj[0].keys()) if obj else []
            if keys and all(set(x.keys()) == set(keys) for x in obj):
                # Compact table format with | separator (safer than comma)
                header = f"[{len(obj)}]{{{','.join(keys)}}}:"
                rows = []
                for item in obj:
                    vals = []
                    for k in keys:
                        v = item.get(k, "")
                        # Truncate long strings, escape newlines
                        s = str(v) if v is not None else ""
                        s = s.replace("\n", " ").replace("|", "\\|")[:80]
                        vals.append(s)
                    rows.append(" " + "|".join(vals))
                return header + "\n" + "\n".join(rows)
        # Fallback: one per line
        return "\n".join(f"- {to_toon(x)}" for x in obj)

    if isinstance(obj, dict):
        lines = []
        for k, v in obj.items():
            if v is None:
                continue
            if isinstance(v, dict):
                # Flatten nested dicts
                for sk, sv in v.items():
                    if sv is not None:
                        lines.append(f"{k}.{sk}: {to_toon(sv)}")
            elif isinstance(v, list):
                lines.append(f"{k}{to_toon(v)}")
            else:
                lines.append(f"{k}: {to_toon(v)}")
        return "\n".join(lines)

    return str(obj)


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
    """Ensure daemon is running and connected.

    Uses HTTP if CHITTA_RPC_PORT is set; falls back to Unix socket.
    Uses atomic lock file creation to prevent race conditions.
    Only ONE process should spawn the daemon - others wait.
    """
    global client

    rpc_port = os.environ.get("CHITTA_RPC_PORT", "")
    if rpc_port:
        base_url = f"http://127.0.0.1:{rpc_port}"
        if not isinstance(client, ChittaHttpClient) or getattr(client, "base_url", "") != base_url:
            client = ChittaHttpClient(base_url)
        if client.connect():
            return True
        # Daemon not up yet — fall through to spawn logic below
        socket_path = get_socket_path()
    else:
        socket_path = get_socket_path()
        if isinstance(client, ChittaHttpClient):
            client = None
        if client and client.sock:
            return True
        client = ChittaClient(socket_path)
        if client.connect():
            return True

    # Socket doesn't exist or can't connect - wait for daemon
    # First, just wait - subconscious.sh hook usually starts daemon
    for _ in range(30):
        time.sleep(0.1)
        if client.connect():
            return True

    # Still no daemon - try to start it with atomic lock.
    # NOT ".lock": that path is the daemon's single-writer mutex, an fcntl lock held on the
    # INODE for the daemon's whole life. This here is a create/unlink start-mutex — a totally
    # different protocol. Pointing both at one file meant we deleted a live daemon's mutex as
    # "stale" (it is always >60s old while healthy), after which the next daemon locked a fresh
    # inode and ran alongside it. Four concurrent "exclusive" owners is how the store died on
    # 2026-07-14. Keep the two protocols on separate paths.
    lock_path = socket_path.replace(".sock", ".startlock")
    lock_fd = None
    we_hold_lock = False

    # Clean stale lock files (older than 60 seconds). Best-effort: a racing
    # process may unlink the same path between the stat and the unlink, and an
    # unwritable lock dir is not fatal — the O_EXCL create below decides.
    try:
        if os.path.exists(lock_path):
            lock_age = time.time() - os.path.getmtime(lock_path)
            if lock_age > 60:
                os.unlink(lock_path)
    except OSError as exc:
        logger.debug("stale start-lock cleanup failed for %s: %s", lock_path, exc)

    try:
        # Try to create lock file atomically (O_EXCL fails if exists)
        lock_fd = os.open(lock_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
        we_hold_lock = True
    except FileExistsError:
        # Another process is starting daemon - wait for it
        for _ in range(50):
            time.sleep(0.1)
            if client.connect():
                return True
        return False

    if we_hold_lock and lock_fd is not None:
        try:
            # Start the daemon. Prefer the systemd unit so there is exactly ONE managed
            # writer with the correct embedder + flags. A raw `chittad daemon` with DEFAULT
            # flags spawns an UNMANAGED SECOND writer: concurrent-writer family churn that
            # corrupts the store, plus hygiene/consolidation ON (overriding the unit's
            # --no-hygiene), which has caused recall stalls + poison snapshot families.
            # Never spawn with default flags.
            home = os.environ.get("HOME", "")
            started = False
            unit = os.path.join(home, ".config", "systemd", "user", "chittad.service")
            if os.path.exists(unit):
                started = os.system("systemctl --user start chittad >/dev/null 2>&1") == 0
            if not started:
                chittad = os.path.join(home, ".claude", "bin", "chittad")
                if os.path.exists(chittad):
                    # Fallback (no systemd unit): match the managed config; at minimum
                    # never enable hygiene/distill, and pin the store path + embedder.
                    mind = os.path.join(home, ".claude", "mind")
                    model = os.path.join(home, ".claude", "bin", "bge-large-en-v1.5.gguf")
                    flags = f"--path {mind} --no-autonomous --no-distill --no-hygiene --no-enrich"
                    if os.path.exists(model):
                        flags += f" --embed-model {model}"
                    os.system(f"{chittad} daemon {flags} >/dev/null 2>&1 &")
            for _ in range(50):
                time.sleep(0.1)
                if client.connect():
                    return True
        finally:
            # Release lock. The unlink is best-effort: if it fails the file is
            # left behind, and the stale-lock sweep above reclaims it after 60s.
            os.close(lock_fd)
            try:
                os.unlink(lock_path)
            except OSError as exc:
                logger.debug("could not release start-lock %s: %s", lock_path, exc)

    return False


def _normalize_args(arguments: dict) -> dict:
    """Coerce Claude Code MCP serialization quirks.

    Claude Code serializes array parameters as JSON-encoded strings
    (e.g. tags='["a","b"]' instead of tags=["a","b"]).
    This normalizer detects and unwraps them before forwarding to the daemon.
    """
    ARRAY_KEYS = {"tags", "shared_realms", "data_paths", "script_paths", "realms"}
    result = {}
    for k, v in arguments.items():
        if k in ARRAY_KEYS and isinstance(v, str):
            stripped = v.strip()
            if stripped.startswith("["):
                try:
                    v = json.loads(stripped)
                except json.JSONDecodeError:
                    pass  # leave as-is; daemon handles comma-separated strings
            elif "," in stripped:
                v = [s.strip() for s in stripped.split(",") if s.strip()]
        result[k] = v
    return result


def daemon_call(tool_name: str, arguments: dict, structured: bool = False) -> str:
    """Call a tool on the daemon.

    Args:
        tool_name: Name of the tool to call
        arguments: Tool arguments
        structured: If True, return structured JSON data instead of text
    """
    global client

    with profile_stage("daemon_connect", tool=tool_name):
        connected = ensure_daemon()
    if not connected:
        return "Error: Failed to connect to daemon"

    arguments = _normalize_args(arguments)

    # Choke-point source_session tagging: every singular `remember` (direct or via
    # a composite wrapper like learn_*/remember_typed that builds its own dict and
    # would otherwise drop the caller session) carries the origin session. The
    # daemon's remember handler honors source_session (field_memory_recall.cpp);
    # remember_batch is tagged per-item at the call_tool layer instead. Best-effort:
    # skip silently when the session can't be resolved.
    if tool_name == "remember" and not arguments.get("source_session"):
        _sid = get_current_session_id(use_cache=True)
        if _sid:
            arguments["source_session"] = _sid

    req = {
        "jsonrpc": "2.0",
        "method": "tools/call",
        "params": {"name": tool_name, "arguments": arguments},
    }

    with profile_stage("daemon_rpc", tool=tool_name):
        response = client.call(req)

    # If no response, connection might be stale - try reconnecting once
    if not response:
        client.close()
        client = None
        if ensure_daemon():
            response = client.call(req)

    if not response:
        return "Error: No response from daemon"

    try:
        data = json.loads(response)
        result = data.get("result", {})

        # Return structured data if requested
        if structured and "structured" in result:
            return json.dumps(result["structured"])

        content = result.get("content", [])

        if content and isinstance(content, list):
            return "\n".join(
                item.get("text", str(item)) if isinstance(item, dict) else str(item)
                for item in content
            )

        return ""
    except (ValueError, AttributeError, TypeError) as e:
        # Daemon sent something that is not the expected JSON-RPC envelope.
        # Surfaced to the caller as text: every handler returns a string, and a
        # malformed response is information the caller needs, not a crash.
        logger.warning("unparseable daemon response for %s: %s", tool_name, e)
        return f"Error: {e}"


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
    """Read specific line range from a file."""
    try:
        with open(file_path, encoding="utf-8", errors="replace") as f:
            lines = f.readlines()

        # Clamp to valid range
        start = max(1, start)
        end = min(len(lines), end)

        return "".join(lines[start - 1 : end])
    except OSError as e:
        # Symbol index can outlive the file it points at (moved, deleted, or
        # permissions changed). Report the path problem instead of failing the
        # whole tool call.
        return f"Error reading file: {e}"


def handle_read_symbol(arguments: dict) -> str:
    """
    Read just a symbol's code, not entire file.

    1. find_symbol(name, kind) → get file + line range
    2. Read only line_start - 3 to line_end + 1
    3. Return: [symbol @ file:start-end] + code

    Token savings: ~10x vs full file read
    """
    name = arguments.get("name", "")
    kind = arguments.get("kind")
    project = arguments.get("project")
    context_lines = int(arguments.get("context", 3))  # Lines before/after

    if not name:
        return "Error: name parameter required"

    # Call find_symbol on daemon with structured=True to get line numbers
    find_args = {"name": name}
    if kind:
        find_args["kind"] = kind

    response = daemon_call("find_symbol", find_args, structured=True)

    # Parse structured response: {"count": N, "symbols": [...]}
    try:
        data = json.loads(response)

        symbols = data.get("symbols", [])
        if not symbols:
            return f"No symbol found: {name}"

        # The daemon's find_symbol has no project filter, so `project` — which
        # both the read_symbol and read_function schemas advertise — is applied
        # here as a preference over the returned candidates. Without it, or with
        # no candidate under that path, this is the historical first-match pick.
        symbol = symbols[0]
        if project:
            symbol = next((s for s in symbols if project in s.get("file", "")), symbol)
        file_path = symbol.get("file", "")
        line_start = symbol.get("line_start", 1)
        line_end = symbol.get("line_end", line_start + 50)
        symbol_kind = symbol.get("kind", kind or "symbol")
        symbol_name = symbol.get("name", name)

    except (json.JSONDecodeError, KeyError, TypeError) as e:
        return f"Error parsing symbol data: {e}\nResponse: {response[:200]}"

    if not file_path or not os.path.exists(file_path):
        return f"Symbol found but file not accessible: {response}"

    # Read with context
    start = max(1, line_start - context_lines)
    end = line_end + 1

    code = read_file_lines(file_path, start, end)

    # Calculate token estimate (rough: ~4 chars per token)
    tokens = len(code) // 4

    header = (
        f"[{symbol_kind} {symbol_name} @ {file_path}:{line_start}-{line_end}] (~{tokens} tokens)"
    )
    return f"{header}\n{code}"


def handle_read_function(arguments: dict) -> str:
    """Convenience wrapper for read_symbol with kind=function."""
    arguments["kind"] = arguments.get("kind", "function")
    return handle_read_symbol(arguments)


def handle_symbol_callers(arguments: dict) -> str:
    """
    Find all callers of a symbol.
    Delegates to the daemon's symbol_callers tool, which walks the callsite
    triplet graph (predicate=calls) and resolves each callsite's file:line
    to the enclosing function/method name.
    """
    name = arguments.get("name", "")
    limit = int(arguments.get("limit", 20))

    if not name:
        return "Error: name parameter required"

    return daemon_call("symbol_callers", {"name": name, "limit": limit})


def handle_symbol_callees(arguments: dict) -> str:
    """
    Find all symbols that this symbol calls.
    Delegates to the daemon's symbol_callees tool, which scans the symbol's
    line range for callsite triplets (predicate=calls).
    """
    name = arguments.get("name", "")
    limit = int(arguments.get("limit", 20))

    if not name:
        return "Error: name parameter required"

    return daemon_call("symbol_callees", {"name": name, "limit": limit})


def handle_verify_correction(arguments: dict) -> str:
    """
    Mark a correction memory as verified at a given evidence locus.
    Stores a new [correction:<id>] verified_at:<locus> memory and
    updates the original correction to append 'verified' in its tags.
    """
    mem_id = str(arguments.get("id", ""))
    evidence_locus = arguments.get("evidence_locus", "")

    if not mem_id:
        return "Error: 'id' parameter required"
    if not evidence_locus:
        return "Error: 'evidence_locus' parameter required"

    # Store verification record
    verify_result = daemon_call(
        "remember",
        {
            "content": f"[correction:{mem_id}] verified_at:{evidence_locus} by:agent",
            "tags": ["correction", "verified"],
            "type": "correction",
            "visibility": 2,
        },
    )

    # Update original memory tags to include 'verified'
    daemon_call("tag", {"id": int(mem_id) if mem_id.isdigit() else mem_id, "tags": ["verified"]})

    return f"Correction {mem_id} verified at {evidence_locus}\n{verify_result}"


def handle_ack_memory(arguments: dict) -> str:
    """
    Increment ack signal for a memory.
    Stores a [ack] memory:<id> score:+1 entry with tag ack-signal.
    """
    mem_id = str(arguments.get("id", ""))
    if not mem_id:
        return "Error: 'id' parameter required"

    # Route to the daemon's ack_memory RPC so the ack actually updates ack_scores
    # (the recall scoring input). Previously this stored an inert [ack] memory and
    # ack_score stayed permanently 0.
    daemon_call("ack_memory", {"id": mem_id})

    return json.dumps({"acked": mem_id})


def handle_nack_memory(arguments: dict) -> str:
    """
    Decrement nack signal for a memory.
    Stores a [nack] memory:<id> score:-1 entry with tag nack-signal.
    """
    mem_id = str(arguments.get("id", ""))
    if not mem_id:
        return "Error: 'id' parameter required"

    # Route to the daemon's nack_memory RPC so the nack actually updates ack_scores.
    daemon_call("nack_memory", {"id": mem_id})

    return json.dumps({"nacked": mem_id})


def handle_memory_outcome(arguments: dict) -> str:
    """
    Record an outcome observation for a memory's utility posterior
    (alpha += weight on success, beta += weight on failure).
    """
    mem_id = str(arguments.get("id", ""))
    if not mem_id:
        return "Error: 'id' parameter required"
    success = arguments.get("success")
    if not isinstance(success, bool):
        return "Error: 'success' (boolean) parameter required"
    params = {"id": mem_id, "success": success}
    weight = arguments.get("weight")
    if weight is not None:
        params["weight"] = float(weight)
    return daemon_call("memory_outcome", params)


_TYPED_NODE_TYPES = frozenset(
    {"digest-node", "symbol-summary", "decision", "open-question", "rollup", "working-brief"}
)
_LINK_PREDICATES = frozenset({"supersedes", "invalidated-by", "anchors-to"})


def handle_remember_typed(arguments: dict) -> str:
    """
    Store a typed memory node with optional graph links.

    node_type: digest-node | symbol-summary | decision | open-question | rollup | working-brief
    links: list of {predicate, target_id} using supersedes | invalidated-by | anchors-to
    """
    node_type = arguments.get("node_type", "")
    content = arguments.get("content", "")
    subject = arguments.get("subject", "")
    links = arguments.get("links", [])
    realm = arguments.get("realm", "brahman")

    if not node_type:
        return "Error: 'node_type' parameter required"
    if node_type not in _TYPED_NODE_TYPES:
        return f"Error: node_type must be one of {sorted(_TYPED_NODE_TYPES)}"
    if not content:
        return "Error: 'content' parameter required"
    if not subject:
        return "Error: 'subject' parameter required"

    # Store primary memory with typed tags
    store_args: dict = {
        "content": content,
        "tags": [f"typed:{node_type}", node_type],
        "type": "wisdom",
        "visibility": 2,
        "realm": realm,
    }

    remember_result = daemon_call("remember", store_args)

    # Extract new ID from result (format varies; try to parse integer)
    new_id = None
    id_match = re.search(r"\b(\d+)\b", remember_result)
    if id_match:
        new_id = id_match.group(1)
    if not new_id:
        new_id = f"{subject[:20].replace(' ', '_').lower()}:{node_type}"

    # Write link triplet memories
    links_written = 0
    if isinstance(links, list):
        for link in links:
            predicate = link.get("predicate", "")
            target_id = str(link.get("target_id", ""))
            if not predicate or not target_id:
                continue
            if predicate not in _LINK_PREDICATES:
                continue
            daemon_call(
                "remember",
                {
                    "content": f"[link] {new_id} {predicate} {target_id}",
                    "tags": ["link", predicate],
                    "type": "episode",
                    "visibility": 2,
                    "realm": realm,
                },
            )
            links_written += 1

    return json.dumps({"id": new_id, "node_type": node_type, "links_written": links_written})


def handle_smart_context(arguments: dict) -> str:
    """
    Build intelligent context combining memories, code symbols, and graph relationships.

    Modes:
    - fast (default): C++ daemon single-RPC (<80ms)
    - full: daemon full_resonate (<200ms)
    - rlm: RLM-style dynamic exploration via soul_repl

    RLM mode lets Claude write exploration code for complex queries.

    When resolver_mode=True (default), prepends digest-node and decision memories
    before the code symbols section.
    """
    mode = arguments.get("mode", "fast")
    resolver_mode = arguments.get("resolver_mode", True)

    # RLM mode: use soul_repl for dynamic exploration
    if mode == "rlm":
        task = arguments.get("task", "")
        query = arguments.get("query", task)

        if not query:
            return "Error: 'task' or 'query' parameter required for RLM mode"

        # Generate exploration code based on the task
        exploration_code = f'''
# RLM-style context gathering for: {query[:100]}
results = {{}}

# 1. Semantic memory search
memories = soul.search("{query[:200]}", limit=10)
results["memories"] = [(m.id, m.score, m.content[:150]) for m in memories if m.score > 0.3]

# 2. Find related code symbols
symbols = soul.symbols(pattern="{query.split()[0] if query else ""}", limit=5)
results["symbols"] = symbols[:5] if symbols else []

# 3. Expand top memory for full context
if memories and memories[0].score > 0.5:
    expanded = soul.expand(memories[0].id, depth=2)
    results["expanded"] = expanded

# 4. Graph relationships for top result
if memories:
    triplets = soul.triplets(subject=f"memory:{{memories[0].id}}", limit=5)
    results["relations"] = [str(t) for t in triplets]

results["trajectory"] = soul.trajectory()
results
'''
        try:
            from soul_repl import execute_soul_code

            result, _ns = execute_soul_code(exploration_code)

            if result.error:
                return f"RLM exploration error: {result.error}"

            # Format output
            output = [f"[RLM Context for: {query[:50]}...]"]
            output.append(f"Trajectory: {len(result.trajectory)} soul calls")
            output.append("")

            if result.result:
                data = result.result
                if "memories" in data:
                    output.append(f"Memories ({len(data['memories'])}):")
                    for mid, score, content in data["memories"][:5]:
                        output.append(f"  [{mid}] {score:.0%} {content[:80]}...")

                if "symbols" in data and data["symbols"]:
                    output.append(f"\nSymbols ({len(data['symbols'])}):")
                    for s in data["symbols"][:3]:
                        output.append(f"  {s.get('name', '?')} @ {s.get('file', '?')}")

                if "expanded" in data:
                    output.append(f"\nExpanded context: {list(data['expanded'].keys())}")

                if "relations" in data and data["relations"]:
                    output.append(f"\nRelations: {data['relations'][:3]}")

            return "\n".join(output)

        except Exception as e:  # noqa: BLE001
            # execute_soul_code runs generated Python in a sandbox, so any
            # exception type is reachable by construction. RLM is an optional
            # accelerator; the daemon path below is the supported answer.
            logger.info("RLM smart_context failed, using daemon path: %s", e)
            return f"RLM mode failed: {e}, falling back to daemon"

    # Default: delegate to C++ daemon (fast single-RPC)
    # Resolver hierarchy: prepend digest-node + decision memories when resolver_mode=True
    if not resolver_mode:
        return daemon_call("smart_context", arguments)

    task = arguments.get("task", "")
    prefix_parts = []

    if task:
        # 1. Check for digest-node memories relevant to the task
        digest_result = daemon_call(
            "recall",
            {
                "query": task,
                "tag": "digest-node",
                "limit": 3,
            },
        )
        if digest_result and "No memories" not in digest_result and "Error" not in digest_result:
            prefix_parts.append("[digest-nodes]\n" + digest_result)

        # 2. Check for decision memories relevant to the task
        decision_result = daemon_call(
            "recall",
            {
                "query": task,
                "tag": "decision",
                "limit": 3,
            },
        )
        if (
            decision_result
            and "No memories" not in decision_result
            and "Error" not in decision_result
        ):
            prefix_parts.append("[decisions]\n" + decision_result)
        else:
            # Also try text search for [dec] prefix
            dec_result = daemon_call(
                "recall",
                {
                    "query": f"[dec] {task}",
                    "limit": 2,
                },
            )
            if dec_result and "No memories" not in dec_result and "Error" not in dec_result:
                prefix_parts.append("[decisions]\n" + dec_result)

    # 3. Fall through to existing symbol/code lookup
    code_result = daemon_call("smart_context", arguments)

    if prefix_parts:
        return "\n\n".join(prefix_parts) + "\n\n[code-context]\n" + code_result
    return code_result


def handle_lookup(arguments: dict) -> str:
    return daemon_call("lookup", arguments)


def _slug(text: str, width: int = 40) -> str:
    """Triplet-node slug: lowercase, underscore-joined, width-capped."""
    return text[:width].replace(" ", "_").lower()


def _store_learning(
    content: str,
    tags: list[str],
    mem_type: str,
    visibility: int,
    subject: str,
    predicate: str,
    obj: str,
) -> str:
    """Store one learning memory and link it into the triplet graph.

    Every handle_learn_* variant differs only in how it renders `content` and
    which triplet edge it draws; this is the write half they share. Returns the
    daemon's remember() response so callers that surface it can, and the rest
    can ignore it.
    """
    remembered = daemon_call(
        "remember",
        {
            "content": content,
            "tags": tags,
            "type": mem_type,
            "visibility": visibility,
        },
    )
    daemon_call("connect", {"subject": subject, "predicate": predicate, "object": obj})
    return remembered


def handle_learn_correction(arguments: dict) -> str:
    """Store a correction, flagging it when similar corrections already exist."""
    wrong = arguments.get("wrong", "")
    correct = arguments.get("correct", "")
    context = arguments.get("context", "")

    if not wrong or not correct:
        return "Error: both 'wrong' and 'correct' parameters required"

    # Repeat-mistake check. Advisory only: daemon_call returns an error string
    # rather than raising, so a failed recall simply yields no matches and no
    # warning — it must never block the correction write.
    existing = daemon_call("recall", {"query": wrong[:100], "tag": "correction", "limit": 3})
    high_matches = [int(m) for m in re.findall(r"\[(\d+)%\]", str(existing)) if int(m) > 70]
    repeat_warning = ""
    if high_matches:
        repeat_warning = (
            f"\n⚠️ REPEAT MISTAKE DETECTED ({len(high_matches)} similar corrections "
            f"exist, highest {max(high_matches)}% match)"
        )

    # SSL form — action first, so a truncated render still shows the solution.
    content = f"[correction] USE: {correct}\nNOT: {wrong}"
    if context:
        content += f"\n@{context.replace(' ', '-').lower()}"

    tags = ["correction", "high-priority"]
    if repeat_warning:
        content += "\n[REPEAT-MISTAKE]"
        tags.append("repeat-mistake")

    wrong_slug = _slug(wrong, 50)
    correct_slug = _slug(correct, 50)
    # visibility 2 (global): corrections apply everywhere, not just this realm.
    _store_learning(
        content,
        tags,
        "correction",
        2,
        subject=correct_slug,
        predicate="corrects",
        obj=wrong_slug,
    )
    return (
        f"Correction stored:\n  USE: {correct}\n  NOT: {wrong}\n"
        f"  Triplet: {correct_slug} → corrects → {wrong_slug}{repeat_warning}"
    )


def handle_learn_preference(arguments: dict) -> str:
    """Store a user preference. Global visibility — preferences apply everywhere."""
    category = arguments.get("category", "general")
    preference = arguments.get("preference", "")
    example = arguments.get("example", "")

    if not preference:
        return "Error: 'preference' parameter required"

    content = f"[preference:{category}] {preference}"
    if example:
        content += f"\nExample: {example}"

    _store_learning(
        content,
        ["preference", category],
        "preference",
        2,
        subject="user",
        predicate=f"prefers_{category}",
        obj=_slug(preference, 50),
    )
    return f"Preference stored:\n  Category: {category}\n  Preference: {preference}"


def handle_learn_insight(arguments: dict) -> str:
    """Store a generalizable insight. Always global — these are cross-project."""
    domain = arguments.get("domain", "general")
    insight = arguments.get("insight", "")
    learned_from = arguments.get("learned_from", "")

    if not insight:
        return "Error: 'insight' parameter required"

    content = f"[insight:{domain}] {insight}"
    if learned_from:
        content += f"\nLearned from: {learned_from}"

    _store_learning(
        content,
        ["insight", domain, "cross-project"],
        "wisdom",
        2,
        subject=domain,
        predicate="has_insight",
        obj=_slug(insight),
    )
    return f"Insight stored (global):\n  Domain: {domain}\n  Insight: {insight}"


def handle_learn_approach(arguments: dict) -> str:
    """Store what approach worked in a given state, building emotional memory."""
    state = arguments.get("state", "general")
    approach = arguments.get("approach", "")
    outcome = arguments.get("outcome", "")

    if not approach:
        return "Error: 'approach' parameter required"

    content = f"[approach:{state}] When {state}: {approach}"
    if outcome:
        content += f"\nOutcome: {outcome}"

    _store_learning(
        content,
        ["approach", state, "emotional-memory"],
        "wisdom",
        2,
        subject=state,
        predicate="helped_by",
        obj=_slug(approach),
    )
    return f"Approach stored:\n  State: {state}\n  Approach: {approach}"


def handle_learn_outcome(arguments: dict) -> str:
    """Record whether a suggestion actually helped, closing the feedback loop."""
    suggestion = arguments.get("suggestion", "")
    helped = arguments.get("helped", False)
    details = arguments.get("details", "")

    if not suggestion:
        return "Error: 'suggestion' parameter required"

    outcome_type = "worked" if helped else "failed"
    content = f"[outcome:{outcome_type}] {suggestion}"
    if details:
        content += f"\nWhy: {details}"

    tags = ["outcome", outcome_type, "success" if helped else "failure"]
    _store_learning(
        content,
        tags,
        "episode",
        2,
        subject=_slug(suggestion),
        predicate="resulted_in",
        obj=outcome_type,
    )
    return f"Outcome recorded:\n  Suggestion: {suggestion}\n  Helped: {helped}"


def handle_learn_milestone(arguments: dict) -> str:
    """Record a relationship milestone. Global — milestones matter everywhere."""
    milestone = arguments.get("milestone", "")
    significance = arguments.get("significance", "")
    date = arguments.get("date", "") or datetime.date.today().isoformat()

    if not milestone:
        return "Error: 'milestone' parameter required"

    content = f"[milestone] {date}: {milestone}"
    if significance:
        content += f"\nSignificance: {significance}"

    _store_learning(
        content,
        ["milestone", "relationship", "achievement"],
        "episode",
        2,
        subject="partnership",
        predicate="achieved",
        obj=_slug(milestone),
    )
    return f"Milestone recorded:\n  Date: {date}\n  Milestone: {milestone}"


def handle_learn_analysis(arguments: dict) -> str:
    """Record an analysis with its data and script locations, so it can be rerun."""
    name = arguments.get("name", "")
    description = arguments.get("description", "")
    data_paths = arguments.get("data_paths", [])
    script_paths = arguments.get("script_paths", [])
    findings = arguments.get("findings", "")
    project = arguments.get("project", "")

    if not name:
        return "Error: 'name' parameter required"

    if not isinstance(data_paths, list):
        data_paths = [data_paths]
    if not isinstance(script_paths, list):
        script_paths = [script_paths]

    content = f"[analysis:{project or 'general'}] {name}"
    if description:
        content += f"\nDescription: {description}"
    if data_paths:
        content += f"\nData: {', '.join(data_paths)}"
    if script_paths:
        content += f"\nScripts: {', '.join(script_paths)}"
    if findings:
        content += f"\nFindings: {findings}"

    analysis_slug = _slug(name)
    # visibility 0: an analysis is project-bound, so it stays private to its realm.
    _store_learning(
        content,
        ["analysis", project or "general", "reproducibility"],
        "episode",
        0,
        subject=project or "general",
        predicate="has_analysis",
        obj=analysis_slug,
    )
    for path in data_paths:
        if path:
            daemon_call(
                "connect",
                {
                    "subject": analysis_slug,
                    "predicate": "uses_data",
                    "object": path[:60],
                },
            )

    # Render the joined form, not the list repr: a caller may pass either a
    # list or a single comma-separated string, and the confirmation should look
    # the same either way.
    return (
        f"Analysis recorded:\n  Name: {name}\n"
        f"  Data: {', '.join(data_paths)}\n  Scripts: {', '.join(script_paths)}"
    )


# ============================================================================
# Curiosity-driven research (background learning agent)
# ============================================================================


def handle_research_topics(arguments: dict) -> str:
    """Get topics that need research from various sources."""
    source = arguments.get("source", "gaps")
    limit = arguments.get("limit", 3)
    realm = arguments.get("realm", "")

    topics = []

    if source == "gaps" or source == "all":
        # Get unresolved curiosity gaps
        gaps_result = daemon_call("curiosity_gaps", {"limit": limit, "realm": realm})
        if gaps_result and "No gaps" not in gaps_result:
            # Parse gaps from response
            for line in gaps_result.split("\n"):
                if line.startswith("#") and "[" in line:
                    # Extract gap ID and content
                    parts = line.split("]", 1)
                    if len(parts) > 1:
                        gap_id = parts[0].split("#")[-1].strip()
                        content = parts[1].strip()
                        topics.append(
                            {
                                "type": "gap",
                                "id": gap_id,
                                "topic": content[:200],
                                "source": "curiosity_gaps",
                            }
                        )

    if source == "weak" or source == "all":
        # Get low-confidence memories that might need verification
        weak_result = daemon_call(
            "recall", {"query": "uncertain unclear unverified", "limit": limit}
        )
        if weak_result and "No memories" not in weak_result:
            for line in weak_result.split("\n"):
                if line.startswith("[") and "%" in line:
                    # Extract confidence and content
                    conf_match = line.split("%")[0].strip("[")
                    if conf_match.isdigit() and int(conf_match) < 30:
                        content = line.split("]", 2)[-1].strip()
                        topics.append(
                            {
                                "type": "weak_memory",
                                "confidence": int(conf_match),
                                "topic": content[:200],
                                "source": "low_confidence",
                            }
                        )

    if not topics:
        return "No research topics found. Consider:\n- Adding curiosity gaps with curiosity_note_gap\n- Or specify source='suggest' for AI-suggested topics"

    output = f"Research Topics ({len(topics)} found):\n"
    output += "=" * 40 + "\n\n"
    for i, t in enumerate(topics[:limit], 1):
        output += f"{i}. [{t['type']}] {t['topic']}\n"
        if t.get("id"):
            output += f"   Gap ID: {t['id']} (use with research_store to resolve)\n"
        output += "\n"

    output += (
        "\nNext: Use WebSearch to research these topics, then call research_store with findings."
    )
    return output


def handle_research_store(arguments: dict) -> str:
    """Store research results as memories with source attribution."""
    topic = arguments.get("topic", "")
    findings = arguments.get("findings", "")
    sources = arguments.get("sources", [])
    gap_id = arguments.get("gap_id")
    confidence = arguments.get("confidence", 0.7)

    if not topic or not findings:
        return "Error: topic and findings are required"

    # Format sources
    source_str = ""
    if sources:
        source_str = "\nSources: " + " | ".join(sources[:3])

    # Create memory content in SSL format
    content = f"[research] {topic}\n{findings}{source_str}"

    # Store as wisdom with research tag
    daemon_call(
        "remember",
        {
            "content": content,
            "type": "wisdom",
            "confidence": confidence,
            "tags": ["research", "web-learned"],
        },
    )

    output = f"Research stored: {topic[:50]}...\n"

    # Resolve curiosity gap if provided
    if gap_id:
        daemon_call("curiosity_resolve", {"id": gap_id, "learned": findings[:500]})
        output += f"Resolved gap #{gap_id}\n"

    # Create triplet linking research to topic
    daemon_call(
        "connect",
        {
            "subject": "research",
            "predicate": "learned_about",
            "object": topic.replace(" ", "_")[:50],
        },
    )

    return output + "Memory created with 'research' tag."


def handle_research_cycle(arguments: dict) -> str:
    """Run one research cycle - returns topic with context for web search."""
    realm = arguments.get("realm", "")

    # Try curiosity_gaps first (tag-based query)
    gaps_result = daemon_call("curiosity_gaps", {"limit": 5, "realm": realm})
    if gaps_result and "No" not in gaps_result and "#" in gaps_result:
        for line in gaps_result.split("\n"):
            if "#" in line and ":" in line:
                parts = line.split(":", 1)
                if len(parts) > 1:
                    gap_id = parts[0].strip().replace("#", "").strip()
                    topic = parts[1].strip()

                    # Get context from related memories
                    context_result = daemon_call("recall", {"query": topic[:100], "limit": 3})

                    output = "Research Cycle: Topic Found\n"
                    output += "=" * 40 + "\n\n"
                    output += f"Topic: {topic}\n"
                    output += f"Gap ID: {gap_id}\n\n"

                    output += "Related memories:\n"
                    if context_result and "No memories" not in context_result:
                        ctx_lines = [
                            line
                            for line in context_result.split("\n")
                            if line.strip() and "[gap]" not in line
                        ][:5]
                        output += "\n".join(ctx_lines) + "\n\n"
                    else:
                        output += "(none found)\n\n"

                    output += "Instructions:\n"
                    output += f"1. Use WebSearch to research: {topic}\n"
                    output += f"2. Call research_store with findings and gap_id={gap_id}\n"
                    return output

    # Fallback: search for gap content via recall
    # Try to find memories with "[gap]" in content
    gaps_result = daemon_call(
        "recall",
        {
            "query": "How does DuckDB HNSW vector indexing",  # Use actual gap content
            "limit": 10,
        },
    )

    if gaps_result and "No memories" not in gaps_result:
        for line in gaps_result.split("\n"):
            if "[gap]" in line:
                # Extract content after the tags
                parts = line.split("]")
                if len(parts) >= 3:
                    content = parts[-1].strip()
                    if content and len(content) > 10:
                        output = "Research Cycle: Topic Found\n"
                        output += "=" * 40 + "\n\n"
                        output += f"Topic: {content[:200]}\n\n"
                        output += "Instructions:\n"
                        output += f"1. Use WebSearch to research: {content[:100]}\n"
                        output += "2. Call research_store with topic and findings\n"
                        return output

    return "No research topics available. Add curiosity gaps with curiosity_note_gap first."


def handle_transcript_search(arguments: dict) -> str:
    """
    Search transcript content directly in Python (fast, no daemon).
    Keyword-based search - ranks by keyword density.
    """
    query = arguments.get("query", "")
    session_id = arguments.get("session_id", "")
    limit = int(arguments.get("limit", 10))

    if not query:
        return "Error: query parameter required"

    # Extract keywords (3+ chars)
    keywords = [w.lower() for w in query.split() if len(w) >= 3]
    if not keywords:
        return "Error: query must contain words with 3+ characters"

    # Get transcript path(s)
    transcript_paths = []
    if session_id:
        # Get specific session's transcript
        result = daemon_call("transcript_get", {"session_id": session_id}, structured=True)
        try:
            data = json.loads(result)
            if "transcript_path" in data:
                transcript_paths.append((session_id, data["transcript_path"]))
        except (json.JSONDecodeError, AttributeError, TypeError) as exc:
            # Daemon returned an error string rather than structured JSON.
            logger.debug("transcript_get gave no usable path for %s: %s", session_id, exc)
    else:
        # Get all pending transcripts
        result = daemon_call("transcript_list", {}, structured=True)
        try:
            data = json.loads(result)
            for t in data.get("transcripts", []):
                transcript_paths.append((t.get("session_id", ""), t.get("transcript_path", "")))
        except (json.JSONDecodeError, AttributeError, TypeError) as exc:
            logger.debug("transcript_list gave no usable paths: %s", exc)

    if not transcript_paths:
        return "No transcripts found"

    # Search transcripts
    results = []
    for sid, path in transcript_paths:
        if not path or not os.path.exists(path):
            continue

        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                for line_num, line in enumerate(f, 1):
                    if not line.strip():
                        continue
                    try:
                        entry = json.loads(line)
                        msg_type = entry.get("type", "")
                        if msg_type not in ("user", "assistant"):
                            continue

                        content = ""
                        msg = entry.get("message", {})
                        msgcontent = msg.get("content", "")
                        if isinstance(msgcontent, str):
                            content = msgcontent
                        elif isinstance(msgcontent, list):
                            for block in msgcontent:
                                if isinstance(block, dict) and "text" in block:
                                    content += block["text"] + "\n"

                        if len(content) < 20:
                            continue

                        # Count keyword matches
                        content_lower = content.lower()
                        match_count = sum(1 for kw in keywords if kw in content_lower)
                        if match_count == 0:
                            continue

                        # Score by keyword density
                        score = match_count / len(keywords)
                        results.append(
                            {
                                "session_id": sid,
                                "line": line_num,
                                "role": msg_type,
                                "content": content[:500] + ("..." if len(content) > 500 else ""),
                                "score": score,
                                "matches": match_count,
                            }
                        )
                    except (json.JSONDecodeError, AttributeError, TypeError):
                        # Transcripts are append-only JSONL written by a live
                        # process: a torn final line, or an entry whose
                        # "message" is not an object, is expected. Skip the
                        # line, keep scanning the file.
                        continue
        except OSError as exc:
            # Transcript deleted or permissions changed between the exists()
            # check and the open. Skip this file, keep scanning the rest.
            logger.debug("skipping unreadable transcript %s: %s", path, exc)
            continue

    # Sort by score descending
    results.sort(key=lambda x: x["score"], reverse=True)
    results = results[:limit]

    if not results:
        return f"No matches found for: {query}"

    # Format output
    output = f"Found {len(results)} matches for: {query}\n"
    output += "=" * 50 + "\n\n"
    for i, r in enumerate(results, 1):
        output += f"{i}. [{r['role']}] (line {r['line']}, score: {r['score']:.2f})\n"
        output += f"   {r['content'][:200]}...\n\n"

    return output


def handle_soul_repl(arguments: dict) -> str:
    """
    RLM-style REPL for programmatic soul exploration.

    Executes Python code in a sandbox with soul.* methods exposed.
    Supports persistent sessions via session_id — variables survive across calls.
    Session namespaces are stored natively in chitta-field (repl_sessions.json),
    not in the wisdom-memory graph.
    """
    code = arguments.get("code", "")
    reset = arguments.get("reset", False)
    session_id = arguments.get("session_id", "")

    if not code.strip():
        return """Soul REPL - RLM-style Memory Exploration

Write Python code to explore memories programmatically.
Available methods:

  soul.search(query, limit=20)     - Semantic search
  soul.recall(query, limit=10)     - Hybrid recall (semantic + keyword + graph)
  soul.expand(memory_id, depth=3)  - Drill down: SSL -> Episode -> Full turns
  soul.triplets(subject=, predicate=, object=)  - Knowledge graph query
  soul.recent(hours=24)            - Recent memories by time
  soul.remember(content, tags=[])  - Store new memory
  soul.symbols(pattern=, kind=)    - Search code symbols
  soul.read_symbol(name)           - Read symbol source
  soul.stats()                     - Soul statistics
  soul.trajectory()                - Get exploration path

Persistent sessions (variables survive across calls):
  Pass session_id="my_session" to keep variables alive.
  Pass reset=true to clear session state.
"""

    try:
        raw = daemon_call(
            "repl_execute",
            {
                "session_id": session_id or "default",
                "code": code,
                "reset": reset,
                "max_output": 10000,
            },
        )
        try:
            result = json.loads(raw)
        except (ValueError, TypeError):
            return raw or "(no output)"

        output = []
        if result.get("output"):
            output.append(result["output"])
        if result.get("error"):
            output.append(f"\nError:\n{result['error']}")
        traj = result.get("trajectory", [])
        if traj:
            output.append(f"\n[Trajectory: {len(traj)} soul calls]")
            for t in traj[-5:]:
                output.append(
                    f"  - {t['method']}({', '.join(f'{k}={repr(v)[:30]}' for k, v in t['args'].items())})"
                )
        if session_id:
            output.append(f"\n[Session: {session_id}]")

        return "\n".join(output) if output else "(no output)"

    except (ValueError, TypeError, KeyError, AttributeError) as e:
        # The daemon reports REPL errors inside its own JSON payload; reaching
        # here means the envelope itself was not the shape we expect.
        logger.warning("malformed repl_execute response: %s", e)
        return f"REPL Error: {e}"


# ============================================================================
# Consolidated gateway handlers — reduce tool count for token efficiency
# ============================================================================


def handle_recall_smart(arguments: dict) -> str:
    """Multi-lane retrieval: query planner → semantic + typed + spreading + session lanes → RRF merge."""
    query = arguments.get("query", "")
    limit = int(arguments.get("limit", 10))
    realm = arguments.get("realm", "")
    skip_llm = arguments.get("skip_llm_plan", False)

    plan = {"entities": [], "speech_act": None, "answer_type": "fact"}

    if not skip_llm:
        try:
            import anthropic

            anthropic_client = anthropic.Anthropic()
            resp = anthropic_client.messages.create(
                model="claude-haiku-4-5-20251001",
                max_tokens=200,
                messages=[
                    {
                        "role": "user",
                        "content": (
                            f"Extract retrieval metadata from this query. Return JSON only, no prose.\n"
                            f"Fields: entities (array of key noun phrases), speech_act "
                            f"(one of: decision/correction/preference/task/result/failure/question/hypothesis/null), "
                            f"answer_type (one of: fact/decision/preference/code/temporal).\n"
                            f"Query: {query}"
                        ),
                    }
                ],
            )
            plan = json.loads(resp.content[0].text.strip())
        except Exception as exc:  # noqa: BLE001
            # The planner is a best-effort accelerator over an optional
            # dependency (`anthropic`), a network call, and unconstrained model
            # output — missing package, auth failure, timeout, and non-JSON
            # replies all mean the same thing here. Recall must still run, so
            # fall through to the empty plan: lanes 2 and 3 simply stay off.
            logger.info("recall_smart query planner unavailable: %s", exc)

    realm_arg = {"realm": realm} if realm else {}

    # Lane 1: semantic recall (always)
    sem_raw = daemon_call("recall", {"query": query, "limit": limit * 2, **realm_arg})
    try:
        sem_hits = json.loads(sem_raw).get("results", [])
    except (ValueError, AttributeError) as exc:
        # daemon_call returns plain text on error rather than raising.
        logger.warning("recall_smart semantic lane returned no JSON: %s", exc)
        sem_hits = []

    lanes = [sem_hits]

    # Lane 2: typed recall (if speech_act detected)
    if plan.get("speech_act"):
        typed_raw = daemon_call(
            "recall", {"query": query, "tag": plan["speech_act"], "limit": limit, **realm_arg}
        )
        try:
            lanes.append(json.loads(typed_raw).get("results", []))
        except (ValueError, AttributeError) as exc:
            # One dead lane must not sink the merge; RRF just fuses fewer lanes.
            logger.debug("recall_smart typed lane returned no JSON: %s", exc)

    # Lane 3: spreading activation (if entities found)
    entities = plan.get("entities", [])
    if entities:
        seed_query = " ".join(entities[:5])
        spread_raw = daemon_call(
            "recall_spreading", {"query": seed_query, "limit": limit, **realm_arg}
        )
        try:
            lanes.append(json.loads(spread_raw).get("results", []))
        except (ValueError, AttributeError) as exc:
            logger.debug("recall_smart spreading lane returned no JSON: %s", exc)

    # Lane 4: session-level recall
    sess_raw = daemon_call("recall_session", {"query": query, "limit": limit, **realm_arg})
    try:
        sess_data = json.loads(sess_raw).get("results", [])
        # Normalize session hits to same shape as memory hits
        for s in sess_data:
            s.setdefault("memory_id", s.get("session_id", ""))
            s.setdefault("text", s.get("best_evidence", ""))
        lanes.append(sess_data)
    except (ValueError, AttributeError, TypeError) as exc:
        logger.debug("recall_smart session lane returned no JSON: %s", exc)

    merged = rrf_merge(lanes, k=60, limit=limit)
    return json.dumps({"results": merged, "plan": plan})


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
    return 0.7 + 0.3 * _KIND_MULTIPLIER.get(str(hit.get("type", "")), 1.0)


@profile_async("recall_gateway")
async def handle_recall_gateway(arguments: dict) -> str:
    """Unified recall with strategy routing and optional cross-encoder reranking.

    Strategies: hybrid (default), semantic, priority, temporal, smart, keyword.
    Every value maps to a tool the daemon actually serves; an unknown strategy
    falls back to plain semantic recall.

    Default is hybrid: measured strict superset of pure-semantic on the golden
    set (nDCG@20 +0.08 active, +2 pass, 0 regressions) — hybrid's BM25 lane
    catches literal tokens (filenames, IDs, paths) that pure-semantic misses at
    low cosine similarity. Callers wanting the old behavior pass strategy="semantic".
    """
    strategy = arguments.pop("strategy", "hybrid")
    # "field" used to map to a `recall_field` RPC the daemon does not implement.
    # Unknown tool names come back as an empty content array, so that strategy
    # silently returned no memories rather than erroring; dropped so it falls
    # through to semantic recall like any other unrecognized strategy.
    tool_map = {
        "semantic": "recall",
        "priority": "recall_by_priority",
        "temporal": "recall_temporal",
        "hybrid": "hybrid_recall",
        "smart": "smart_recall",
        "keyword": "recall_keyword",
        # daemon-native lane names: `recall` defaults to fused; make the
        # published strategy values resolve explicitly instead of by fallthrough.
        "fused": "recall",
    }
    tool = tool_map.get(strategy, "recall")

    loop = asyncio.get_running_loop()
    reranker = await run_reranker(get_reranker)
    if not reranker:
        return await loop.run_in_executor(_executor, daemon_call, tool, arguments)

    query = arguments.get("query", "")
    limit = int(arguments.get("limit", 10))
    fetch_args = dict(arguments, limit=limit * RERANK_FETCH_MUL)
    raw_str = await loop.run_in_executor(_executor, daemon_call, tool, fetch_args, True)
    try:
        raw = json.loads(raw_str)
        results = raw.get("results", [])
    except (ValueError, AttributeError) as exc:
        # No structured payload to rerank; the unreranked call below is the
        # fallback, so this is a downgrade rather than a failure.
        logger.debug("recall overfetch returned no JSON, skipping rerank: %s", exc)
        results = []
    if not results or len(results) <= limit:
        return await loop.run_in_executor(_executor, daemon_call, tool, arguments)

    pairs = [(query, h.get("text", "")) for h in results]
    scores = await run_reranker(reranker.predict, pairs)
    if len(scores) != len(results):
        # A backend returning a different number of scores than candidates is
        # broken. zip would silently truncate and drop memories from recall, so
        # fall back to the daemon's own ranking instead. Checked explicitly
        # rather than with zip(strict=True), which is 3.10+.
        logger.warning(
            "reranker returned %d scores for %d candidates; skipping rerank",
            len(scores),
            len(results),
        )
        return await loop.run_in_executor(_executor, daemon_call, tool, arguments)
    # The cross-encoder scores text similarity only; keep the daemon's kind
    # prior (chitta-field scoring/config.rs) so a correction still outranks a
    # verbatim transcript fragment of equal similarity. Logits pass through a
    # sigmoid so the prior is a bounded envelope, as in the daemon.
    ranked = sorted(
        zip(scores, results),
        key=lambda x: -(1.0 / (1.0 + math.exp(-float(x[0])))) * _kind_envelope(x[1]),
    )
    reranked = [h for _, h in ranked[:limit]]
    return json.dumps({"results": reranked})


def handle_sadhana_gateway(arguments: dict) -> str:
    """Unified sadhana control.

    Actions: start, stop, pause, resume, status, list, checkpoint,
             set_goal, set_interval, set_model
    """
    action = arguments.pop("action", "status")
    tool = f"sadhana_{action}"
    return daemon_call(tool, arguments)


def handle_learn_gateway(arguments: dict) -> str:
    """Unified learning gateway.

    Types: correction, preference, insight, approach, outcome, milestone, analysis
    """
    learn_type = arguments.pop("type", "")
    if not learn_type:
        return "Error: 'type' parameter required (correction/preference/insight/approach/outcome/milestone/analysis)"
    handler_map = {
        "correction": handle_learn_correction,
        "preference": handle_learn_preference,
        "insight": handle_learn_insight,
        "approach": handle_learn_approach,
        "outcome": handle_learn_outcome,
        "milestone": handle_learn_milestone,
        "analysis": handle_learn_analysis,
    }
    handler = handler_map.get(learn_type)
    if not handler:
        return f"Unknown learn type: {learn_type}. Use: {', '.join(handler_map.keys())}"
    return handler(arguments)


def handle_research_gateway(arguments: dict) -> str:
    """Unified research gateway.

    Actions: topics, store, cycle
    """
    action = arguments.pop("action", "cycle")
    handler_map = {
        "topics": handle_research_topics,
        "store": handle_research_store,
        "cycle": handle_research_cycle,
    }
    handler = handler_map.get(action)
    if not handler:
        return f"Unknown research action: {action}. Use: {', '.join(handler_map.keys())}"
    return handler(arguments)


def handle_triplets_gateway(arguments: dict) -> str:
    """Unified triplet operations.

    Actions: connect (default), query, history, query_as_of, supersede, traverse, pagerank
    """
    action = arguments.pop("action", "connect")
    tool_map = {
        "connect": "connect_temporal",
        "query": "query_triplets_temporal",
        "history": "triplet_history",
        "query_as_of": "triplet_query_as_of",
        "supersede": "triplet_supersede",
        "traverse": "graph_traverse",
        "pagerank": "graph_pagerank",
    }
    tool = tool_map.get(action)
    if not tool:
        return f"Unknown action: {action}. Use: {', '.join(tool_map.keys())}"
    return daemon_call(tool, arguments)


def handle_memory_edit_gateway(arguments: dict) -> str:
    """Unified memory editing.

    Actions: set_type, set_priority
    """
    action = arguments.pop("action", "")
    if not action:
        return "Error: 'action' required (set_type/set_priority)"
    tool_map = {
        "set_type": "set_memory_type",
        "set_priority": "set_priority_tier",
    }
    tool = tool_map.get(action)
    if not tool:
        return f"Unknown action: {action}. Use: {', '.join(tool_map.keys())}"
    # The composite tool exposes the concise public key `id`, while both
    # daemon editing handlers use `memory_id`. Normalize at the gateway.
    if "id" in arguments and "memory_id" not in arguments:
        arguments["memory_id"] = arguments.pop("id")
    return daemon_call(tool, arguments)


# Map composite tool names to handlers
def handle_run_hint_enricher(arguments: dict) -> str:
    script = os.path.join(os.path.dirname(__file__), "enrichers", "hint_enricher.py")
    mind = os.environ.get("MIND", os.path.expanduser("~/.claude/mind"))
    cmd = [
        "python3",
        script,
        "--mind",
        mind,
        "--limit",
        str(arguments.get("limit", 100)),
        "--model",
        str(arguments.get("model", "chitta-hint-tuned")),
    ]
    if arguments.get("dry_run"):
        cmd.append("--dry-run")
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        out = result.stdout.strip()
        err = result.stderr.strip()
        if result.returncode != 0:
            return f"[hint_enricher] error (rc={result.returncode})\n{err}\n{out}"
        return out if out else (err if err else "[hint_enricher] done (no output)")
    except (OSError, subprocess.SubprocessError) as e:
        # Interpreter or script missing, or the 300s timeout elapsed.
        return f"[hint_enricher] failed: {e}"


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
    """
    Get current session ID using multiple detection strategies.

    Order of precedence:
    1. CLAUDE_SESSION_ID environment variable (most reliable if set)
    2. PPID lookup in session_registry (find session where pid = our parent)
    3. Cached current_session_id (from session_register or transcript_register)
    4. Single active session fallback (if exactly one exists)

    Note: Sidecar file removed - unreliable with multiple concurrent sessions.
    Note: use_cache=False forces fresh PPID lookup (use for messaging tools).
    """
    global current_session_id

    # 1. Check environment variable (most reliable)
    env_session = os.environ.get("CLAUDE_SESSION_ID")

    # 2. PPID lookup - query session_registry for our parent process
    # Always perform this lookup to enable validation against env_session
    ppid = os.getppid()
    ppid_session = None
    sessions = []
    try:
        result = daemon_call("session_list", {"active_only": True}, structured=True)
        data = json.loads(result)
        sessions = data.get("sessions", [])

        # Look for session matching our PPID
        for s in sessions:
            if s.get("pid") == ppid:
                ppid_session = s.get("session_id")
                break
    except (json.JSONDecodeError, KeyError, TypeError):
        pass

    # Validate: warn if env_session != ppid_session (potential stale env or wrong context)
    if env_session and ppid_session and env_session != ppid_session:
        logger.warning(
            f"Session mismatch: env={env_session} ppid={ppid_session} (pid={ppid}). "
            f"Using env_session. This may indicate stale CLAUDE_SESSION_ID."
        )

    # Return env_session if set (primary source of truth)
    if env_session:
        current_session_id = env_session
        return current_session_id

    # Return PPID-detected session if found
    if ppid_session:
        current_session_id = ppid_session
        return current_session_id

    # 3. Return cached session if set (and cache allowed)
    if use_cache and current_session_id:
        return current_session_id

    # 4. Single active session fallback
    try:
        if not sessions:
            result = daemon_call("session_list", {"active_only": True}, structured=True)
            data = json.loads(result)
            sessions = data.get("sessions", [])
        if len(sessions) == 1:
            current_session_id = sessions[0].get("session_id")
            return current_session_id
    except (json.JSONDecodeError, KeyError, TypeError):
        pass

    return None


def get_current_realm() -> str | None:
    """
    Get current realm using multiple detection strategies.

    Order of precedence:
    1. Cached current_realm (from previous detection)
    2. CHITTA_REALM environment variable
    3. .cc-soul-realm file in current directory
    4. Git repository name (becomes project:<repo-name>)
    """
    global current_realm

    # 1. Return cached realm if set
    if current_realm:
        return current_realm

    # 2. CHITTA_REALM env var
    env_realm = os.environ.get("CHITTA_REALM")
    if env_realm:
        current_realm = env_realm
        return current_realm

    # 3. .cc-soul-realm file
    try:
        realm_file = os.path.join(os.getcwd(), ".cc-soul-realm")
        if os.path.exists(realm_file):
            with open(realm_file) as f:
                realm = f.read().strip()
                if realm:
                    current_realm = realm
                    return current_realm
    except OSError as exc:
        # Deleted cwd, unreadable file, or undecodable bytes: fall through to
        # the git-repo strategy rather than failing realm detection outright.
        logger.debug(".cc-soul-realm unreadable: %s", exc)

    # 4. Git repo name
    try:
        result = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True, timeout=2
        )
        if result.returncode == 0 and result.stdout.strip():
            repo_name = os.path.basename(result.stdout.strip())
            current_realm = f"project:{repo_name}"
            return current_realm
    except (OSError, subprocess.SubprocessError) as exc:
        # git missing, not a repo, or slower than the 2s timeout. No realm is a
        # valid answer — callers only inject a realm when one is detected.
        logger.debug("git realm detection failed: %s", exc)

    return None


_SQZ_BIN = os.path.expanduser("~/.claude/bin/sqz")
_SQZ_THRESHOLD = 500


def _sqz_compress(text: str, tool_name: str = "mcp") -> str:
    if len(text) < _SQZ_THRESHOLD:
        return text
    sqz = _SQZ_BIN if os.path.isfile(_SQZ_BIN) else None
    if not sqz:
        return text
    try:
        proc = subprocess.run(
            [sqz, "compress", "--cmd", tool_name],
            input=text,
            capture_output=True,
            text=True,
            timeout=5,
        )
        return proc.stdout if proc.returncode == 0 and proc.stdout else text
    except (OSError, subprocess.SubprocessError) as exc:
        # Compression is a token optimization, never a correctness requirement:
        # if sqz is missing, unrunnable, or slower than its 5s timeout, the
        # uncompressed text is still the right answer.
        logger.debug("sqz compression skipped: %s", exc)
        return text


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
    async def run():
        init_options = InitializationOptions(
            server_name="chitta-mcp",
            server_version="0.1.0",
            capabilities=ServerCapabilities(tools=ToolsCapability()),
        )
        lag_task = asyncio.create_task(monitor_loop_lag())
        try:
            async with stdio_server() as (read_stream, write_stream):
                await server.run(read_stream, write_stream, init_options)
        finally:
            await _stop_loop_lag_monitor(lag_task)

    asyncio.run(run())


def _mcp_token() -> str:
    token_file = Path(os.environ.get("MIND", Path.home() / ".claude" / "mind")) / ".mcp_token"
    try:
        return token_file.read_text().strip()
    except FileNotFoundError:
        return ""


def create_http_session_manager():
    from mcp.server.streamable_http_manager import StreamableHTTPSessionManager

    class BoundedSessionManager(StreamableHTTPSessionManager):
        async def handle_request(self, scope, receive, send):
            # Reject expired IDs even between periodic sweeps, before the SDK
            # looks them up and refreshes last_seen.
            await self._server_instances.reap()
            await super().handle_request(scope, receive, send)

    global _http_sessions
    session_manager = BoundedSessionManager(
        app=server,
        json_response=True,
        stateless=False,
    )
    _http_sessions = HttpSessionTable(
        session_manager._session_owners,
        idle_s=_session_setting("CHITTA_MCP_SESSION_IDLE_S", 1800.0, float),
        max_sessions=_session_setting("CHITTA_MCP_MAX_SESSIONS", 64, int),
    )
    # Keep SDK transport cleanup and ownership checks; bound its creation path.
    session_manager._server_instances = _http_sessions
    session_manager._session_creation_lock = _http_sessions

    return session_manager


def _run_http(port: int):
    """Run as streamable HTTP MCP server for Codex, Cursor, Copilot CLI etc."""
    from starlette.applications import Starlette
    from starlette.responses import JSONResponse, PlainTextResponse
    from starlette.routing import Mount

    token = _mcp_token()

    session_manager = create_http_session_manager()

    from starlette.routing import Route

    async def health(request):
        return JSONResponse(
            {
                "status": "ok",
                "server": "chitta-mcp",
                "transport": "http",
                "mcp_sessions": http_session_stats(),
            }
        )

    async def auth_middleware(scope, receive, send):
        if scope["type"] == "http" and scope.get("path", "").startswith("/mcp"):
            if token:
                auth = dict(scope.get("headers", [])).get(b"authorization", b"").decode()
                if auth != f"Bearer {token}":
                    response = PlainTextResponse("Unauthorized", status_code=401)
                    await response(scope, receive, send)
                    return
        await app(scope, receive, send)

    app = Starlette(
        routes=[
            Route("/health", health),
            Mount("/mcp", app=session_manager.handle_request),
        ],
    )

    async def run():
        import uvicorn

        config = uvicorn.Config(
            auth_middleware,
            host="127.0.0.1",
            port=port,
            log_level="warning",
        )
        http_server = uvicorn.Server(config)

        logger.warning(f"chitta-mcp HTTP listening on http://127.0.0.1:{port}/mcp")

        lag_task = asyncio.create_task(monitor_loop_lag())
        session_task = asyncio.create_task(_http_sessions.monitor())
        try:
            async with session_manager.run():
                await http_server.serve()
        finally:
            await _stop_loop_lag_monitor(session_task)
            await _stop_loop_lag_monitor(lag_task)

    asyncio.run(run())


if __name__ == "__main__":
    main()
