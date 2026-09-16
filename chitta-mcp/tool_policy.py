"""Tool discovery policy and advanced gateway.

Implementations receive the server runtime explicitly as ``ctx`` so the public
server facade owns shared state and dependency overrides across all entry points.
"""

from __future__ import annotations

from mcp.types import Tool

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
        if tool not in ctx.HIDDEN_TOOLS:
            # Check if it's a valid daemon tool at all
            return f"Unknown tool: {tool}\nUse action='list' to see available hidden tools."

        tool_args = dict(tool_args)
        error = ctx.inject_message_session(tool, tool_args)
        if error:
            return error

        # Call the hidden tool via daemon
        result = ctx.daemon_call(tool, tool_args)
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
