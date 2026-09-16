// register_memory_core_tools — chunk extracted from register_tools() so editing
// tool metadata only retemplates one chunk at a time.

#include "../../include/chitta/rpc/field_handler.hpp"

namespace chitta {

void FieldRpcHandler::register_memory_core_tools() {
    register_tool_table({
        {"remember", "Store text in memory with optional tags and realm",
            {{"type", "object"},
                {"properties", {
                    {"content", {{"type", "string"}, {"description", "Text to remember"}}},
                    {"type", {{"type", "string"}, {"description", "Node type (wisdom, insight, signal, episode)"}}},
                    {"confidence", {{"type", "number"}, {"description", "Initial confidence 0-1 (default: 0.8)"}}},
                    {"tags", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Optional tags"}}},
                    {"realm", {{"type", "string"}, {"description", "Primary realm (default: brahman)"}}},
                    {"visibility", {{"type", "integer"}, {"description", "0=Private, 1=Shared, 2=Global (default: 0)"}}},
                    {"shared_realms", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Additional realms"}}}
                }}, {"required", {"content"}}
            },
            &FieldRpcHandler::tool_remember, handlers_["remember"]},

        {"remember_batch", "Store multiple memories in a single round-trip (high-throughput bulk ingest)",
            {{"type", "object"},
                {"properties", {
                    {"items", {{"type", "array"}, {"description", "Array of memory objects (same fields as remember)"},
                        {"items", {{"type", "object"}, {"properties", {
                            {"content", {{"type", "string"}}},
                            {"realm", {{"type", "string"}}},
                            {"tags", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                            {"valid_from", {{"type", "string"}}},
                            {"source_session", {{"type", "string"}}},
                            {"visibility", {{"type", "integer"}}}
                        }}, {"required", {"content"}}}}}},
                    {"realm", {{"type", "string"}, {"description", "Default realm for all items"}}}
                }}, {"required", {"items"}}
            },
            &FieldRpcHandler::tool_remember_batch, handlers_["remember_batch"]},

        {"flush_embeddings", "Flush the pending embedding queue synchronously — call after remember_batch to ensure HNSW/semantic recall is available immediately. Returns {flushed: N}.",
            {{"type", "object"}, {"properties", json::object()}},
            &FieldRpcHandler::tool_flush_embeddings, handlers_["flush_embeddings"]},

        {"recall", "Search memory by semantic similarity with realm filtering",
            {{"type", "object"},
                {"properties", {
                    {"query", {{"type", "string"}, {"description", "Search query"}}},
                    {"limit", {{"type", "integer"}, {"description", "Max results (default 10)"}}},
                    {"min_confidence", {{"type", "number"}, {"description", "Minimum confidence threshold"}}},
                    {"tag", {{"type", "string"}, {"description", "Filter by tag"}}},
                    {"realm", {{"type", "string"}, {"description", "Filter by realm"}}},
                    {"include_global", {{"type", "boolean"}, {"description", "Include global memories (default: true)"}}},
                    {"sources", {{"type", "boolean"}, {"default", true}, {"description", "Include indexed repository sources; false returns memory ranking only (default: true)"}}},
                    {"separation_mode", {{"type", "boolean"}, {"description", "Diverse results via MMR (default: false)"}}},
                    {"strategy", {{"type", "string"}, {"description", "Retrieval lane: fused (default), keyword (BM25 only, realm-scoped), field (Hopfield/DAM)"}}},
                    {"pool", {{"type", "integer"}, {"description", "Candidate pool depth before the recall-biased pre-filter (default 60, max 160; env CHITTA_RECALL_POOL)"}}},
                    {"prefilter", {{"type", "boolean"}, {"description", "Recall-biased pre-filter (default true; false or CHITTA_RECALL_PREFILTER=0 restores the narrow-pool path)"}}},
                    {"gwt_mode", {{"type", "boolean"}, {"description", "Global Workspace Theory mode (default: false)"}}},
                    {"explain", {{"type", "boolean"}, {"description", "Include score decomposition per hit (default: false)"}}}
                }}, {"required", {"query"}}
            },
            &FieldRpcHandler::tool_recall, handlers_["recall"]},

        {"recall_temporal", "Search memories within a time window (defaults to last 7 days)",
            {{"type", "object"},
                {"properties", {
                    {"query", {{"type", "string"}, {"description", "Optional semantic search query"}}},
                    {"start", {{"type", "string"}, {"description", "Start date (ISO8601 or YYYY-MM-DD)"}}},
                    {"end", {{"type", "string"}, {"description", "End date"}}},
                    {"limit", {{"type", "integer"}, {"description", "Max results (default 20)"}}},
                    {"realm", {{"type", "string"}, {"description", "Filter by realm"}}},
                    {"include_global", {{"type", "boolean"}, {"description", "Include global memories"}}}
                }}
            },
            &FieldRpcHandler::tool_recall_temporal, handlers_["recall_temporal"]},
        {"recall_temporal_events", nullptr, nullptr, &FieldRpcHandler::tool_recall_temporal_events, handlers_["recall_temporal_events"]},

        {"recall_keyword", "BM25 keyword search",
            {{"type","object"},{"properties",{
                {"query",{{"type","string"},{"description","Search query"}}},
                {"limit",{{"type","integer"},{"description","Max results (default 10)"}}},
                {"realm",{{"type","string"},{"description","Filter by realm (empty = all visible)"}}},
                {"explain",{{"type","boolean"},{"description","Include score decomposition per hit (default: false)"}}}
            }},{"required",{"query"}}},
            &FieldRpcHandler::tool_recall_keyword, handlers_["recall_keyword"]},

        {"provenance_check", "Deterministic anti-reprocessing check: has this file/task already been processed? Exact keyed lookup of a prior [done] record by content-hash and/or input path — bypasses fuzzy recall. Returns found + the record.",
            {{"type","object"},{"properties",{
                {"sha",{{"type","string"},{"description","Content hash of the input (tried first — content identity)"}}},
                {"input",{{"type","string"},{"description","Input path (fallback handle)"}}}
            }},{"required",json::array()}},
            &FieldRpcHandler::tool_provenance_check, handlers_["provenance_check"]},

        {"correction_check", "Deterministic durable-correction check (capability #2): does any stored [correction] trigger recur in this turn? Exact keyed bigram probe of the turn text against corrected-mistake phrases — reserves an injection slot, bypassing fuzzy recall (which loses corrections on cosine similarity ~99% of the time). Returns found + the correction(s), newest first (latest-wins).",
            {{"type","object"},{"properties",{
                {"text",{{"type","string"},{"description","Turn / context text to scan for a recurring corrected mistake"}}}
            }},{"required",{"text"}}},
            &FieldRpcHandler::tool_correction_check, handlers_["correction_check"]},

        {"task_state", "Deterministic task-state check (capability #3): what is the current state of task X? Exact keyed lookup of the LATEST stored [task] record by its slug — bypasses fuzzy recall so an agent resuming a discontinuous session gets the durable status/next-step. Returns found + the record.",
            {{"type","object"},{"properties",{
                {"id",{{"type","string"},{"description","Task slug (the id: token of the [task] record)"}}}
            }},{"required",{"id"}}},
            &FieldRpcHandler::tool_task_state, handlers_["task_state"]},

        // Exploration primitives
        {"explore_recall", "Lightweight recall - titles/scores only",
            {{"type","object"},{"properties",{
                {"query",{{"type","string"},{"description","Search query"}}},
                {"limit",{{"type","integer"},{"description","Max results (default 10)"}}}
            }},{"required",{"query"}}},
            &FieldRpcHandler::tool_explore_recall, handlers_["explore_recall"]},

        {"explore_peek", "Get summary of a memory (first 200 chars)",
            {{"type","object"},{"properties",{
                {"id",{{"type","string"},{"description","Memory ID"}}}
            }},{"required",{"id"}}},
            &FieldRpcHandler::tool_explore_peek, handlers_["explore_peek"]},

        {"explore_expand", "Get full content of a memory",
            {{"type","object"},{"properties",{
                {"id",{{"type","string"},{"description","Memory ID"}}}
            }},{"required",{"id"}}},
            &FieldRpcHandler::tool_explore_expand, handlers_["explore_expand"]},

        {"explore_neighbors", "Get nodes connected via triplets",
            {{"type","object"},{"properties",{
                {"node",{{"type","string"},{"description","Node name"}}},
                {"direction",{{"type","string"},{"description","outgoing, incoming, or both"}}}
            }},{"required",{"node"}}},
            &FieldRpcHandler::tool_explore_neighbors, handlers_["explore_neighbors"]},

        // Graph tools
        {"connect", "Create a triplet relationship",
            {{"type","object"},{"properties",{
                {"subject",{{"type","string"},{"description","Subject entity"}}},
                {"predicate",{{"type","string"},{"description","Relationship type"}}},
                {"object",{{"type","string"},{"description","Object entity"}}}
            }},{"required",{"subject","predicate","object"}}},
            &FieldRpcHandler::tool_connect, handlers_["connect"]},

        {"query_graph", "Query triplets by subject or object",
            {{"type","object"},{"properties",{
                {"subject",{{"type","string"},{"description","Query by subject"}}},
                {"object",{{"type","string"},{"description","Query by object"}}}
            }}},
            &FieldRpcHandler::tool_query, handlers_["query_graph"]},

        {"query_triplets_temporal", "Query triplets at a point in time",
            {{"type","object"},{"properties",{
                {"subject",{{"type","string"}}},{"predicate",{{"type","string"}}},
                {"object",{{"type","string"}}},
                {"at_date",{{"type","string"},{"description","YYYY-MM-DD"}}},
                {"limit",{{"type","integer"}}}
            }}},
            &FieldRpcHandler::tool_query_triplets_temporal, handlers_["query_triplets_temporal"]},

        {"triplet_history", "Get history of a subject-predicate relationship",
            {{"type","object"},{"properties",{
                {"subject",{{"type","string"}}},{"predicate",{{"type","string"}}},
                {"limit",{{"type","integer"}}}
            }},{"required",{"subject","predicate"}}},
            &FieldRpcHandler::tool_triplet_history, handlers_["triplet_history"]},

        {"triplet_query_as_of", "Query triplets for a subject valid at a given world timestamp, excluding superseded entries",
            {{"type","object"},{"properties",{
                {"subject",{{"type","string"},{"description","Subject node to query"}}},
                {"world_ms",{{"type","integer"},{"description","World-time epoch ms (default: now)"}}}
            }},{"required",{"subject"}}},
            &FieldRpcHandler::tool_triplet_query_as_of, handlers_["triplet_query_as_of"]},

        {"triplet_supersede", "Mark one triplet as superseded by another (bi-temporal update)",
            {{"type","object"},{"properties",{
                {"old_id",{{"type","integer"},{"description","Triplet ID being superseded"}}},
                {"new_id",{{"type","integer"},{"description","Replacing triplet ID"}}},
                {"at_ms",{{"type","integer"},{"description","Ingestion-time of supersession (default: now)"}}}
            }},{"required",{"old_id","new_id"}}},
            &FieldRpcHandler::tool_triplet_supersede, handlers_["triplet_supersede"]},

        {"graph_traverse", "BFS graph traversal from a start node over triplet edges",
            {{"type","object"},{"properties",{
                {"start",{{"type","string"},{"description","Starting node"}}},
                {"edge_types",{{"type","array"},{"items",{{"type","string"}}},{"maxItems",32},{"description","Predicate filter (empty = all)"}}},
                {"max_hops",{{"type","integer"},{"description","Max BFS depth (default 3)"}}},
                {"max_results",{{"type","integer"},{"description","Max nodes returned (default 50)"}}},
                {"direction",{{"type","string"},{"enum",{"outgoing","incoming","both"}},{"description","Edge direction (default outgoing)"}}}
            }},{"required",{"start"}}},
            &FieldRpcHandler::tool_graph_traverse, handlers_["graph_traverse"]},

        {"graph_pagerank", "Personalized PageRank over the triplet graph",
            {{"type","object"},{"properties",{
                {"seeds",{{"type","array"},{"items",{{"type","string"}}},{"maxItems",32},{"description","Seed nodes"}}},
                {"edge_types",{{"type","array"},{"items",{{"type","string"}}},{"maxItems",32},{"description","Predicate filter (empty = all)"}}},
                {"damping",{{"type","number"},{"description","Damping factor (default 0.85)"}}},
                {"iterations",{{"type","integer"},{"description","PPR iterations (default 20)"}}},
                {"top_k",{{"type","integer"},{"description","Nodes to return (default 20)"}}}
            }},{"required",{"seeds"}}},
            &FieldRpcHandler::tool_graph_pagerank, handlers_["graph_pagerank"]},

        {"connect_temporal", "Create triplet with temporal validity",
            {{"type","object"},{"properties",{
                {"subject",{{"type","string"}}},{"predicate",{{"type","string"}}},{"object",{{"type","string"}}},
                {"valid_from",{{"type","string"}}},{"valid_to",{{"type","string"}}},
                {"context_date",{{"type","string"}}}
            }},{"required",{"subject","predicate","object"}}},
            &FieldRpcHandler::tool_connect_temporal, handlers_["connect_temporal"]},

        // Strength/forget
        {"strengthen", "Increase confidence of a memory",
            {{"type","object"},{"properties",{
                {"id",{{"type","string"},{"description","Node ID"}}},
                {"amount",{{"type","number"},{"description","Amount (default 0.1)"}}}
            }},{"required",{"id"}}},
            &FieldRpcHandler::tool_strengthen, handlers_["strengthen"]},

        {"weaken", "Decrease confidence of a memory",
            {{"type","object"},{"properties",{
                {"id",{{"type","string"}}},{"amount",{{"type","number"}}}
            }},{"required",{"id"}}},
            &FieldRpcHandler::tool_weaken, handlers_["weaken"]},

        {"forget", "Remove a memory",
            {{"type","object"},{"properties",{
                {"id",{{"type","string"},{"description","Node ID to forget"}}}
            }},{"required",{"id"}}},
            &FieldRpcHandler::tool_forget, handlers_["forget"]},

        {"ack_memory", "Record a positive ack signal for a memory (raises its recall score)",
            {{"type","object"},{"properties",{
                {"id",{{"type","string"},{"description","Node ID to ack"}}}
            }},{"required",{"id"}}},
            &FieldRpcHandler::tool_ack_memory, handlers_["ack_memory"]},

        {"nack_memory", "Record a negative nack signal for a memory (lowers its recall score)",
            {{"type","object"},{"properties",{
                {"id",{{"type","string"},{"description","Node ID to nack"}}}
            }},{"required",{"id"}}},
            &FieldRpcHandler::tool_nack_memory, handlers_["nack_memory"]},

        {"memory_outcome", "Record an observed outcome for a memory into its Beta utility posterior (success raises alpha, failure raises beta)",
            {{"type","object"},{"properties",{
                {"id",{{"type","string"},{"description","Node ID the outcome is about"}}},
                {"success",{{"type","boolean"},{"description","Did acting on this memory work?"}}},
                {"weight",{{"type","number"},{"description","Observation weight, capped at 5.0 (default 1.0)"}}}
            }},{"required",{"id","success"}}},
            &FieldRpcHandler::tool_memory_outcome, handlers_["memory_outcome"]},

        {"batch_forget", "Delete multiple nodes by ID",
            {{"type","object"},{"properties",{
                {"ids",{{"type","array"},{"items",{{"type","string"}}}}},
                {"pattern",{{"type","string"}}}
            }}},
            &FieldRpcHandler::tool_batch_forget, handlers_["batch_forget"]},

        // Observe/Grow
        {"observe", "Store an observation/learning (SSL v0.4)",
            {{"type","object"},{"properties",{
                {"category",{{"type","string"}}},{"title",{{"type","string"}}},
                {"content",{{"type","string"}}},{"tags",{{"type","string"}}},
                {"confidence",{{"type","number"}}},
                {"valence",{{"type","number"},{"description","Affect valence: -1.0 to +1.0"}}},
                {"arousal",{{"type","number"},{"description","Affect arousal: 0.0 to 1.0"}}},
                {"flags",{{"type","string"},{"description","Structural flags: ORIGIN,CORE,PIVOT,GENESIS,TURNING"}}},
                {"refs",{{"type","string"},{"description","Cross-references: comma-separated tag names or memory IDs"}}},
                {"granularity",{{"type","integer"},{"description","SSL v0.4 granularity tier: 0=atom,1=episode,2=claim,3=operator,4=boundary"}}},
                {"derivation",{{"type","string"},{"description","SSL v0.4 <=@ provenance: comma-separated source memory IDs this was abstracted from (required at G:1+)"}}},
                {"source_loc",{{"type","string"},{"description","SSL v0.4 src: external source grounding, e.g. file:line or doc section"}}}
            }},{"required",{"title","content"}}},
            &FieldRpcHandler::tool_observe, handlers_["observe"]},

        {"full_resonate", "Semantic search with full context",
            {{"type","object"},{"properties",{
                {"query",{{"type","string"}}},{"k",{{"type","integer"}}},
                {"realm",{{"type","string"}}},{"include_global",{{"type","boolean"}}},
                {"exclude_kinds",{{"type","array"},{"items",{{"type","string"}}}}},
                {"partnership_only",{{"type","boolean"}}},{"separation_mode",{{"type","boolean"}}}
            }},{"required",{"query"}}},
            &FieldRpcHandler::tool_full_resonate, handlers_["full_resonate"]},

        {"grow", "Add wisdom, belief, failure, aspiration, or dream",
            {{"type","object"},{"properties",{
                {"type",{{"type","string"}}},{"content",{{"type","string"}}},
                {"title",{{"type","string"}}},{"tags",{{"type","string"}}}
            }},{"required",{"type","content"}}},
            &FieldRpcHandler::tool_grow, handlers_["grow"]},

        {"get", "Get a node by ID",
            {{"type","object"},{"properties",{
                {"id",{{"type","string"}}}
            }},{"required",{"id"}}},
            &FieldRpcHandler::tool_get, handlers_["get"]},

        {"get_embeddings", "Batch-fetch raw embedding vectors for memory IDs. Returns JSON object mapping id→vector. Used for computing S-entropy (Gram matrix effective rank) over recall candidates.",
            {{"type","object"},{"properties",{
                {"ids", {{"type","array"},{"items",{{"type","string"}}},{"description","Array of memory ID strings"}}}
            }},{"required",{"ids"}}},
            &FieldRpcHandler::tool_get_embeddings, handlers_["get_embeddings"]},

        {"expand_memory", "Expand a memory to full hierarchical context",
            {{"type","object"},{"properties",{
                {"id",{{"type","string"}}},{"depth",{{"type","integer"}}}
            }},{"required",{"id"}}},
            &FieldRpcHandler::tool_expand_memory, handlers_["expand_memory"]},

        {"update", "Update node content",
            {{"type","object"},{"properties",{
                {"id",{{"type","string"}}},{"content",{{"type","string"}}}
            }},{"required",{"id","content"}}},
            &FieldRpcHandler::tool_update, handlers_["update"]},

        {"query", "Query triplets with flexible filters",
            {{"type","object"},{"properties",{
                {"subject",{{"type","string"}}},{"predicate",{{"type","string"}}},
                {"object",{{"type","string"}}}
            }}},
            &FieldRpcHandler::tool_query, handlers_["query"]},

        {"tag", "Add or remove tags from a node",
            {{"type","object"},{"properties",{
                {"id",{{"type","string"}}},{"add",{{"type","string"}}},{"remove",{{"type","string"}}}
            }},{"required",{"id"}}},
            &FieldRpcHandler::tool_tag, handlers_["tag"]},

        {"set_affect", "Set affect dimensions (valence, arousal) on a memory",
            {{"type","object"},{"properties",{
                {"id",{{"type","string"}}},
                {"valence",{{"type","number"},{"description","Emotional valence: -1.0 to +1.0"}}},
                {"arousal",{{"type","number"},{"description","Emotional arousal: 0.0 to 1.0"}}}
            }},{"required",{"id","valence","arousal"}}},
            &FieldRpcHandler::tool_set_affect, handlers_["set_affect"]},

        // ── Code Intelligence tools ─────────────────────────────────────────
        {"list_by_status", "List memories filtered by lifecycle status (active/superseded/contradicted/archived)",
            {{"type","object"},{"properties",{
                {"status",{{"type","string"},{"description","Filter: active, superseded, contradicted, archived, or all"},{"default","superseded"}}},
                {"limit",{{"type","integer"},{"description","Max results"},{"default",50}}},
                {"realm",{{"type","string"},{"description","Filter by realm"}}}
            }},{"required",json::array()}},
            &FieldRpcHandler::tool_list_by_status, handlers_["list_by_status"]},

        {"list_memories_brief", "Fast memory index",
            {{"type","object"},{"properties",{
                {"limit",{{"type","integer"}}},{"realm",{{"type","string"}}},
                {"kind",{{"type","string"}}},{"priority_tier",{{"type","integer"}}}
            }}},
            &FieldRpcHandler::tool_list_memories_brief, handlers_["list_memories_brief"]},

        {"set_priority_tier", "Set memory priority tier",
            {{"type","object"},{"properties",{
                {"memory_id",{{"type","integer"}}},{"tier",{{"type","integer"}}}
            }},{"required",{"memory_id","tier"}}},
            &FieldRpcHandler::tool_set_priority_tier, handlers_["set_priority_tier"]},

        {"recall_by_priority", "Budget-aware recall",
            {{"type","object"},{"properties",{
                {"query",{{"type","string"}}},{"budget_tokens",{{"type","integer"}}},
                {"realm",{{"type","string"}}},{"include_global",{{"type","boolean"}}}
            }}},
            &FieldRpcHandler::tool_recall_by_priority, handlers_["recall_by_priority"]},

        {"set_memory_type", "Set memory semantic type",
            {{"type","object"},{"properties",{
                {"memory_id",{{"type","integer"}}},{"type",{{"type","string"}}}
            }},{"required",{"memory_id","type"}}},
            &FieldRpcHandler::tool_set_memory_type, handlers_["set_memory_type"]},

        {"memory_type_stats", "Get memory type statistics",
            {{"type","object"},{"properties",{{"realm",{{"type","string"}}}}}},
            &FieldRpcHandler::tool_memory_type_stats, handlers_["memory_type_stats"]},

        {"forget_kind", "Bulk-delete all memories of a given kind (e.g. 'habit'). Optionally filter by realm.",
            {{"type","object"},{"properties",{
                {"kind",{{"type","string"},{"description","Memory kind to delete (e.g. habit, unknown)"}}},
                {"realm",{{"type","string"},{"description","Optional realm filter"}}},
                {"limit",{{"type","integer"},{"description","Max to delete (default 5000)"}}}
            }},{"required",{"kind"}}},
            &FieldRpcHandler::tool_forget_kind, handlers_["forget_kind"]},

        {"smart_recall", "Intelligent memory recall with hierarchical expansion",
            {{"type","object"},{"properties",{
                {"query",{{"type","string"}}},{"limit",{{"type","integer"}}},
                {"expand_top",{{"type","integer"}}},{"realm",{{"type","string"}}},
                {"include_global",{{"type","boolean"}}},
                {"separation_mode",{{"type","boolean"}}},{"gwt_mode",{{"type","boolean"}}}
            }},{"required",{"query"}}},
            &FieldRpcHandler::tool_smart_recall, handlers_["smart_recall"]},

        {"hybrid_recall", "Combined vector + BM25 + graph recall",
            {{"type","object"},{"properties",{
                {"query",{{"type","string"}}},{"limit",{{"type","integer"}}},
                {"tag",{{"type","string"}}},{"realm",{{"type","string"}}},
                {"vector_weight",{{"type","number"}}},{"bm25_weight",{{"type","number"}}},
                {"graph_weight",{{"type","number"}}},{"recency_weight",{{"type","number"}}},
                {"explain",{{"type","boolean"},{"description","Include score decomposition per hit (default: false)"}}}
            }},{"required",{"query"}}},
            &FieldRpcHandler::tool_hybrid_recall, handlers_["hybrid_recall"]},

        {"recall_lanes", "Fan-in prompt recall lanes over one daemon RPC",
            {{"type","object"},{"properties",{
                {"query",{{"type","string"},{"description","Prompt query shared by sem, hyb, kw, corr, and corrk"}}},
                {"ctx_query",{{"type","string"},{"description","Optional context query; defaults to query"}}},
                {"realm",{{"type","string"},{"description","Realm for sem, ctx, hyb, and kw"}}},
                {"limits",{{"type","object"},{"description","Per-lane result limits (sem, ctx, hyb, kw, corr)"},
                    {"properties",{
                        {"sem",{{"type","integer"},{"default",6}}},
                        {"ctx",{{"type","integer"},{"default",4}}},
                        {"hyb",{{"type","integer"},{"default",5}}},
                        {"kw",{{"type","integer"},{"default",3}}},
                        {"corr",{{"type","integer"},{"default",3}}}
                    }}}},
                {"lanes",{{"type","array"},{"items",{{"type","string"}}},{"description","Subset of sem, ctx, hyb, kw, corr, corrk (default all)"}}},
                {"budget_ms",{{"type","integer"},{"minimum",1},{"description","Per-lane wall-clock budget override; capped by CHITTA_RPC_BUDGET_MS"}}}
            }},{"required",{"query"}}},
            &FieldRpcHandler::tool_recall_lanes, handlers_["recall_lanes"]},

        {"recall_session", "Session-level recall: groups chunk evidence by source session using noisy-OR aggregation. Returns ranked sessions with best evidence.",
            {{"type","object"},{"properties",{
                {"query",{{"type","string"},{"description","Natural language query"}}},
                {"limit",{{"type","integer"},{"description","Max sessions to return (default 10)"}}},
                {"realm",{{"type","string"},{"description","Realm filter (optional)"}}}
            }},{"required",{"query"}}},
            &FieldRpcHandler::tool_recall_session, handlers_["recall_session"]},

        {"recall_spreading", "Retrieve memories via entity graph spreading activation",
            {{"type","object"},{"properties",{
                {"query",{{"type","string"},{"description","Query; entity seeds extracted automatically"}}},
                {"limit",{{"type","integer"},{"description","Max results (default 10)"}}},
                {"realm",{{"type","string"},{"description","Realm filter (optional)"}}}
            }},{"required",{"query"}}},
            &FieldRpcHandler::tool_recall_spreading, handlers_["recall_spreading"]},

        {"structured_recall", "Three-lens recall: facts, context, and temporal agents merged for high-fidelity retrieval",
            {{"type","object"},{"properties",{
                {"query",{{"type","string"}}},{"limit",{{"type","integer"}}},
                {"realm",{{"type","string"}}}
            }},{"required",{"query"}}},
            &FieldRpcHandler::tool_structured_recall, handlers_["structured_recall"]},


        {"ask", "Natural language insight query: retrieves and synthesizes memories to answer a question about the user, session, or project",
            {{"type","object"},{"properties",{
                {"question",{{"type","string"}}},{"limit",{{"type","integer"}}},
                {"realm",{{"type","string"}}}
            }},{"required",{"question"}}},
            &FieldRpcHandler::tool_ask, handlers_["ask"]},

        {"expand_query", "Expand query into typed variants",
            {{"type","object"},{"properties",{
                {"query",{{"type","string"}}}
            }},{"required",{"query"}}},
            &FieldRpcHandler::tool_expand_query, handlers_["expand_query"]},

        // Anticipation/Habit/Profile/Goal/Calibration

        // ── CEC: Event tape + CDAWG ──────────────────────────────────────────────
        {"log_event", "Log a structured action event to the CEC tape and CDAWG (tool, entity, outcome: 0=success 1=fail 2=error 3=partial)",
            {{"type","object"},{"properties",{
                {"tool",       {{"type","string"}}},
                {"entity",     {{"type","string"}}},
                {"outcome",    {{"type","integer"}}},
                {"session_id", {{"type","integer"}}},
                {"ts_ms",      {{"type","integer"}}}
            }},{"required",{"tool","entity"}}},
            &FieldRpcHandler::tool_log_event, handlers_["log_event"]},

        {"recall_last_action", "Return last k occurrences of (tool, entity) from the CEC event tape",
            {{"type","object"},{"properties",{
                {"tool",   {{"type","string"}}},
                {"entity", {{"type","string"}}},
                {"k",      {{"type","integer"}}}
            }},{"required",{"tool","entity"}}},
            &FieldRpcHandler::tool_recall_last_action, handlers_["recall_last_action"]},

        {"recall_failure_pattern", "Return top-k CDAWG states with high failure rates (fail_ratio > 0.6, fail_count >= 3)",
            {{"type","object"},{"properties",{
                {"k", {{"type","integer"}}}
            }}},
            &FieldRpcHandler::tool_recall_failure_pattern, handlers_["recall_failure_pattern"]},

        {"recall_causal_antecedent", "PMI-ranked causal antecedents: what actions typically precede (tool, entity)?",
            {{"type","object"},{"properties",{
                {"tool",   {{"type","string"}}},
                {"entity", {{"type","string"}}},
                {"k",      {{"type","integer"}}}
            }},{"required",{"tool","entity"}}},
            &FieldRpcHandler::tool_recall_causal_antecedent, handlers_["recall_causal_antecedent"]},

        {"recall_hdcbind", "Heteroassociative HDC query: given known_role=known_val, infer query_role. Roles: tool, entity, outcome.",
            {{"type","object"},{"properties",{
                {"known_role", {{"type","string"}}},
                {"known_val",  {{"type","string"}}},
                {"query_role", {{"type","string"}}},
                {"k",          {{"type","integer"}}}
            }},{"required",{"known_role","known_val","query_role"}}},
            &FieldRpcHandler::tool_recall_hdcbind, handlers_["recall_hdcbind"]},

        {"consolidation_pass", "Run Sequitur grammar consolidation: find frequent bigrams in EventTape and promote rules to the triplet KG (subject=rule:..., predicates: compresses/avg_outcome/support/tape_range).",
            {{"type","object"},{"properties",json::object()}},
            &FieldRpcHandler::tool_consolidation_pass, handlers_["consolidation_pass"]},

        {"recall_counterfactual", "CDAWG sibling-edge counterfactual: what alternative tool/entity would have had a lower failure rate in this same context?",
            {{"type","object"},{"properties",{
                {"tool",    {{"type","string"}}},
                {"entity",  {{"type","string"}}},
                {"outcome", {{"type","integer"},{"description","0=success 1=fail 2=error 3=partial (default 1)"}}},
                {"k",       {{"type","integer"}}}
            }},{"required",{"tool","entity"}}},
            &FieldRpcHandler::tool_recall_counterfactual, handlers_["recall_counterfactual"]},

        {"refutation_stats", "Show Sequitur rules that are being falsified: rules whose antecedent appears but is NOT followed by the expected consequent. Returns live/refuted counts and top-k by refutation ratio.",
            {{"type","object"},{"properties",{
                {"k", {{"type","integer"},{"description","Max rules to show (default 10)"}}}
            }}},
            &FieldRpcHandler::tool_refutation_stats, handlers_["refutation_stats"]},

        {"recall_motif_value", "Return top-k CDAWG motif states reachable from (tool, entity) ranked by Q-value: which action sequences have the highest expected success rate from this context?",
            {{"type","object"},{"properties",{
                {"tool",   {{"type","string"}}},
                {"entity", {{"type","string"}}},
                {"k",      {{"type","integer"},{"description","Max states to return (default 5)"}}}
            }},{"required",{"tool","entity"}}},
            &FieldRpcHandler::tool_recall_motif_value, handlers_["recall_motif_value"]},

        {"recall_analogy", "Analogical recall over the triplet lane (vector-symbolic, no LLM/GPU). Two modes embedding similarity cannot express, because both are about structure rather than wording. 'proportional' solves a:b :: c:? — give three entities, get ranked fillers for the fourth. 'structural' finds memories whose relation graph has the same SHAPE as a probe memory, with entity names factored out, so a pattern learned in one project matches the same pattern in another; set exclude_realm (or cross_realm) for that cross-project transfer. Use when you want 'what else looks like this?', not 'what mentions these words?'.",
            {{"type","object"},{"properties",{
                {"mode",          {{"type","string"},{"description","'proportional' (a:b :: c:?) or 'structural' (shape match). Default: structural"}}},
                {"a",             {{"type","string"},{"description","proportional: source entity"}}},
                {"b",             {{"type","string"},{"description","proportional: what a maps to"}}},
                {"c",             {{"type","string"},{"description","proportional: target entity; the tool returns its counterpart to b"}}},
                {"memory_id",     {{"type","integer"},{"description","structural: probe memory whose relation shape is matched (excluded from results)"}}},
                {"text",          {{"type","string"},{"description","structural: free-text probe, used when memory_id is absent; anchors on the entities it mentions"}}},
                {"realm",         {{"type","string"},{"description","structural: restrict results to this realm"}}},
                {"exclude_realm", {{"type","string"},{"description","structural: drop results from this realm — the cross-project transfer case"}}},
                {"cross_realm",   {{"type","boolean"},{"description","structural: shorthand for excluding the probe memory's own realm (default false)"}}},
                {"limit",         {{"type","integer"},{"description","Max results, 1-100 (default 8)"}}}
            }}},
            &FieldRpcHandler::tool_recall_analogy, handlers_["recall_analogy"]},

        {"span_query", "Retrieve verbatim transcript atoms (file paths, URLs, file:line locators, bash commands, error signatures) captured across all sessions. No LLM/GPU: exact-substring recall over a deduplicated span index. Realm-scoped by default to prevent cross-project bleed. Use when you need an exact locator you saw before, not a paraphrase.",
            {{"type","object"},{"properties",{
                {"query",  {{"type","string"},{"description","Substring or token to match against stored atoms"}}},
                {"realm",  {{"type","string"},{"description","Restrict to this realm (project); empty = unscoped"}}},
                {"k",      {{"type","integer"},{"description","Max atoms to return (default 6)"}}}
            }},{"required",{"query"}}},
            &FieldRpcHandler::tool_span_query, handlers_["span_query"]},

        {"span_backfill", "Backfill the span lane over all transcripts under projects_dir (default ~/.claude/projects). Incremental + idempotent via per-file watermarks. Reports unique atoms, new atoms, redaction count.",
            {{"type","object"},{"properties",{
                {"projects_dir", {{"type","string"},{"description","Transcript root (default ~/.claude/projects)"}}}
            }}},
            &FieldRpcHandler::tool_span_backfill, handlers_["span_backfill"]},

        {"span_backfill_memories", "Backfill the memory↔span edge: run the atom extractor over every live memory's text so distilled beliefs link to the verbatim paths/commands/ids they mention. Idempotent (per-memory content hash); superseded memories relink. Reports memories linked + new spans.",
            {{"type","object"},{"properties",{}}},
            &FieldRpcHandler::tool_span_backfill_memories, handlers_["span_backfill_memories"]},

        {"densify_backfill", "#13 retro-backfill of SameSession edges over existing session-tagged memories (same K=3 decaying-weight rule as the write-time hook). apply=false is a dry run: reports sessions, memories, sibling pairs, and a group-size histogram, writing nothing. apply=true creates the edges (idempotent; rollback via remove_assoc_edges_by_type).",
            {{"type","object"},{"properties",{{"apply",{{"type","boolean"}}}}}},
            &FieldRpcHandler::tool_densify_backfill, handlers_["densify_backfill"]},

        {"semantic_backfill", "Dense-kNN SemanticNeighbor edge backfill: for each non-deleted memory, link its top-k realm-scoped HNSW neighbors (cosine>=min_cos) with bidirectional similarity edges (wire 6). The first genuine memory<->memory knowledge relation in the assoc graph, distinct from CoRetrieved's retrieval-history prior. apply=false is a dry run (counts candidate edges, no writes). apply=true creates them (idempotent; rollback via remove_assoc_edges_by_type edge_type=6).",
            {{"type","object"},{"properties",{{"apply",{{"type","boolean"}}},{"k",{{"type","integer"}}},{"min_cos",{{"type","number"}}}}}},
            &FieldRpcHandler::tool_semantic_backfill, handlers_["semantic_backfill"]},

        {"assoc_census", "Read-only assoc-graph census: per-EdgeType directed-edge count plus weight histogram (<0.05 / 0.05-0.2 / 0.2-0.5 / 0.5-0.8 / >=0.8). Measure-first gate for plasticity levers.",
            {{"type","object"},{"properties",{}}},
            &FieldRpcHandler::tool_assoc_census, handlers_["assoc_census"]},

        {"assoc_decay", "Gate B: decay + floor-prune one assoc EdgeType (wire numbering 0-5, default 3=CoRetrieved). Every edge weight is multiplied by factor; edges below prune_below are removed. apply=false is a dry run (counts only). One-shot saturation migration: factor=1.0, prune_below=0.2. Snapshot before apply.",
            {{"type","object"},{"properties",{
                {"edge_type",{{"type","integer"}}},
                {"factor",{{"type","number"}}},
                {"prune_below",{{"type","number"}}},
                {"apply",{{"type","boolean"}}}
            }}},
            &FieldRpcHandler::tool_assoc_decay, handlers_["assoc_decay"]},

        {"span_stats", "Report span lane size: unique atoms, on-disk bytes, total redactions.",
            {{"type","object"},{"properties",{}}},
            &FieldRpcHandler::tool_span_stats, handlers_["span_stats"]},

        {"executor_flush", "Promote shadow intervention policies that have passed the 20-event / lift>0.15 gate, demote policies whose source rule is refuted, and report active policy stats. Safe to call any time; idempotent.",
            {{"type","object"},{"properties",{}}},
            &FieldRpcHandler::tool_executor_flush, handlers_["executor_flush"]},

        {"list_policies", "List CEC intervention policies (shadow and active). Each entry shows rule source, kind (OpenTask/TurnInjection/GuardPolicy), shadow event count, lift, and fire count.",
            {{"type","object"},{"properties",{
                {"active_only", {{"type","boolean"},{"description","If true, only return active (promoted) policies"}}}
            }}},
            &FieldRpcHandler::tool_list_policies, handlers_["list_policies"]},

        {"recall_true_counterfactual", "Return decision points where (tool, entity) was explicitly considered and rejected. Uses the DecisionTape (Phase 10), not CDAWG sibling inference. Requires prior log_event_ex or log_decision calls.",
            {{"type","object"},{"properties",{
                {"tool",    {{"type","string"}}},
                {"entity",  {{"type","string"}}},
                {"outcome", {{"type","integer"},{"description","0=success 1=fail 2=error (default 0)"}}},
                {"k",       {{"type","integer"},{"description","Max results (default 5)"}}}
            }},{"required",{"tool","entity"}}},
            &FieldRpcHandler::tool_recall_true_counterfactual, handlers_["recall_true_counterfactual"]},

        {"hypothesis_probes", "Top-k Sequitur rules ranked by expected information gain (Wilson probe_value). Maximized at p_hat=0.5 — rules the system is most uncertain about. Run consolidation_pass first to populate.",
            {{"type","object"},{"properties",{
                {"k", {{"type","integer"},{"description","Max rules to return (default 10)"}}}
            }}},
            &FieldRpcHandler::tool_hypothesis_probes, handlers_["hypothesis_probes"]},
        {"turiya_status", nullptr, nullptr, &FieldRpcHandler::tool_turiya_status, handlers_["turiya_status"]},
        {"tape_stats", nullptr, nullptr, &FieldRpcHandler::tool_tape_stats, handlers_["tape_stats"]},
        {"verbalize_rules", nullptr, nullptr, &FieldRpcHandler::tool_verbalize_rules, handlers_["verbalize_rules"]},
        {"queue_experiments", nullptr, nullptr, &FieldRpcHandler::tool_queue_experiments, handlers_["queue_experiments"]},
        {"fep_status", nullptr, nullptr, &FieldRpcHandler::tool_fep_status, handlers_["fep_status"]},
        {"routed_recall", nullptr, nullptr, &FieldRpcHandler::tool_routed_recall, handlers_["routed_recall"]},
        {"witness_memory", nullptr, nullptr, &FieldRpcHandler::tool_witness_memory, handlers_["witness_memory"]},
        {"reconcile_pass", nullptr, nullptr, &FieldRpcHandler::tool_reconcile_pass, handlers_["reconcile_pass"]},
        {"harvest_scope", nullptr, nullptr, &FieldRpcHandler::tool_harvest_scope, handlers_["harvest_scope"]},
        {"seed_hdc_geometry", nullptr, nullptr, &FieldRpcHandler::tool_seed_hdc_geometry, handlers_["seed_hdc_geometry"]},
        {"ledger_append", nullptr, nullptr, &FieldRpcHandler::tool_ledger_append, handlers_["ledger_append"]},
        {"ledger_query", nullptr, nullptr, &FieldRpcHandler::tool_ledger_query, handlers_["ledger_query"]},
        {"ledger_compile", nullptr, nullptr, &FieldRpcHandler::tool_ledger_compile, handlers_["ledger_compile"]},
        {"ledger_contradictions", nullptr, nullptr, &FieldRpcHandler::tool_ledger_contradictions, handlers_["ledger_contradictions"]},
        {"predicate_attach", nullptr, nullptr, &FieldRpcHandler::tool_predicate_attach, handlers_["predicate_attach"]},
        {"predicate_run", nullptr, nullptr, &FieldRpcHandler::tool_predicate_run, handlers_["predicate_run"]},
        {"predicate_list", nullptr, nullptr, &FieldRpcHandler::tool_predicate_list, handlers_["predicate_list"]},

        {"log_event_ex", "Log a CEC event with regret-shaping telemetry (token_cost, latency_ms, retry_count). Updates Q-values with utility = outcome_reward - 0.001*token_cost - 0.00001*latency_ms - 0.1*retry_count.",
            {{"type","object"},{"properties",{
                {"tool",        {{"type","string"}}},
                {"entity",      {{"type","string"}}},
                {"outcome",     {{"type","integer"},{"description","0=success 1=fail 2=error 3=partial"}}},
                {"session_id",  {{"type","integer"}}},
                {"ts_ms",       {{"type","integer"}}},
                {"token_cost",  {{"type","integer"},{"description","Tokens consumed (0=unknown)"}}},
                {"latency_ms",  {{"type","integer"},{"description","Wall-clock ms (0=unknown)"}}},
                {"retry_count", {{"type","integer"},{"description","Retries before this outcome"}}}
            }},{"required",{"tool","entity"}}},
            &FieldRpcHandler::tool_log_event_ex, handlers_["log_event_ex"]},

        {"log_decision", "Log a decision point to the DecisionTape: chosen action + alternatives considered and rejected. Enables recall_true_counterfactual.",
            {{"type","object"},{"properties",{
                {"chosen_tool",      {{"type","string"}}},
                {"chosen_entity",    {{"type","string"}}},
                {"chosen_outcome",   {{"type","integer"},{"description","0=success 1=fail 2=error 3=partial"}}},
                {"rejected_json",    {{"type","string"},{"description","JSON array of [sym_u64, rejection_reason_u8] pairs"}}},
                {"confidence_delta", {{"type","number"},{"description","chosen_confidence - best_alternative_confidence"}}},
                {"ts_ms",            {{"type","integer"}}}
            }},{"required",{"chosen_tool","chosen_entity"}}},
            &FieldRpcHandler::tool_log_decision, handlers_["log_decision"]},
    });
}

} // namespace chitta
