"""Tool discovery policy and advanced gateway.

Implementations receive the server runtime explicitly as ``ctx`` so the public
server facade owns shared state and dependency overrides across all entry points.
"""

from __future__ import annotations

from mcp.types import Tool
from tools_static import COMPOSITE_TOOLS, DAEMON_HANDLER_NAMES, TOOLS

# Explicit discovery budget. Evidence and every tier change are recorded in
# CHANGELOG.md (2026-09-16 Phase 4). New tools default to advanced; adding a
# schema must never silently spend the core context budget.
CORE_TOOLS = {
    "advanced",
    "checkpoint",
    "code_context",
    "codebase_overview",
    "connect_temporal",
    "correction_check",
    "dream_list",
    "dream_start",
    "dream_status",
    "dream_wander",
    "explore_expand",
    "explore_neighbors",
    "explore_peek",
    "explore_recall",
    "find_symbol",
    "forget",
    "get",
    "habit_list",
    "habit_match",
    "habit_strengthen",
    "health_check",
    "learn",
    "learn_codebase",
    "ledger_get",
    "ledger_list",
    "ledger_load",
    "ledger_save",
    "long_task_active",
    "long_task_complete",
    "long_task_event",
    "long_task_snapshot",
    "long_task_start",
    "long_task_update",
    "lookup",
    "memory_edit",
    "memory_outcome",
    "msg_send",
    "observe",
    "query_graph",
    "read_symbol",
    "code_query",
    "realm_detect",
    "recall",
    "recall_analogy",
    "recall_keyword",
    "recall_lanes",
    "remember",
    "research",
    "run_hint_enricher",
    "sadhana",
    "set_memory_type",
    "smart_context",
    "smart_recall",
    "soul_context",
    "triplets",
}

# Maintenance and hook plumbing remain callable without being advertised.
INTERNAL_TOOLS = {
    # Maintenance
    "cleanup",
    "hygiene_run",
    "hygiene_stats",
    "consolidation_scan",
    "consolidation_merge",
    "consolidation_auto",
    "batch_forget",
    "reembed_memories",
    "dedupe_symbols",
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

# Include native handlers without MCP schemas: the gateway can call them, and
# their absence from tools/list must not make them undiscoverable via advanced.
ALL_TOOLS = {tool.name for tool in TOOLS + COMPOSITE_TOOLS} | DAEMON_HANDLER_NAMES
ADVANCED_TOOLS = ALL_TOOLS - CORE_TOOLS - INTERNAL_TOOLS
HIDDEN_TOOLS = INTERNAL_TOOLS | ADVANCED_TOOLS


def handle_advanced(ctx, arguments: dict) -> str:
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
        if tool not in ALL_TOOLS or tool == "advanced":
            # Promotions must preserve existing advanced(tool=...) calls too.
            return f"Unknown tool: {tool}\nUse action='list' to see available hidden tools."

        tool_args = dict(tool_args)
        error = ctx.inject_message_session(tool, tool_args)
        if error:
            return error

        # Preserve MCP-only implementations when a composite moves to advanced.
        handler = ctx.COMPOSITE_HANDLERS.get(tool)
        result = handler(tool_args) if handler else ctx.daemon_call(tool, tool_args)
        return f"[{tool}]\n{result}"

    # List hidden tools
    if action == "list" or not action:
        output = "Hidden Tools (callable via advanced gateway)\n"
        output += "=" * 50 + "\n\n"

        if not category or category == "advanced":
            output += "ADVANCED TOOLS (user-facing but hidden to save context):\n"
            output += "-" * 50 + "\n"
            for name in sorted(ctx.ADVANCED_TOOLS):
                output += f"  • {name}\n"
            output += f"\n  Total: {len(ctx.ADVANCED_TOOLS)} tools\n\n"

        if not category or category == "internal":
            output += "INTERNAL TOOLS (maintenance/hooks only):\n"
            output += "-" * 50 + "\n"
            for name in sorted(ctx.INTERNAL_TOOLS):
                output += f"  • {name}\n"
            output += f"\n  Total: {len(ctx.INTERNAL_TOOLS)} tools\n\n"

        output += "Usage:\n"
        output += '  {"tool": "<name>", "arguments": {...}}\n'
        output += "\nExample:\n"
        output += '  {"tool": "pin_memory", "arguments": {"id": 123, "reason": "hot context"}}\n'
        return output

    return "Unknown action. Use action='list' or specify tool='<name>'."


def strip_null_values(ctx, obj):
    """Recursively remove keys with null values from dicts."""
    if isinstance(obj, dict):
        return {k: ctx.strip_null_values(v) for k, v in obj.items() if v is not None}
    elif isinstance(obj, list):
        return [ctx.strip_null_values(item) for item in obj]
    return obj


def clean_tool_schema(ctx, tool: Tool) -> Tool:
    """Return a Tool with null values stripped from inputSchema."""
    clean_schema = ctx.strip_null_values(tool.inputSchema)
    return ctx.Tool(name=tool.name, description=tool.description, inputSchema=clean_schema)
