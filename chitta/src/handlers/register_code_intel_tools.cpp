// register_code_intel_tools — chunk extracted from register_tools() so editing
// tool metadata only retemplates one chunk at a time.

#include "../../include/chitta/rpc/field_handler.hpp"

namespace chitta {

void FieldRpcHandler::register_code_intel_tools() {
    register_tool_table({
        {"extract_symbols", "Extract symbols from source file using tree-sitter",
            {{"type","object"},{"properties",{
                {"path",{{"type","string"},{"description","File path to analyze"}}}
            }},{"required",{"path"}}},
            &FieldRpcHandler::tool_extract_symbols, handlers_["extract_symbols"]},

        {"learn_codebase", "Learn codebase by extracting symbols. path can be a local directory or a remote git URL (https://github.com/..., git@github.com:...). Remote repos are shallow-cloned into a temp dir, indexed, then deleted.",
            {{"type","object"},{"properties",{
                {"path",{{"type","string"},{"description","Local path or remote git URL"}}},
                {"project",{{"type","string"},{"description","Project name (defaults to repo/dir name)"}}},
                {"branch",{{"type","string"},{"description","Branch, tag, or commit to clone (remote only)"}}},
                {"max_files",{{"type","integer"}}},{"exclude",{{"type","string"}}},
                {"incremental",{{"type","boolean"}}},{"force",{{"type","boolean"}}}
            }},{"required",{"path"}}},
            &FieldRpcHandler::tool_learn_codebase, handlers_["learn_codebase"]},

        {"find_symbol", "Search for symbols by name",
            {{"type","object"},{"properties",{
                {"name",{{"type","string"}}},{"kind",{{"type","string"}}}
            }},{"required",{"name"}}},
            &FieldRpcHandler::tool_find_symbol, handlers_["find_symbol"]},

        {"symbol_callers", "Find all symbols that call the given symbol",
            {{"type","object"},{"properties",{
                {"name",{{"type","string"}}},{"id",{{"type","integer"}}},
                {"kind",{{"type","string"}}},{"project",{{"type","string"}}}
            }}},
            &FieldRpcHandler::tool_symbol_callers, handlers_["symbol_callers"]},

        {"symbol_callees", "Find all symbols that the given symbol calls",
            {{"type","object"},{"properties",{
                {"name",{{"type","string"}}},{"id",{{"type","integer"}}},
                {"kind",{{"type","string"}}},{"project",{{"type","string"}}},
                {"path",{{"type","string"},{"description","Path substring to disambiguate duplicate names"}}}
            }}},
            &FieldRpcHandler::tool_symbol_callees, handlers_["symbol_callees"]},

        {"read_symbol", "Read actual source code for a symbol",
            {{"type","object"},{"properties",{
                {"name",{{"type","string"}}},{"id",{{"type","integer"}}},
                {"kind",{{"type","string"}}},{"project",{{"type","string"}}}
            }}},
            &FieldRpcHandler::tool_read_symbol, handlers_["read_symbol"]},

        {"read_function", "Read source of a function/method by name",
            {{"type","object"},{"properties",{
                {"name",{{"type","string"}}},{"project",{{"type","string"}}}
            }},{"required",{"name"}}},
            &FieldRpcHandler::tool_read_function, handlers_["read_function"]},

        {"search_symbols", "Semantic search for code symbols",
            {{"type","object"},{"properties",{
                {"query",{{"type","string"}}},{"kind",{{"type","string"}}},
                {"limit",{{"type","integer"}}},{"project",{{"type","string"}}}
            }},{"required",{"query"}}},
            &FieldRpcHandler::tool_search_symbols, handlers_["search_symbols"]},

        {"code_context", "Get code context summary",
            {{"type","object"},{"properties",{
                {"path",{{"type","string"}}}
            }}},
            &FieldRpcHandler::tool_code_context, handlers_["code_context"]},

        {"smart_context", "Build intelligent context combining memories, code, and graph",
            {{"type","object"},{"properties",{
                {"task",{{"type","string"}}},{"mode",{{"type","string"}}},
                {"limit",{{"type","integer"}}},{"memories",{{"type","boolean"}}},
                {"code",{{"type","boolean"}}},{"neighbors",{{"type","boolean"}}},
                {"realm",{{"type","string"}}}
            }},{"required",{"task"}}},
            &FieldRpcHandler::tool_smart_context, handlers_["smart_context"]},

        {"codebase_overview", "Get full indexed codebase structure",
            {{"type","object"},{"properties",{
                {"project",{{"type","string"}}},{"format",{{"type","string"}}},
                {"include_callsites",{{"type","boolean"}}}
            }}},
            &FieldRpcHandler::tool_codebase_overview, handlers_["codebase_overview"]},

        {"clear_codebase", "Remove all code intel data for a project",
            {{"type","object"},{"properties",{
                {"project",{{"type","string"}}},{"dry_run",{{"type","boolean"}}}
            }},{"required",{"project"}}},
            &FieldRpcHandler::tool_clear_codebase, handlers_["clear_codebase"]},

        {"clear_triplets", "Delete triplets by subject pattern",
            {{"type","object"},{"properties",{
                {"pattern",{{"type","string"}}},{"dry_run",{{"type","boolean"}}}
            }},{"required",{"pattern"}}},
            &FieldRpcHandler::tool_clear_triplets, handlers_["clear_triplets"]},

        {"resolve_callsites", "Resolve callsites to symbols for call graph",
            {{"type","object"},{"properties",{
                {"project",{{"type","string"}}}
            }}},
            &FieldRpcHandler::tool_resolve_callsites, handlers_["resolve_callsites"]},

        {"type_hierarchy", "Get type hierarchy for a type",
            {{"type","object"},{"properties",{
                {"name",{{"type","string"}}},{"direction",{{"type","string"}}}
            }},{"required",{"name"}}},
            &FieldRpcHandler::tool_type_hierarchy, handlers_["type_hierarchy"]},

        {"file_imports", "Get imports for a file",
            {{"type","object"},{"properties",{
                {"path",{{"type","string"}}}
            }},{"required",{"path"}}},
            &FieldRpcHandler::tool_file_imports, handlers_["file_imports"]},

        {"file_dependents", "Get files that import a module/file",
            {{"type","object"},{"properties",{
                {"module",{{"type","string"}}}
            }},{"required",{"module"}}},
            &FieldRpcHandler::tool_file_dependents, handlers_["file_dependents"]},

        {"embed_symbols", "Fast embed symbol metadata",
            {{"type","object"},{"properties",{
                {"batch_size",{{"type","integer"}}},{"reset",{{"type","boolean"}}}
            }}},
            &FieldRpcHandler::tool_embed_symbols, handlers_["embed_symbols"]},

        {"dedupe_symbols", "Remove duplicate symbols",
            {{"type","object"},{"properties",json::object()}},
            &FieldRpcHandler::tool_dedupe_symbols, handlers_["dedupe_symbols"]},

        {"describe_symbol", "Set description for a code symbol",
            {{"type","object"},{"properties",{
                {"symbol_id",{{"type","integer"}}},{"description",{{"type","string"}}}
            }},{"required",{"symbol_id","description"}}},
            &FieldRpcHandler::tool_describe_symbol, handlers_["describe_symbol"]},

        {"enrichment_status", "Get code enrichment progress",
            {{"type","object"},{"properties",json::object()}},
            &FieldRpcHandler::tool_enrichment_status, handlers_["enrichment_status"]},

        // ── System tools ────────────────────────────────────────────────────
        {"restore_code_intel_confidence", "Restore confidence for code intel memories",
            {{"type","object"},{"properties",{
                {"confidence",{{"type","number"}}},{"dry_run",{{"type","boolean"}}}
            }}},
            &FieldRpcHandler::tool_restore_code_intel_confidence, handlers_["restore_code_intel_confidence"]},

        // ── Theme tools ─────────────────────────────────────────────────────
    });
}

} // namespace chitta
