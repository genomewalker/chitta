#include <chitta/queue_path.hpp>
// Chitta CLI - Multi-mode memory operations
// Command-line interface for soul integration
//
// Modes:
//   CLI mode:    chitta <tool> [args...]  - Direct tool invocation
//   Thin client: chitta                   - Forward JSON-RPC to daemon
//
// CLI Examples:
//   chitta recall "query"
//   chitta recall "query" --zoom sparse
//   chitta soul_context
//   chitta observe --category decision --title "..." --content "..."
//   chitta grow --type wisdom --title "..." --content "..."
//
// Options:
//   --socket-path PATH  Unix socket path
//   --json              CLI mode: output raw JSON instead of text
//   --toon              CLI mode: output TOON format (compact, shell-friendly)

#include <chitta/socket_client.hpp>
#include <chitta/tool_spec.hpp>
#include <chitta/rpc/sandbox.hpp>  // append_line_atomic
#include <unistd.h>  // getppid
#include <fstream>
#include <array>
#include <thread>
#include <chitta/version.hpp>
#include <set>
#include <iostream>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <nlohmann/json.hpp>

// TOON (Token-Oriented Object Notation) encoder
// Compact format for LLM inputs: ~40% fewer tokens than JSON
// Spec: https://github.com/toon-format/toon
namespace toon {

// Escape value for TOON (only escape commas and newlines in array values)
std::string escape_value(const std::string& s) {
    bool needs_quote = false;
    for (char c : s) {
        if (c == ',' || c == '\n' || c == '\r') {
            needs_quote = true;
            break;
        }
    }
    if (!needs_quote) return s;

    // Quote the value and escape internal quotes
    std::string result = "\"";
    for (char c : s) {
        if (c == '"') result += "\"\"";
        else if (c == '\n') result += "\\n";
        else if (c == '\r') result += "\\r";
        else result += c;
    }
    result += "\"";
    return result;
}

std::string value_to_string(const nlohmann::json& v) {
    if (v.is_null()) return "";
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_number_integer()) return std::to_string(v.get<int64_t>());
    if (v.is_number_float()) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.6g", v.get<double>());
        return buf;
    }
    if (v.is_string()) return escape_value(v.get<std::string>());
    // For nested objects/arrays, fall back to compact JSON
    return v.dump();
}

std::string encode(const nlohmann::json& j, int indent = 0) {
    std::string result;
    std::string prefix(indent, ' ');

    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            const auto& key = it.key();
            const auto& val = it.value();

            if (val.is_object()) {
                result += prefix + key + ":\n";
                result += encode(val, indent + 1);
            } else if (val.is_array() && !val.empty()) {
                // Check if uniform array of objects (tabular)
                bool is_tabular = true;
                std::vector<std::string> fields;
                for (const auto& item : val) {
                    if (!item.is_object()) {
                        is_tabular = false;
                        break;
                    }
                    if (fields.empty()) {
                        for (auto& [k, v] : item.items()) {
                            fields.push_back(k);
                        }
                    }
                }

                if (is_tabular && !fields.empty()) {
                    // Tabular format: array[N]{field1,field2}:\n val1,val2\n
                    result += prefix + key + "[" + std::to_string(val.size()) + "]{";
                    for (size_t i = 0; i < fields.size(); ++i) {
                        if (i > 0) result += ",";
                        result += fields[i];
                    }
                    result += "}:\n";
                    for (const auto& item : val) {
                        result += prefix + " ";
                        for (size_t i = 0; i < fields.size(); ++i) {
                            if (i > 0) result += ",";
                            if (item.contains(fields[i])) {
                                result += value_to_string(item[fields[i]]);
                            }
                        }
                        result += "\n";
                    }
                } else if (!val.empty() && val[0].is_primitive()) {
                    // Simple array: array[N]: val1,val2,val3
                    result += prefix + key + "[" + std::to_string(val.size()) + "]: ";
                    for (size_t i = 0; i < val.size(); ++i) {
                        if (i > 0) result += ",";
                        result += value_to_string(val[i]);
                    }
                    result += "\n";
                } else {
                    // Mixed array - fall back to JSON
                    result += prefix + key + ": " + val.dump() + "\n";
                }
            } else {
                result += prefix + key + ": " + value_to_string(val) + "\n";
            }
        }
    } else if (j.is_array()) {
        // Top-level array
        nlohmann::json wrapper;
        wrapper["results"] = j;
        return encode(wrapper, indent);
    } else {
        result = value_to_string(j);
    }

    return result;
}

}  // namespace toon

// Tool parameter spec types and the spec-driven coercion rules live in
// chitta/tool_spec.hpp so tool_spec_test exercises the shipped code.
using chitta::ToolParam;
using chitta::ToolSpec;
using chitta::coerce_bool_params;

// Tool specifications - only includes tools implemented in daemon
static const std::vector<ToolSpec> TOOL_SPECS = {
    // Core memory tools
    {"remember", "Store text in memory with optional tags",
     {{"content", "Text to remember", true, nullptr},
      {"type", "Node type: wisdom|belief|episode", false, "episode"},
      {"kind", "Alias for --type", false, nullptr},
      {"tags", "Comma-separated tags", false, nullptr},
      {"realm", "Primary realm (default: brahman)", false, nullptr},
      {"visibility", "0=Private, 1=Shared, 2=Global", false, "0"},
      {"source_session", "Session ID for grouping (used by recall_session)", false, nullptr}}},

    {"recall", "Search memory by semantic similarity",
     {{"query", "Search query", true, nullptr},
      {"limit", "Max results", false, "10"},
      {"tag", "Filter by tag", false, nullptr},
      {"realm", "Filter by realm (empty = all visible)", false, nullptr},
      {"strategy", "Retrieval lane: fused (default), keyword (BM25 only, realm-scoped), field", false, nullptr},
      {"pool", "Candidate pool depth before the recall-biased pre-filter (max 160)", false, "60"},
      {"prefilter", "Recall-biased pre-filter (false = narrow-pool legacy path)", false, "true"}}},

    {"recall_lanes", "Fan-in prompt recall lanes over one daemon RPC",
     {{"query", "Prompt query", true, nullptr},
      {"ctx_query", "Optional context query (defaults to query)", false, nullptr},
      {"realm", "Filter by realm", false, nullptr},
      {"limits", "JSON object of sem/ctx/hyb/kw/corr limits", false, nullptr},
      {"lanes", "JSON array or comma-separated lane names", false, nullptr},
      {"budget_ms", "Per-lane wall-clock budget override", false, nullptr}}},

    {"correction_check", "Deterministic durable-correction check (capability #2): does a stored [correction] trigger recur in this turn? Exact keyed bigram probe — reserves an injection slot, bypasses fuzzy recall.",
     {{"text", "Turn/context text to scan for a recurring corrected mistake", true, nullptr}}},

    {"provenance_check", "Deterministic anti-reprocessing check (capability #1): has this file/task already been processed? Exact keyed lookup of a prior [done] record by content-hash and/or input path — bypasses fuzzy recall.",
     {{"sha", "Content hash of the input (tried first — content identity)", false, nullptr},
      {"input", "Input path (fallback handle)", false, nullptr}}},

    {"task_state", "Deterministic task-state check (capability #3): what is the current state of task X? Exact keyed lookup of the LATEST stored [task] record by its slug — bypasses fuzzy recall.",
     {{"id", "Task slug (the id: token of the [task] record)", true, nullptr}}},

    {"recall_temporal", "Search memories within a time window",
     {{"query", "Optional semantic search query", false, nullptr},
      {"start", "Start date (ISO8601 or YYYY-MM-DD)", false, nullptr},
      {"end", "End date (ISO8601 or YYYY-MM-DD)", false, nullptr},
      {"limit", "Max results", false, "20"},
      {"realm", "Filter by realm", false, nullptr},
      {"include_global", "Include global memories", false, "true"}}},

    {"recall_temporal_events", "Bridge query: find entities active in a time window via EventTape, then recall their memories. Use when you don't know the realm — EventTape discovers which realms had activity.",
     {{"start", "Start date (ISO8601 or YYYY-MM-DD)", false, nullptr},
      {"end",   "End date (ISO8601 or YYYY-MM-DD)",   false, nullptr},
      {"limit", "Max results", false, "20"}}},

    {"recall_session", "Session-level recall: groups chunk evidence by source session using noisy-OR aggregation",
     {{"query", "Natural language query", true, nullptr},
      {"limit", "Max sessions to return", false, "10"},
      {"realm", "Filter by realm", false, nullptr}}},

    {"recall_spreading", "Retrieve memories via entity graph spreading activation",
     {{"query", "Query; capitalized words/@refs/quoted strings used as entity seeds", true, nullptr},
      {"limit", "Max results", false, "10"},
      {"realm", "Memory realm", false, nullptr}}},

    {"span_query", "Retrieve verbatim transcript atoms (paths, URLs, file:line, bash commands, errors). Exact-substring, no LLM/GPU, realm-scoped.",
     {{"query", "Substring/token to match", true, nullptr},
      {"realm", "Restrict to realm (empty = unscoped)", false, nullptr},
      {"k", "Max atoms", false, "6"}}},

    {"span_backfill", "Backfill the span lane over all transcripts (incremental, idempotent).",
     {{"projects_dir", "Transcript root (default ~/.claude/projects)", false, nullptr}}},

    {"span_backfill_memories", "Backfill the memory↔span edge over all live memories (idempotent).",
     {}},

    {"densify_backfill", "#13 retro-backfill of SameSession edges over existing session-tagged memories. Dry-run by default (counts sibling pairs + group-size histogram, no writes).",
     {{"apply", "Create the edges (default false = dry-run count only)", false, "false"}}},

    {"semantic_backfill", "Dense-kNN SemanticNeighbor edge backfill: link each memory to its top-k realm-scoped HNSW neighbors (cosine>=min_cos) with bidirectional similarity edges. First real memory<->memory knowledge relation. Dry-run by default (counts, no writes).",
     {{"apply", "Create the edges (default false = dry-run count only)", false, "false"},
      {"k", "Neighbors per memory (default 6)", false, "6"},
      {"min_cos", "Minimum cosine to link (default 0.6)", false, "0.6"}}},

    {"assoc_census", "Read-only assoc-graph census: per-EdgeType directed-edge count plus weight histogram. Measure-first gate for plasticity levers.",
     {}},

    {"assoc_decay", "Gate B: decay + floor-prune one assoc EdgeType. Every edge weight is multiplied by factor; edges below prune_below are removed. Dry-run by default. One-shot saturation migration: factor=1.0, prune_below=0.2. Snapshot before apply.",
     {{"edge_type", "Wire numbering 0-5 (default 3=CoRetrieved)", false, "3"},
      {"factor", "Weight multiplier in (0,1] (default 1.0 = no decay)", false, "1.0"},
      {"prune_below", "Remove edges below this weight (default 0.0 = keep all)", false, "0.0"},
      {"apply", "Perform the mutation (default false = dry-run count only)", false, "false"}}},

    {"span_stats", "Span lane size: unique atoms, disk bytes, redactions.",
     {}},

    {"queue_status", "Live work-queue depth: pending (in-flight + unclaimed), processed, distilled, failed, pending embeddings, dead-letters.",
     {}},

    {"smart_recall", "Intelligent recall with automatic query intent classification",
     {{"query", "Natural language query (e.g., 'last week', 'show preferences')", true, nullptr},
      {"limit", "Max results", false, "20"},
      {"realm", "Filter by realm", false, nullptr},
      {"include_global", "Include global memories", false, "true"}}},

    {"list_by_aspect", "List memories by semantic aspect",
     {{"aspect", "Aspect: preferences|corrections|insights|failures|decisions|approaches|milestones|goals|habits|beliefs|wisdom|code|gaps", true, nullptr},
      {"limit", "Max results", false, "50"},
      {"min_confidence", "Minimum confidence", false, "0.1"}}},

    {"list_aspects", "List available semantic aspects",
     {}},

    {"grow", "Add wisdom, belief, failure, aspiration, or dream",
     {{"type", "Type: wisdom|belief|failure|aspiration|dream", true, nullptr},
      {"content", "Content to store", true, nullptr},
      {"title", "Short title", false, nullptr},
      {"tags", "Comma-separated tags", false, nullptr},
      {"realm", "Primary realm (default: brahman)", false, nullptr},
      {"visibility", "0=Private, 1=Shared, 2=Global", false, "0"}}},

    {"get", "Get a node by ID",
     {{"id", "Node ID", true, nullptr}}},

    {"get_embeddings", "Batch-fetch raw embedding vectors for memory IDs (for S-entropy / Gram matrix)",
     {{"ids", "JSON array of memory ID strings", true, nullptr}}},

    {"update", "Update node content",
     {{"id", "Node ID", true, nullptr},
      {"content", "New content", true, nullptr}}},

    {"forget", "Remove a memory",
     {{"id", "Node ID to forget", true, nullptr}}},

    {"strengthen", "Increase confidence of a memory",
     {{"id", "Node ID", true, nullptr},
      {"amount", "Amount to strengthen (0-1)", false, "0.1"}}},

    {"ack_memory", "Record a positive ack signal for a memory (raises its recall score)",
     {{"id", "Node ID to ack", true, nullptr}}},

    {"nack_memory", "Record a negative nack signal for a memory (lowers its recall score)",
     {{"id", "Node ID to nack", true, nullptr}}},

    {"memory_outcome", "Record an outcome observation for a memory's utility posterior",
     {{"id", "Node ID", true, nullptr},
      {"success", "true|false: did the action following this memory's injection succeed", true, nullptr},
      {"weight", "Observation weight (0-5]", false, "1.0"}}},

    {"weaken", "Decrease confidence of a memory",
     {{"id", "Node ID", true, nullptr},
      {"amount", "Amount to weaken (0-1)", false, "0.1"}}},

    {"tag", "Add or remove tags from a node",
     {{"id", "Node ID", true, nullptr},
      {"add", "Tag to add", false, nullptr},
      {"remove", "Tag to remove", false, nullptr}}},

    // Graph/Triplet tools
    {"connect", "Create triplet: subject → predicate → object",
     {{"subject", "Subject entity", true, nullptr},
      {"predicate", "Relationship type", true, nullptr},
      {"object", "Object entity", true, nullptr}}},

    {"query", "Query triplets with flexible filters",
     {{"subject", "Subject filter", false, nullptr},
      {"predicate", "Predicate filter", false, nullptr},
      {"object", "Object filter", false, nullptr}}},

    {"query_graph", "Query triplets by subject or object",
     {{"subject", "Query by subject", false, nullptr},
      {"object", "Query by object", false, nullptr}}},

    // Hook tools
    {"observe", "Store observation (used by hooks for [LEARN] extraction)",
     {{"title", "Short title", true, nullptr},
      {"content", "Full content", true, nullptr},
      {"category", "Category: wisdom|insight|signal|episode", false, "episode"},
      {"tags", "Comma-separated tags", false, nullptr},
      {"realm", "Primary realm (default: brahman)", false, nullptr},
      {"source", "Source: hook_regex|hook_compliance|distillation|mcp_tool", false, "mcp_tool"},
      {"confidence", "Override confidence (0.0-1.0)", false, nullptr},
      {"evidence", "What triggered this observation", false, nullptr},
      {"force_supersede_ids", "Comma-separated memory IDs to explicitly supersede", false, nullptr}}},

    {"full_resonate", "Semantic search with full context (for hooks)",
     {{"query", "Search query", true, nullptr},
      {"k", "Max results", false, "10"},
      {"realm", "Filter by realm (empty = all visible)", false, nullptr},
      {"exclude-kinds", "Comma-separated kinds to exclude", false, nullptr},
      {"partnership-only", "Exclude code intel (symbol, projectessence, modulestate, patternstate)", false, "false"}}},

    // Context tools
    {"soul_context", "Get current soul state and statistics",
     {}},

    {"health_check", "Check daemon health and readiness",
     {}},

    {"version_check", "Get version information",
     {}},

    // Maintenance tools
    {"cycle", "Run maintenance cycle (decay, cleanup)",
     {{"force", "Force full cycle", false, "false"}}},

    {"cleanup", "Remove weak/garbage nodes",
     {{"dry_run", "Preview only", false, "true"}}},

    // Import/Export tools
    {"import_soul", "Import .soul file (SSL format)",
     {{"file", "Path to .soul file", false, nullptr},
      {"content", "SSL content (alternative to file)", false, nullptr}}},

    {"export_soul", "Export memories to SSL format",
     {{"file", "Output file path", false, nullptr},
      {"tag", "Filter by tag", false, nullptr},
      {"limit", "Max nodes to export", false, "100"}}},

    // Code intelligence tools
    {"extract_symbols", "Extract symbols from source file using tree-sitter",
     {{"path", "File path to analyze", true, nullptr}}},

    {"learn_codebase", "Learn codebase by extracting all symbols",
     {{"path", "Directory path to analyze", true, nullptr},
      {"project", "Project name (auto-detected if empty)", false, nullptr},
      {"max_files", "Max files to process", false, "500"},
      {"exclude", "Comma-separated directories to exclude", false, nullptr},
      {"force", "Reprocess all files even if unchanged (repopulates callsites)", false, "false"}}},

    {"find_symbol", "Search for symbols by name",
     {{"name", "Symbol name to search", true, nullptr},
      {"kind", "Symbol kind filter (function, class, method)", false, nullptr},
      {"path", "Only symbols whose file path contains this substring (repo/dir scoping)", false, nullptr},
      {"lang", "Language filter by extension (cpp, c, python, rust, js, go, java, ruby)", false, nullptr},
      {"limit", "Max results", false, "20"}}},

    {"dedupe_symbols", "GC the symbol index: stale-line duplicates, excluded paths, dead files",
     {{"dry_run", "Count only, remove nothing (default true)", false, "true"},
      {"check_fs", "Treat symbols whose file no longer exists as dead", false, "true"},
      {"exclude", "Comma-separated path substrings to purge", false, "plugins/cache,_deps,node_modules,__pycache__,.venv"}}},

    {"search_symbols", "Semantic search for code symbols",
     {{"query", "Search query", true, nullptr},
      {"kind", "Symbol kind filter", false, nullptr},
      {"limit", "Max results", false, "10"},
      {"project", "Project name to disambiguate", false, nullptr}}},

    {"code_context", "Get code context summary",
     {{"path", "File path", false, nullptr}}},

    {"smart_context", "Build intelligent context combining memories, code, and graph",
     {{"task", "Task description", true, nullptr},
      {"mode", "Context mode", false, nullptr},
      {"limit", "Max results", false, nullptr},
      {"realm", "Realm filter", false, nullptr}}},

    {"codebase_overview", "Get full indexed codebase structure",
     {{"project", "Project name", false, nullptr},
      {"format", "Output format", false, nullptr}}},

    {"symbol_callers", "Find all symbols that call the given symbol (reverse call graph)",
     {{"name", "Symbol name to find callers for", false, nullptr},
      {"id", "Symbol ID (alternative to name)", false, nullptr},
      {"kind", "Filter by symbol kind when using name", false, nullptr},
      {"project", "Project name to disambiguate", false, nullptr}}},

    {"symbol_callees", "Find all symbols that the given symbol calls (forward call graph)",
     {{"name", "Symbol name to find callees for", false, nullptr},
      {"id", "Symbol ID (alternative to name)", false, nullptr},
      {"kind", "Filter by symbol kind when using name", false, nullptr},
      {"project", "Project name to disambiguate", false, nullptr}}},

    {"read_symbol", "Read the actual source code for a symbol by name or ID",
     {{"name", "Symbol name to read", false, nullptr},
      {"id", "Symbol ID (alternative to name)", false, nullptr},
      {"kind", "Filter by symbol kind (function, class, method)", false, nullptr},
      {"project", "Project name to disambiguate", false, nullptr}}},

    {"read_function", "Read source code of a function/method by name",
     {{"name", "Function name to read", true, nullptr},
      {"project", "Project name to disambiguate", false, nullptr}}},

    {"type_hierarchy", "Get type hierarchy (base classes, implemented interfaces) for a type",
     {{"name", "Type name to query", true, nullptr},
      {"direction", "ancestors, descendants, or both (default: both)", false, "both"}}},

    {"file_imports", "Get all imports/includes for a source file",
     {{"path", "File path to get imports for", true, nullptr}}},

    {"file_dependents", "Get all files that import/include the given module",
     {{"name", "Module/file name to find dependents for", true, nullptr}}},

    {"resolve_callsites", "Resolve callsites to symbols and populate call_edge table",
     {{"project", "Filter to specific project path", false, nullptr}}},

    {"describe_symbol", "Set description for a code symbol (stores directly in symbol table)",
     {{"symbol-id", "Symbol ID to describe", true, nullptr},
      {"description", "Semantic description of the symbol", true, nullptr}}},

    {"cleanup_code_wisdom", "Migration: delete [code] wisdom memories and clear orphaned symbol.memory_id",
     {{"dry-run", "Preview only without changes (default: true)", false, "true"}}},

    {"code_context", "Get code context summary",
     {{"path", "Limit to files under this path", false, nullptr}}},

    {"smart_context", "Build intelligent context combining memories, code symbols, and graph relationships",
     {{"task", "Query to find context for", true, nullptr},
      {"mode", "fast: <80ms, full: <200ms", false, "full"},
      {"limit", "Token limit", false, "300"},
      {"memories", "Include semantic memories", false, "true"},
      {"code", "Include code symbols", false, "true"},
      {"neighbors", "Include triplet neighbors", false, "true"},
      {"realm", "Filter by realm", false, nullptr}}},

    // Realm tools
    {"realm_list", "List all known realms",
     {}},

    {"realm_get", "Get realm for a memory",
     {{"id", "Memory ID", true, nullptr}}},

    {"realm_set", "Set primary realm for a memory",
     {{"id", "Memory ID", true, nullptr},
      {"realm", "Realm name", true, nullptr}}},

    {"realm_add", "Add memory to additional realm",
     {{"id", "Memory ID", true, nullptr},
      {"realm", "Realm name to add", true, nullptr}}},

    {"realm_remove", "Remove memory from realm",
     {{"id", "Memory ID", true, nullptr},
      {"realm", "Realm name to remove", true, nullptr}}},

    {"realm_visibility", "Set realm visibility for a memory",
     {{"id", "Memory ID", true, nullptr},
      {"visibility", "Visibility: private|shared|global", true, nullptr}}},

    {"realm_detect", "Detect current realm from environment, .cc-soul-realm file, or git repository",
     {}},

    // Ledger tools (session continuity)
    {"ledger_save", "Save session checkpoint for continuity",
     {{"session_id", "Session identifier", true, nullptr},
      {"project", "Project scope", false, "default"},
      {"mood", "Current feeling: confident|uncertain|flowing|frustrated", false, nullptr},
      {"coherence", "Coherence score 0-1", false, nullptr},
      {"confidence", "Confidence score 0-1", false, nullptr},
      {"todos", "JSON array of {content, status} objects", false, nullptr},
      {"active_files", "JSON array of file paths", false, nullptr},
      {"decisions", "JSON array of key decisions", false, nullptr},
      {"next_steps", "JSON array of next steps", false, nullptr},
      {"blockers", "JSON array of blockers", false, nullptr},
      {"discoveries", "JSON array of discoveries", false, nullptr},
      {"snapshot", "Full checkpoint text for reconstruction", false, nullptr}}},

    {"ledger_load", "Load most recent checkpoint",
     {{"session_id", "Session identifier (optional)", false, nullptr},
      {"project", "Project filter (optional)", false, nullptr}}},

    {"ledger_list", "List recent checkpoints",
     {{"project", "Project filter (optional)", false, nullptr},
      {"limit", "Max entries to return", false, "10"}}},

    {"ledger_get", "Get specific checkpoint by ID",
     {{"id", "Checkpoint ID", true, nullptr}}},

    {"ledger_delete", "Delete checkpoint",
     {{"id", "Checkpoint ID to delete", true, nullptr}}},

    // Transcript tools (for distillation)
    {"read_transcript", "Read JSONL transcript file directly with pagination",
     {{"path", "Path to .jsonl transcript file (or use session_id)", false, nullptr},
      {"session_id", "Session ID to find transcript for", false, nullptr},
      {"start_turn", "Starting turn index", false, "0"},
      {"limit", "Maximum turns to return", false, "20"}}},

    {"transcript_register", "Register a transcript file for distillation tracking",
     {{"session_id", "Claude session ID", true, nullptr},
      {"transcript_path", "Path to .jsonl transcript file", true, nullptr},
      {"realm", "Project/realm isolation", false, "default"}}},

    {"transcript_get", "Get transcript state for a session",
     {{"session_id", "Session ID to look up", true, nullptr}}},

    {"transcript_list", "List all registered transcripts",
     {}},

    {"transcript_update", "Update transcript processing progress",
     {{"session_id", "Session ID", true, nullptr},
      {"last_line", "Last processed line number", true, nullptr}}},

    {"transcript_remove", "Remove transcript from tracking",
     {{"session_id", "Session ID to remove", true, nullptr}}},

    {"transcript_parse", "Parse new turns from a transcript JSONL file",
     {{"session_id", "Session ID to parse", true, nullptr},
      {"min_turns", "Minimum turns to return", false, "4"}}},

    {"distill_status", "Get distillation system status: transcripts, realms, pending work",
     {}},

    {"distill_now", "Synchronously distill ONE session now and return the counts (learnings + value-facts)",
     {{"session_id", "Session ID to distill", true, nullptr},
      {"transcript_path", "Optional JSONL path; registers it if given, else resolves durable register", false, nullptr},
      {"realm", "Optional realm (default brahman or the registered realm)", false, nullptr}}},

    {"epiplexity_check", "Compute epiplexity (ε) score for a seed - measures reconstruction quality",
     {{"original", "Original full text", true, nullptr},
      {"seed", "Compressed SSL seed", true, nullptr},
      {"reconstructed", "Text reconstructed from seed", true, nullptr}}},

    // Memory listing
    {"list_memories_brief", "List memories with full content, realm, and tags (JSON-per-line). Used by enrichers.",
     {{"limit",  "Max memories to return (default 200)", false, "200"},
      {"offset", "Pagination offset",                    false, "0"},
      {"realm",  "Filter by realm",                      false, ""},
      {"kind",   "Filter by kind",                       false, ""}}},

    // Bulk operations
    {"forget_kind", "Bulk-delete all memories of a given kind (e.g. habit, unknown)",
     {{"kind", "Memory kind to delete", true, nullptr},
      {"realm", "Optional realm filter", false, nullptr},
      {"limit", "Max to delete (default 5000)", false, "5000"}}},

    // Embedding tools
    {"embed_symbols", "Fast embed symbol metadata (~100/sec, no LLM needed)",
     {{"batch_size", "Symbols per batch", false, "100"},
      {"reset", "Clear all embeddings first", false, "false"}}},

    {"reembed_memories", "Re-embed memories with missing/zero embeddings",
     {{"all", "Re-embed ALL memories with NULL embeddings", false, "false"},
      {"limit", "Max memories to process", false, "100"},
      {"kind", "Filter by kind: belief, wisdom, episode, correction, preference", false, nullptr},
      {"min_confidence", "Min confidence threshold", false, "0"},
      {"dry_run", "Preview without updating", false, "false"}}},

    // Background processing tools
    {"background_status", "Get background processing and embedding scheduler status",
     {}},

    {"background_schedule", "Schedule background task",
     {{"task_type", "Task type to schedule", true, nullptr},
      {"params", "JSON params for task", false, nullptr}}},

    // Usage outcome tracking
    {"learn_outcome", "Record whether a surfaced memory helped (positive/negative/neutral)",
     {{"memory-id", "Memory ID that was surfaced", true, nullptr},
      {"outcome", "Did it help? positive|negative|neutral", true, nullptr},
      {"context", "What task triggered recall", false, nullptr}}},

    // SUS Phase 1: Memory exposure logging
    {"log_exposure", "Log memory exposure events for SUS metrics",
     {{"session_id", "Session ID", true, nullptr},
      {"turn_id", "Turn index (integer)", true, nullptr},
      {"hook_type", "Hook type: session_start or user_prompt", true, nullptr},
      {"memory_ids", "JSON array of memory IDs e.g. [1,2,3]", true, nullptr},
      {"ranks", "JSON array of rank positions e.g. [1,2,3]", false, nullptr},
      {"resonance_scores", "JSON array of scores e.g. [0.8,0.7]", false, nullptr}}},

    // SUS Phase 1: Metrics query
    {"get_sus_metrics", "Get Soul Utility Score metrics (SUS): R, P, D components and composite",
     {{"days", "Lookback window in days (default: 7)", false, nullptr}}},

    // Episode auto-distillation
    {"episode_cluster_status", "Find clusters of similar episodes for distillation into wisdom",
     {{"similarity-threshold", "Minimum similarity for cluster", false, "0.85"},
      {"min-occurrences", "Minimum episodes in cluster", false, "3"}}},

    // Hygiene and calibration
    {"hygiene_stats", "Get memory health statistics",
     {}},

    {"hygiene_run", "Run memory hygiene: decay, prune, consolidate",
     {{"prune-threshold", "Confidence below which to prune", false, "0.1"},
      {"min-age-days", "Minimum age for pruning", false, "7"},
      {"consolidation-threshold", "Similarity threshold for consolidation", false, "0.85"},
      {"max-consolidations", "Max consolidations per run", false, "10"}}},

    {"rebuild_fts_index", "Rebuild FTS index for BM25 keyword search on memory",
     {}},

    {"calibration_record", "Record a prediction outcome for calibration",
     {{"domain", "Domain: code|architecture|debugging|etc", true, nullptr},
      {"success", "Did the prediction succeed? true|false", true, nullptr}}},

    {"calibration_score", "Get calibration score for a domain",
     {{"domain", "Domain to check", true, nullptr}}},

    // Theme tools (xMemory-inspired hierarchical memory)
    {"theme_list", "List all themes with stats",
     {{"realm", "Filter by realm", false, nullptr}}},

    {"theme_get", "Get theme details with representatives",
     {{"id", "Theme ID", true, nullptr}}},

    {"theme_recall", "Two-stage retrieval: representatives then expansion",
     {{"query", "Search query", true, nullptr},
      {"limit", "Max results", false, "10"},
      {"realm", "Filter by realm", false, nullptr}}},

    {"theme_stats", "Get theme organization statistics",
     {{"realm", "Filter by realm", false, nullptr}}},

    {"theme_maintain", "Force theme maintenance: split, merge, reassign",
     {{"realm", "Filter by realm", false, nullptr}}},

    {"theme_assign_orphans", "Assign orphan memories to themes in batches",
     {{"batch_size", "Memories per batch", false, "100"},
      {"realm", "Filter by realm", false, nullptr}}},

    // Cross-session messaging
    {"msg_send", "Send a message to another session, a realm, or all sessions",
     {{"target", "Target: session_id, realm name, or '*' for all", true, nullptr},
      {"content", "Message content", true, nullptr},
      {"target_type", "direct|realm|global (auto-detected if omitted)", false, nullptr},
      {"priority", "0=info, 1=normal, 2=important, 3=urgent", false, "1"},
      {"content_type", "text|json|ssl", false, "text"},
      {"ttl", "TTL in seconds (0 = no expiry)", false, "3600"},
      {"session_id", "Sender session ID", false, nullptr}}},

    {"msg_inbox", "Check unread cross-session messages",
     {{"session_id", "Session ID (default: current)", false, nullptr},
      {"limit", "Max messages", false, "20"},
      {"min_priority", "Minimum priority level", false, "0"},
      {"auto_ack", "Auto-acknowledge returned messages", false, "false"}}},

    {"msg_ack", "Acknowledge a cross-session message",
     {{"message_id", "Message ID", true, nullptr},
      {"session_id", "Session ID (default: current)", false, nullptr}}},

    {"msg_ack_all", "Acknowledge all unread messages",
     {{"session_id", "Session ID (default: current)", false, nullptr}}},

    {"msg_history", "List recent cross-session messages",
     {{"session_id", "Session ID (default: current)", false, nullptr},
      {"direction", "sent|received|both", false, "both"},
      {"limit", "Max messages", false, "50"}}},

    {"ledger_op", "Daemon-owned task ledger operation",
     {{"op", "Operation name", true, nullptr}, {"args", "Operation arguments JSON", false, "{}"}}},
    {"session_register", "Register session for cross-session messaging",
     {{"session_id", "Session ID", true, nullptr},
      {"realm", "Realm", false, "brahman"},
      {"pid", "Process ID", false, nullptr},
      {"transcript_path", "Path to transcript .jsonl file (stable ID)", false, nullptr},
      {"project_dir", "Working directory", false, nullptr},
      {"metadata", "JSON metadata", false, "{}"}}},

    {"session_heartbeat", "Update session heartbeat",
     {{"session_id", "Session ID", true, nullptr},
      {"metadata", "Updated JSON metadata", false, nullptr}}},

    {"session_list", "List active sessions",
     {{"realm", "Filter by realm", false, nullptr},
      {"status", "Filter by status", false, "active"}}},

    {"session_deregister", "Remove session from registry",
     {{"session_id", "Session ID", true, nullptr}}},

    {"session_sync", "Sync session registry with running Claude processes and transcript files",
     {{"projects_dir", "Claude projects directory (default: ~/.claude/projects)", false, nullptr}}},

    {"migrate_vss", "Migrate embeddings from main DB VARCHAR to VSS DB FLOAT[768]",
     {}},

    // Goal management
    {"goal_list", "List goals with optional status filter",
     {{"status", "Filter: active, completed, abandoned (default: active)", false, "active"},
      {"realm", "Filter by realm", false, nullptr},
      {"limit", "Max results", false, "20"}}},

    {"goal_set", "Set a new goal",
     {{"title", "Short goal name", true, nullptr},
      {"description", "Detailed description", false, nullptr},
      {"deadline", "Optional deadline (Unix timestamp)", false, nullptr},
      {"realm", "Realm", false, nullptr}}},

    {"goal_progress", "Update goal progress (0-1 float)",
     {{"id", "Goal ID", true, nullptr},
      {"progress", "Progress 0-1 (e.g., 0.5 = 50%)", true, nullptr},
      {"milestone", "Milestone name to mark complete", false, nullptr}}},

    // Habit tracking
    {"habit_list", "List learned habits",
     {{"realm", "Filter by realm", false, nullptr},
      {"min_strength", "Minimum strength threshold", false, "0"},
      {"limit", "Max results", false, "20"}}},

    {"habit_observe", "Record a trigger→response pattern",
     {{"trigger", "What triggers the habit", true, nullptr},
      {"response", "What should happen when triggered", true, nullptr},
      {"realm", "Realm", false, nullptr}}},

    // Curiosity gaps
    {"curiosity_gaps", "List unresolved curiosity gaps",
     {{"realm", "Filter by realm", false, nullptr},
      {"limit", "Max results", false, "10"}}},

    {"curiosity_resolve", "Mark a curiosity gap as resolved",
     {{"id", "Gap memory ID", true, nullptr},
      {"learned", "What was learned", false, nullptr}}},

    {"expand_query", "Expand a query into typed variants for hybrid retrieval",
     {{"query", "Query to expand", true, nullptr}}},

    // Dream (autonomous curiosity exploration)
    {"dream_start", "Start a dream: autonomous web exploration on a topic",
     {{"topic", "Topic to explore", true, nullptr},
      {"realm", "Realm", false, nullptr},
      {"brain_provider", "Brain provider (claude/local)", false, "local"},
      {"brain_model", "Brain model name", false, "gemma4:26b"},
      {"max_concurrent", "Max concurrent dreams allowed", false, "2"}}},

    {"dream_wander", "Auto-pick a topic from curiosity gaps and start dreaming",
     {{"realm", "Realm", false, nullptr}}},

    {"dream_list", "List recent dreams",
     {{"limit", "Max results", false, "10"},
      {"realm", "Filter by realm", false, nullptr}}},

    {"dream_status", "Get full status of a dream including sadhana history",
     {{"id", "Dream ID", true, nullptr}}},

    {"dream_cancel", "Cancel an active dream (stops sadhana, marks cancelled)",
     {{"id", "Dream ID", true, nullptr}}},

    {"dream_force_woke", "Force a stuck dream to woke status (marks complete, stops sadhana)",
     {{"id", "Dream ID", true, nullptr}}},

    {"impl_start", "Start the self-improvement implementation sadhana (reads [impl] memories, implements changes, review gate, commits if approved)",
     {{"repo", "Absolute path to cc-soul repo (default: auto-detected)", false, nullptr},
      {"interval_seconds", "Seconds between cycles (default: 86400)", false, "86400"},
      {"max_turns", "Max turns per cycle (default: 15)", false, "15"},
      {"realm", "Memory realm (default: brahman)", false, "brahman"}}},

    // Sadhana lifecycle
    {"sadhana_stop", "Stop a running sadhana permanently",
     {{"id", "Sadhana ID", true, nullptr}}},

    {"sadhana_pause", "Pause a running sadhana (can be resumed)",
     {{"id", "Sadhana ID", true, nullptr}}},

    // Behavioral probe (drift-memory feature)
    {"probe_seed", "Store an exemplar text as a centroid for a behavioral class",
     {{"class", "Behavioral class: sycophantic|hedging|shallow|direct", true, nullptr},
      {"text", "Exemplar text for this class", true, nullptr},
      {"note", "Optional annotation", false, nullptr}}},

    {"behavioral_probe", "Score text against behavioral centroid clusters",
     {{"text", "Text to probe (e.g. a Claude response)", true, nullptr}}},

    {"probe_calibrate", "Add a confirmed exemplar to a behavioral class to refine its centroid",
     {{"class", "Behavioral class to update", true, nullptr},
      {"text", "Confirmed exemplar text", true, nullptr}}},

    {"probe_status", "Show how many exemplars exist per behavioral class", {}},

    // Ingest, Wiki, Training export
    {"ingest_source", "Ingest external content (URL, file, directory) into memory via SSL distillation",
     {{"source", "URL, file path, or directory path", true, nullptr},
      {"realm", "Target realm", false, "brahman"},
      {"type", "Source type: auto|url|file|directory", false, "auto"},
      {"model", "LLM model for distillation", false, nullptr},
      {"max_chunks", "Max chunks to process", false, "30"}}},

    {"wiki_export", "Export memories as Obsidian-compatible .md wiki with backlinks",
     {{"output_dir", "Output directory", false, nullptr},
      {"realm", "Filter to specific realm", false, nullptr},
      {"max_memories", "Max memories per realm", false, "5000"}}},

    {"health_check_start", "Start autonomous health-check sadhana",
     {{"interval_seconds", "Check interval in seconds", false, "3600"},
      {"realm", "Realm to monitor", false, "brahman"},
      {"max_turns", "Max check cycles (0 = unlimited)", false, "0"}}},

    {"export_training_pairs", "Export query-passage pairs as JSONL for embedding fine-tuning",
     {{"output_path", "Output JSONL path", false, nullptr},
      {"realm", "Filter to specific realm", false, nullptr},
      {"max_pairs", "Max pairs to export", false, "10000"},
      {"min_confidence", "Min confidence threshold", false, "0.5"},
      {"include_negatives", "Generate hard negatives", false, "true"}}},

    {"lookup", "Unified memory lookup — intent-routing search across keyword/semantic/triplet/temporal/code backends",
     {{"query", "Search query", true, nullptr},
      {"limit", "Max results", false, "10"},
      {"realm", "Filter by realm", false, nullptr},
      {"mode", "Search mode: auto|fast|deep", false, "auto"},
      {"explain", "Include intent classification and score breakdown", false, "false"}}},

    // Contradiction detection
    {"detect_contradictions", "Detect contradictions for a stored memory against realm peers",
     {{"memory_id", "Memory ID to check", true, nullptr},
      {"realm", "Realm to scan", false, nullptr}}},

    {"scan_contradictions", "Background scan: find contradiction candidates across all memories in a realm",
     {{"realm", "Realm to scan", false, nullptr},
      {"limit", "Max candidates to return", false, "50"}}},

    {"resolve_contradiction", "Resolve a contradiction: declare winner supersedes loser",
     {{"winner_id", "Memory ID that is correct", true, nullptr},
      {"loser_id", "Memory ID to demote", true, nullptr},
      {"reason", "Explanation for the resolution", false, "manual resolution"}}},

    {"why_active", "Explain why a memory is active: status, epistemic source, confirmations, contradictions",
     {{"id", "Memory ID", true, nullptr}}},

    {"what_superseded", "Show the full supersession chain for a memory",
     {{"id", "Memory ID", true, nullptr}}},

    {"show_conflicts", "Semantic search + show contradiction pairs for matching memories",
     {{"query", "Search query", true, nullptr},
      {"limit", "Max results", false, "20"},
      {"realm", "Filter by realm", false, nullptr}}},

    // CEC: Event tape + CDAWG
    {"log_event", "Log a structured action event to the CEC tape and CDAWG",
     {{"tool", "Tool name (e.g. Edit, Bash)", true, nullptr},
      {"entity", "Entity acted on (file, URL, command)", true, nullptr},
      {"outcome", "0=success 1=fail 2=error 3=partial", false, "0"},
      {"session_id", "Session ID (optional)", false, "0"},
      {"ts_ms", "Timestamp ms (0=now)", false, "0"}}},

    {"log_event_ex", "Log a CEC event with regret-shaping telemetry (token_cost, latency_ms, retry_count)",
     {{"tool",        "Tool name",                                          true,  nullptr},
      {"entity",      "Entity acted on (file, URL, command)",               true,  nullptr},
      {"outcome",     "0=success 1=fail 2=error 3=partial",                 false, "0"},
      {"session_id",  "Session ID (optional)",                              false, "0"},
      {"ts_ms",       "Timestamp ms (0=now)",                               false, "0"},
      {"token_cost",  "Tokens consumed (0=unknown)",                        false, "0"},
      {"latency_ms",  "Wall-clock ms (0=unknown)",                          false, "0"},
      {"retry_count", "Retries before this outcome",                        false, "0"}}},

    {"log_decision", "Log a decision point to the DecisionTape: chosen action + rejected alternatives",
     {{"chosen_tool",      "Tool that was chosen",                          true,  nullptr},
      {"chosen_entity",    "Entity for the chosen tool",                    true,  nullptr},
      {"chosen_outcome",   "Outcome class for the chosen action",           false, "0"},
      {"rejected_json",    "JSON array of [sym_u64, reason_u8] pairs",      false, "[]"},
      {"confidence_delta", "chosen_confidence - best_alternative_confidence", false, "0"},
      {"ts_ms",            "Timestamp ms (0=now)",                          false, "0"}}},

    {"recall_last_action", "Return last k occurrences of (tool, entity) from the CEC event tape",
     {{"tool", "Tool name", true, nullptr},
      {"entity", "Entity name", true, nullptr},
      {"k", "Max results", false, "5"}}},

    {"recall_failure_pattern", "Return top-k CDAWG states with high failure rates",
     {{"k", "Max results", false, "5"}}},

    {"recall_causal_antecedent", "PMI-ranked causal antecedents: what actions typically precede (tool, entity)?",
     {{"tool", "Tool name", true, nullptr},
      {"entity", "Entity name", true, nullptr},
      {"k", "Max results", false, "5"}}},
    {"recall_hdcbind", "Heteroassociative HDC query: given known_role=known_val, infer query_role. Roles: tool, entity, outcome.",
     {{"known_role", "Known role (tool|entity|outcome)", true, nullptr},
      {"known_val",  "Value for the known role", true, nullptr},
      {"query_role", "Role to infer (tool|entity|outcome)", true, nullptr},
      {"k", "Max results", false, "5"}}},
    {"recall_counterfactual", "CDAWG sibling-edge counterfactual: what alternative tool/entity would have had a lower failure rate in the same context?",
     {{"tool",    "Tool name that was used", true, nullptr},
      {"entity",  "Entity that was acted on", true, nullptr},
      {"outcome", "Outcome class (0=success 1=fail 2=error 3=partial)", false, "1"},
      {"k",       "Max alternatives", false, "5"}}},
    {"consolidation_pass", "Run Sequitur grammar consolidation: promote frequent bigrams to triplet KG. Use --preview 1 for dry run.",
     {{"preview", "If 1, dry run — show rules without writing", false, "0"},
      {"k",       "Rules to show in preview", false, "5"}}},
    {"refutation_stats", "Show Sequitur rules being falsified: antecedent appears but expected consequent does not. Reports live/refuted counts and top-k by refutation ratio.",
     {{"k", "Max rules to show", false, "10"}}},
    {"recall_motif_value", "Top-k CDAWG motif states from (tool, entity) ranked by Q-value. Answers: which action sequences have highest expected success rate from this context?",
     {{"tool",   "Tool name to query from", true,  nullptr},
      {"entity", "Entity to query from",    true,  nullptr},
      {"k",      "Max states to return",    false, "5"}}},
    {"executor_flush", "Promote shadow CEC policies past the 20-event/lift>0.15 gate and demote policies whose source rule is refuted. Reports promoted/demoted ids and store stats.",
     {}},
    {"list_policies", "List CEC intervention policies. Shows id, source rule, kind, shadow event count, Q-lift, and fire count.",
     {{"active_only", "If true, show only active (promoted) policies", false, "false"}}},
    {"recall_true_counterfactual", "Return DecisionTape entries where (tool, entity) was explicitly rejected. True counterfactual data vs CDAWG sibling inference.",
     {{"tool",    "Tool name",                          true,  nullptr},
      {"entity",  "Entity name",                        true,  nullptr},
      {"outcome", "Outcome class (0=success, default)", false, "0"},
      {"k",       "Max results",                        false, "5"}}},
    {"hypothesis_probes", "Top-k Sequitur rules by Wilson probe_value (expected info gain). Highest = most uncertain — best candidates for deliberate disambiguation.",
     {{"k", "Max rules to return", false, "10"}}},
    {"fep_status", "CEC Phase 15 FEP prior organ: obs_count, states_modeled, ewma_drift (context_drift if >0.5), ewma_shock (emission_shock if >0.3). Context drift = world-model silently degrading; emission shock = known context, novel outcome.",
     {}},
    {"turiya_status", "CEC Phase 11 Turīya witness: read-only health vector across all CEC organs. Reports CDAWG growth, Sequitur rule churn, Q-value variance, refutation pressure, and hypothesis uncertainty. Diagnoses: healthy|stale|q_collapse|refutation_flood|high_uncertainty|fep_context_drift|fep_emission_shock.",
     {}},
    {"tape_stats", "CEC Phase 12 EventTape statistics: event count, tombstoned events (temporal compression), unique sessions, tools, entities, and failure events.",
     {}},
    {"verbalize_rules", "CEC Phase 13 Verbalization: convert top-k Sequitur rules to natural language using a deterministic template. No LLM. Shows what the agent has learned as readable heuristics.",
     {{"k", "Max rules to return", false, "10"}}},
    {"queue_experiments", "CEC Phase 14 Self-directed experimentation: file OpenTask interventions for the k most uncertain Sequitur rules (probe_value > 0.4). Skips rules with refutation_ratio >= 0.3 (adversarial gate). Also triggered automatically by consolidation_pass when turiya_status reports high_uncertainty.",
     {{"k", "Max experiments to queue", false, "5"}}},
    {"witness_memory", "CEC Phase 17 candidate promotion: provide an outcome-witness to promote a candidate-band memory to established. Witnesses: correction|outcome|hit_rate_delta. Open-weight-generated writes start in candidate band until witnessed.",
     {{"memory_id",    "Memory ID to promote",                   true,  ""},
      {"witness_kind", "Type: correction|outcome|hit_rate_delta", true,  ""}}},
    {"reconcile_pass", "CEC Phase 17 R0 reconcile operator: scan all assoc_edges for MemoryKind legality violations and content contradictions. Model-free and deterministic. Reports illegal_edges, contradictions, unresolved counts.",
     {}},
    {"harvest_scope", "CEC Phase 17 open-weight harvest targeting: derive scope from current Turīya anomalies + router miss patterns. Output JSON used by scripts/harvest_ow.py for demand-driven extraction.",
     {}},
    {"seed_hdc_geometry", "CEC Phase 17 Part D: seed HDC codebook from vocab_geometry harvest JSON. Binarizes f32 PCA directions from open-weight embedding matrix into HdcVec entries, replacing random hash projections for harvested tokens.",
     {{"json_path", "Path to qwen2.5-7b_vocab_geometry.json produced by harvest_ow.py --mode vocab_geometry", true, ""}}},
    {"routed_recall", "CEC Phase 16 CPU-native query router: dispatches to cheapest lane (exact/fuzzy/temporal/causal/hybrid) without an LLM call. Returns needs_disambiguation with named unbound slots when the typed grammar cannot fully bind the request.",
     {{"subject",       "Exact triplet subject — compiles to relational lookup",    false, ""},
      {"predicate",     "Exact triplet predicate",                                  false, ""},
      {"freetext",      "Free-text query — fuzzy ANN+BM25 lane",                    false, ""},
      {"realm",         "Filter by realm",                                           false, ""},
      {"causal_tool",   "CEC causal query: tool name",                              false, ""},
      {"causal_entity", "CEC causal query: entity name (required with causal_tool)",false, ""},
      {"time_from_ms",  "Temporal lower bound (epoch ms)",                          false, ""},
      {"time_to_ms",    "Temporal upper bound (epoch ms)",                          false, ""},
      {"k",             "Max hits to return",                                        false, "10"}}},
    {"ledger_append", "v6.0 Interaction Ledger: record a retrieve/inject/outcome/override event. Pass full InteractionEvent JSON.",
     {{"kind",        "Event kind: Retrieve | Inject | Outcome | Override",         true,  ""},
      {"session_id",  "Session UUID",                                                true,  ""},
      {"payload",     "Typed payload JSON matching EventPayload variant",            true,  ""},
      {"thread_id",   "Optional thread ID",                                          false, ""},
      {"causal_parent","Optional parent event_id",                                  false, ""}}}  ,
    {"ledger_query", "v6.0 Interaction Ledger: query events by kind/session/time.",
     {{"kind",       "Filter by kind: Retrieve | Inject | Outcome | Override",      false, ""},
      {"session_id", "Filter by session UUID",                                       false, ""},
      {"since_ms",   "Only events at or after this epoch ms",                        false, ""},
      {"limit",      "Max events to return",                                          false, "50"}}},
    {"ledger_compile", "v6.0 Interaction Ledger: compile Override events into versioned VersionedAssertions.",
     {}},
    {"ledger_contradictions", "v6.0 Interaction Ledger: list contested (subject, predicate) pairs with >1 active assertion.",
     {}},
    {"ledger_health", "Get ledger event counts by kind and queue health metrics.",
     {}},
    {"predicate_attach", "v7.0 Falsifiable memories: attach an executable shell predicate to a memory. check_cmd is run to verify the memory is still true.",
     {}},
    {"predicate_run", "v7.0 Falsifiable memories: run all predicates for a memory; returns passed/failed counts and epistemic_status. Decays confidence on failure.",
     {}},
    {"predicate_list", "v7.0 Falsifiable memories: list all predicates attached to a memory and their current status.",
     {}},
};

static std::set<std::string> build_known_tools() {
    std::set<std::string> tools;
    for (const auto& spec : TOOL_SPECS) {
        tools.insert(spec.name);
    }
    return tools;
}

static const std::set<std::string> KNOWN_TOOLS = build_known_tools();

static const ToolSpec* find_tool_spec(const std::string& name) {
    for (const auto& spec : TOOL_SPECS) {
        if (spec.name == name) return &spec;
    }
    return nullptr;
}

static void print_tool_help(const std::string& tool) {
    const ToolSpec* spec = find_tool_spec(tool);
    if (!spec) {
        std::cerr << "Unknown tool: " << tool << "\n";
        return;
    }

    std::cerr << "chitta " << spec->name << " - " << spec->description << "\n\n";

    if (spec->params.empty()) {
        std::cerr << "  No parameters required.\n";
        return;
    }

    std::cerr << "Parameters:\n";
    for (const auto& p : spec->params) {
        std::cerr << "  --" << p.name;
        if (p.required) {
            std::cerr << " (required)";
        } else if (p.default_val) {
            std::cerr << " [default: " << p.default_val << "]";
        }
        std::cerr << "\n      " << p.description << "\n";
    }

    // Show example
    std::cerr << "\nExample:\n  chitta " << spec->name;
    for (const auto& p : spec->params) {
        if (p.required) {
            std::cerr << " --" << p.name << " \"...\"";
        }
    }
    std::cerr << "\n";
}

static const char* prog_name(const char* path) {
    const char* last = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '/') last = p + 1;
    }
    return last;
}

void print_usage(const char* prog) {
    const char* name = prog_name(prog);
    std::cerr << "Usage:\n"
              << "  " << name << " <tool> --param value ...   Invoke tool\n"
              << "  " << name << " <tool> --help              Show tool parameters\n"
              << "  " << name << " [options]                  Interactive mode (JSON-RPC)\n"
              << "\n"
              << "Examples:\n"
              << "  " << name << " recall --query \"search terms\"\n"
              << "  " << name << " soul_context\n"
              << "  " << name << " observe --title \"Decision\" --content \"Chose X over Y\"\n"
              << "  " << name << " grow --type wisdom --content \"Pattern discovered\"\n"
              << "  " << name << " learn_codebase --path /path/to/project\n"
              << "\n"
              << "Tool categories:\n"
              << "  Memory:      remember, recall, grow, get, update, forget, strengthen, weaken, tag\n"
              << "  Triplets:    connect, query, query_graph\n"
              << "  Hooks:       observe, full_resonate\n"
              << "  Explore:     explore_recall, explore_peek, explore_expand, explore_neighbors\n"
              << "  Context:     soul_context, health_check, version_check, lookup\n"
              << "  Maintenance: cycle, cleanup, hygiene_stats, hygiene_run\n"
              << "  Import/Export: import_soul, export_soul\n"
              << "  Code Intel:  extract_symbols, learn_codebase, find_symbol, search_symbols, code_context\n"
              << "  Call Graph:  symbol_callers, symbol_callees, resolve_callsites\n"
              << "  Type/Import: type_hierarchy, file_imports, file_dependents, read_symbol, read_function\n"
              << "  Realm:       realm_detect, realm_list, realm_get, realm_set, realm_add, realm_remove, realm_visibility\n"
              << "  Ledger:      ledger_save, ledger_load, ledger_list, ledger_get, ledger_delete\n"
              << "  Transcript:  read_transcript, transcript_register, transcript_get, transcript_list, transcript_update, transcript_remove\n"
              << "  Learning:    learn_outcome, episode_cluster_status, calibration_record, calibration_score\n"
              << "  Theme:       theme_list, theme_get, theme_recall, theme_stats, theme_maintain, theme_assign_orphans\n"
              << "  Bulk ops:    forget_kind, list_memories_brief\n"
              << "  CEC events:  log_event, log_event_ex, log_decision\n"
              << "  CEC recall:  recall_last_action, recall_causal_antecedent, recall_failure_pattern,\n"
              << "               recall_counterfactual, recall_true_counterfactual, recall_motif_value\n"
              << "  CEC market:  hypothesis_probes, refutation_stats\n"
              << "  CEC exec:    executor_flush, list_policies, consolidation_pass\n"
              << "  CEC witness: turiya_status, tape_stats, verbalize_rules\n"
              << "  CEC experiment: queue_experiments\n"
              << "  CEC phase15: fep_status\n"
              << "  CEC phase16: routed_recall\n"
              << "  CEC phase17: witness_memory, reconcile_pass, harvest_scope, seed_hdc_geometry\n"
              << "\n"
              << "Global options:\n"
              << "  --socket-path PATH  Unix socket path\n"
              << "  --json              Output raw JSON instead of text\n"
              << "  --toon              Output TOON format (compact, ~40% fewer tokens)\n"
              << "  --text-only         Output only text content (for piping/hooks)\n"
              << "  --help              Show this help message\n";
}

enum class OutputFormat { Text, Json, Toon, TextOnly };

// CLI mode: invoke tool directly
int run_cli(const std::string& socket_path, const std::string& tool,
            int argc, char* argv[], int arg_start, OutputFormat output_format) {
    using json = nlohmann::json;

    // Check for --help first
    for (int i = arg_start; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_tool_help(tool);
            return 0;
        }
    }

    // Build arguments JSON from command line (all named, no positional)
    json args = json::object();

    for (int i = arg_start; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg.rfind("--", 0) == 0) {
            // Named argument: --key value
            std::string key = arg.substr(2);
            // Normalize hyphens to underscores for consistency with JSON keys
            std::replace(key.begin(), key.end(), '-', '_');
            if (i + 1 < argc && (argv[i + 1][0] != '-' ||
                                   (argv[i + 1][0] == '-' && std::isdigit(static_cast<unsigned char>(argv[i + 1][1]))))) {
                std::string value = argv[++i];
                // Try to parse as JSON object/array, number, or boolean
                if (value == "true") {
                    args[key] = true;
                } else if (value == "false") {
                    args[key] = false;
                } else if (!value.empty() && (value[0] == '{' || value[0] == '[')) {
                    // Try to parse as JSON object or array
                    try {
                        args[key] = json::parse(value);
                    } catch (...) {
                        args[key] = value;  // Fall back to string
                    }
                } else {
                    // Only parse as number if entire string is numeric
                    bool is_numeric = !value.empty();
                    bool has_dot = false;
                    for (size_t j = 0; j < value.size(); ++j) {
                        char c = value[j];
                        if (c == '-' && j == 0) continue;  // Leading minus OK
                        if (c == '.' && !has_dot) { has_dot = true; continue; }
                        if (!std::isdigit(c)) { is_numeric = false; break; }
                    }
                    if (is_numeric && !value.empty()) {
                        try {
                            if (has_dot) {
                                args[key] = std::stod(value);
                            } else if (value[0] != '-') {
                                // Use stoull for non-negative: avoids overflow on large uint64_t
                                // memory IDs (> INT64_MAX ~9.2e18) that stoll would throw on.
                                args[key] = std::stoull(value);
                            } else {
                                args[key] = std::stoll(value);
                            }
                        } catch (...) {
                            args[key] = value;
                        }
                    } else {
                        args[key] = value;
                    }
                }
            } else {
                args[key] = true;  // Flag without value
            }
        } else {
            // Positional argument - show help instead of silently ignoring
            std::cerr << "Error: Unexpected positional argument: " << arg << "\n";
            std::cerr << "All arguments must be named (--param value).\n\n";
            print_tool_help(tool);
            return 1;
        }
    }

    // Normalize comma-separated strings to arrays for known array keys
    static const std::set<std::string> ARRAY_KEYS = {"tags", "shared_realms", "lanes"};
    for (const auto& key : ARRAY_KEYS) {
        if (args.contains(key) && args[key].is_string()) {
            std::string val = args[key].get<std::string>();
            if (key == "lanes" || val.find(',') != std::string::npos) {
                json arr = json::array();
                std::istringstream iss(val);
                std::string item;
                while (std::getline(iss, item, ',')) {
                    size_t start = item.find_first_not_of(' ');
                    size_t end = item.find_last_not_of(' ');
                    if (start != std::string::npos)
                        arr.push_back(item.substr(start, end - start + 1));
                }
                args[key] = arr;
            }
        }
    }

    // Validate required parameters
    const ToolSpec* spec = find_tool_spec(tool);
    if (spec) {
        coerce_bool_params(*spec, args);
        std::vector<std::string> missing;
        for (const auto& p : spec->params) {
            if (p.required && !args.contains(p.name)) {
                missing.push_back(p.name);
            }
        }
        if (!missing.empty()) {
            std::cerr << "Error: Missing required parameter(s): ";
            for (size_t i = 0; i < missing.size(); ++i) {
                if (i > 0) std::cerr << ", ";
                std::cerr << "--" << missing[i];
            }
            std::cerr << "\n\n";
            print_tool_help(tool);
            return 1;
        }
    }

    // Connect to daemon (safe mode: never kill/restart)
    chitta::SocketClient client(socket_path);
    if (!client.connect_only()) {
        const auto& err = client.last_error();
        std::cerr << "Error: " << err << "\n";
        if (err.find("still loading") != std::string::npos || err.find("warming up") != std::string::npos)
            std::cerr << "Hint: Daemon is starting up — retry in a few seconds\n";
        else if (err.find("incompatible") != std::string::npos)
            std::cerr << "Hint: Restart daemon with 'systemctl --user restart chittad'\n";
        else
            std::cerr << "Hint: Start daemon with 'chittad daemon' or let hooks start it\n";
        return 1;
    }

    // For session-aware tools, inject PPID for session lookup if session_id not provided
    static const std::set<std::string> SESSION_TOOLS = {
        "msg_inbox", "msg_send", "msg_ack", "msg_ack_all", "msg_history",
        "ledger_save", "narrative_log", "narrative_history",
        "anticipation_filter", "anticipation_gate_status",
        "recall", "smart_recall", "hybrid_recall", "recall_lanes"
    };
    if (SESSION_TOOLS.count(tool) && !args.contains("session_id")) {
        pid_t ppid = getppid();
        if (ppid > 1) {  // Sanity check: not init
            args["pid"] = static_cast<int64_t>(ppid);
        }
    }

    // Normalize --kind as alias for --type in remember/grow
    if ((tool == "remember" || tool == "grow") &&
        args.contains("kind") && !args.contains("type")) {
        args["type"] = args["kind"];
        args.erase("kind");
    }

    json tool_req = {
        {"jsonrpc", "2.0"},
        {"method", "tools/call"},
        {"params", {
            {"name", tool},
            {"arguments", args}
        }},
        {"id", 1}
    };

    auto resp = client.request(tool_req.dump());
    if (!resp) {
        std::cerr << "Error: Tool call failed: " << client.last_error() << "\n";
        return 1;
    }

    try {
        // DEBUG: Print raw response for troubleshooting
        if (std::getenv("CHITTA_DEBUG")) {
            std::cerr << "[DEBUG] Raw response (" << resp->size() << " bytes): " << resp->substr(0, 200) << "\n";
        }
        auto result = json::parse(*resp);

        if (result.contains("error")) {
            std::cerr << "Error: " << result["error"]["message"].get<std::string>() << "\n";
            return 1;
        }

        switch (output_format) {
            case OutputFormat::Json:
                // Raw JSON output
                if (result.contains("result") && result["result"].contains("structured")) {
                    std::cout << result["result"]["structured"].dump(2, ' ', false, json::error_handler_t::replace) << "\n";
                } else {
                    std::cout << result.dump(2, ' ', false, json::error_handler_t::replace) << "\n";
                }
                break;

            case OutputFormat::Toon:
                // TOON format (compact, shell-friendly)
                if (result.contains("result") && result["result"].contains("structured")) {
                    std::cout << toon::encode(result["result"]["structured"]);
                } else if (result.contains("result")) {
                    std::cout << toon::encode(result["result"]);
                } else {
                    std::cout << toon::encode(result);
                }
                break;

            case OutputFormat::TextOnly:
                // Just the text content from results (for hooks/piping)
                if (result.contains("result") && result["result"].contains("structured")) {
                    auto& structured = result["result"]["structured"];
                    if (structured.contains("results") && structured["results"].is_array()) {
                        for (const auto& item : structured["results"]) {
                            if (item.contains("text")) {
                                std::cout << item["text"].get<std::string>() << "\n";
                            }
                        }
                    } else if (structured.contains("text")) {
                        std::cout << structured["text"].get<std::string>() << "\n";
                    }
                }
                break;

            case OutputFormat::Text:
            default:
                // Human-readable text output
                if (result.contains("result") && result["result"].contains("content")) {
                    auto& content = result["result"]["content"];
                    if (content.is_array() && !content.empty() && content[0].contains("text")) {
                        std::cout << content[0]["text"].get<std::string>() << "\n";
                    }
                }
                break;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error parsing response: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

/// Thin client mode: forward stdin → daemon → stdout
int run_thin_client(const std::string& socket_path) {
    chitta::SocketClient client(socket_path);

    // Safe connect: never kill/restart daemon
    if (!client.connect_only()) {
        const auto& err = client.last_error();
        std::cerr << "[chitta] " << err << "\n";
        if (err.find("still loading") != std::string::npos || err.find("warming up") != std::string::npos)
            std::cerr << "[chitta] Hint: Daemon is starting up — retry in a few seconds\n";
        else if (err.find("incompatible") != std::string::npos)
            std::cerr << "[chitta] Hint: Restart daemon with 'systemctl --user restart chittad'\n";
        else
            std::cerr << "[chitta] Hint: Start daemon with 'chittad daemon'\n";
        return 1;
    }

    std::cerr << "[chitta] Connected to daemon at " << socket_path << "\n";
    std::cerr << "[chitta] Listening on stdin...\n";

    // Forward requests
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        auto response = client.request(line);
        if (response) {
            std::cout << *response << "\n";
            std::cout.flush();
        } else {
            std::cerr << "[chitta] Request failed: " << client.last_error() << "\n";

            // Try to reconnect (safe: don't restart daemon)
            client.disconnect();
            if (!client.connect_only()) {
                std::cerr << "[chitta] Reconnection failed: " << client.last_error() << "\n";
                return 1;
            }
            std::cerr << "[chitta] Reconnected to daemon\n";

            // Retry the request
            response = client.request(line);
            if (response) {
                std::cout << *response << "\n";
                std::cout.flush();
            } else {
                std::cout << R"({"jsonrpc":"2.0","error":{"code":-32603,"message":"Daemon connection lost"},"id":null})" << "\n";
                std::cout.flush();
            }
        }
    }

    std::cerr << "[chitta] Shutdown complete\n";
    return 0;
}

// Detect realm from environment/git/config
// Priority: CHITTA_REALM env > .cc-soul-realm file > git repo name > "brahman"
static std::string detect_realm() {
    // 1. Environment variable
    if (const char* env_realm = std::getenv("CHITTA_REALM")) {
        return env_realm;
    }

    // 2. Config file in current directory
    std::ifstream realm_file(".cc-soul-realm");
    if (realm_file.good()) {
        std::string realm;
        std::getline(realm_file, realm);
        // Trim whitespace
        realm.erase(0, realm.find_first_not_of(" \t\n\r"));
        realm.erase(realm.find_last_not_of(" \t\n\r") + 1);
        if (!realm.empty()) {
            return realm;
        }
    }

    // 3. Git repository name
    std::array<char, 256> buffer;
    std::string git_root;
    // --git-common-dir resolves a linked worktree to its main repository, so a
    // Codex stream worktree shares the project realm instead of its own.
    FILE* pipe = popen("d=$(git rev-parse --git-common-dir 2>/dev/null) && cd \"$(dirname \"$d\")\" 2>/dev/null && pwd -P", "r");
    if (pipe) {
        if (fgets(buffer.data(), buffer.size(), pipe)) {
            git_root = buffer.data();
            // Remove trailing newline
            if (!git_root.empty() && git_root.back() == '\n') {
                git_root.pop_back();
            }
        }
        pclose(pipe);
    }

    if (!git_root.empty()) {
        // Extract repo name from path
        size_t last_slash = git_root.rfind('/');
        std::string repo_name = (last_slash != std::string::npos)
            ? git_root.substr(last_slash + 1)
            : git_root;
        return "project:" + repo_name;
    }

    // 4. Default
    return "brahman";
}

int main(int argc, char* argv[]) {
    std::string socket_path = [] {
        if (const char* configured = std::getenv("CHITTA_SOCKET_PATH")) {
            if (*configured) return std::string(configured);
        }
        return chitta::SocketClient::default_socket_path();
    }();
    OutputFormat output_format = OutputFormat::Text;
    std::string tool;
    int tool_arg_index = 0;
    std::vector<std::string> tool_positional;  // positional args after the tool name, flags stripped

    // Pre-scan for --socket-path, --json, --toon flags, find tool name, and
    // collect any positional args that follow the tool (flags interleaved
    // after the tool name are honored as global flags, not tool args).
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0) {
            std::cout << "chitta " << CHITTA_VERSION << "\n";
            return 0;
        } else if (std::strcmp(argv[i], "--socket-path") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (std::strcmp(argv[i], "--json") == 0) {
            output_format = OutputFormat::Json;
        } else if (std::strcmp(argv[i], "--toon") == 0) {
            output_format = OutputFormat::Toon;
        } else if (std::strcmp(argv[i], "--text-only") == 0) {
            output_format = OutputFormat::TextOnly;
        } else if (tool.empty() && argv[i][0] != '-') {
            tool = argv[i];
            tool_arg_index = i;
        } else if (!tool.empty() && argv[i][0] != '-') {
            tool_positional.emplace_back(argv[i]);
        }
    }

    // Handle realm_detect command (client-side, no daemon needed)
    if (tool == "realm_detect") {
        std::string realm = detect_realm();
        switch (output_format) {
            case OutputFormat::Json:
                std::cout << "{\"realm\":\"" << realm << "\"}\n";
                break;
            case OutputFormat::Toon:
                std::cout << "realm: " << realm << "\n";
                break;
            default:
                std::cout << realm << "\n";
                break;
        }
        return 0;
    }

    // Handle status command (daemon health check)
    if (tool == "status") {
        chitta::SocketClient client(socket_path);
        if (!client.connect()) {
            std::cout << "Daemon: not running\n";
            std::cout << "Socket: " << socket_path << " (not found)\n";
            return 1;
        }
        auto version = client.check_version();
        if (version) {
            std::cout << "Daemon: running\n";
            std::cout << "Socket: " << socket_path << "\n";
            std::cout << "Version: " << version->software << "\n";
            std::cout << "Protocol: " << version->protocol_major << "." << version->protocol_minor << "\n";
            return 0;
        }
        std::cout << "Daemon: running (version unknown)\n";
        return 0;
    }

    // queue_write: enqueue a JSONL request line to the mind-local queue.
    // Client-side only (no daemon required). Hooks use this instead of
    // `jq -c ... >> file` so args are parsed/compacted natively and the
    // append is a single atomic write() syscall (see sandbox::append_line_atomic).
    //
    // Usage: chitta queue_write <tool> <args-json>
    //   args-json may be multi-line / pretty-printed — it is normalized.
    //   Pass "-" to read args-json from stdin.
    if (tool == "queue_write") {
        if (tool_positional.size() < 2) {
            std::cerr << "usage: chitta queue_write <tool> <args-json|->\n";
            return 2;
        }
        std::string qtool = tool_positional[0];
        std::string raw_args = tool_positional[1];
        if (raw_args == "-") {
            std::string chunk((std::istreambuf_iterator<char>(std::cin)),
                              std::istreambuf_iterator<char>());
            raw_args = std::move(chunk);
        }

        nlohmann::json args_json = nlohmann::json::parse(raw_args, nullptr, false);
        if (args_json.is_discarded()) {
            std::cerr << "queue_write: invalid JSON for args\n";
            return 2;
        }

        char ack_buf[48];
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::snprintf(ack_buf, sizeof(ack_buf), "ack-%lld-%d",
                      static_cast<long long>(now_ms),
                      static_cast<int>(::getpid()));

        nlohmann::json entry = {
            {"ack_id", ack_buf},
            {"tool",   qtool},
            {"args",   args_json},
            {"ts",     now_ms / 1000}
        };

        const auto queue_path = chitta::queue_path_for_mind(chitta::SocketClient::default_mind_path());

        if (!chitta::sandbox::append_line_atomic(
                queue_path,
                entry.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace))) {
            std::cerr << "queue_write: append failed: " << queue_path << "\n";
            return 1;
        }
        return 0;
    }

    // Handle shutdown command specially (not a tool, direct daemon control)
    if (tool == "shutdown") {
        chitta::SocketClient client(socket_path);
        if (!client.connect()) {
            std::cerr << "No daemon running\n";
            return 1;
        }
        if (client.request_shutdown()) {
            std::cout << "Daemon shutdown requested\n";
            // 30s timeout: daemon may be finishing distillation or other long-running work
            if (client.wait_for_socket_gone(30000)) {
                std::cout << "Daemon stopped\n";
            }
            return 0;
        }
        std::cerr << "Failed to request shutdown\n";
        return 1;
    }

    // Check for --help without a tool
    if (tool.empty()) {
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
                print_usage(argv[0]);
                return 0;
            }
        }
    }

    // Handle MCP mode (JSON-RPC bridge to daemon via socket)
    if (tool == "mcp") {
        // MCP mode: connect to daemon via socket, forward JSON-RPC
        chitta::SocketClient client(socket_path);

        // Auto-starting a daemon from a client is how a SECOND chittad ended up on
        // the live mind dir on 2026-09-14 (the socket was slow, not absent) and
        // compacted away the running daemon's WAL segment. The systemd unit owns
        // the daemon; opt back in with CHITTA_CLI_AUTOSTART=1 for ad-hoc setups.
        const char* autostart_env = std::getenv("CHITTA_CLI_AUTOSTART");
        const bool autostart = autostart_env && std::string(autostart_env) == "1";
        if (!client.connect() && !autostart) {
            nlohmann::json error;
            error["jsonrpc"] = "2.0";
            error["error"]["code"] = -32603;
            error["error"]["message"] = "chittad is not reachable at " + socket_path + " (auto-start disabled; start the daemon or set CHITTA_CLI_AUTOSTART=1)";
            error["id"] = nullptr;
            std::cout << error.dump() << std::endl;
            return 1;
        }
        if (!client.connected()) {
            // Try to auto-start the daemon
            std::string chittad_path;
            if (const char* home = std::getenv("HOME")) {
                chittad_path = std::string(home) + "/.claude/bin/chittad";
            } else {
                chittad_path = "chittad";
            }

            // Start daemon in background
            std::string cmd = chittad_path + " daemon >/dev/null 2>&1 &";
            std::system(cmd.c_str());

            // Wait for daemon to start (up to 5 seconds)
            for (int i = 0; i < 50; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (client.connect()) break;
            }

            if (!client.connected()) {
                // Still can't connect - output MCP error
                nlohmann::json error;
                error["jsonrpc"] = "2.0";
                error["error"]["code"] = -32603;
                error["error"]["message"] = "Failed to start daemon";
                error["id"] = nullptr;
                std::cout << error.dump() << std::endl;
                return 1;
            }
        }

        // Read JSON-RPC from stdin, forward to daemon, write response to stdout
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.empty()) continue;

            try {
                auto mcp_request = nlohmann::json::parse(line);
                std::string method = mcp_request.value("method", "");
                auto request_id = mcp_request.value("id", nlohmann::json());

                // Handle MCP protocol methods
                if (method == "initialize") {
                    // MCP initialization response - match client's protocol version
                    auto params = mcp_request.value("params", nlohmann::json::object());
                    std::string client_version = params.value("protocolVersion", "2024-11-05");

                    nlohmann::json response;
                    response["jsonrpc"] = "2.0";
                    response["result"]["protocolVersion"] = client_version;  // Echo client's version
                    response["result"]["capabilities"]["tools"] = nlohmann::json::object();
                    response["result"]["serverInfo"]["name"] = "chitta";
                    response["result"]["serverInfo"]["version"] = CHITTA_VERSION;
                    response["id"] = request_id;
                    std::cout << response.dump() << std::endl;
                } else if (method == "notifications/initialized") {
                    // No response needed for notifications
                    continue;
                } else if (method == "tools/list") {
                    // Forward to daemon
                    nlohmann::json daemon_req;
                    daemon_req["jsonrpc"] = "2.0";
                    daemon_req["id"] = 1;
                    daemon_req["method"] = "tools/list";
                    daemon_req["params"] = nlohmann::json::object();

                    auto result_str = client.request(daemon_req.dump());
                    if (result_str) {
                        auto daemon_resp = nlohmann::json::parse(*result_str);
                        auto tools = daemon_resp.value("result", nlohmann::json::object()).value("tools", nlohmann::json::array());

                        nlohmann::json response;
                        response["jsonrpc"] = "2.0";
                        response["result"]["tools"] = tools;
                        response["id"] = request_id;
                        std::cout << response.dump() << std::endl;
                    }
                } else if (method == "tools/call") {
                    // Forward tool call to daemon
                    auto params = mcp_request.value("params", nlohmann::json::object());
                    std::string tool_name = params.value("name", "");
                    auto arguments = params.value("arguments", nlohmann::json::object());

                    // For session-aware tools, inject PPID for session lookup if session_id not provided
                    // In MCP bridge mode, getppid() returns Claude's PID
                    static const std::set<std::string> SESSION_TOOLS = {
                        "msg_inbox", "msg_send", "msg_ack", "msg_ack_all", "msg_history",
                        "ledger_save", "narrative_log", "narrative_history",
                        "anticipation_filter", "anticipation_gate_status",
                        "recall", "smart_recall", "hybrid_recall", "recall_lanes"
                    };
                    if (SESSION_TOOLS.count(tool_name) && !arguments.contains("session_id")) {
                        pid_t ppid = getppid();
                        if (ppid > 1) {  // Sanity check: not init
                            arguments["pid"] = static_cast<int64_t>(ppid);
                        }
                    }

                    nlohmann::json daemon_req;
                    daemon_req["jsonrpc"] = "2.0";
                    daemon_req["id"] = 1;
                    daemon_req["method"] = "tools/call";
                    daemon_req["params"]["name"] = tool_name;
                    daemon_req["params"]["arguments"] = arguments;

                    auto result_str = client.request(daemon_req.dump());
                    if (result_str) {
                        auto daemon_resp = nlohmann::json::parse(*result_str);
                        auto result = daemon_resp.value("result", nlohmann::json::object());

                        // Daemon already returns MCP-compatible content array
                        // Just pass it through
                        nlohmann::json response;
                        response["jsonrpc"] = "2.0";
                        response["result"]["content"] = result.value("content", nlohmann::json::array());
                        response["id"] = request_id;
                        std::cout << response.dump() << std::endl;
                    }
                } else {
                    // Unknown method
                    nlohmann::json response;
                    response["jsonrpc"] = "2.0";
                    response["error"]["code"] = -32601;
                    response["error"]["message"] = "Method not found: " + method;
                    response["id"] = request_id;
                    std::cout << response.dump() << std::endl;
                }
            } catch (const std::exception& e) {
                nlohmann::json error;
                error["jsonrpc"] = "2.0";
                error["error"]["code"] = -32700;
                error["error"]["message"] = e.what();
                error["id"] = nullptr;
                std::cout << error.dump() << std::endl;
            }
        }

        return 0;
    }

    // Check for CLI mode: tool is a known tool name
    if (!tool.empty() && KNOWN_TOOLS.count(tool)) {
        return run_cli(socket_path, tool, argc, argv, tool_arg_index + 1, output_format);
    }

    // No tool specified - run interactive mode or show usage
    if (tool.empty()) {
        return run_thin_client(socket_path);
    }

    // Unknown tool
    std::cerr << "Unknown option: " << tool << "\n";
    print_usage(argv[0]);
    return 1;
}
