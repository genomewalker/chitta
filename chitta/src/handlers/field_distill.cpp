// field_distill RPC handlers — bodies for declarations in
// chitta/include/chitta/rpc/handlers/field_distill.hpp.

#include "../../include/chitta/rpc/field_handler.hpp"
#include <chitta/distillation.hpp>
#include <chitta/daemon_config.hpp>
#include <chitta/llm_http.hpp>

namespace chitta {

ToolResult FieldRpcHandler::tool_distill_status(const json&) {
    json result = {
        {"model",   get_distill_model()},
        {"enabled", get_distill_enabled()},
        {"think", distill_think_enabled()},
        {"backend", "chitta-field"},
    };
    return ToolResult::ok(
        "Distill model: " + get_distill_model() +
        (get_distill_enabled() ? " (enabled)" : " (disabled)") +
        (distill_think_enabled() ? " think=true" : " think=false"),
        result);
}

// Synchronous, on-demand distill of ONE session — force-and-observe.
// Runs the SAME lock-fixed path as the queue's process_distill: this handler is
// classified is_subprocess_tool (see is_subprocess_tool), so handle() holds NO
// rpc_mutex_ across it; run_distillation does the LLM pass lock-free and takes
// only the brief commit lock via acquire_lock(). Recall stays responsive.
ToolResult FieldRpcHandler::tool_distill_now(const json& params) {
    if (!get_distill_enabled())
        return ToolResult::error("distillation disabled");
    if (!field_store_)
        return ToolResult::error("no store");

    std::string session_id = params.value("session_id", "");
    if (session_id.empty())
        return ToolResult::error("session_id is required");

    // Transcript resolution mirrors QueueProcessor::process_distill: an explicit
    // transcript_path wins (and is registered durably so future distills track it);
    // otherwise resolve from the durable transcript/register event.
    std::string transcript_path = params.value("transcript_path", "");
    std::string realm = params.value("realm", "");
    if (!transcript_path.empty()) {
        json reg = {{"session_id", session_id},
                    {"transcript_path", transcript_path},
                    {"realm", realm.empty() ? "brahman" : realm}};
        field_store_->emit_event("transcript", "register", session_id, reg.dump());
    } else {
        auto regev = field_store_->get_latest_event("transcript", "register", session_id);
        if (regev) {
            try {
                auto r = json::parse(*regev);
                transcript_path = r.value("transcript_path", "");
                if (realm.empty()) realm = r.value("realm", "brahman");
            } catch (...) {}
        }
    }
    if (transcript_path.empty())
        return ToolResult::error("no transcript registered for " + session_id +
                                 " (pass transcript_path)");
    if (realm.empty()) realm = "brahman";

    // Catch up on new transcript bytes, then resume from durable progress watermark.
    field_store_->span_ingest(transcript_path);
    TranscriptState ts;
    ts.session_id = session_id;
    ts.transcript_path = transcript_path;
    ts.realm = realm;
    ts.last_processed_line = 0;
    auto prog = field_store_->get_latest_event("transcript", "progress", session_id);
    if (prog) {
        try { ts.last_processed_line = json::parse(*prog).value("last_line", (int64_t)0); }
        catch (...) {}
    }

    DistillConfig cfg;              // defaults; endpoint empty → NativeDistiller discovers GPU
    DistillResult res;
    bool ok = run_distillation(*field_store_, yantra_, ts, cfg, this, true, &res);

    int64_t lines_processed = res.last_line - ts.last_processed_line;
    if (lines_processed < 0) lines_processed = 0;
    bool capped = cfg.max_lines_per_pass > 0 && lines_processed >= cfg.max_lines_per_pass;

    json result = {
        {"success",             ok && res.success},
        {"session_id",          session_id},
        {"realm",               realm},
        {"learnings_stored",    res.learnings_stored},
        {"learnings_deduped",   res.learnings_deduped},
        {"value_facts_stored",  res.value_facts_stored},
        {"value_facts_deduped", res.value_facts_deduped},
        {"triplets_created",    res.triplets_created},
        {"last_line",           res.last_line},
        {"lines_processed",     lines_processed},
        {"capped",              capped},
    };
    if (!ok && !res.error.empty()) result["error"] = res.error;

    std::ostringstream ss;
    ss << (ok ? "distilled " : "distill FAILED ") << session_id
       << ": +" << res.learnings_stored << " learnings, +"
       << res.value_facts_stored << " value-facts (dedup "
       << res.value_facts_deduped << "), line " << res.last_line;
    return ToolResult::ok(ss.str(), result);
}

ToolResult FieldRpcHandler::tool_distill_set_model(const json& params) {
    std::string model = params.value("model", "");
    if (model.empty()) return ToolResult::error("model is required");

    set_distill_model(model);

    if (params.contains("enabled") && params["enabled"].is_boolean()) {
        set_distill_enabled(params["enabled"].get<bool>());
    }

    json result = {{"model", model}, {"enabled", get_distill_enabled()}};
    return ToolResult::ok("Distill model set to " + model, result);
}

ToolResult FieldRpcHandler::tool_suggestion_track(const json& params) {
    std::string content = params.value("content", "");
    if (content.empty()) return ToolResult::error("content is required");

    std::string realm = params.value("realm", "brahman");
    std::vector<float> embedding;
    if (params.contains("_preembedding")) {
        embedding = params["_preembedding"].get<std::vector<float>>();
    } else {
        embedding = embed_text(content);
    }
    uint64_t id = field_store_->remember("suggestion", realm, content, embedding, 0.7f, 0.01f);

    std::string id_str = std::to_string(id);
    return ToolResult::ok("Tracked suggestion #" + id_str, {{"id", id_str}});
}

ToolResult FieldRpcHandler::tool_suggestion_pending(const json& params) {
    size_t limit = static_cast<size_t>(params.value("limit", 20));
    auto hits = field_store_->recall_by_kind("suggestion", limit);

    json pending = json::array();
    for (const auto& h : hits) {
        if (h.confidence < 0.85f) {
            pending.push_back({
                {"id",         std::to_string(h.memory_id)},
                {"content",    h.content},
                {"confidence", h.confidence},
                {"realm",      h.realm},
            });
        }
    }

    std::ostringstream ss;
    ss << pending.size() << " pending suggestion(s)";
    return ToolResult::ok(ss.str(), {{"suggestions", pending}, {"count", pending.size()}});
}

ToolResult FieldRpcHandler::tool_suggestion_resolve(const json& params) {
    auto [id, id_str] = parse_id(params);
    if (id <= 0) return ToolResult::error("id is required");

    bool helped = params.value("helped", false);
    std::string details = params.value("details", "");

    field_store_->emit_event(
        "suggestion",
        helped ? "resolved_positive" : "resolved_negative",
        id_str,
        details);

    if (helped) {
        field_store_->strengthen(static_cast<uint64_t>(id), 0.15f);
    } else {
        field_store_->weaken(static_cast<uint64_t>(id), 0.1f);
    }

    json result = {
        {"id",      id_str},
        {"helped",  helped},
        {"outcome", helped ? "resolved_positive" : "resolved_negative"},
    };
    return ToolResult::ok("Suggestion #" + id_str + " resolved", result);
}

ToolResult FieldRpcHandler::tool_suggestion_count(const json&) {
    auto hits = field_store_->recall_by_kind("suggestion", 1000);
    size_t count = hits.size();
    return ToolResult::ok(std::to_string(count) + " suggestion(s) tracked",
        {{"count", count}});
}

ToolResult FieldRpcHandler::tool_consolidation_scan(const json& params) {
    size_t limit = static_cast<size_t>(params.value("limit", 20));
    std::string realm = params.value("realm", "");

    // chitta-field does not expose stored embeddings for pairwise comparison.
    // Return empty pairs with a diagnostic note.
    json result = {
        {"pairs",  json::array()},
        {"count",  0},
        {"note",   "Pairwise embedding comparison not available in chitta-field; "
                   "use consolidation_auto to trigger server-side scan"},
        {"scanned", limit},
    };
    return ToolResult::ok("Consolidation scan: 0 candidate pairs found (stub)", result);
}

ToolResult FieldRpcHandler::tool_consolidation_merge(const json& params) {
    auto [primary_id, primary_str] = parse_id(params, "primary_id");
    auto [secondary_id, secondary_str] = parse_id(params, "secondary_id");
    std::string merged_content = params.value("merged_content", "");

    if (primary_id <= 0 || secondary_id <= 0)
        return ToolResult::error("primary_id and secondary_id are required");
    if (merged_content.empty())
        return ToolResult::error("merged_content is required");
    if (field_store_->get_content(static_cast<uint64_t>(primary_id)).empty())
        return ToolResult::error("primary_id not found: " + primary_str);
    if (field_store_->get_content(static_cast<uint64_t>(secondary_id)).empty())
        return ToolResult::error("secondary_id not found: " + secondary_str);

    std::vector<float> embedding;
    if (params.contains("_preembedding")) {
        embedding = params["_preembedding"].get<std::vector<float>>();
    } else {
        embedding = embed_text(merged_content);
    }
    uint64_t new_id = field_store_->remember(
        "wisdom", "brahman", merged_content, embedding, 0.85f, 0.001f);

    field_store_->strengthen(static_cast<uint64_t>(primary_id), 0.1f);
    field_store_->forget(static_cast<uint64_t>(secondary_id));

    json result = {
        {"merged_id",    std::to_string(new_id)},
        {"primary_id",   primary_str},
        {"secondary_id", secondary_str},
        {"action",       "secondary forgotten, primary strengthened"},
    };
    return ToolResult::ok("Merged memories into #" + std::to_string(new_id), result);
}

ToolResult FieldRpcHandler::tool_consolidation_auto(const json& params) {
    float threshold = params.value("similarity_threshold", 0.92f);
    int max_merges  = params.value("max_merges", 10);

    field_store_->emit_event("consolidation", "auto_requested", "",
        "{\"threshold\":" + std::to_string(threshold) +
        ",\"max_merges\":" + std::to_string(max_merges) + "}");

    json result = {
        {"status",     "scheduled"},
        {"threshold",  threshold},
        {"max_merges", max_merges},
        {"note",       "Auto-consolidation event emitted; processed by next cycle"},
    };
    return ToolResult::ok("Auto-consolidation scheduled", result);
}

ToolResult FieldRpcHandler::tool_metacognition_corrections(const json& params) {
    size_t limit = static_cast<size_t>(params.value("limit", 20));
    auto hits = field_store_->recall_by_kind("correction", limit);

    json corrections = hits_to_results_json(hits);
    std::ostringstream ss;
    ss << corrections.size() << " correction(s) on record:\n";
    for (const auto& c : corrections) {
        ss << "  [" << c.value("id", "?") << "] "
           << c.value("text", "").substr(0, 80) << "\n";
    }

    return ToolResult::ok(ss.str(), {{"corrections", corrections}, {"count", corrections.size()}});
}

ToolResult FieldRpcHandler::tool_metacognition_outcomes(const json& params) {
    size_t limit = static_cast<size_t>(params.value("limit", 20));
    auto hits = field_store_->recall_keyword("suggestion outcome", limit);

    json outcomes = hits_to_results_json(hits);
    return ToolResult::ok(std::to_string(outcomes.size()) + " outcome record(s) found",
        {{"outcomes", outcomes}, {"count", outcomes.size()}});
}

ToolResult FieldRpcHandler::tool_metacognition_evaluate(const json&) {
    json stats_j;
    try {
        stats_j = json::parse(field_store_->memory_stats());
    } catch (...) {
        stats_j = json::object();
    }

    auto corrections = field_store_->recall_by_kind("correction", 100);
    auto suggestions = field_store_->recall_by_kind("suggestion", 100);

    size_t resolved_positive = 0;
    for (const auto& s : suggestions) {
        if (s.confidence >= 0.85f) resolved_positive++;
    }

    float suggestion_rate = suggestions.empty() ? 0.0f :
        static_cast<float>(resolved_positive) / static_cast<float>(suggestions.size());

    std::ostringstream ss;
    ss << "Self-Evaluation\n"
       << "══════════════\n"
       << "Total memories : " << field_store_->memory_count() << "\n"
       << "Corrections    : " << corrections.size() << "\n"
       << "Suggestions    : " << suggestions.size()
       << " (" << static_cast<int>(suggestion_rate * 100) << "% resolved positively)\n";
    if (stats_j.contains("avg_confidence")) {
        ss << "Avg confidence : " << std::fixed << std::setprecision(2)
           << stats_j.value("avg_confidence", 0.0f) << "\n";
    }

    json result = {
        {"memory_count",       field_store_->memory_count()},
        {"correction_count",   corrections.size()},
        {"suggestion_count",   suggestions.size()},
        {"suggestion_hit_rate", suggestion_rate},
        {"avg_confidence",     stats_j.value("avg_confidence", 0.0f)},
    };
    return ToolResult::ok(ss.str(), result);
}

ToolResult FieldRpcHandler::tool_epiplexity_check(const json& params) {
    std::string original     = params.value("original", "");
    std::string reconstructed = params.value("reconstructed", "");

    if (original.empty() || reconstructed.empty())
        return ToolResult::error("original and reconstructed are required");

    // Simple character-level similarity score
    size_t orig_len  = original.size();
    size_t recon_len = reconstructed.size();
    size_t min_len   = std::min(orig_len, recon_len);
    size_t matches   = 0;
    for (size_t i = 0; i < min_len; ++i) {
        if (original[i] == reconstructed[i]) matches++;
    }
    float char_sim = orig_len > 0 ? static_cast<float>(matches) / static_cast<float>(orig_len) : 0.0f;

    // Compression ratio: reconstructed / original length
    float compression_ratio = orig_len > 0
        ? static_cast<float>(recon_len) / static_cast<float>(orig_len)
        : 1.0f;

    // Estimate bits (approximate Shannon entropy at 1 byte/char)
    float compressed_bits = static_cast<float>(recon_len) * 8.0f;
    float score = char_sim * (1.0f - std::abs(compression_ratio - 1.0f) * 0.5f);
    score = std::max(0.0f, std::min(1.0f, score));

    json result = {
        {"score",             score},
        {"char_similarity",   char_sim},
        {"compression_ratio", compression_ratio},
        {"compressed_bits",   compressed_bits},
        {"original_len",      orig_len},
        {"reconstructed_len", recon_len},
    };
    std::ostringstream ss;
    ss << "Epiplexity score: " << std::fixed << std::setprecision(3) << score
       << " (char_sim=" << char_sim
       << ", compression=" << compression_ratio << ")";
    return ToolResult::ok(ss.str(), result);
}

ToolResult FieldRpcHandler::tool_ssl_convert(const json& params) {
    std::string content  = params.value("content", "");
    std::string domain   = params.value("domain", "note");
    std::string location = params.value("location", "");

    if (content.empty()) return ToolResult::error("content is required");

    std::string converted = to_ssl_format(content, domain, location);
    json result = {{"converted", converted}, {"already_ssl", converted == content}};
    return ToolResult::ok(converted, result);
}

ToolResult FieldRpcHandler::tool_curiosity_note_gap(const json& params) {
    std::string gap = params.value("gap", "");
    if (gap.empty()) return ToolResult::error("gap is required");

    std::string context = params.value("context", "");
    std::string realm   = params.value("realm", "brahman");

    std::string content = gap;
    if (!context.empty()) content = gap + "\nContext: " + context;

    std::vector<float> embedding;
    if (params.contains("_preembedding")) {
        embedding = params["_preembedding"].get<std::vector<float>>();
    } else {
        embedding = embed_text(content);
    }
    uint64_t id = field_store_->remember("question", realm, content, embedding, 0.7f, 0.0f);

    std::string id_str = std::to_string(id);
    return ToolResult::ok("Gap noted #" + id_str, {{"id", id_str}});
}

ToolResult FieldRpcHandler::tool_curiosity_gaps(const json& params) {
    size_t limit  = static_cast<size_t>(params.value("limit", 20));
    std::string realm = params.value("realm", "");

    auto hits = field_store_->recall_by_kind("question", limit);

    json gaps = json::array();
    for (const auto& h : hits) {
        if (!realm.empty() && h.realm != realm) continue;
        gaps.push_back({
            {"id",         std::to_string(h.memory_id)},
            {"gap",        h.content},
            {"confidence", h.confidence},
            {"realm",      h.realm},
        });
    }

    std::ostringstream ss;
    ss << gaps.size() << " open gap(s)";
    return ToolResult::ok(ss.str(), {{"gaps", gaps}, {"count", gaps.size()}});
}

ToolResult FieldRpcHandler::tool_curiosity_resolve(const json& params) {
    auto [id, id_str] = parse_id(params);
    if (id <= 0) return ToolResult::error("id is required");
    if (field_store_->get_content(static_cast<uint64_t>(id)).empty())
        return ToolResult::error("gap not found: " + id_str);

    std::string learned = params.value("learned", "");

    field_store_->emit_event("curiosity", "resolved", id_str, learned);
    field_store_->strengthen(static_cast<uint64_t>(id), 0.2f);

    json result = {{"id", id_str}, {"status", "resolved"}};
    if (!learned.empty()) result["learned"] = learned;
    return ToolResult::ok("Gap #" + id_str + " resolved", result);
}

} // namespace chitta
