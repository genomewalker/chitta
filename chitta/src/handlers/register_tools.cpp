// register_tools / classify_tools — moved out of field_handler.hpp class
// body so editing tool metadata does not retemplate every chittad TU.

#include "../../include/chitta/rpc/field_handler.hpp"

namespace chitta {

void FieldRpcHandler::register_tools() {
    // ceiling: CHITTA_SANDBOX (CONTRACTS.md §8) guards only `remember`, so a
    // sandboxed caller can still mutate the store through the other write-class
    // RPCs — checkpoint, realm_add, learn_outcome, forget, connect, strengthen,
    // weaken, observe, memory_edit; upgrade: wire each through the same
    // sandbox.hpp gate tool_remember uses. (2026-09-02)
    // ── Memory tools ────────────────────────────────────────────────────
    register_memory_core_tools();
    register_code_intel_tools();
    register_distill_drift_tools();
    register_session_transcript_tools();
    register_system_tools();
    register_misc_tools();
    register_protocol_tools();

    // Keep tools/list honest: the same central table drives runtime clamping.
    // Only read handlers are annotated/clamped; writes retain their published schemas.
    for (auto& tool : tools_) {
        const std::string name = tool.value("name", "");
        if (!is_read_only_tool(name) || !tool.contains("inputSchema")) continue;
        auto& schema = tool["inputSchema"];
        if (!schema.is_object() || !schema.contains("properties")
            || !schema["properties"].is_object()) continue;
        auto& properties = schema["properties"];
        for (const auto& limit : rpc::kReadParameterLimits) {
            auto it = properties.find(std::string(limit.name));
            if (it == properties.end() || !it->is_object()) continue;
            (*it)["minimum"] = limit.minimum;
            (*it)["maximum"] = limit.maximum;
        }
    }
}

void FieldRpcHandler::classify_tools() {
    static const std::vector<std::string> internal_tools = {
        "cleanup", "hygiene_run",
        "consolidation_scan", "consolidation_merge", "consolidation_auto",
        "batch_forget", "reembed_memories",
        "dedupe_symbols",
        "metacognition_corrections", "metacognition_outcomes",
        "distill_status", "enrichment_status", "epiplexity_check",
        "clear_codebase", "clear_triplets", "describe_symbol", "extract_symbols",
        "file_dependents", "file_imports", "resolve_callsites",
        "restore_code_intel_confidence", "ssl_convert", "subconscious_stats",
        "suggestion_count", "suggestion_pending", "suggestion_resolve", "suggestion_track",
        "transcript_get", "transcript_list", "transcript_parse", "transcript_register",
        "transcript_remove", "transcript_search", "transcript_update",
        "type_hierarchy", "version_check",
        "export_soul", "import_soul",
        "cycle", "anticipation_gate_status", "anticipation_record_outcome",
        "session_register", "session_heartbeat", "session_deregister", "msg_ack",
        "file_index_session", "file_index_all",
        "chitta_health",
        "ingest_source", "wiki_export", "health_check_start", "export_training_pairs",
        "repl_session_get", "repl_session_set", "repl_session_delete", "repl_session_list",
        "repl_execute"
    };

    static const std::vector<std::string> advanced_tools = {
        "strengthen", "weaken", "tag", "update", "get", "query_graph",
        "realm_add", "realm_detect", "realm_get", "realm_list", "realm_remove", "realm_set", "realm_visibility",
        "goal_set", "goal_get", "goal_list", "goal_complete", "goal_progress",
        "habit_observe", "habit_match", "habit_list", "habit_strengthen", "habit_weaken",
        "anticipation_predict", "anticipation_observe", "anticipation_list", "anticipation_success",
        "calibration_record", "calibration_score",
        "profile_get", "profile_observe", "profile_update",
        "curiosity_gaps", "curiosity_note_gap", "curiosity_resolve",
        "narrative_history",
        "memory_history", "memory_revert", "pin_memory", "unpin_memory", "list_pinned",
        "memory_lock", "memory_unlock", "memory_lock_status",
        "propose_change", "list_merge_queue", "resolve_merge",
        "file_timeline", "file_at_time", "file_restore",
        "surprise_learning_stats",
        "upsert_wisdom_candidate", "update_wisdom_lifecycle",
        "query_wisdom_candidates", "wisdom_promotion_stats",
        "attach_debt_evidence",
        "update_scorer_model", "learned_scorer_stats", "effective_scorer_weights",
        // Layer 7: Intervention Ledger
        "start_intervention", "add_observation", "close_intervention",
        "record_attribution", "query_interventions", "get_intervention",
        "intervention_stats", "list_open_interventions",
        // Layer 8: Agent Protocol Memory
        "register_task", "update_task", "add_delegation", "link_evidence",
        "add_probe", "resolve_probe", "set_criterion",
        "get_task", "query_tasks", "agent_protocol_stats",
        // Layer 9: Wisdom Homeostasis
        "enroll_wisdom_lineage", "transition_wisdom_lineage", "close_rederive",
        "query_wisdom_lineages", "get_wisdom_lineage", "wisdom_lineage_stats",
        "tick_lineage_staleness", "lineage_expiry_check"
    };

    for (const auto& name : internal_tools) tool_visibility_[name] = "internal";
    for (const auto& name : advanced_tools) tool_visibility_[name] = "advanced";
}

} // namespace chitta
