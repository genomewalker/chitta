// register_misc_tools — chunk extracted from register_tools() so editing
// tool metadata only retemplates one chunk at a time.

#include "../../include/chitta/rpc/field_handler.hpp"

namespace chitta {

void FieldRpcHandler::register_misc_tools() {
    register_tool_table({
        {"anticipation_observe", "Record context->action pattern",
            {{"type","object"},{"properties",{
                {"context",{{"type","string"}}},{"action",{{"type","string"}}},{"realm",{{"type","string"}}}
            }},{"required",{"context","action"}}},
            &FieldRpcHandler::tool_anticipation_observe, handlers_["anticipation_observe"]},

        {"anticipation_predict", "Predict likely actions",
            {{"type","object"},{"properties",{
                {"context",{{"type","string"}}},{"limit",{{"type","integer"}}},
                {"min_confidence",{{"type","number"}}},{"realm",{{"type","string"}}}
            }},{"required",{"context"}}},
            &FieldRpcHandler::tool_anticipation_predict, handlers_["anticipation_predict"]},

        {"anticipation_success", "Mark prediction as successful",
            {{"type","object"},{"properties",{{"id",{{"type","integer"}}}}},{"required",{"id"}}},
            &FieldRpcHandler::tool_anticipation_success, handlers_["anticipation_success"]},

        {"anticipation_list", "List learned anticipation patterns",
            {{"type","object"},{"properties",{
                {"realm",{{"type","string"}}},{"limit",{{"type","integer"}}},{"sort_by",{{"type","string"}}}
            }}},
            &FieldRpcHandler::tool_anticipation_list, handlers_["anticipation_list"]},

        {"anticipation_filter", "Get predictions passing annoyance gate",
            {{"type","object"},{"properties",{
                {"session_id",{{"type","string"}}},{"max",{{"type","integer"}}}
            }}},
            &FieldRpcHandler::tool_anticipation_filter, handlers_["anticipation_filter"]},

        {"anticipation_gate_status", "Show annoyance gate state",
            {{"type","object"},{"properties",{{"session_id",{{"type","string"}}}}}},
            &FieldRpcHandler::tool_anticipation_gate_status, handlers_["anticipation_gate_status"]},

        {"anticipation_record_outcome", "Record prediction outcome",
            {{"type","object"},{"properties",{
                {"candidate_id",{{"type","integer"}}},{"correct",{{"type","boolean"}}}
            }},{"required",{"candidate_id","correct"}}},
            &FieldRpcHandler::tool_anticipation_record_outcome, handlers_["anticipation_record_outcome"]},

        {"habit_observe", "Record trigger->response pattern",
            {{"type","object"},{"properties",{
                {"trigger",{{"type","string"}}},{"response",{{"type","string"}}},{"realm",{{"type","string"}}}
            }},{"required",{"trigger","response"}}},
            &FieldRpcHandler::tool_habit_observe, handlers_["habit_observe"]},

        {"habit_match", "Find matching habits",
            {{"type","object"},{"properties",{
                {"context",{{"type","string"}}},{"min_strength",{{"type","number"}}},{"realm",{{"type","string"}}}
            }},{"required",{"context"}}},
            &FieldRpcHandler::tool_habit_match, handlers_["habit_match"]},

        {"habit_strengthen", "Strengthen a habit",
            {{"type","object"},{"properties",{
                {"id",{{"type","integer"}}},{"amount",{{"type","number"}}}
            }},{"required",{"id"}}},
            &FieldRpcHandler::tool_habit_strengthen, handlers_["habit_strengthen"]},

        {"habit_weaken", "Weaken a habit",
            {{"type","object"},{"properties",{
                {"id",{{"type","integer"}}},{"amount",{{"type","number"}}}
            }},{"required",{"id"}}},
            &FieldRpcHandler::tool_habit_weaken, handlers_["habit_weaken"]},

        {"habit_list", "List formed habits",
            {{"type","object"},{"properties",{
                {"realm",{{"type","string"}}},{"min_strength",{{"type","number"}}},
                {"limit",{{"type","integer"}}},{"sort_by",{{"type","string"}}}
            }}},
            &FieldRpcHandler::tool_habit_list, handlers_["habit_list"]},

        {"profile_get", "Get user profile",
            {{"type","object"},{"properties",{{"user_id",{{"type","string"}}}}}},
            &FieldRpcHandler::tool_profile_get, handlers_["profile_get"]},

        {"profile_update", "Update profile field",
            {{"type","object"},{"properties",{
                {"user_id",{{"type","string"}}},{"field",{{"type","string"}}},{"value",{{"type","string"}}}
            }},{"required",{"field","value"}}},
            &FieldRpcHandler::tool_profile_update, handlers_["profile_update"]},

        {"profile_observe", "Record user observation",
            {{"type","object"},{"properties",{
                {"observation_type",{{"type","string"}}},{"value",{{"type","string"}}},{"user_id",{{"type","string"}}}
            }},{"required",{"observation_type","value"}}},
            &FieldRpcHandler::tool_profile_observe, handlers_["profile_observe"]},

        {"goal_set", "Define a long-term goal",
            {{"type","object"},{"properties",{
                {"title",{{"type","string"}}},{"description",{{"type","string"}}},
                {"milestones",{{"type","string"}}},{"deadline",{{"type","integer"}}},{"realm",{{"type","string"}}}
            }},{"required",{"title"}}},
            &FieldRpcHandler::tool_goal_set, handlers_["goal_set"]},

        {"goal_get", "Get goal by ID",
            {{"type","object"},{"properties",{{"id",{{"type","integer"}}}}},{"required",{"id"}}},
            &FieldRpcHandler::tool_goal_get, handlers_["goal_get"]},

        {"goal_list", "List goals",
            {{"type","object"},{"properties",{
                {"status",{{"type","string"}}},{"realm",{{"type","string"}}},
                {"limit",{{"type","integer"}}},{"sort_by",{{"type","string"}}}
            }}},
            &FieldRpcHandler::tool_goal_list, handlers_["goal_list"]},

        {"goal_progress", "Update goal progress",
            {{"type","object"},{"properties",{
                {"id",{{"type","integer"}}},{"progress",{{"type","number"}}},{"milestone",{{"type","string"}}}
            }},{"required",{"id","progress"}}},
            &FieldRpcHandler::tool_goal_progress, handlers_["goal_progress"]},

        {"goal_complete", "Mark goal completed",
            {{"type","object"},{"properties",{
                {"id",{{"type","integer"}}},{"outcome",{{"type","string"}}}
            }},{"required",{"id","outcome"}}},
            &FieldRpcHandler::tool_goal_complete, handlers_["goal_complete"]},

        {"calibration_record", "Record prediction outcome",
            {{"type","object"},{"properties",{
                {"domain",{{"type","string"}}},{"success",{{"type","boolean"}}}
            }},{"required",{"domain","success"}}},
            &FieldRpcHandler::tool_calibration_record, handlers_["calibration_record"]},

        {"calibration_score", "Get accuracy score",
            {{"type","object"},{"properties",{{"domain",{{"type","string"}}}}}},
            &FieldRpcHandler::tool_calibration_score, handlers_["calibration_score"]},

        // Narrative
        {"narrative_status", "Get work mode and segment summary",
            {{"type","object"},{"properties",{{"session_id",{{"type","string"}}}}}},
            &FieldRpcHandler::tool_narrative_status, handlers_["narrative_status"]},

        {"narrative_log", "Append event to session log",
            {{"type","object"},{"properties",{
                {"session_id",{{"type","string"}}},{"kind",{{"type","string"}}},{"summary",{{"type","string"}}},
                {"tool_name",{{"type","string"}}},{"success",{{"type","boolean"}}},
                {"payload",{{"type","string"}}},{"files_mentioned",{{"type","string"}}}
            }},{"required",{"kind","summary"}}},
            &FieldRpcHandler::tool_narrative_log, handlers_["narrative_log"]},

        {"narrative_history", "Get work mode segment history",
            {{"type","object"},{"properties",{
                {"session_id",{{"type","string"}}},{"limit",{{"type","integer"}}}
            }}},
            &FieldRpcHandler::tool_narrative_history, handlers_["narrative_history"]},

        // Sadhana
        {"sadhana_start", "Create and start an autonomous agent",
            {{"type","object"},{"properties",{
                {"goal",{{"type","string"}}},{"brain_provider",{{"type","string"}}},
                {"brain_model",{{"type","string"}}},{"interval_seconds",{{"type","integer"}}},
                {"max_turns",{{"type","integer"}}},{"realm",{{"type","string"}}},
                {"goal_dsl",{{"type","object"}}}
            }},{"required",{"goal"}}},
            &FieldRpcHandler::tool_sadhana_start, handlers_["sadhana_start"]},

        {"sadhana_pause", "Pause a sadhana",
            {{"type","object"},{"properties",{{"id",{{"type","integer"}}}}},{"required",{"id"}}},
            &FieldRpcHandler::tool_sadhana_pause, handlers_["sadhana_pause"]},

        {"sadhana_resume", "Resume a paused sadhana",
            {{"type","object"},{"properties",{{"id",{{"type","integer"}}}}},{"required",{"id"}}},
            &FieldRpcHandler::tool_sadhana_resume, handlers_["sadhana_resume"]},

        {"sadhana_stop", "Stop a sadhana",
            {{"type","object"},{"properties",{
                {"id",{{"type","integer"}}},{"success",{{"type","boolean"}}},{"reason",{{"type","string"}}}
            }},{"required",{"id"}}},
            &FieldRpcHandler::tool_sadhana_stop, handlers_["sadhana_stop"]},

        {"sadhana_status", "Get sadhana status",
            {{"type","object"},{"properties",{
                {"id",{{"type","integer"}}},{"history_limit",{{"type","integer"}}}
            }},{"required",{"id"}}},
            &FieldRpcHandler::tool_sadhana_status, handlers_["sadhana_status"]},

        {"sadhana_list", "List sadhanas",
            {{"type","object"},{"properties",{
                {"state",{{"type","string"}}},{"realm",{{"type","string"}}},{"limit",{{"type","integer"}}}
            }}},
            &FieldRpcHandler::tool_sadhana_list, handlers_["sadhana_list"]},

        {"sadhana_set_model", "Change brain model",
            {{"type","object"},{"properties",{
                {"id",{{"type","integer"}}},{"model",{{"type","string"}}}
            }},{"required",{"id","model"}}},
            &FieldRpcHandler::tool_sadhana_set_model, handlers_["sadhana_set_model"]},

        {"sadhana_set_goal", "Change sadhana goal",
            {{"type","object"},{"properties",{
                {"id",{{"type","integer"}}},{"goal",{{"type","string"}}}
            }},{"required",{"id","goal"}}},
            &FieldRpcHandler::tool_sadhana_set_goal, handlers_["sadhana_set_goal"]},

        {"sadhana_set_interval", "Change tick interval",
            {{"type","object"},{"properties",{
                {"id",{{"type","integer"}}},{"interval",{{"type","integer"}}}
            }},{"required",{"id","interval"}}},
            &FieldRpcHandler::tool_sadhana_set_interval, handlers_["sadhana_set_interval"]},

        {"sadhana_set_max_turns", "Set max turns per cycle",
            {{"type","object"},{"properties",{
                {"id",{{"type","integer"}}},{"max_turns",{{"type","integer"}}}
            }},{"required",{"id","max_turns"}}},
            &FieldRpcHandler::tool_sadhana_set_max_turns, handlers_["sadhana_set_max_turns"]},

        {"sadhana_checkpoint", "Report mid-cycle checkpoint",
            {{"type","object"},{"properties",{
                {"id",{{"type","integer"}}},
                {"status",{{"type","string"},{"enum",{"progressed","achieved","blocked"}}}},
                {"summary",{{"type","string"}}}
            }},{"required",{"id","status","summary"}}},
            &FieldRpcHandler::tool_sadhana_checkpoint, handlers_["sadhana_checkpoint"]},

        // Dream
        {"dream_start", "Start an autonomous dream",
            {{"type","object"},{"properties",{
                {"topic",{{"type","string"}}},{"realm",{{"type","string"}}},{"publish_path",{{"type","string"}}},
                {"brain_provider",{{"type","string"},{"description","Brain provider: claude or local"}}},
                {"brain_model",{{"type","string"},{"description","Model name, e.g. gemma4:26b or sonnet"}}}
            }},{"required",{"topic"}}},
            &FieldRpcHandler::tool_dream_start, handlers_["dream_start"]},

        {"dream_wander", "Auto-select topic and dream",
            {{"type","object"},{"properties",{
                {"realm",{{"type","string"}}},{"publish_path",{{"type","string"}}}
            }}},
            &FieldRpcHandler::tool_dream_wander, handlers_["dream_wander"]},

        {"dream_cancel", "Cancel a dream",
            {{"type","object"},{"properties",{{"id",{{"type","integer"}}}}},{"required",{"id"}}},
            &FieldRpcHandler::tool_dream_cancel, handlers_["dream_cancel"]},

        {"dream_force_woke", "Force stuck dream to woke",
            {{"type","object"},{"properties",{{"id",{{"type","integer"}}}}},{"required",{"id"}}},
            &FieldRpcHandler::tool_dream_force_woke, handlers_["dream_force_woke"]},

        {"dream_list", "List recent dreams",
            {{"type","object"},{"properties",{
                {"limit",{{"type","integer"}}},{"realm",{{"type","string"}}}
            }}},
            &FieldRpcHandler::tool_dream_list, handlers_["dream_list"]},

        {"dream_status", "Get dream details",
            {{"type","object"},{"properties",{{"id",{{"type","integer"}}}}},{"required",{"id"}}},
            &FieldRpcHandler::tool_dream_status, handlers_["dream_status"]},

        {"think_wander", "Trigger internal memory synthesis",
            {{"type","object"},{"properties",{{"realm",{{"type","string"}}}}}},
            &FieldRpcHandler::tool_think_wander, handlers_["think_wander"]},

        {"impl_start", "Start self-improvement implementation sadhana",
            {{"type","object"},{"properties",{
                {"repo",{{"type","string"}}},{"interval_seconds",{{"type","integer"}}},
                {"max_turns",{{"type","integer"}}},{"realm",{{"type","string"}}}
            }}},
            &FieldRpcHandler::tool_impl_start, handlers_["impl_start"]},

        // Context Repository
        {"memory_history", "View memory version history",
            {{"type","object"},{"properties",{
                {"id",{{"type","integer"}}},{"limit",{{"type","integer"}}}
            }},{"required",{"id"}}},
            &FieldRpcHandler::tool_memory_history, handlers_["memory_history"]},

        {"memory_revert", "Revert memory to previous version",
            {{"type","object"},{"properties",{
                {"id",{{"type","integer"}}},{"version",{{"type","integer"}}},{"reason",{{"type","string"}}}
            }},{"required",{"id","version"}}},
            &FieldRpcHandler::tool_memory_revert, handlers_["memory_revert"]},

        {"pin_memory", "Pin memory to keep hot",
            {{"type","object"},{"properties",{
                {"id",{{"type","integer"}}},{"reason",{{"type","string"}}}
            }},{"required",{"id"}}},
            &FieldRpcHandler::tool_pin_memory, handlers_["pin_memory"]},

        {"unpin_memory", "Unpin a memory",
            {{"type","object"},{"properties",{{"id",{{"type","integer"}}}}},{"required",{"id"}}},
            &FieldRpcHandler::tool_unpin_memory, handlers_["unpin_memory"]},

        {"list_pinned", "List pinned memories",
            {{"type","object"},{"properties",{
                {"realm",{{"type","string"}}},{"limit",{{"type","integer"}}}
            }}},
            &FieldRpcHandler::tool_list_pinned, handlers_["list_pinned"]},

        {"memory_lock", "Acquire memory lock",
            {{"type","object"},{"properties",{
                {"id",{{"type","integer"}}},{"holder_id",{{"type","string"}}},
                {"holder_type",{{"type","string"}}},{"duration",{{"type","integer"}}}
            }},{"required",{"id","holder_id"}}},
            &FieldRpcHandler::tool_memory_lock, handlers_["memory_lock"]},

        {"memory_unlock", "Release memory lock",
            {{"type","object"},{"properties",{
                {"id",{{"type","integer"}}},{"holder_id",{{"type","string"}}}
            }},{"required",{"id","holder_id"}}},
            &FieldRpcHandler::tool_memory_unlock, handlers_["memory_unlock"]},

        {"memory_lock_status", "Check lock status",
            {{"type","object"},{"properties",{{"id",{{"type","integer"}}}}},{"required",{"id"}}},
            &FieldRpcHandler::tool_memory_lock_status, handlers_["memory_lock_status"]},

        {"propose_change", "Propose change to memory",
            {{"type","object"},{"properties",{
                {"id",{{"type","integer"}}},{"content",{{"type","string"}}},{"proposed_by",{{"type","string"}}}
            }},{"required",{"id","content","proposed_by"}}},
            &FieldRpcHandler::tool_propose_change, handlers_["propose_change"]},

        {"list_merge_queue", "List pending merge proposals",
            {{"type","object"},{"properties",{
                {"status",{{"type","string"}}},{"limit",{{"type","integer"}}}
            }}},
            &FieldRpcHandler::tool_list_merge_queue, handlers_["list_merge_queue"]},

        {"resolve_merge", "Resolve merge proposal",
            {{"type","object"},{"properties",{
                {"merge_id",{{"type","integer"}}},{"resolution",{{"type","string"}}},{"status",{{"type","string"}}}
            }},{"required",{"merge_id","status"}}},
            &FieldRpcHandler::tool_resolve_merge, handlers_["resolve_merge"]},

        // File Time Machine
        {"file_timeline", "Show files modified in time range",
            {{"type","object"},{"properties",{
                {"query",{{"type","string"}}},{"session_id",{{"type","string"}}},
                {"path",{{"type","string"}}},{"file_pattern",{{"type","string"}}},
                {"limit",{{"type","integer"}}},{"cross_session",{{"type","boolean"}}}
            }}},
            &FieldRpcHandler::tool_file_timeline, handlers_["file_timeline"]},

        {"file_at_time", "Get file content at time (stub)",
            {{"type","object"},{"properties",{
                {"file_path",{{"type","string"}}},{"time",{{"type","string"}}},
                {"session_id",{{"type","string"}}},{"show_diff",{{"type","boolean"}}}
            }},{"required",{"file_path"}}},
            &FieldRpcHandler::tool_file_at_time, handlers_["file_at_time"]},

        {"file_restore", "Restore file version (stub)",
            {{"type","object"},{"properties",{
                {"file_path",{{"type","string"}}},{"version_id",{{"type","integer"}}},{"preview",{{"type","boolean"}}}
            }},{"required",{"file_path"}}},
            &FieldRpcHandler::tool_file_restore, handlers_["file_restore"]},

        {"file_index_session", "Index file-history from session",
            {{"type","object"},{"properties",{
                {"session_id",{{"type","string"}}},{"force",{{"type","boolean"}}}
            }},{"required",{"session_id"}}},
            &FieldRpcHandler::tool_file_index_session, handlers_["file_index_session"]},

        {"file_index_all", "Index all sessions for cross-session timeline",
            {{"type","object"},{"properties",{
                {"force",{{"type","boolean"}}},{"limit",{{"type","integer"}}}
            }}},
            &FieldRpcHandler::tool_file_index_all, handlers_["file_index_all"]},

        // Misc
        {"learn_outcome", "Record memory usage outcome",
            {{"type","object"},{"properties",{
                {"memory_id",{{"type","string"}}},
                {"outcome",{{"type","string"},{"enum",{"positive","negative","neutral"}}}},
                {"context",{{"type","string"}}}
            }},{"required",{"memory_id","outcome"}}},
            &FieldRpcHandler::tool_learn_outcome, handlers_["learn_outcome"]},

        {"log_exposure", "Log memory exposure (SUS)",
            {{"type","object"},{"properties",{
                {"session_id",{{"type","string"}}},{"turn_id",{{"type","integer"}}},
                {"hook_type",{{"type","string"}}},{"memory_ids",{{"type","array"},{"items",{{"type","integer"}}}}},
                {"ranks",{{"type","array"},{"items",{{"type","integer"}}}}},
                {"resonance_scores",{{"type","array"},{"items",{{"type","number"}}}}}
            }},{"required",{"session_id","turn_id","hook_type","memory_ids"}}},
            &FieldRpcHandler::tool_log_exposure, handlers_["log_exposure"]},

        {"get_sus_metrics", "Get Soul Utility Score metrics",
            {{"type","object"},{"properties",{{"days",{{"type","integer"}}}}}},
            &FieldRpcHandler::tool_get_sus_metrics, handlers_["get_sus_metrics"]},

        {"episode_cluster_status", "Find similar episode clusters",
            {{"type","object"},{"properties",{
                {"similarity_threshold",{{"type","number"}}},{"min_occurrences",{{"type","integer"}}}
            }}},
            &FieldRpcHandler::tool_episode_cluster_status, handlers_["episode_cluster_status"]},

        {"insight_promote", "Promote memory to global",
            {{"type","object"},{"properties",{
                {"id",{{"type","integer"}}},{"reason",{{"type","string"}}}
            }},{"required",{"id"}}},
            &FieldRpcHandler::tool_insight_promote, handlers_["insight_promote"]},

        {"insight_global", "List global insights",
            {{"type","object"},{"properties",{
                {"limit",{{"type","integer"}}},{"tag",{{"type","string"}}}
            }}},
            &FieldRpcHandler::tool_insight_global, handlers_["insight_global"]},

        {"list_by_aspect", "List memories by semantic aspect",
            {{"type","object"},{"properties",{
                {"aspect",{{"type","string"}}},{"limit",{{"type","integer"}}},{"min_confidence",{{"type","number"}}}
            }},{"required",{"aspect"}}},
            &FieldRpcHandler::tool_list_by_aspect, handlers_["list_by_aspect"]},

        {"list_aspects", "List available semantic aspects",
            {{"type","object"},{"properties",json::object()}},
            &FieldRpcHandler::tool_list_aspects, handlers_["list_aspects"]},

        {"query_claims", "Query semantic claims",
            {{"type","object"},{"properties",{
                {"subject",{{"type","string"}}},{"predicate",{{"type","string"}}},
                {"scope",{{"type","string"}}},{"active_only",{{"type","boolean"}}},{"limit",{{"type","integer"}}}
            }}},
            &FieldRpcHandler::tool_query_claims, handlers_["query_claims"]},

        {"get_policies", "Get active policies",
            {{"type","object"},{"properties",{
                {"scope",{{"type","string"}}},{"type",{{"type","string"}}},{"limit",{{"type","integer"}}}
            }}},
            &FieldRpcHandler::tool_get_policies, handlers_["get_policies"]},

        {"get_entities", "Get tracked entities",
            {{"type","object"},{"properties",{
                {"type",{{"type","string"}}},{"limit",{{"type","integer"}}}
            }}},
            &FieldRpcHandler::tool_get_entities, handlers_["get_entities"]},

        {"get_relationship_events", "Get relationship events",
            {{"type","object"},{"properties",{
                {"event_type",{{"type","string"}}},{"session_id",{{"type","string"}}},{"limit",{{"type","integer"}}}
            }}},
            &FieldRpcHandler::tool_get_relationship_events, handlers_["get_relationship_events"]},

        // ── Context compaction ─────────────────────────────────────────────
        {"ingest_source", "Ingest external content (URL, file, directory) into memory via SSL distillation",
            {{"type","object"},{"properties",{
                {"source",{{"type","string"},{"description","URL, file path, or directory path"}}},
                {"realm",{{"type","string"},{"description","Target realm (default: brahman)"}}},
                {"type",{{"type","string"},{"description","Source type: auto|url|file|directory (default: auto)"}}},
                {"model",{{"type","string"},{"description","LLM model (default: gemma4:26b)"}}},
                {"endpoint",{{"type","string"},{"description","OpenAI-compatible endpoint (auto-discovered if empty)"}}},
                {"max_chunks",{{"type","integer"},{"description","Max chunks to process (default: 30)"}}}
            }},{"required",{"source"}}},
            &FieldRpcHandler::tool_ingest_source, handlers_["ingest_source"]},

        // ── Tier 2: Wiki export ────────────────────────────────────────────
        {"wiki_export", "Export memories as Obsidian-compatible .md wiki with backlinks",
            {{"type","object"},{"properties",{
                {"output_dir",{{"type","string"},{"description","Output directory (default: ~/.claude/wiki/)"}}},
                {"realm",{{"type","string"},{"description","Filter to specific realm (default: all)"}}},
                {"max_memories",{{"type","integer"},{"description","Max memories per realm (default: 5000)"}}}
            }}},
            &FieldRpcHandler::tool_wiki_export, handlers_["wiki_export"]},

        // ── Tier 3: Health-check sadhana ───────────────────────────────────
        {"health_check_start", "Start autonomous health-check sadhana that monitors memory quality, dedup ratio, and embedding coverage",
            {{"type","object"},{"properties",{
                {"interval_seconds",{{"type","integer"},{"description","Check interval in seconds (default: 3600)"}}},
                {"realm",{{"type","string"},{"description","Realm to monitor (default: brahman)"}}},
                {"max_turns",{{"type","integer"},{"description","Max check cycles (default: 0 = unlimited)"}}}
            }}},
            &FieldRpcHandler::tool_health_check_start, handlers_["health_check_start"]},

        // ── Tier 4: Export training pairs ──────────────────────────────────
        {"export_training_pairs", "Export query-passage pairs as JSONL for BGE embedding fine-tuning",
            {{"type","object"},{"properties",{
                {"output_path",{{"type","string"},{"description","Output JSONL path (default: ~/.claude/training/pairs.jsonl)"}}},
                {"realm",{{"type","string"},{"description","Filter to specific realm (default: all)"}}},
                {"max_pairs",{{"type","integer"},{"description","Max pairs to export (default: 10000)"}}},
                {"min_confidence",{{"type","number"},{"description","Min confidence threshold (default: 0.5)"}}},
                {"include_negatives",{{"type","boolean"},{"description","Generate hard negatives (default: true)"}}}
            }}},
            &FieldRpcHandler::tool_export_training_pairs, handlers_["export_training_pairs"]},

        // ── Soul REPL Session Store ──────────────────────────────────────────
    });
}

} // namespace chitta
