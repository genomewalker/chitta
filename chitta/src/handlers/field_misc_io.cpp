// field_misc.io — bodies extracted from field_misc.cpp by theme.
// Declarations remain in chitta/include/chitta/rpc/handlers/field_misc.hpp.

#include "../../include/chitta/rpc/field_handler.hpp"

namespace chitta {

ToolResult FieldRpcHandler::tool_ingest_source(const json& params) {
    if (!field_store_) return ToolResult::error("Field store unavailable");

    std::string source = params.value("source", "");
    if (source.empty()) return ToolResult::error("source is required");

    std::string realm = params.value("realm", "brahman");

    IngestConfig config;
    if (params.contains("model") && params["model"].is_string())
        config.model = params["model"].get<std::string>();
    if (params.contains("endpoint") && params["endpoint"].is_string())
        config.endpoint = params["endpoint"].get<std::string>();
    if (params.contains("max_chunks") && params["max_chunks"].is_number_integer())
        config.max_chunks = params["max_chunks"].get<int>();
    config.verbose = true;

    SourceType type = SourceType::Auto;
    std::string type_str = params.value("type", "auto");
    if (type_str == "url") type = SourceType::Url;
    else if (type_str == "file") type = SourceType::File;
    else if (type_str == "directory") type = SourceType::Directory;

    EmbedFn embedder = [this](const std::string& text) { return embed_text(text); };
    Ingester ingester(*field_store_, embedder, config);

    auto result = ingester.ingest(source, realm, type);

    if (!result.success) return ToolResult::error(result.error);

    return ToolResult::ok(
        "Ingested " + std::to_string(result.learnings_stored) + " learnings from " + source,
        {{"source", source},
         {"realm", realm},
         {"chunks_processed", result.chunks_processed},
         {"learnings_stored", result.learnings_stored},
         {"learnings_deduped", result.learnings_deduped},
         {"triplets_created", result.triplets_created}});
}

ToolResult FieldRpcHandler::tool_wiki_export(const json& params) {
    if (!field_store_) return ToolResult::error("Field store unavailable");

    WikiExportConfig config;
    if (params.contains("output_dir") && params["output_dir"].is_string())
        config.output_dir = params["output_dir"].get<std::string>();
    if (params.contains("realm") && params["realm"].is_string())
        config.realm = params["realm"].get<std::string>();
    if (params.contains("max_memories") && params["max_memories"].is_number_integer())
        config.max_memories = params["max_memories"].get<size_t>();

    WikiExporter exporter(*field_store_, config);
    auto result = exporter.export_all();

    if (!result.success) return ToolResult::error(result.error);

    return ToolResult::ok(
        "Exported " + std::to_string(result.memories_exported) + " memories to wiki",
        {{"pages_written", result.pages_written},
         {"memories_exported", result.memories_exported},
         {"backlinks_created", result.backlinks_created},
         {"output_dir", config.output_dir}});
}

ToolResult FieldRpcHandler::tool_health_check_start(const json& params) {
    if (!sadhana_manager_)
        return ToolResult::error("Sadhana manager not initialized");

    int interval = params.value("interval_seconds", 3600);
    std::string realm = params.value("realm", "brahman");
    int max_turns = params.value("max_turns", 0);

    json goal_dsl = {
        {"kind", "health_check"},
        {"checks", json::array({"memory_count", "dedup_ratio", "embedding_coverage",
                                 "stale_memories", "triplet_density"})}
    };

    std::string goal = "[health] Monitor memory quality in realm=" + realm;

    int64_t id = sadhana_manager_.load()->create(
        goal, "local", "gemma4:26b", interval, realm, goal_dsl, max_turns);

    if (id == 0) return ToolResult::error("Failed to create health-check sadhana");

    if (!sadhana_manager_.load()->start(id))
        return ToolResult::error("Created sadhana " + std::to_string(id) + " but failed to start");

    return ToolResult::ok("Started health-check sadhana " + std::to_string(id),
        {{"id", id}, {"state", "running"}, {"interval_seconds", interval}, {"realm", realm}});
}

ToolResult FieldRpcHandler::tool_export_training_pairs(const json& params) {
    if (!field_store_) return ToolResult::error("Field store unavailable");

    EmbeddingExportConfig config;
    if (params.contains("output_path") && params["output_path"].is_string())
        config.output_path = params["output_path"].get<std::string>();
    if (params.contains("realm") && params["realm"].is_string())
        config.realm = params["realm"].get<std::string>();
    if (params.contains("max_pairs") && params["max_pairs"].is_number_integer())
        config.max_pairs = params["max_pairs"].get<size_t>();
    if (params.contains("min_confidence") && params["min_confidence"].is_number())
        config.min_confidence = params["min_confidence"].get<float>();
    if (params.contains("include_negatives") && params["include_negatives"].is_boolean())
        config.include_negatives = params["include_negatives"].get<bool>();

    EmbedFn embedder = [this](const std::string& text) { return embed_text(text); };
    EmbeddingExporter exporter(*field_store_, embedder, config);
    auto result = exporter.export_pairs();

    if (!result.success) return ToolResult::error(result.error);

    return ToolResult::ok(
        "Exported " + std::to_string(result.pairs_exported) + " training pairs",
        {{"pairs_exported", result.pairs_exported},
         {"negatives_generated", result.negatives_generated},
         {"output_path", result.output_path}});
}
} // namespace chitta
