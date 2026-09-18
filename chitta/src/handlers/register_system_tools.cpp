#include <chitta/llm_http.hpp>
// register_system_tools — chunk extracted from register_tools() so editing
// tool metadata only retemplates one chunk at a time.

#include "../../include/chitta/rpc/field_handler.hpp"

#include <iomanip>
#include <sstream>

namespace chitta {

void FieldRpcHandler::register_system_tools() {
    tools_.push_back({{"name", "endpoint_list"},
        {"description", "List LLM endpoints, served models, probe latency and role winners; never starts GPU jobs"},
        {"inputSchema", {{"type", "object"}, {"properties", {{"probe", {{"type", "boolean"}, {"default", false}}}}}}}});
    handlers_["endpoint_list"] = [this](const json& params) {
        auto report = endpoint_list(params.value("probe", false), distill_model_);
        return ToolResult::ok(endpoint_table(report), report);
    };

    register_tool_table({
        {"memory_status", "Get effective status of a memory: active, superseded, or contradicted — checks incoming supersedes triplets",
            {{"type","object"},{"properties",{
                {"id",{{"type","string"}}},{"memory_id",{{"type","integer"}}}
            }},{"required",json::array()}},
            &FieldRpcHandler::tool_memory_status, handlers_["memory_status"]},

        {"memory_provenance", "Show why a memory exists: source, evidence, superseded_by, supersedes relations",
            {{"type","object"},{"properties",{
                {"id",{{"type","string"},{"description","Memory ID to inspect"}}},
                {"memory_id",{{"type","integer"},{"description","Memory ID (numeric)"}}}
            }},{"required",json::array()}},
            &FieldRpcHandler::tool_memory_provenance, handlers_["memory_provenance"]},

        {"compact_wal", "Compact WAL: save full snapshot then delete covered segments",
            {{"type","object"},{"properties",json::object()}},
            &FieldRpcHandler::tool_compact_wal, handlers_["compact_wal"]},

        {"trim_realm_names", "Fix realm names with trailing whitespace/newlines",
            {{"type","object"},{"properties",json::object()}},
            &FieldRpcHandler::tool_trim_realm_names, handlers_["trim_realm_names"]},

        {"remap_realms", "Bulk-remap realms per {old:new} mapping (mapping_file=path or mapping=object; dry_run defaults true)",
            {{"type","object"},{"properties",{
                {"mapping_file",{{"type","string"}}},
                {"mapping",{{"type","object"}}},
                {"dry_run",{{"type","boolean"}}}
            }}},
            &FieldRpcHandler::tool_remap_realms, handlers_["remap_realms"]},

        {"save_spectral_snapshot", "Save spectral stats snapshot for drift tracking",
            {{"type","object"},{"properties",json::object()}},
            &FieldRpcHandler::tool_save_spectral_snapshot, handlers_["save_spectral_snapshot"]},

        {"spectral_drift", "Compare current embedding geometry with last snapshot",
            {{"type","object"},{"properties",json::object()}},
            &FieldRpcHandler::tool_spectral_drift, handlers_["spectral_drift"]},

        {"queue_status", "Live work-queue depth (pending in-flight + unclaimed), processed/distilled/failed counters, pending embeddings, dead-letters",
            {{"type","object"},{"properties",json::object()}},
            &FieldRpcHandler::tool_queue_status, handlers_["queue_status"]},

        {"ledger_health", "Get ledger event counts by kind and queue health metrics",
            {{"type","object"},{"properties",json::object()}},
            &FieldRpcHandler::tool_ledger_health, handlers_["ledger_health"]},

        {"health_check", "Check daemon health",
            {{"type","object"},{"properties",json::object()}},
            &FieldRpcHandler::tool_health_check, handlers_["health_check"]},

        {"version_check", "Get version info",
            {{"type","object"},{"properties",json::object()}},
            &FieldRpcHandler::registered_version_check, handlers_["version_check"]},

        {"cycle", "Run maintenance cycle",
            {{"type","object"},{"properties",{
                {"force",{{"type","boolean"}}}
            }}},
            &FieldRpcHandler::tool_cycle, handlers_["cycle"]},

        {"cleanup", "Remove garbage nodes",
            {{"type","object"},{"properties",{
                {"dry_run",{{"type","boolean"}}}
            }}},
            &FieldRpcHandler::tool_cleanup, handlers_["cleanup"]},

        {"soul_context", "Get current soul state and statistics",
            {{"type","object"},{"properties",json::object()}},
            &FieldRpcHandler::tool_soul_context, handlers_["soul_context"]},

        {"resonance_stats", "Show ResonanceLearner Bayesian bandit stats",
            {{"type","object"},{"properties",json::object()}},
            &FieldRpcHandler::tool_resonance_stats, handlers_["resonance_stats"]},

        {"subconscious_stats", "Get subconscious background processor stats",
            {{"type","object"},{"properties",json::object()}},
            &FieldRpcHandler::tool_subconscious_stats, handlers_["subconscious_stats"]},

        // Deliberately NOT folded into health_check: this is an O(N) scan of the store, and
        // health_check is polled by the watchdog (an O(N) scan there is what got the daemon
        // killed for missing its own ping). Call it on demand.
        {"embed_coverage", "Semantic-index coverage: how many memories SHOULD have a vector vs how many DO. pending_count cannot answer this — the embed queue drains on failure as well as success, so a lost embedding leaves the queue empty.",
            {{"type","object"},{"properties",json::object()}},
            &FieldRpcHandler::registered_embed_coverage, handlers_["embed_coverage"]},

        // Which vector space is this memory's stored vector actually in? Two threads drain the
        // same pending queue (subconscious.cpp embed_loop and simple_cli.cpp backfill) and they
        // have historically prefixed the document text differently, so the answer is per-memory
        // and cannot be inferred from anything the store reports. Re-embed the content both ways
        // with the daemon's own yantra and see which one the stored vector matches.
        {"embed_probe", "For a memory id: cosine of its STORED vector against a fresh single-prefix embed vs a double-prefix embed of the same content. ~1.0 identifies which vector space the memory actually lives in.",
            {{"type","object"},
                {"properties",{{"id",{{"type","string"}}}}},
                {"required",json::array({"id"})}},
            &FieldRpcHandler::registered_embed_probe, handlers_["embed_probe"]},

        {"stageb_set_surface", "Stage B: set a memory's natural-language retrieval surface and re-embed from it (keeps telegraphic content as display). Pass surface='' to clear and re-embed from content. Returns the id and embedded flag.",
            {{"type","object"},
                {"properties",{{"id",{{"type","string"}}},{"surface",{{"type","string"}}}}},
                {"required",json::array({"id","surface"})}},
            &FieldRpcHandler::registered_stageb_set_surface, handlers_["stageb_set_surface"]},

        {"pending_embed_ids", "Return IDs of memories awaiting embedding (stuck embed queue)",
            {{"type","object"},{"properties",json::object()}},
            &FieldRpcHandler::registered_pending_embed_ids, handlers_["pending_embed_ids"]},

        {"reembed_memories", "Re-embed memories with proper embeddings",
            {{"type","object"},{"properties",{
                {"all",{{"type","boolean"}}},{"kind",{{"type","string"}}},
                {"min_confidence",{{"type","number"}}},{"limit",{{"type","integer"}}},
                {"dry_run",{{"type","boolean"}}}
            }}},
            &FieldRpcHandler::tool_reembed_memories, handlers_["reembed_memories"]},

        {"rebuild_fts_index", "Rebuild FTS index for BM25 search",
            {{"type","object"},{"properties",json::object()}},
            &FieldRpcHandler::tool_rebuild_fts_index, handlers_["rebuild_fts_index"]},

        {"prune_episodes", "Prune old/excess episode memories. Deletes episodes older than max_age_days (strength<0.3) and caps total count at max_count.",
            {{"type","object"},{"properties",{
                {"max_age_days",{{"type","integer"},{"description","Delete episodes older than this (default 90)"}}},
                {"max_count",{{"type","integer"},{"description","Cap total episode count at this (default 10000)"}}}
            }}},
            &FieldRpcHandler::registered_prune_episodes, handlers_["prune_episodes"]},

        {"write_gate_stats", "Show write-gate admission stats: staged memory count and oldest staged memory age",
            {{"type","object"},{"properties",json::object()}},
            &FieldRpcHandler::tool_write_gate_stats, handlers_["write_gate_stats"]},

        {"symbol_event_log", "Query the symbol-keyed event log. Filter by symbol_name and/or file_path.",
            {{"type","object"},{"properties",{
                {"symbol_name",{{"type","string"},{"description","Filter by symbol name"}}},
                {"file_path",{{"type","string"},{"description","Filter by file path"}}},
                {"limit",{{"type","integer"},{"description","Max events to return (default 50)"}}}
            }}},
            &FieldRpcHandler::tool_symbol_event_log, handlers_["symbol_event_log"]},

        {"mark_memory_invalidated", "Mark a memory as invalidated by a commit hash or symbol-change ID.",
            {{"type","object"},{"properties",{
                {"memory_id",{{"type","integer"},{"description","Memory ID to mark"}}},
                {"reason",{{"type","string"},{"description","Commit hash or change ID causing invalidation"}}}
            }},{"required",{"memory_id"}}},
            &FieldRpcHandler::tool_mark_memory_invalidated, handlers_["mark_memory_invalidated"]},

        {"what_do_i_know_about", "Introspection: return claims + provenance + staleness + contradictions for a topic",
            {{"type","object"},{"properties",{
                {"topic",{{"type","string"},{"description","Topic or question to introspect"}}},
                {"k",{{"type","integer"},{"description","Max claims to return (default 10)"}}},
                {"realm",{{"type","string"},{"description","Realm filter"}}},
                {"confidence_min",{{"type","number"},{"description","Minimum confidence threshold (default 0.0)"}}},
                {"include_stale",{{"type","boolean"},{"description","Include stale memories (default true)"}}},
                {"include_contradictions",{{"type","boolean"},{"description","Include contradiction info (default true)"}}}
            }},{"required",{"topic"}}},
            &FieldRpcHandler::tool_what_do_i_know_about, handlers_["what_do_i_know_about"]},

        {"cross_harness_conflicts", "Find memories where claude-code and codex harnesses disagree on the same topic",
            {{"type","object"},{"properties",{
                {"realm",{{"type","string"},{"description","Realm filter (default: all)"}}},
                {"limit",{{"type","integer"},{"description","Max conflict pairs to return (default 20)"}}},
                {"min_disagreement_score",{{"type","number"},{"description","Minimum 1-cosine_similarity threshold (default 0.3)"}}}
            }}},
            &FieldRpcHandler::tool_cross_harness_conflicts, handlers_["cross_harness_conflicts"]},

        {"hygiene_stats", "Get memory hygiene statistics",
            {{"type","object"},{"properties",json::object()}},
            &FieldRpcHandler::tool_hygiene_stats, handlers_["hygiene_stats"]},

        {"hygiene_run", "Run memory hygiene: decay, prune, consolidate",
            {{"type","object"},{"properties",{
                {"prune_threshold",{{"type","number"}}},{"min_age_days",{{"type","number"}}},
                {"consolidation_threshold",{{"type","number"}}},{"max_consolidations",{{"type","integer"}}}
            }}},
            &FieldRpcHandler::tool_hygiene_run, handlers_["hygiene_run"]},

        {"import_soul", "Import .soul file (SSL format). Supports --realm and --source_session for targeted ingestion.",
            {{"type","object"},{"properties",{
                {"file",{{"type","string"}}},{"content",{{"type","string"}}},
                {"realm",{{"type","string"},{"description","Target realm (default: brahman)"}}},
                {"source_session",{{"type","string"},{"description","Tag all imported memories with this session ID"}}},
                {"confidence",{{"type","number"},{"description","Confidence for imported memories (default: 0.8)"}}}
            }}},
            &FieldRpcHandler::tool_import_soul, handlers_["import_soul"]},

        {"export_soul", "Export memories to SSL format",
            {{"type","object"},{"properties",{
                {"file",{{"type","string"}}},{"tag",{{"type","string"}}},
                {"limit",{{"type","integer"}}}
            }}},
            &FieldRpcHandler::tool_export_soul, handlers_["export_soul"]},

        {"chitta_health", "Report feedback loop health diagnostics",
            {{"type","object"},{"properties",json::object()}},
            &FieldRpcHandler::tool_chitta_health, handlers_["chitta_health"]},

        {"theme_list", "List all themes with statistics",
            {{"type","object"},{"properties",{
                {"realm",{{"type","string"}}},{"limit",{{"type","integer"}}}
            }}},
            &FieldRpcHandler::tool_theme_list, handlers_["theme_list"]},

        {"theme_get", "Get theme details",
            {{"type","object"},{"properties",{
                {"id",{{"type","integer"}}}
            }},{"required",{"id"}}},
            &FieldRpcHandler::tool_theme_get, handlers_["theme_get"]},

        {"theme_recall", "Two-stage theme-based retrieval",
            {{"type","object"},{"properties",{
                {"query",{{"type","string"}}},{"limit",{{"type","integer"}}},{"realm",{{"type","string"}}}
            }},{"required",{"query"}}},
            &FieldRpcHandler::tool_theme_recall, handlers_["theme_recall"]},

        {"theme_stats", "Get theme organization statistics",
            {{"type","object"},{"properties",{{"realm",{{"type","string"}}}}}},
            &FieldRpcHandler::tool_theme_stats, handlers_["theme_stats"]},

        // theme_maintain and theme_assign_orphans removed — no theme engine backend

        // ── Realm tools ─────────────────────────────────────────────────────
        {"realm_list", "List all known realms",
            {{"type","object"},{"properties",json::object()}},
            &FieldRpcHandler::registered_realm_list, handlers_["realm_list"]},

        {"realm_get", "Get all realms a memory belongs to",
            {{"type","object"},{"properties",{{"id",{{"type","string"}}}}},{"required",{"id"}}},
            &FieldRpcHandler::tool_realm_get, handlers_["realm_get"]},

        {"realm_set", "Set primary realm for a memory",
            {{"type","object"},{"properties",{
                {"id",{{"type","string"}}},{"realm",{{"type","string"}}}
            }},{"required",{"id","realm"}}},
            &FieldRpcHandler::tool_realm_set, handlers_["realm_set"]},

        {"realm_add", "Add memory to a shared realm (stub)",
            {{"type","object"},{"properties",{
                {"id",{{"type","string"}}},{"realm",{{"type","string"}}}
            }},{"required",{"id","realm"}}},
            &FieldRpcHandler::tool_realm_add, handlers_["realm_add"]},

        {"realm_remove", "Remove memory from a shared realm (stub)",
            {{"type","object"},{"properties",{
                {"id",{{"type","string"}}},{"realm",{{"type","string"}}}
            }},{"required",{"id","realm"}}},
            &FieldRpcHandler::tool_realm_remove, handlers_["realm_remove"]},

        {"realm_visibility", "Set visibility level (stub)",
            {{"type","object"},{"properties",{
                {"id",{{"type","string"}}},{"visibility",{{"type","integer"}}}
            }},{"required",{"id","visibility"}}},
            &FieldRpcHandler::tool_realm_visibility, handlers_["realm_visibility"]},

        {"realm_detect", "Detect current realm from environment",
            {{"type","object"},{"properties",json::object()}},
            &FieldRpcHandler::registered_realm_detect, handlers_["realm_detect"]},

        // ── Ledger + Long Task tools ────────────────────────────────────────
        {"ledger_save", "Save session checkpoint",
            {{"type","object"},{"properties",{
                {"session_id",{{"type","string"}}},{"project",{{"type","string"}}},
                {"transcript_path",{{"type","string"}}},{"mood",{{"type","string"}}},
                {"coherence",{{"type","number"}}},{"confidence",{{"type","number"}}},
                {"todos",{{"type","array"}}},{"active_files",{{"type","array"},{"items",{{"type","string"}}}}},
                {"decisions",{{"type","array"},{"items",{{"type","string"}}}}},
                {"next_steps",{{"type","array"},{"items",{{"type","string"}}}}},
                {"blockers",{{"type","array"},{"items",{{"type","string"}}}}},
                {"discoveries",{{"type","array"},{"items",{{"type","string"}}}}},
                {"snapshot",{{"type","string"}}}
            }},{"required",json::array()}},
            &FieldRpcHandler::tool_ledger_save, handlers_["ledger_save"]},

        {"ledger_load", "Load most recent checkpoint",
            {{"type","object"},{"properties",{
                {"session_id",{{"type","string"}}},{"project",{{"type","string"}}},
                {"include_snapshot",{{"type","boolean"},{"description","Include snapshot preview (truncated) in response"}}}
            }}},
            &FieldRpcHandler::tool_ledger_load, handlers_["ledger_load"]},

        {"ledger_list", "List recent checkpoints",
            {{"type","object"},{"properties",{
                {"project",{{"type","string"}}},{"limit",{{"type","integer"}}}
            }}},
            &FieldRpcHandler::tool_ledger_list, handlers_["ledger_list"]},

        {"ledger_get", "Get checkpoint by key or session/project",
            {
                {"type","object"},
                {"properties", {
                    {"key", {{"type","string"}, {"description","Canonical checkpoint key (session:project)"}}},
                    {"session_id", {{"type","string"}}},
                    {"project", {{"type","string"}}},
                    {"include_snapshot", {{"type","boolean"}, {"description","Include snapshot preview (truncated) in response"}}}
                }}
            },
            &FieldRpcHandler::tool_ledger_get, handlers_["ledger_get"]},

        {"ledger_delete", "Delete checkpoint",
            {
                {"type","object"},
                {"properties", {
                    {"key", {{"type","string"}, {"description","Canonical checkpoint key (session:project)"}}},
                    {"session_id", {{"type","string"}}},
                    {"project", {{"type","string"}}}
                }}
            },
            &FieldRpcHandler::tool_ledger_delete, handlers_["ledger_delete"]},

        {"long_task_start", "Start a long-running task",
            {{"type","object"},{"properties",{
                {"task_id",{{"type","string"}}},{"goal",{{"type","string"}}},{"realm",{{"type","string"}}},
                {"hard_checks",{{"type","array"},{"items",{{"type","string"}}}}},
                {"soft_checks",{{"type","array"},{"items",{{"type","string"}}}}},
                {"work_items",{{"type","array"},{"items",{{"type","string"}}}}}
            }},{"required",{"task_id","goal"}}},
            &FieldRpcHandler::tool_long_task_start, handlers_["long_task_start"]},

        {"long_task_get", "Get a long-running task by ID",
            {{"type","object"},{"properties",{
                {"task_id",{{"type","string"}}}
            }},{"required",{"task_id"}}},
            &FieldRpcHandler::tool_long_task_get, handlers_["long_task_get"]},

        {"long_task_active", "Get active long-running task",
            {{"type","object"},{"properties",{{"realm",{{"type","string"}}}}}},
            &FieldRpcHandler::tool_long_task_active, handlers_["long_task_active"]},

        {"long_task_update", "Update long-running task progress",
            {{"type","object"},{"properties",{
                {"task_id",{{"type","string"}}},{"completed_summary",{{"type","string"}}},
                {"work_items",{{"type","array"},{"items",{{"type","string"}}}}},
                {"blockers",{{"type","array"},{"items",{{"type","string"}}}}}
            }},{"required",{"task_id"}}},
            &FieldRpcHandler::tool_long_task_update, handlers_["long_task_update"]},

        {"long_task_complete", "Mark task as completed",
            {{"type","object"},{"properties",{
                {"task_id",{{"type","string"}}},{"outcome",{{"type","string"}}}
            }},{"required",{"task_id","outcome"}}},
            &FieldRpcHandler::tool_long_task_complete, handlers_["long_task_complete"]},

        {"long_task_event", "Append event to task log",
            {{"type","object"},{"properties",{
                {"task_id",{{"type","string"}}},{"kind",{{"type","string"}}},
                {"payload",{{"type","string"}}},{"tags",{{"type","array"},{"items",{{"type","string"}}}}},
                {"related_entities",{{"type","array"},{"items",{{"type","string"}}}}}
            }},{"required",{"task_id","kind"}}},
            &FieldRpcHandler::tool_long_task_event, handlers_["long_task_event"]},

        {"checkpoint", "Save session state",
            {{"type","object"},{"properties",{
                {"realm",{{"type","string"}}},{"mood",{{"type","string"}}},
                {"summary",{{"type","string"}}},{"next_steps",{{"type","array"},{"items",{{"type","string"}}}}},
                {"active_files",{{"type","array"},{"items",{{"type","string"}}}}},
                {"discoveries",{{"type","array"},{"items",{{"type","string"}}}}}
            }}},
            &FieldRpcHandler::tool_unified_checkpoint, handlers_["checkpoint"]},

        {"long_task_snapshot", "Get synthesized task context",
            {{"type","object"},{"properties",{
                {"task_id",{{"type","string"}}},{"mode",{{"type","string"}}},{"max_tokens",{{"type","integer"}}}
            }},{"required",{"task_id"}}},
            &FieldRpcHandler::tool_long_task_snapshot, handlers_["long_task_snapshot"]},

        {"long_task_evaluate", "Evaluate task completion",
            {{"type","object"},{"properties",{
                {"task_id",{{"type","string"}}}
            }},{"required",{"task_id"}}},
            &FieldRpcHandler::tool_long_task_evaluate, handlers_["long_task_evaluate"]},

        // ── Session/Transcript tools ────────────────────────────────────────
        {"skill_upload", "Upload a new skill version",
            {{"type","object"},{"properties",{
                {"skill_id",{{"type","string"}}},{"content",{{"type","string"}}},
                {"uploaded_by",{{"type","string"}}},{"tags",{{"type","array"},{"items",{{"type","string"}}}}}
            }},{"required",{"skill_id","content"}}},
            &FieldRpcHandler::tool_skill_upload, handlers_["skill_upload"]},

        {"skill_read", "Read a skill version (0=latest)",
            {{"type","object"},{"properties",{
                {"skill_id",{{"type","string"}}},{"version",{{"type","integer"}}}
            }},{"required",{"skill_id"}}},
            &FieldRpcHandler::tool_skill_read, handlers_["skill_read"]},

        {"skill_list", "List all skills",
            {{"type","object"},{"properties",json::object()}},
            &FieldRpcHandler::registered_skill_list, handlers_["skill_list"]},

        {"skill_search", "Search skills by query",
            {{"type","object"},{"properties",{
                {"query",{{"type","string"}}},{"limit",{{"type","integer"}}}
            }},{"required",{"query"}}},
            &FieldRpcHandler::tool_skill_search, handlers_["skill_search"]},

        {"skill_deprecate", "Deprecate a skill",
            {{"type","object"},{"properties",{
                {"skill_id",{{"type","string"}}}
            }},{"required",{"skill_id"}}},
            &FieldRpcHandler::tool_skill_deprecate, handlers_["skill_deprecate"]},

        // ── Agent Registry ─────────────────────────────────────────────────
        {"agent_upsert", "Register or update an agent",
            {{"type","object"},{"properties",{
                {"agent_id",{{"type","string"}}},{"display_name",{{"type","string"}}},
                {"description",{{"type","string"}}}
            }},{"required",{"agent_id"}}},
            &FieldRpcHandler::tool_agent_upsert, handlers_["agent_upsert"]},

        {"agent_get", "Get agent record",
            {{"type","object"},{"properties",{
                {"agent_id",{{"type","string"}}}
            }},{"required",{"agent_id"}}},
            &FieldRpcHandler::tool_agent_get, handlers_["agent_get"]},

        {"agent_list", "List all agents",
            {{"type","object"},{"properties",json::object()}},
            &FieldRpcHandler::registered_agent_list, handlers_["agent_list"]},

        {"agent_disable", "Disable an agent",
            {{"type","object"},{"properties",{
                {"agent_id",{{"type","string"}}}
            }},{"required",{"agent_id"}}},
            &FieldRpcHandler::tool_agent_disable, handlers_["agent_disable"]},

        // ── Misc tools ──────────────────────────────────────────────────────

        // Memory management
        {"why_active", "Explain why a memory is active: status, epistemic source, confirmations, contradictions",
            {{"type","object"},{"properties",{
                {"id",{{"type","string"},{"description","Memory ID to inspect"}}}
            }},{"required",{"id"}}},
            &FieldRpcHandler::tool_why_active, handlers_["why_active"]},

        {"what_superseded", "Show the full supersession chain for a memory",
            {{"type","object"},{"properties",{
                {"id",{{"type","string"},{"description","Memory ID to trace"}}}
            }},{"required",{"id"}}},
            &FieldRpcHandler::tool_what_superseded, handlers_["what_superseded"]},

        {"show_conflicts", "Semantic search + show contradiction pairs for matching memories",
            {{"type","object"},{"properties",{
                {"query",{{"type","string"},{"description","Search query"}}},
                {"limit",{{"type","integer"},{"description","Max memories to scan (default 20)"}}},
                {"realm",{{"type","string"},{"description","Filter by realm"}}}
            }},{"required",{"query"}}},
            &FieldRpcHandler::tool_show_conflicts, handlers_["show_conflicts"]},

        {"detect_contradictions", "Detect contradictions for a stored memory against realm peers",
            {{"type","object"},{"properties",{
                {"memory_id",{{"type","string"},{"description","Memory ID to check"}}},
                {"realm",{{"type","string"},{"description","Realm to scan (default global)"}}}
            }},{"required",{"memory_id"}}},
            &FieldRpcHandler::tool_detect_contradictions, handlers_["detect_contradictions"]},

        {"scan_contradictions", "Background scan: find contradiction candidates across all memories in a realm",
            {{"type","object"},{"properties",{
                {"realm",{{"type","string"},{"description","Realm to scan"}}},
                {"limit",{{"type","integer"},{"description","Max candidates to return (default 50)"}}}
            }}},
            &FieldRpcHandler::tool_scan_contradictions, handlers_["scan_contradictions"]},

        {"resolve_contradiction", "Resolve a contradiction: declare winner supersedes loser, store CORRECTION memory",
            {{"type","object"},{"properties",{
                {"winner_id",{{"type","string"},{"description","Memory ID that is correct"}}},
                {"loser_id",{{"type","string"},{"description","Memory ID to demote"}}},
                {"reason",{{"type","string"},{"description","Explanation for the resolution"}}}
            }},{"required",{"winner_id","loser_id"}}},
            &FieldRpcHandler::tool_resolve_contradiction, handlers_["resolve_contradiction"]},

        // Operator controls
        {"approve_memory", "Approve a Proposed memory, promoting it to Active",
            {{"type","object"},{"properties",{
                {"id",{{"type","string"},{"description","Memory ID to approve"}}}
            }},{"required",{"id"}}},
            &FieldRpcHandler::tool_approve_memory, handlers_["approve_memory"]},

        {"reject_memory", "Reject a Proposed memory, archiving it",
            {{"type","object"},{"properties",{
                {"id",{{"type","string"},{"description","Memory ID to reject"}}}
            }},{"required",{"id"}}},
            &FieldRpcHandler::tool_reject_memory, handlers_["reject_memory"]},

        {"promote_memory", "Promote a memory one tier: Proposed→Observed→Verified→Active",
            {{"type","object"},{"properties",{
                {"id",{{"type","string"},{"description","Memory ID"}}}
            }},{"required",{"id"}}},
            &FieldRpcHandler::tool_promote_memory, handlers_["promote_memory"]},

        {"conflict_inspector", "Semantic search + show status and contradiction partners for each hit",
            {{"type","object"},{"properties",{
                {"query",{{"type","string"},{"description","Search query"}}},
                {"limit",{{"type","integer"},{"description","Max memories to scan (default 10)"}}},
                {"realm",{{"type","string"},{"description","Filter by realm"}}}
            }},{"required",{"query"}}},
            &FieldRpcHandler::tool_conflict_inspector, handlers_["conflict_inspector"]},

        {"disable_source", "Add a source to the deny-list via triplet",
            {{"type","object"},{"properties",{
                {"source",{{"type","string"},{"description","Source identifier to deny"}}}
            }},{"required",{"source"}}},
            &FieldRpcHandler::tool_disable_source, handlers_["disable_source"]},

        // Override memory_history handler with richer operator version
        {"memory_history", nullptr, nullptr, &FieldRpcHandler::tool_operator_memory_history, handlers_["memory_history"]},

        // ── Tier 1: Ingest source ──────────────────────────────────────────
    });
}

ToolResult FieldRpcHandler::registered_version_check(const json&) { return tool_version_check(); }

ToolResult FieldRpcHandler::registered_embed_coverage(const json&) {
    auto [eligible, embedded] = field_store_->embed_coverage();
    size_t missing = eligible > embedded ? eligible - embedded : 0;
    double pct = eligible ? (100.0 * embedded / eligible) : 100.0;
    std::ostringstream ss;
    ss << "embeddable memories : " << eligible << "\n"
       << "  with a vector     : " << embedded << "\n"
       << "  MISSING (bm25only): " << missing  << "\n"
       << "  coverage          : " << std::fixed << std::setprecision(1) << pct << "%\n";
    json out = {{"eligible", eligible}, {"embedded", embedded},
                {"missing", missing},   {"coverage_pct", pct},
                {"pending", field_store_->raw_pending_count()}};
    return ToolResult::ok(ss.str(), out);
}

ToolResult FieldRpcHandler::registered_embed_probe(const json& p) {
    uint64_t id = std::stoull(p.at("id").get<std::string>());
    std::vector<float> stored(EMBED_DIM, 0.0f);
    size_t n = field_store_->get_embedding(id, stored.data(), stored.size());
    if (n == 0) return ToolResult::error("memory has no stored vector");
    std::string content = field_store_->get_content(id);
    if (content.empty()) return ToolResult::error("memory has no content");

    // transform(EmbedMode::Document) prepends "search_document: " itself.
    const std::string body = chitta::ssl::retrieval_text(content);
    auto single = yantra_->transform(body, EmbedMode::Document).nu.data;
    auto dbl    = yantra_->transform("search_document: " + body, EmbedMode::Document).nu.data;
    auto query  = yantra_->transform(content, EmbedMode::Query).nu.data;

    auto cos = [&](const std::vector<float>& a) {
        double d = 0, na = 0, nb = 0;
        for (size_t i = 0; i < stored.size() && i < a.size(); ++i) {
            d += double(stored[i]) * a[i]; na += double(a[i]) * a[i];
            nb += double(stored[i]) * stored[i];
        }
        return (na > 0 && nb > 0) ? d / (std::sqrt(na) * std::sqrt(nb)) : 0.0;
    };
    double cs = cos(single), cd = cos(dbl), cq = cos(query);
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(4)
       << "cos(stored, single-prefix) : " << cs << "\n"
       << "cos(stored, DOUBLE-prefix) : " << cd << "\n"
       << "cos(stored, query-of-self) : " << cq << "\n"
       << "space: " << (cd > cs ? "DOUBLE (wrong)" : "single (correct)") << "\n";
    return ToolResult::ok(ss.str(), {{"id", std::to_string(id)},
        {"cos_single", cs}, {"cos_double", cd}, {"cos_self_query", cq},
        {"space", cd > cs ? "double" : "single"}});
}

ToolResult FieldRpcHandler::registered_stageb_set_surface(const json& p) {
    uint64_t id = std::stoull(p.at("id").get<std::string>());
    std::string surface = p.value("surface", std::string());
    field_store_->set_retrieval_surface(id, surface);
    // Re-embed from the surface when set, else from the telegraphic content —
    // mirrors embed_loop's selection so the stored vector matches the live path.
    std::string text = surface;
    if (text.empty()) {
        std::string content = field_store_->get_content(id);
        if (content.empty()) return ToolResult::error("memory has no content and no surface");
        text = chitta::ssl::retrieval_text(content);
    }
    if (!yantra_) return ToolResult::error("embedder unavailable");
    auto vec = yantra_->transform(text, EmbedMode::Document).nu.data;
    bool embedded = false;
    bool zero = true;
    for (float f : vec) { if (f != 0.0f) { zero = false; break; } }
    if (!zero) { field_store_->backfill_embedding(id, vec); embedded = true; }
    return ToolResult::ok(embedded ? "surface set + re-embedded" : "surface set (embed failed/zero)",
        {{"id", std::to_string(id)}, {"embedded", embedded}, {"surface_len", surface.size()}});
}

ToolResult FieldRpcHandler::registered_pending_embed_ids(const json& p) {
    bool purge = p.value("purge", false);
    bool force = p.value("force", false);
    size_t cleared = 0;
    if (purge) {
        cleared = field_store_->purge_orphan_embed_pending();
        if (cleared == 0 && force) {
            // Fallback: force-clear all currently pending IDs
            auto stuck = field_store_->pending_embeddings(200);
            if (!stuck.empty()) cleared = field_store_->force_clear_embed_pending(stuck);
        }
    }
    auto ids = field_store_->pending_embeddings(200);
    json arr = json::array();
    for (auto id : ids) arr.push_back(id);
    std::string msg = "Pending embed IDs (" + std::to_string(ids.size()) + ")";
    if (purge) msg += " — cleared " + std::to_string(cleared);
    msg += ":\n";
    for (auto id : ids) msg += "  " + std::to_string(id) + "\n";
    return ToolResult::ok(msg, {{"ids", arr}, {"count", ids.size()}, {"cleared", cleared}});
}

ToolResult FieldRpcHandler::registered_prune_episodes(const json& p) {
    uint64_t max_age = p.value("max_age_days", 90);
    size_t   max_cnt = p.value("max_count", 10000);
    int64_t  deleted = field_store_->prune_episodes(max_age, max_cnt);
    std::string msg = "pruned " + std::to_string(deleted) + " episode memories";
    return ToolResult::ok(msg, {{"deleted", deleted}});
}

ToolResult FieldRpcHandler::registered_realm_list(const json&) { return tool_realm_list(); }

ToolResult FieldRpcHandler::registered_realm_detect(const json&) { return tool_realm_detect(); }

ToolResult FieldRpcHandler::registered_skill_list(const json&) { return tool_skill_list(); }

ToolResult FieldRpcHandler::registered_agent_list(const json&) { return tool_agent_list(); }

} // namespace chitta
