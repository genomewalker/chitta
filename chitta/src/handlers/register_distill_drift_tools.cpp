// register_distill_drift_tools — chunk extracted from register_tools() so editing
// tool metadata only retemplates one chunk at a time.

#include "../../include/chitta/rpc/field_handler.hpp"

namespace chitta {

void FieldRpcHandler::register_distill_drift_tools() {
    register_tool_table({
        {"distill_status", "Get distillation status",
            {{"type","object"},{"properties",json::object()}},
            &FieldRpcHandler::tool_distill_status, handlers_["distill_status"]},

        {"distill_now", "Synchronously distill ONE session now and return the counts "
                "(learnings + value-facts stored/deduped). Runs the lock-fixed distill path "
                "in-daemon; recall stays responsive.",
            {{"type","object"},{"properties",{
                {"session_id",{{"type","string"},{"description","Session ID to distill"}}},
                {"transcript_path",{{"type","string"},{"description","Optional JSONL path; registers it if given, else resolves from durable transcript/register"}}},
                {"realm",{{"type","string"},{"description","Optional realm (default brahman or the registered realm)"}}}
            }},{"required",{"session_id"}}},
            &FieldRpcHandler::tool_distill_now, handlers_["distill_now"]},

        {"distill_set_model", "Change distillation model",
            {{"type","object"},{"properties",{
                {"model",{{"type","string"}}},{"enabled",{{"type","boolean"}}}
            }},{"required",{"model"}}},
            &FieldRpcHandler::tool_distill_set_model, handlers_["distill_set_model"]},

        {"suggestion_track", "Track a suggestion",
            {{"type","object"},{"properties",{
                {"content",{{"type","string"}}},{"context",{{"type","string"}}},{"realm",{{"type","string"}}}
            }},{"required",{"content"}}},
            &FieldRpcHandler::tool_suggestion_track, handlers_["suggestion_track"]},

        {"suggestion_pending", "List pending suggestions",
            {{"type","object"},{"properties",{
                {"realm",{{"type","string"}}},{"limit",{{"type","integer"}}}
            }}},
            &FieldRpcHandler::tool_suggestion_pending, handlers_["suggestion_pending"]},

        {"suggestion_resolve", "Record suggestion outcome",
            {{"type","object"},{"properties",{
                {"id",{{"type","integer"}}},{"helped",{{"type","boolean"}}},{"details",{{"type","string"}}}
            }},{"required",{"id","helped"}}},
            &FieldRpcHandler::tool_suggestion_resolve, handlers_["suggestion_resolve"]},

        {"suggestion_count", "Count pending suggestions",
            {{"type","object"},{"properties",{{"realm",{{"type","string"}}}}}},
            &FieldRpcHandler::tool_suggestion_count, handlers_["suggestion_count"]},

        {"consolidation_scan", "Find similar memory pairs",
            {{"type","object"},{"properties",{
                {"similarity_threshold",{{"type","number"}}},{"limit",{{"type","integer"}}},
                {"realm",{{"type","string"}}}
            }}},
            &FieldRpcHandler::tool_consolidation_scan, handlers_["consolidation_scan"]},

        {"consolidation_merge", "Merge two similar memories",
            {{"type","object"},{"properties",{
                {"primary_id",{{"type","integer"}}},{"secondary_id",{{"type","integer"}}},
                {"merged_content",{{"type","string"}}}
            }},{"required",{"primary_id","secondary_id"}}},
            &FieldRpcHandler::tool_consolidation_merge, handlers_["consolidation_merge"]},

        {"consolidation_auto", "Auto-merge highly similar memories",
            {{"type","object"},{"properties",{
                {"similarity_threshold",{{"type","number"}}},{"max_merges",{{"type","integer"}}}
            }}},
            &FieldRpcHandler::tool_consolidation_auto, handlers_["consolidation_auto"]},

        {"metacognition_corrections", "Analyze patterns in corrections",
            {{"type","object"},{"properties",{{"limit",{{"type","integer"}}}}}},
            &FieldRpcHandler::tool_metacognition_corrections, handlers_["metacognition_corrections"]},

        {"metacognition_outcomes", "Analyze suggestion outcomes",
            {{"type","object"},{"properties",{{"limit",{{"type","integer"}}}}}},
            &FieldRpcHandler::tool_metacognition_outcomes, handlers_["metacognition_outcomes"]},

        {"metacognition_evaluate", "Self-evaluate learning effectiveness",
            {{"type","object"},{"properties",json::object()}},
            &FieldRpcHandler::tool_metacognition_evaluate, handlers_["metacognition_evaluate"]},

        {"epiplexity_check", "Compute epiplexity score",
            {{"type","object"},{"properties",{
                {"original",{{"type","string"}}},{"seed",{{"type","string"}}},{"reconstructed",{{"type","string"}}}
            }},{"required",{"original","seed","reconstructed"}}},
            &FieldRpcHandler::tool_epiplexity_check, handlers_["epiplexity_check"]},

        {"ssl_convert", "Convert raw text to SSL format",
            {{"type","object"},{"properties",{
                {"content",{{"type","string"}}},{"domain",{{"type","string"}}},{"location",{{"type","string"}}}
            }},{"required",{"content"}}},
            &FieldRpcHandler::tool_ssl_convert, handlers_["ssl_convert"]},

        {"curiosity_note_gap", "Record a knowledge gap",
            {{"type","object"},{"properties",{
                {"gap",{{"type","string"}}},{"context",{{"type","string"}}},{"realm",{{"type","string"}}}
            }},{"required",{"gap"}}},
            &FieldRpcHandler::tool_curiosity_note_gap, handlers_["curiosity_note_gap"]},

        {"curiosity_gaps", "List knowledge gaps",
            {{"type","object"},{"properties",{
                {"limit",{{"type","integer"}}},{"realm",{{"type","string"}}}
            }}},
            &FieldRpcHandler::tool_curiosity_gaps, handlers_["curiosity_gaps"]},

        {"curiosity_resolve", "Mark gap as resolved",
            {{"type","object"},{"properties",{
                {"id",{{"type","integer"}}},{"learned",{{"type","string"}}}
            }},{"required",{"id"}}},
            &FieldRpcHandler::tool_curiosity_resolve, handlers_["curiosity_resolve"]},

        // ── Skill Registry ──────────────────────────────────────────────────
        {"lookup", "Unified memory lookup. Classifies intent, fans out to keyword/semantic/triplet/temporal/code backends, fuses with weighted RRF. Use this as the default memory search.",
            {{"type","object"},{"properties",{
                {"query",{{"type","string"}}},
                {"limit",{{"type","integer"}}},
                {"realm",{{"type","string"}}},
                {"mode",{{"type","string"},{"enum",{"auto","fast","deep"}}}},
                {"explain",{{"type","boolean"}}}
            }},{"required",{"query"}}},
            &FieldRpcHandler::tool_lookup, handlers_["lookup"]},

        {"compact_context", "Memory-aware context compaction. Scores conversation messages by recency, "
                "semantic relevance to query, and memory coverage (content already in memory is safer to drop). "
                "Returns a subset of messages fitting the target token budget.",
            {{"type", "object"},
                {"properties", {
                    {"messages", {{"type", "array"}, {"description", "Conversation messages [{role,content}]"},
                        {"items", {{"type", "object"}}}}},
                    {"query", {{"type", "string"}, {"description", "Upcoming task hint for semantic scoring"}}},
                    {"target_ratio", {{"type", "number"}, {"description", "Fraction of tokens to KEEP (default 0.4)"}}},
                    {"distill_novel", {{"type", "boolean"}, {"description", "Reserved for future use"}}}
                }}, {"required", {"messages"}}
            },
            &FieldRpcHandler::tool_compact_context, handlers_["compact_context"]},

        // ── Trajectory compaction (Latent Briefing) ─────────────────────────
        {"trajectory_compact", "Attention-weighted turn selection from a transcript. "
                "Embeds each turn, scores by cosine similarity to the task description, "
                "applies MAD adaptive threshold, enforces token budget. "
                "Returns a lossless subset of the most task-relevant turns.",
            {{"type", "object"},
                {"properties", {
                    {"task", {{"type", "string"}, {"description", "What the downstream agent needs to accomplish"}}},
                    {"session_id", {{"type", "string"}, {"description", "Session ID (auto-finds transcript)"}}},
                    {"path", {{"type", "string"}, {"description", "Direct path to JSONL transcript"}}},
                    {"budget_tokens", {{"type", "integer"}, {"description", "Target token budget (default 4000)"}}},
                    {"mad_k", {{"type", "number"}, {"description", "MAD threshold multiplier (default 1.5, lower=more turns)"}}},
                    {"role_filter", {{"type", "string"}, {"description", "Filter by role: user, assistant, or empty for all"}}},
                    // ceiling: advertised but inert — tool_trajectory_compact reads
                    // no such flag, so system turns are always excluded and a caller
                    // passing true is silently ignored; upgrade: thread it into the
                    // role filter there, or drop it from this schema. (2026-09-02)
                    {"include_system", {{"type", "boolean"}, {"description", "Include system turns (default false)"}}}
                }}, {"required", {"task"}}
            },
            &FieldRpcHandler::tool_trajectory_compact, handlers_["trajectory_compact"]},

        // ── Layer 1: Executable Constraints ─────────────────────────────────
        {"set_evidence_type", "Tag a memory with its epistemological evidence class (observation/inference/hearsay/authoritative/prediction)",
            {{"type","object"},{"properties",{
                {"id",{{"type","string"},{"description","Memory ID"}}},
                {"evidence_type",{{"type","string"},{"description","One of: observation, inference, hearsay, authoritative, prediction"}}}
            }},{"required",{"id","evidence_type"}}},
            &FieldRpcHandler::tool_set_evidence_type, handlers_["set_evidence_type"]},

        {"get_evidence_type", "Retrieve the evidence type tag of a memory",
            {{"type","object"},{"properties",{
                {"id",{{"type","string"},{"description","Memory ID"}}}
            }},{"required",{"id"}}},
            &FieldRpcHandler::tool_get_evidence_type, handlers_["get_evidence_type"]},

        {"labile_memories", "List memories recalled multiple times recently — candidates for reconsolidation (excludes freshly-written hook memories)",
            {{"type","object"},{"properties",{
                {"realm",{{"type","string"},{"description","Filter by realm"}}},
                {"limit",{{"type","integer"},{"description","Max results (default 20)"}}},
                {"window_hours",{{"type","number"},{"description","Recency window in hours (default 48)"}}},
                {"min_access",{{"type","integer"},{"description","Min recall count to qualify (default 2)"}}}
            }}},
            &FieldRpcHandler::tool_labile_memories, handlers_["labile_memories"]},

        {"reconsolidate", "Update content of a memory during its labile window (reconsolidation)",
            {{"type","object"},{"properties",{
                {"id",{{"type","string"},{"description","Memory ID to update"}}},
                {"content",{{"type","string"},{"description","New/corrected content"}}},
                {"reason",{{"type","string"},{"description","Optional reason for reconsolidation"}}}
            }},{"required",{"id","content"}}},
            &FieldRpcHandler::tool_reconsolidate, handlers_["reconsolidate"]},

        {"5w_search", "Multi-dimensional semantic search across who/what/when/where/why axes",
            {{"type","object"},{"properties",{
                {"who",{{"type","string"},{"description","Who is involved"}}},
                {"what",{{"type","string"},{"description","What is happening/topic"}}},
                {"when",{{"type","string"},{"description","Temporal description"}}},
                {"where",{{"type","string"},{"description","Location or context"}}},
                {"why",{{"type","string"},{"description","Motivation or reason"}}},
                {"realm",{{"type","string"},{"description","Filter by realm"}}},
                {"limit",{{"type","integer"},{"description","Max results (default 10)"}}}
            }}},
            &FieldRpcHandler::tool_5w_search, handlers_["5w_search"]},

        {"recall_ucb1", "Recall with UCB1 exploration bonus — surfaces novel under-accessed memories alongside relevant ones",
            {{"type","object"},{"properties",{
                {"query",{{"type","string"},{"description","Search query"}}},
                {"realm",{{"type","string"},{"description","Filter by realm"}}},
                {"limit",{{"type","integer"},{"description","Max results (default 10)"}}},
                {"exploration",{{"type","number"},{"description","Exploration weight sqrt(2)≈1.414 (default)"}}},
                {"fetch_k",{{"type","integer"},{"description","Candidate pool size before re-ranking (default 40)"}}}
            }},{"required",{"query"}}},
            &FieldRpcHandler::tool_recall_ucb1, handlers_["recall_ucb1"]},

        {"find_near_duplicates", "Find memory pairs with high semantic similarity (near-duplicates)",
            {{"type","object"},{"properties",{
                {"realm",{{"type","string"},{"description","Filter by realm"}}},
                {"limit",{{"type","integer"},{"description","Max pairs to return (default 20)"}}},
                {"threshold",{{"type","number"},{"description","Cosine similarity threshold (default 0.90)"}}}
            }}},
            &FieldRpcHandler::tool_find_near_duplicates, handlers_["find_near_duplicates"]},

        {"consolidate_similar", "Merge near-duplicate memories — keeps stronger, soft-deletes weaker",
            {{"type","object"},{"properties",{
                {"realm",{{"type","string"},{"description","Filter by realm"}}},
                {"threshold",{{"type","number"},{"description","Similarity threshold (default 0.92)"}}},
                {"dry_run",{{"type","boolean"},{"description","Preview without deleting (default true)"}}},
                {"limit",{{"type","integer"},{"description","Max pairs to merge (default 10)"}}}
            }}},
            &FieldRpcHandler::tool_consolidate_similar, handlers_["consolidate_similar"]},

        {"cooccurrence_graph", "Show top co-activated memory associations for a given memory",
            {{"type","object"},{"properties",{
                {"id",{{"type","string"},{"description","Memory ID"}}},
                {"limit",{{"type","integer"},{"description","Max edges to return (default 10)"}}}
            }},{"required",{"id"}}},
            &FieldRpcHandler::tool_cooccurrence_graph, handlers_["cooccurrence_graph"]},

        {"labile_memories_top", "List the most-accessed (most labile) memories — candidates for reconsolidation",
            {{"type","object"},{"properties",{
                {"realm",{{"type","string"},{"description","Filter by realm"}}},
                {"limit",{{"type","integer"},{"description","Max results (default 20)"}}}
            }}},
            &FieldRpcHandler::tool_labile_memories_top, handlers_["labile_memories_top"]},

        // ── Behavioral Probe ──────────────────────────────────────────────────
        {"probe_seed", "Store an exemplar text as a centroid for a behavioral class (sycophantic/hedging/shallow/direct). Bootstrap the probe with representative examples.",
            {{"type","object"},{"properties",{
                {"class",{{"type","string"},{"description","Behavioral class: sycophantic | hedging | shallow | direct"}}},
                {"text",{{"type","string"},{"description","Exemplar text for this class"}}},
                {"note",{{"type","string"},{"description","Optional annotation"}}}
            }},{"required",{"class","text"}}},
            &FieldRpcHandler::tool_probe_seed, handlers_["probe_seed"]},

        {"behavioral_probe", "Score text against behavioral centroid clusters. Returns per-class similarity scores (sycophantic/hedging/shallow/direct) and overall quality estimate. Requires prior probe_seed calls.",
            {{"type","object"},{"properties",{
                {"text",{{"type","string"},{"description","Text to probe (e.g. a Claude response)"}}}
            }},{"required",{"text"}}},
            &FieldRpcHandler::tool_behavioral_probe, handlers_["behavioral_probe"]},

        {"probe_calibrate", "Add a confirmed exemplar to a behavioral class to refine its centroid. Use when you have a clear example of the behavior.",
            {{"type","object"},{"properties",{
                {"class",{{"type","string"},{"description","Behavioral class to update"}}},
                {"text",{{"type","string"},{"description","Confirmed exemplar text"}}}
            }},{"required",{"class","text"}}},
            &FieldRpcHandler::tool_probe_calibrate, handlers_["probe_calibrate"]},

        {"probe_status", "Show how many exemplars exist per behavioral class. Use to verify the probe is seeded before running behavioral_probe.",
            {{"type","object"}},
            &FieldRpcHandler::tool_probe_status, handlers_["probe_status"]},

        // Contradiction engine
    });
}

} // namespace chitta
