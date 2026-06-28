// field_memory_recall RPC handlers — bodies for declarations in
// chitta/include/chitta/rpc/handlers/field_memory_recall.hpp.
#include <ctime>
#include <iomanip>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include "chitta/speech_act.hpp"
#include "chitta/ssl_gloss.hpp"

#include "../../include/chitta/rpc/field_handler.hpp"

namespace {
// Parse "YYYY-MM-DD" → Unix epoch ms, or 0 on failure.
int64_t parse_date_ms(const std::string& s) {
    if (s.size() < 10) return 0;
    std::tm t{};
    t.tm_year = std::stoi(s.substr(0, 4)) - 1900;
    t.tm_mon  = std::stoi(s.substr(5, 2)) - 1;
    t.tm_mday = std::stoi(s.substr(8, 2));
    t.tm_isdst = -1;
    std::time_t epoch = timegm(&t);
    return epoch < 0 ? 0 : static_cast<int64_t>(epoch) * 1000;
}
} // namespace

namespace chitta {

ToolResult FieldRpcHandler::tool_remember(const json& params) {
    std::string content = params.value("content", "");
    if (content.empty()) return ToolResult::error("content is required");

    if (sandbox::is_sandboxed()) {
        std::string dead_id = sandbox::dead_letter_write(
            failed_queue_path_, queue_fail_count_, "remember", params);
        return ToolResult::ok(
            "Sandboxed write diverted to dead-letter queue",
            {{"sandboxed", true}, {"dead_lettered_id", dead_id}, {"tool", "remember"}});
    }

    std::string kind  = params.value("type", "episode");
    std::string realm = params.value("realm", "brahman");
    float confidence  = params.value("confidence", 0.8f);
    float decay_rate  = 0.001f;

    // Code-intel kinds get zero decay
    static const std::unordered_set<std::string> code_kinds = {
        "symbol", "projectessence", "modulestate", "patternstate"
    };
    if (code_kinds.count(kind)) decay_rate = 0.0f;

    // Auto speech-act classification for episode memories
    if (kind == "episode") {
        if (auto act = classify_speech_act(content)) kind = *act;
    }

    // Always pass empty embedding — backfill thread will embed asynchronously.
    // Calling embed_ssl_aware() here was inside the exclusive rpc_mutex_ lock,
    // blocking all readers for the full llama.cpp inference duration.
    std::vector<float> embedding;

    int64_t authored_at_ms = 0;
    if (params.contains("valid_from")) {
        std::string vf = params.value("valid_from", "");
        if (!vf.empty()) authored_at_ms = parse_date_ms(vf);
    }
    uint64_t id = field_store_->remember(kind, realm, content, embedding, confidence, decay_rate,
                                         authored_at_ms);
    // Notify backfill thread: a new pending memory was just stored.
    fire_write_notify();

    // Phase 3: for SSL memories, create a pure-NL alias memory so the gloss
    // gets its own embedding in NL space (higher cosine vs natural-language queries).
    // Alias kind is excluded from default recall results but is searchable.
    // SSL alias: store with empty embedding — backfill thread will embed it.
    // Previously called embed_text() here inside the write lock.
    static const std::string ssl_arrow = "\xe2\x86\x92";
    if (id > 0 && kind != "alias" && content.find(ssl_arrow) != std::string::npos) {
        auto gloss = chitta::ssl::gloss_ssl_content(content);
        if (!gloss.empty() && gloss != content) {
            uint64_t alias_id = field_store_->remember(
                "alias", realm, gloss, {}, confidence, decay_rate);
            if (alias_id > 0) {
                field_store_->add_triplet(
                    std::to_string(alias_id), "alias-of", std::to_string(id));
            }
        }
    }

    // Tag with source_session if provided (used by recall_session grouping)
    if (id > 0 && params.contains("source_session")) {
        std::string ss = params.value("source_session", "");
        if (!ss.empty()) field_store_->set_source_session(id, ss);
    }

    // Create triplets from tags if provided (array or comma-separated string)
    if (params.contains("tags")) {
        if (params["tags"].is_array()) {
            for (const auto& tag : params["tags"]) {
                if (tag.is_string()) {
                    field_store_->add_triplet(
                        std::to_string(id), "tagged", tag.get<std::string>());
                }
            }
        } else if (params["tags"].is_string()) {
            std::istringstream iss(params["tags"].get<std::string>());
            std::string tag;
            while (std::getline(iss, tag, ',')) {
                tag.erase(0, tag.find_first_not_of(' '));
                tag.erase(tag.find_last_not_of(' ') + 1);
                if (!tag.empty()) field_store_->add_triplet(std::to_string(id), "tagged", tag);
            }
        }
    }

    // Apply visibility if provided (0=Private default, 1=Shared, 2=Global)
    int visibility = params.value("visibility", 0);
    if (visibility > 0) {
        std::string id_str_vis = std::to_string(id);
        try {
            field_store_->emit_event("realm", "visibility", "memory:" + id_str_vis,
                                     std::to_string(visibility));
        } catch (...) {}
    }

    // Post-store contradiction detection (fast path: no embedding needed)
    json contradiction_hits = json::array();
    if (id > 0) {
        try {
            std::string cjson = field_store_->detect_contradictions(id, realm);
            auto parsed = json::parse(cjson, nullptr, false);
            if (!parsed.is_discarded() && parsed.is_array() && !parsed.empty())
                contradiction_hits = std::move(parsed);
        } catch (...) {}
    }

    std::string id_str = std::to_string(id);
    json result_data = {{"id", id_str}, {"type", kind}, {"realm", realm}};
    if (!contradiction_hits.empty())
        result_data["contradictions"] = contradiction_hits;
    return ToolResult::ok("Stored memory #" + id_str, result_data);
}

ToolResult FieldRpcHandler::tool_remember_batch(const json& params) {
    if (!params.contains("items") || !params["items"].is_array())
        return ToolResult::error("items array is required");

    if (sandbox::is_sandboxed())
        return ToolResult::error("remember_batch not available in sandbox mode");

    const auto& items = params["items"];
    json ids = json::array();
    int stored = 0;

    for (const auto& item : items) {
        std::string content = item.value("content", "");
        if (content.empty()) { ids.push_back(nullptr); continue; }

        std::string kind  = item.value("type", "episode");
        std::string realm = item.value("realm", params.value("realm", "brahman"));
        float confidence  = item.value("confidence", 0.8f);
        float decay_rate  = 0.001f;

        static const std::unordered_set<std::string> code_kinds = {
            "symbol", "projectessence", "modulestate", "patternstate"
        };
        if (code_kinds.count(kind)) decay_rate = 0.0f;
        if (kind == "episode") {
            if (auto act = classify_speech_act(content)) kind = *act;
        }

        // Pass empty embedding — backfill thread embeds asynchronously.
        std::vector<float> embedding;

        int64_t authored_at_ms = 0;
        if (item.contains("valid_from")) {
            std::string vf = item.value("valid_from", "");
            if (!vf.empty()) authored_at_ms = parse_date_ms(vf);
        }

        uint64_t id = field_store_->remember(kind, realm, content, embedding,
                                             confidence, decay_rate, authored_at_ms);
        if (id == 0) { ids.push_back(nullptr); continue; }

        if (item.contains("source_session")) {
            std::string ss = item.value("source_session", "");
            if (!ss.empty()) field_store_->set_source_session(id, ss);
        }

        if (item.contains("tags") && item["tags"].is_array()) {
            for (const auto& tag : item["tags"]) {
                if (tag.is_string())
                    field_store_->add_triplet(std::to_string(id), "tagged",
                                             tag.get<std::string>());
            }
        }

        ids.push_back(std::to_string(id));
        stored++;
    }

    return ToolResult::ok("Stored " + std::to_string(stored) + " memories",
                          {{"ids", ids}, {"stored", stored}});
}

ToolResult FieldRpcHandler::tool_flush_embeddings(const json& /*params*/) {
    if (!subconscious_) return ToolResult::ok("ok", {{"flushed", 0}, {"note", "no embedder"}});
    size_t n = subconscious_->flush_embedding_queue();
    return ToolResult::ok("Flushed " + std::to_string(n) + " embeddings", {{"flushed", (int)n}});
}

ToolResult FieldRpcHandler::tool_recall(const json& params) {
    std::string query = params.value("query", "");
    if (query.empty()) return ToolResult::error("query is required");

    size_t limit         = static_cast<size_t>(params.value("limit", 10));
    std::string realm    = params.value("realm", "");
    std::string tag      = params.value("tag", "");
    std::string strategy = params.value("strategy", "");
    bool expand          = params.value("expand", true);

    // Field-RAG / Modern Hopfield mode: bypass RRF, run DAM relaxation.
    if (strategy == "field") {
        auto emb = params.contains("_preembedding")
            ? params["_preembedding"].get<std::vector<float>>()
            : embed_query(query);
        if (!emb.empty()) {
            auto hits = field_store_->recall_field(emb, query, limit, realm);
            hits.erase(
                std::remove_if(hits.begin(), hits.end(),
                    [](const FieldRecallHit& h) { return h.content.empty(); }),
                hits.end());
            json results = hits_to_results_json(hits, false);
            return ToolResult::ok(std::to_string(hits.size()) + " field memories", {{"results", results}});
        }
    }

    // Fetch more than needed so tag filtering has candidates to work with
    size_t fetch_limit = tag.empty() ? limit : limit * 8;

    // Multi-lane RRF: original query (2×, weighted) + SSL-shaped variants + BM25
    // Works directly on FieldRecallHit to preserve Hebbian/temporal scoring metadata.
    std::vector<FieldRecallHit> hits;
    if (expand && query_has_entities(query)) {
        std::vector<std::string> forms = {query, query}; // original 2× = boosted weight
        for (auto& v : chitta::ssl::ssl_query_variants(query)) forms.push_back(v);

        // RRF over FieldRecallHit lanes — preserves full scoring metadata
        std::unordered_map<uint64_t, float>         rrf_scores;
        std::unordered_map<uint64_t, FieldRecallHit> best_hit;
        const float kRRF = 60.0f;

        auto rrf_lane = [&](const std::vector<FieldRecallHit>& lane) {
            int rank = 1;
            for (const auto& h : lane) {
                rrf_scores[h.memory_id] += 1.0f / (kRRF + rank);
                if (best_hit.find(h.memory_id) == best_hit.end())
                    best_hit[h.memory_id] = h;
                ++rank;
            }
        };

        std::vector<float> base_emb;
        if (params.contains("_preembedding"))
            base_emb = params["_preembedding"].get<std::vector<float>>();
        // Only attempt semantic lanes if we have a working embedding (base_emb non-empty).
        // When yantra is unavailable, skip SSL-variant embed calls (each costs a full timeout).
        if (!base_emb.empty()) {
            for (const auto& f : forms) {
                auto emb = (f == query) ? base_emb : embed_query(f);
                if (emb.empty()) continue;
                rrf_lane(field_store_->recall(emb, std::min(fetch_limit, (size_t)20), realm));
            }
        }
        // BM25 lane
        rrf_lane(field_store_->recall_keyword(query, std::min(fetch_limit, (size_t)20), realm));
        // HDC lane (skipped when disable_hdc=true for ablation/benchmarking)
        if (!params.value("disable_hdc", false)) {
            rrf_lane(field_store_->recall_hdc(query, std::min(fetch_limit, (size_t)20), realm));
        }

        // Collect sorted by RRF score, keep original hit metadata
        std::vector<std::pair<float, uint64_t>> ranked;
        ranked.reserve(rrf_scores.size());
        for (auto& [id, score] : rrf_scores) ranked.emplace_back(score, id);
        std::sort(ranked.begin(), ranked.end(), std::greater<>());

        // Normalize RRF scores to [0,1] so display_pct shows meaningful percentages.
        // Raw RRF values are ~1/(60+rank) ≈ 0.016 — meaningless as relevance.
        float max_rrf = ranked.empty() ? 1.0f : ranked[0].first;
        if (max_rrf < 1e-6f) max_rrf = 1.0f;

        for (auto& [score, id] : ranked) {
            if (hits.size() >= fetch_limit) break;
            auto& h = best_hit[id];
            if (!h.content.empty()) {
                h.score = score / max_rrf;
                hits.push_back(std::move(h));
            }
        }
    } else {
        // _preembedding is set only when pre-embed succeeded (certainty > 0).
        // Never call embed_query() here — that's inside rpc_mutex_ and blocks readers.
        if (params.contains("_preembedding")) {
            auto emb = params["_preembedding"].get<std::vector<float>>();
            hits = field_store_->recall(emb, fetch_limit, realm);
        } else {
            hits = field_store_->recall_keyword(query, fetch_limit);
        }
    }

    // Filter orphaned HNSW entries (deleted payloads with lingering vectors)
    hits.erase(
        std::remove_if(hits.begin(), hits.end(),
            [](const FieldRecallHit& h) { return h.content.empty(); }),
        hits.end());

    // Hard-filter by tag: keep only memories that have (id, "tagged", tag) triplet
    if (!tag.empty()) {
        std::string triplets_json = field_store_->query_object(tag);
        std::unordered_set<uint64_t> tagged_ids;
        try {
            auto tj = json::parse(triplets_json);
            for (const auto& t : tj) {
                if (t.value("predicate", "") == "tagged") {
                    try { tagged_ids.insert(std::stoull(t.value("subject", "0"))); }
                    catch (...) {}
                }
            }
        } catch (...) {}

        if (!tagged_ids.empty()) {
            // Filter semantic hits to tagged set
            hits.erase(
                std::remove_if(hits.begin(), hits.end(),
                    [&](const FieldRecallHit& h) { return tagged_ids.find(h.memory_id) == tagged_ids.end(); }),
                hits.end());

            // If no semantic hits matched tags, fetch tagged memories directly
            if (hits.empty()) {
                for (uint64_t tid : tagged_ids) {
                    if (hits.size() >= limit) break;
                    std::string content = field_store_->get_content(tid);
                    if (content.empty()) continue;
                    std::string meta_json = field_store_->get_memory_metadata(tid);
                    FieldRecallHit h;
                    h.memory_id    = tid;
                    h.score        = 1.0f;
                    h.semantic_score = 0.0f;
                    h.content      = content;
                    try {
                        auto m = json::parse(meta_json);
                        h.ts_ms        = m.value("ts_ms", int64_t(0));
                        h.strength     = m.value("strength", 0.5f);
                        h.confidence   = m.value("confidence", 0.5f);
                        h.access_count = m.value("access_count", uint32_t(0));
                        h.kind         = m.value("kind", "episode");
                        h.realm        = m.value("realm", "");
                    } catch (...) {}
                    // Realm scoping: this direct tagged-memory fetch bypasses the recall lanes,
                    // so filter here too — never surface a tagged memory from another realm.
                    if (!realm.empty() && h.realm != realm) continue;
                    hits.push_back(std::move(h));
                }
            }

            if (hits.size() > limit) hits.resize(limit);
        }
    }

    // Hebbian co-occurrence: strengthen associations between co-retrieved memories
    if (hits.size() >= 2) {
        std::vector<uint64_t> ids;
        ids.reserve(hits.size());
        for (const auto& h : hits) ids.push_back(h.memory_id);
        field_store_->record_co_retrieval(ids);
    }

    // Drift scoring: anti-perseveration penalty + curiosity boost
    {
        const size_t total = field_store_->memory_count();
        if (total >= 5 && !hits.empty()) {
            const float max_access = 10.0f;
            const float exploration = 1.0f;
            for (auto& h : hits) {
                float saturation = std::min(1.0f, static_cast<float>(h.access_count) / max_access);
                float anti_perserv = 1.0f - 0.25f * saturation;
                float curiosity = (total > 0)
                    ? exploration * std::sqrt(std::log(static_cast<float>(total) + 1.0f) /
                                              (static_cast<float>(h.access_count) + 1.0f))
                    : 0.0f;
                float curiosity_mul = 1.0f + std::min(curiosity, 0.3f);
                h.score = h.score * anti_perserv * curiosity_mul;
            }
            std::sort(hits.begin(), hits.end(),
                      [](const FieldRecallHit& a, const FieldRecallHit& b) {
                          return a.score > b.score;
                      });
        }
    }

    bool explain = params.value("explain", false);
    json results_json = hits_to_results_json(hits, explain);

    // Abstain signal: if no candidate clears the calibrated relevance bar, say so honestly
    // instead of presenting a weak best-of-a-bad-batch as if confident. relevance(cos) =
    // sigma(3.27*cos - 0.85) (the same Platt calibration the Rust scorer uses); tau=0.45 <=>
    // centered cosine 0.20 (~1.85 sigma above the random-pair spread). Room room-f366bf5a.
    float max_rel = 0.0f;
    for (const auto& h : hits)
        max_rel = std::max(max_rel,
                           1.0f / (1.0f + std::exp(-(3.27f * h.semantic_score - 0.85f))));
    bool weak = !hits.empty() && max_rel < 0.45f;

    std::ostringstream ss;
    if (weak)
        ss << "[weak: no strongly-relevant memory (max relevance "
           << static_cast<int>(max_rel * 100) << "%); results may be tangential]\n";
    ss << "Found " << hits.size() << " results";
    if (!realm.empty()) ss << " in realm '" << realm << "'";
    ss << ":\n";
    for (const auto& r : results_json) {
        int pct = display_pct(r);
        ss << "[" << pct << "%] [" << r.value("type", "?") << "]";
        int64_t ts = r.value("ts_ms", int64_t(0));
        if (ts > 0) {
            std::time_t t = static_cast<std::time_t>(ts / 1000);
            std::tm* tm = std::gmtime(&t);
            char buf[16];
            std::strftime(buf, sizeof(buf), "%Y-%m-%d", tm);
            ss << " (on: " << buf << ")";
        }
        ss << " " << r.value("text", "").substr(0, 400) << "\n";
    }

    std::string recall_status = results_json.empty() ? "empty" : (weak ? "weak" : "ok");
    auto result = ToolResult::ok(ss.str(), {{"results", results_json}, {"realm", realm},
                                            {"status", recall_status},
                                            {"abstain", weak}, {"max_relevance", max_rel}});
    fire_recall_callback(results_json, 1);
    return result;
}

ToolResult FieldRpcHandler::tool_recall_temporal(const json& params) {
    std::string query = params.value("query", "");
    std::string realm = params.value("realm", "");
    size_t limit      = static_cast<size_t>(params.value("limit", 20));

    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    int64_t start_ms = 0, end_ms = 0;
    if (params.contains("start")) {
        auto ts = parse_timestamp_str(params["start"].get<std::string>());
        if (ts) start_ms = *ts;
    }
    if (params.contains("end")) {
        auto ts = parse_timestamp_str(params["end"].get<std::string>());
        if (ts) end_ms = *ts;
    }

    // Default: last 7 days when neither specified
    if (start_ms == 0 && end_ms == 0) {
        end_ms = now_ms;
        start_ms = end_ms - (7LL * 24 * 3600 * 1000);
    }
    // If only start specified, default end to now
    if (end_ms == 0) {
        end_ms = now_ms;
    }

    auto hits = field_store_->recall_temporal(start_ms, end_ms, limit, realm);

    // If query provided, re-rank by semantic similarity
    if (!query.empty() && !hits.empty()) {
        auto qemb = embed_query(query);
        if (!qemb.empty()) {
            auto semantic_hits = field_store_->recall(qemb, limit, realm);
            semantic_hits.erase(
                std::remove_if(semantic_hits.begin(), semantic_hits.end(),
                    [](const FieldRecallHit& h) { return h.content.empty(); }),
                semantic_hits.end());
            json temporal_json = hits_to_results_json(hits);
            json semantic_json = hits_to_results_json(semantic_hits);
            json merged = merge_results(temporal_json, semantic_json);
            if (merged.size() > limit) merged.erase(merged.begin() + static_cast<int>(limit), merged.end());

            std::ostringstream ss;
            ss << "Found " << merged.size() << " temporal+semantic results:\n";
            for (const auto& r : merged) {
                ss << "[" << r.value("type", "?") << "] " << r.value("text", "").substr(0, 400) << "\n";
            }
            return ToolResult::ok(ss.str(), {{"results", merged}, {"count", merged.size()}, {"realm", realm}});
        }
    }

    json results_json = hits_to_results_json(hits);
    std::ostringstream ss;
    ss << "Found " << hits.size() << " memories:\n";
    for (const auto& r : results_json) {
        ss << "[" << r.value("type", "?") << "] " << r.value("text", "").substr(0, 400) << "\n";
    }
    return ToolResult::ok(ss.str(), {{"results", results_json}, {"count", hits.size()}, {"realm", realm}});
}

ToolResult FieldRpcHandler::tool_recall_temporal_events(const json& params) {
    size_t limit = static_cast<size_t>(params.value("limit", 20));
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    int64_t start_ms = 0, end_ms = now_ms;
    if (params.contains("start")) {
        auto ts = parse_timestamp_str(params["start"].get<std::string>());
        if (ts) start_ms = *ts;
    }
    if (params.contains("end")) {
        auto ts = parse_timestamp_str(params["end"].get<std::string>());
        if (ts) end_ms = *ts;
    }
    if (start_ms == 0) start_ms = end_ms - (7LL * 24 * 3600 * 1000);

    auto hits = field_store_->recall_temporal_events(start_ms, end_ms, limit);
    json results_json = hits_to_results_json(hits);
    std::ostringstream ss;
    ss << "Found " << hits.size() << " memories via EventTape entities:\n";
    for (const auto& r : results_json) {
        ss << "[" << r.value("type", "?") << "] " << r.value("text", "").substr(0, 400) << "\n";
    }
    return ToolResult::ok(ss.str(), {{"results", results_json}, {"count", hits.size()}});
}

ToolResult FieldRpcHandler::tool_recall_keyword(const json& params) {
    std::string query = params.value("query", "");
    if (query.empty()) return ToolResult::error("query is required");
    size_t k = static_cast<size_t>(params.value("limit", 10));

    auto hits = field_store_->recall_keyword(query, k);
    bool explain = params.value("explain", false);
    json results_json = hits_to_results_json(hits, explain);

    std::ostringstream ss;
    ss << "Found " << hits.size() << " keyword results for '" << query << "':\n";
    for (const auto& r : results_json) {
        int pct = display_pct(r);
        ss << "[" << pct << "%] " << r.value("text", "").substr(0, 400) << "\n";
    }
    return ToolResult::ok(ss.str(), {{"results", results_json}});
}

ToolResult FieldRpcHandler::tool_hybrid_recall(const json& params) {
    std::string query = params.value("query", "");
    if (query.empty()) return ToolResult::error("query is required");

    size_t limit      = static_cast<size_t>(params.value("limit", 10));
    std::string realm = params.value("realm", "");

    auto embedding = embed_query(query);

    std::vector<FieldRecallHit> semantic_hits;
    if (!embedding.empty()) {
        semantic_hits = field_store_->recall(embedding, limit, realm);
        semantic_hits.erase(
            std::remove_if(semantic_hits.begin(), semantic_hits.end(),
                [](const FieldRecallHit& h) { return h.content.empty(); }),
            semantic_hits.end());
    }
    auto keyword_hits  = field_store_->recall_keyword(query, limit, realm);

    // Drift scoring on each source's raw hits (no Hebbian — merged sources would over-count)
    {
        const size_t total = field_store_->memory_count();
        if (total >= 5) {
            auto apply_drift = [&](std::vector<FieldRecallHit>& hits) {
                const float max_access = 10.0f;
                const float exploration = 1.0f;
                for (auto& h : hits) {
                    float saturation = std::min(1.0f, static_cast<float>(h.access_count) / max_access);
                    float anti_perserv = 1.0f - 0.25f * saturation;
                    float curiosity = exploration * std::sqrt(std::log(static_cast<float>(total) + 1.0f) /
                                                              (static_cast<float>(h.access_count) + 1.0f));
                    float curiosity_mul = 1.0f + std::min(curiosity, 0.3f);
                    h.score = h.score * anti_perserv * curiosity_mul;
                }
            };
            apply_drift(semantic_hits);
            apply_drift(keyword_hits);
        }
    }

    bool explain = params.value("explain", false);
    json semantic = hits_to_results_json(semantic_hits, explain);
    json keyword  = hits_to_results_json(keyword_hits, explain);
    json merged   = merge_results(semantic, keyword);

    if (merged.size() > limit) merged.erase(merged.begin() + static_cast<int>(limit), merged.end());

    std::ostringstream ss;
    ss << "Hybrid recall: " << merged.size() << " results\n";
    for (const auto& r : merged) {
        int pct = display_pct(r);
        ss << "[" << pct << "%] " << r.value("text", "").substr(0, 400) << "\n";
    }
    auto result = ToolResult::ok(ss.str(), {{"results", merged}, {"realm", realm}});
    fire_recall_callback(merged, 1);
    return result;
}

ToolResult FieldRpcHandler::tool_smart_recall(const json& params) {
    std::string query = params.value("query", "");
    if (query.empty()) return ToolResult::error("query is required");

    size_t limit       = static_cast<size_t>(params.value("limit", 20));
    std::string realm  = params.value("realm", "");

    // Ask route learner for best retrieval strategy
    auto route_sel = field_store_->select_route(query);
    uint64_t episode_id = route_sel.episode_id;
    uint8_t  route      = route_sel.route;
    // 0=Semantic, 1=Keyword, 2=Temporal, 3=Artifact, 4=Hybrid, 5=Full

    auto eq = expand_query(query);
    bool is_code = looks_like_code_query(query);
    if (is_code) route = 1;  // code queries always keyword

    // Drift scoring lambda
    auto apply_drift_smart = [&](std::vector<FieldRecallHit>& hits) {
        const size_t total = field_store_->memory_count();
        if (total < 5 || hits.empty()) return;
        const float max_access = 10.0f;
        const float exploration = 1.0f;
        for (auto& h : hits) {
            float saturation = std::min(1.0f, static_cast<float>(h.access_count) / max_access);
            float anti_perserv = 1.0f - 0.25f * saturation;
            float curiosity = exploration * std::sqrt(std::log(static_cast<float>(total) + 1.0f) /
                                                      (static_cast<float>(h.access_count) + 1.0f));
            float curiosity_mul = 1.0f + std::min(curiosity, 0.3f);
            h.score = h.score * anti_perserv * curiosity_mul;
        }
    };

    json results;
    static const char* route_names[] = {"semantic","keyword","temporal","artifact","hybrid","full"};
    std::string route_name = route < 6 ? route_names[route] : "hybrid";

    if (route == 1) {  // Keyword
        auto kw_hits = field_store_->recall_keyword(eq.lex, limit);
        apply_drift_smart(kw_hits);
        results = hits_to_results_json(kw_hits);
    } else if (route == 2) {  // Temporal
        int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        auto temp_hits = field_store_->recall_temporal(now_ms - (int64_t)30*24*3600*1000, now_ms, limit, realm);
        apply_drift_smart(temp_hits);
        results = hits_to_results_json(temp_hits);
    } else {  // Semantic / Hybrid / Full / Artifact — all use semantic+keyword merge
        auto embedding = embed_query(eq.vec);
        if (embedding.empty()) {
            // Fallback to keyword if embedding fails
            auto kw_hits = field_store_->recall_keyword(eq.lex, limit);
            apply_drift_smart(kw_hits);
            results = hits_to_results_json(kw_hits);
            field_store_->route_feedback(episode_id, -0.1f);  // slight penalty for forced fallback
            episode_id = 0;  // skip normal feedback
        } else {
            auto sem_hits = field_store_->recall(embedding, limit, realm);
            sem_hits.erase(
                std::remove_if(sem_hits.begin(), sem_hits.end(),
                    [](const FieldRecallHit& h) { return h.content.empty(); }),
                sem_hits.end());
            if (route == 0) {  // Semantic only
                apply_drift_smart(sem_hits);
                results = hits_to_results_json(sem_hits);
            } else {  // Hybrid / Full
                auto kw_hits = field_store_->recall_keyword(eq.lex, limit);
                apply_drift_smart(sem_hits);
                apply_drift_smart(kw_hits);
                results = merge_results(hits_to_results_json(sem_hits), hits_to_results_json(kw_hits));
            }
        }
    }

    if (results.size() > limit) results.erase(results.begin() + static_cast<int>(limit), results.end());

    // Auto-expand top results
    size_t expand_top = static_cast<size_t>(params.value("expand_top", 2));
    if (expand_top > 0 && !results.empty()) {
        for (size_t i = 0; i < std::min(expand_top, results.size()); ++i) {
            std::string content = field_store_->get_content(
                std::stoull(results[i].value("id", "0")));
            if (!content.empty()) {
                results[i]["full_text"] = content;
            }
        }
    }

    std::ostringstream ss;
    ss << "Smart recall (" << route_name << ", ep=" << episode_id << "): " << results.size() << " results\n";
    for (const auto& r : results) {
        int pct = display_pct(r);
        ss << "[" << pct << "%] [" << r.value("type", "?") << "] "
           << r.value("text", "").substr(0, 400) << "\n";
    }
    auto result = ToolResult::ok(ss.str(), {{"results", results}, {"intent", is_code ? "code" : "semantic"}});
    fire_recall_callback(results, 1);
    return result;
}

ToolResult FieldRpcHandler::tool_recall_session(const json& params) {
    std::string query = params.value("query", "");
    if (query.empty()) return ToolResult::error("query is required");

    size_t k          = static_cast<size_t>(params.value("limit", 10));
    std::string realm = params.value("realm", "");

    auto embedding = embed_query(query);
    auto hits = field_store_->recall_session(embedding, query, k, realm);

    json results = json::array();
    for (const auto& h : hits) {
        results.push_back({
            {"session_id",      h.session_id},
            {"score",           h.score},
            {"chunk_count",     h.chunk_count},
            {"max_chunk_score", h.max_chunk_score},
            {"best_evidence",   h.best_evidence},
        });
    }

    std::ostringstream ss;
    ss << "Session recall: " << hits.size() << " sessions for '" << query << "'\n";
    for (const auto& res : results) {
        int pct = static_cast<int>(res.value("score", 0.0f) * 100);
        ss << "[" << pct << "%] [" << res.value("chunk_count", 0) << " chunks] "
           << res.value("session_id", "?") << "\n"
           << "  " << res.value("best_evidence", std::string{}).substr(0, 200) << "\n";
    }
    return ToolResult::ok(ss.str(), {{"results", results}, {"realm", realm}});
}

ToolResult FieldRpcHandler::tool_recall_spreading(const json& params) {
    if (!params.contains("query"))
        return ToolResult::error("query required");
    std::string query = params.value("query", "");
    size_t      limit = static_cast<size_t>(params.value("limit", 10));
    std::string realm = params.value("realm", "");

    auto hits = field_store_->recall_spreading(query, limit, realm);
    json results = json::array();
    for (auto& h : hits) {
        results.push_back({
            {"memory_id", h.memory_id},
            {"score",     h.score},
            {"text",      h.text},
            {"kind",      h.kind},
            {"realm",     h.realm},
        });
    }
    return ToolResult::ok(json{{"results", results}}.dump());
}

ToolResult FieldRpcHandler::tool_full_resonate(const json& params) {
    std::string query = params.value("query", "");
    if (query.empty()) return ToolResult::error("query is required");

    size_t k          = static_cast<size_t>(params.value("k", 10));
    std::string realm = params.value("realm", "");

    auto q0 = embed_query(query);
    if (q0.empty()) {
        // Embed queue saturated or model unavailable — fall back to keyword-only.
        auto kw_hits = field_store_->recall_keyword(query, k);
        json kw_json = hits_to_results_json(kw_hits);
        std::ostringstream ss;
        ss << "Found " << kw_json.size() << " results (keyword fallback, embed unavailable):\n";
        for (const auto& r : kw_json) {
            int pct = static_cast<int>(r.value("relevance", 0.0f) * 100);
            ss << "[" << pct << "%] " << r.value("text", "").substr(0, 400) << "\n";
        }
        return ToolResult::ok(ss.str(), {{"results", kw_json}, {"passes", 0}, {"fallback", "keyword"}});
    }

    auto query_embedding = q0;  // mutable copy refined each pass
    float prev_entropy = -1.0f;
    std::vector<uint64_t> prev_top_ids;
    json final_merged;
    int passes_run = 0;

    for (int pass = 0; pass < kMaxResonancePasses; ++pass) {
        passes_run = pass + 1;

        // Run existing resonance phases
        auto resonate_sem = field_store_->recall(query_embedding, k, realm);
        resonate_sem.erase(
            std::remove_if(resonate_sem.begin(), resonate_sem.end(),
                [](const FieldRecallHit& h) { return h.content.empty(); }),
            resonate_sem.end());
        json semantic = hits_to_results_json(resonate_sem);
        json keyword  = hits_to_results_json(field_store_->recall_keyword(query, k));
        json merged   = merge_results(semantic, keyword);
        if (merged.size() > k) merged.erase(merged.begin() + static_cast<int>(k), merged.end());
        final_merged = merged;

        // Collect top-k IDs and scored pairs for entropy
        std::vector<uint64_t> cur_top_ids;
        std::vector<std::pair<uint64_t, float>> cur_scored;
        for (const auto& r : merged) {
            uint64_t mid = 0;
            try { mid = std::stoull(r.value("id", "0")); } catch (...) {}
            if (mid != 0) {
                cur_top_ids.push_back(mid);
                cur_scored.emplace_back(mid, r.value("relevance", 0.0f));
            }
        }

        // Early stop check
        float cur_entropy = score_entropy(cur_scored);
        bool ids_unchanged = (cur_top_ids == prev_top_ids);
        bool entropy_converged = (prev_entropy >= 0.0f &&
                                  std::abs(cur_entropy - prev_entropy) < kEntropyStopDelta);
        prev_entropy = cur_entropy;
        prev_top_ids = cur_top_ids;

        if (pass == kMaxResonancePasses - 1 || ids_unchanged || entropy_converged) {
            // Final pass — record recall batch for Hebbian learning
            if (!cur_top_ids.empty() && field_store_->handle()) {
                std::vector<int8_t> centroid_q;
                float centroid_scale;
                project_to_sketch(q0, centroid_q, centroid_scale);
                uint64_t ctx_hash = query_context_hash(q0, realm);
                int64_t ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                cf_record_recall_batch(
                    field_store_->handle(),
                    cur_top_ids.data(), cur_top_ids.size(),
                    centroid_q.data(), centroid_q.size(),
                    centroid_scale,
                    ctx_hash,
                    ts_ms,
                    kBaseAssocDelta
                );
            }
            break;
        }

        // Refine query embedding for next pass: fetch top-k embeddings
        if (!cur_top_ids.empty() && field_store_->handle()) {
            std::vector<char> emb_buf(cur_top_ids.size() * q0.size() * sizeof(float) + 4096);
            size_t emb_written = 0;
            int emb_rc = cf_get_memory_embeddings_batch(
                field_store_->handle(),
                cur_top_ids.data(), cur_top_ids.size(),
                emb_buf.data(), emb_buf.size(), &emb_written);

            if (emb_rc == 0 && emb_written > 0) {
                // Parse JSON result: array of {id, embedding: [f32...]}
                auto emb_json = json::parse(
                    std::string_view(emb_buf.data(), emb_written), nullptr, false);
                if (!emb_json.is_discarded() && emb_json.is_array() && !emb_json.empty()) {
                    // Compute mean embedding
                    std::vector<float> mean_emb(q0.size(), 0.0f);
                    size_t emb_count = 0;
                    for (const auto& entry : emb_json) {
                        if (entry.contains("embedding") && entry["embedding"].is_array()) {
                            const auto& evec = entry["embedding"];
                            if (evec.size() == q0.size()) {
                                for (size_t d = 0; d < q0.size(); ++d) {
                                    mean_emb[d] += evec[d].get<float>();
                                }
                                ++emb_count;
                            }
                        }
                    }
                    if (emb_count > 0) {
                        for (float& v : mean_emb) v /= static_cast<float>(emb_count);
                        // Blend: q_next = alpha * q0 + (1-alpha) * mean
                        for (size_t d = 0; d < query_embedding.size(); ++d) {
                            query_embedding[d] = kResonanceAlpha * q0[d]
                                               + (1.0f - kResonanceAlpha) * mean_emb[d];
                        }
                        // Normalize
                        float norm = 0.0f;
                        for (float v : query_embedding) norm += v * v;
                        norm = std::sqrt(norm);
                        if (norm > 1e-9f) {
                            for (float& v : query_embedding) v /= norm;
                        }
                    }
                }
            }
        }
    }

    if (recall_callback_ && !prev_top_ids.empty()) {
        recall_callback_(prev_top_ids, passes_run);
    }

    std::ostringstream ss;
    ss << "Found " << final_merged.size() << " results (" << passes_run << " pass"
       << (passes_run > 1 ? "es" : "") << "):\n";
    for (const auto& r : final_merged) {
        int pct = display_pct(r);
        ss << "[" << pct << "%] [" << r.value("type", "?") << "] "
           << r.value("text", "").substr(0, 400) << "\n";
    }
    return ToolResult::ok(ss.str(), {{"results", final_merged}, {"passes", passes_run}});
}

ToolResult FieldRpcHandler::tool_route_stats(const json&) {
    // Returns route learner statistics via smart_recall diagnostic
    // Since we can't directly inspect the learner, we use the route episode
    // tracking: count how many times each route was selected recently
    // by querying the structured_recall episode metadata from last N calls.
    // For now: return a placeholder showing the route learner is active.
    json stats;
    stats["status"] = "active";
    stats["description"] = "Route learner is Thompson-sampling over 6 routes (Semantic/Keyword/Temporal/Artifact/Hybrid/Full)";
    stats["note"] = "Use smart_recall with different queries to observe route selection. Episode IDs are returned in structured result.";
    stats["routes"] = json::array({"Semantic","Keyword","Temporal","Artifact","Hybrid","Full"});
    std::string text = "Route learner: active, 6-arm Thompson sampling\nUse smart_recall to observe route selection via episode_id in results";
    return ToolResult::ok(text, stats);
}

// ── CEC: Event tape + CDAWG ──────────────────────────────────────────────────

ToolResult FieldRpcHandler::tool_log_event(const json& params) {
    std::string tool   = params.value("tool", "");
    std::string entity = params.value("entity", "");
    if (tool.empty() || entity.empty())
        return ToolResult::error("tool and entity are required");
    uint8_t outcome    = static_cast<uint8_t>(params.value("outcome", 0));
    uint64_t session   = static_cast<uint64_t>(params.value("session_id", 0));
    int64_t  ts_ms     = static_cast<int64_t>(params.value("ts_ms", 0));
    field_store_->log_event(tool, entity, outcome, session, ts_ms);
    return ToolResult::ok("event logged");
}

ToolResult FieldRpcHandler::tool_log_event_ex(const json& params) {
    std::string tool   = params.value("tool", "");
    std::string entity = params.value("entity", "");
    if (tool.empty() || entity.empty())
        return ToolResult::error("tool and entity are required");
    uint8_t  outcome     = static_cast<uint8_t>(params.value("outcome", 0));
    uint64_t session     = static_cast<uint64_t>(params.value("session_id", 0));
    int64_t  ts_ms       = static_cast<int64_t>(params.value("ts_ms", 0));
    uint32_t token_cost  = static_cast<uint32_t>(params.value("token_cost", 0));
    uint32_t latency_ms  = static_cast<uint32_t>(params.value("latency_ms", 0));
    uint8_t  retry_count = static_cast<uint8_t>(params.value("retry_count", 0));
    field_store_->log_event_ex(tool, entity, outcome, session, ts_ms, token_cost, latency_ms, retry_count);
    return ToolResult::ok("event logged");
}

ToolResult FieldRpcHandler::tool_log_decision(const json& params) {
    std::string chosen_tool   = params.value("chosen_tool", "");
    std::string chosen_entity = params.value("chosen_entity", "");
    if (chosen_tool.empty() || chosen_entity.empty())
        return ToolResult::error("chosen_tool and chosen_entity are required");
    uint8_t  chosen_outcome   = static_cast<uint8_t>(params.value("chosen_outcome", 0));
    std::string rejected_json = params.value("rejected_json", "[]");
    float confidence_delta    = params.value("confidence_delta", 0.0f);
    int64_t ts_ms             = static_cast<int64_t>(params.value("ts_ms", 0));
    field_store_->log_decision(chosen_tool, chosen_entity, chosen_outcome, rejected_json, confidence_delta, ts_ms);
    return ToolResult::ok("decision logged");
}

ToolResult FieldRpcHandler::tool_recall_last_action(const json& params) {
    std::string tool   = params.value("tool", "");
    std::string entity = params.value("entity", "");
    size_t k           = static_cast<size_t>(params.value("k", 5));
    auto hits = field_store_->recall_last_action(tool, entity, k);
    std::ostringstream ss;
    ss << "Last " << hits.size() << " occurrences of " << tool << " on " << entity << ":\n";
    json arr = json::array();
    for (const auto& h : hits) {
        ss << "  " << h.content << "\n";
        json item; item["content"] = h.content; item["ts_ms"] = h.ts_ms;
        arr.push_back(std::move(item));
    }
    return ToolResult::ok(ss.str(), {{"hits", arr}});
}

ToolResult FieldRpcHandler::tool_recall_failure_pattern(const json& params) {
    size_t k = static_cast<size_t>(params.value("k", 5));
    std::string raw = field_store_->recall_failure_pattern_json(k);
    auto parsed = json::parse(raw, nullptr, false);
    json arr = (parsed.is_array()) ? parsed : json::array();
    std::ostringstream ss;
    ss << "Top " << arr.size() << " failure patterns:\n";
    json out = json::array();
    for (const auto& p : arr) {
        ss << "  " << p.value("content", "") << "\n";
        json item;
        item["content"]    = p.value("content", "");
        item["fail_ratio"] = p.value("fail_ratio", 0.0f);
        item["fail_count"] = p.value("fail_count", (uint64_t)0);
        item["ts_ms"]      = p.value("ts_ms", (int64_t)0);
        out.push_back(std::move(item));
    }
    return ToolResult::ok(ss.str(), {{"patterns", out}});
}

ToolResult FieldRpcHandler::tool_recall_causal_antecedent(const json& params) {
    std::string tool   = params.value("tool", "");
    std::string entity = params.value("entity", "");
    size_t k           = static_cast<size_t>(params.value("k", 5));
    if (tool.empty() || entity.empty())
        return ToolResult::error("tool and entity are required");
    std::string raw = field_store_->recall_causal_antecedent_json(tool, entity, k);
    auto parsed = json::parse(raw, nullptr, false);
    json arr = (parsed.is_array()) ? parsed : json::array();
    std::ostringstream ss;
    ss << "PMI-ranked causal antecedents for " << tool << " on " << entity << ":\n";
    json out = json::array();
    for (const auto& p : arr) {
        ss << "  " << p.value("content", "") << "\n";
        json item;
        item["content"] = p.value("content", "");
        item["pmi"]     = p.value("pmi", 0.0f);
        item["count"]   = p.value("count", (uint64_t)0);
        out.push_back(std::move(item));
    }
    return ToolResult::ok(ss.str(), {{"antecedents", out}});
}

ToolResult FieldRpcHandler::tool_recall_hdcbind(const json& params) {
    std::string known_role = params.value("known_role", "");
    std::string known_val  = params.value("known_val", "");
    std::string query_role = params.value("query_role", "");
    size_t k               = static_cast<size_t>(params.value("k", 5));
    if (known_role.empty() || known_val.empty() || query_role.empty())
        return ToolResult::error("known_role, known_val, and query_role are required");
    std::string raw = field_store_->recall_hdcbind_json(known_role, known_val, query_role, k);
    auto parsed = json::parse(raw, nullptr, false);
    json arr = parsed.is_array() ? parsed : json::array();
    std::ostringstream ss;
    ss << "HDC bind query: given " << known_role << "=" << known_val
       << ", infer " << query_role << ":\n";
    json out = json::array();
    for (const auto& p : arr) {
        ss << "  " << p.value("content", "") << "\n";
        json item;
        item["name"]       = p.value("name", "");
        item["similarity"] = p.value("similarity", 0.0f);
        item["content"]    = p.value("content", "");
        out.push_back(std::move(item));
    }
    return ToolResult::ok(ss.str(), {{"results", out}});
}

ToolResult FieldRpcHandler::tool_recall_counterfactual(const json& params) {
    std::string tool   = params.value("tool", "");
    std::string entity = params.value("entity", "");
    uint8_t outcome    = static_cast<uint8_t>(params.value("outcome", 1));
    size_t k           = static_cast<size_t>(params.value("k", 5));
    if (tool.empty() || entity.empty())
        return ToolResult::error("tool and entity are required");
    std::string raw = field_store_->recall_counterfactual_json(tool, entity, outcome, k);
    auto parsed = json::parse(raw, nullptr, false);
    json arr = parsed.is_array() ? parsed : json::array();
    std::ostringstream ss;
    ss << "Counterfactual alternatives for " << tool << "(" << entity << ") outcome="
       << (int)outcome << ":\n";
    for (const auto& p : arr) {
        ss << "  " << p.value("content", "") << "\n";
    }
    return ToolResult::ok(ss.str(), {{"alternatives", arr}});
}

ToolResult FieldRpcHandler::tool_consolidation_pass(const json& params) {
    bool preview_only = params.value("preview", false);
    if (preview_only) {
        size_t k = static_cast<size_t>(params.value("k", 5));
        std::string raw = field_store_->consolidation_preview_json(k);
        auto parsed = json::parse(raw, nullptr, false);
        json arr = parsed.is_array() ? parsed : json::array();
        std::ostringstream ss;
        ss << "Top-" << k << " consolidation rules (no writes):\n";
        for (const auto& item : arr) {
            ss << "  " << item.value("key","?") << " (×" << item.value("support",0) << ")\n";
        }
        return ToolResult::ok(ss.str(), {{"rules", arr}});
    }

    // Binary-level self-guard (launcher-independent). consolidation_pass is an
    // is_subprocess_tool: it runs in the daemon WITHOUT rpc_mutex_ while holding
    // the Rust CDAWG write lock for the whole Sequitur+FEP rebuild (tens of
    // minutes on a large store). Nothing serialized concurrent callers, so a
    // Stop/PreCompact hook firing this RPC on every session boundary could pile
    // up N simultaneous passes — each contending the CDAWG lock, blocking
    // log_event and resource-starving recall. The launchers live in hook copies
    // scattered across plugin versions; guarding each is unenforceable. This is
    // the single choke point every caller funnels through, so the guard is here.
    bool disabled = std::getenv("CHITTA_DISABLE_CONSOLIDATION") != nullptr;
    if (!disabled && !mind_path_.empty()) {
        std::error_code ec;
        disabled = std::filesystem::exists(
            std::filesystem::path(mind_path_) / ".disable_consolidation", ec);
    }
    if (disabled) {
        return ToolResult::ok("Consolidation disabled (marker/env) — skipped.",
                              {{"skipped", "disabled"}});
    }
    static std::atomic<bool> consolidation_in_flight{false};
    bool expected = false;
    if (!consolidation_in_flight.compare_exchange_strong(expected, true)) {
        return ToolResult::ok("Consolidation already running — skipped (single-instance guard).",
                              {{"skipped", "already_running"}});
    }
    struct InFlightGuard {
        std::atomic<bool>& flag;
        ~InFlightGuard() { flag.store(false, std::memory_order_release); }
    } inflight_guard{consolidation_in_flight};

    std::string raw = field_store_->consolidation_pass_json();
    auto parsed = json::parse(raw, nullptr, false);
    size_t found    = parsed.is_object() ? parsed.value("rules_found",    0) : 0;
    size_t promoted = parsed.is_object() ? parsed.value("rules_promoted", 0) : 0;
    std::string msg = "Consolidation pass complete: " + std::to_string(found)
                    + " rules found, " + std::to_string(promoted) + " promoted to KG.";
    // Return a sample key to verify queryability
    std::string preview_raw = field_store_->consolidation_preview_json(1);
    auto prev = json::parse(preview_raw, nullptr, false);
    std::string sample_key = (prev.is_array() && !prev.empty()) ? prev[0].value("key","") : "";
    json out;
    out["rules_found"]    = found;
    out["rules_promoted"] = promoted;
    out["sample_key"]     = sample_key;
    return ToolResult::ok(msg, {{"result", out}});
}

ToolResult FieldRpcHandler::tool_refutation_stats(const json& params) {
    size_t k = static_cast<size_t>(params.value("k", 10));
    std::string stats = field_store_->refutation_stats_json(k);
    return ToolResult::ok(stats, {{"stats", stats}});
}

ToolResult FieldRpcHandler::tool_recall_motif_value(const json& params) {
    std::string tool   = params.value("tool",   "");
    std::string entity = params.value("entity", "");
    size_t k           = static_cast<size_t>(params.value("k", 5));
    if (tool.empty() || entity.empty())
        return ToolResult::error("tool and entity are required");
    std::string raw = field_store_->recall_motif_value_json(tool, entity, k);
    auto parsed = json::parse(raw, nullptr, false);
    std::ostringstream ss;
    if (parsed.is_array() && !parsed.empty()) {
        ss << "Top-" << parsed.size() << " motif states from " << tool << "(" << entity << "):\n";
        for (const auto& h : parsed) {
            ss << "  " << h.value("content", "") << "\n";
        }
    } else {
        ss << "No motif data yet for " << tool << "(" << entity << ") — accumulate more events.";
    }
    return ToolResult::ok(ss.str(), {{"hits", parsed.is_array() ? parsed : json::array()}});
}

ToolResult FieldRpcHandler::tool_executor_flush(const json& /*params*/) {
    std::string raw = field_store_->executor_flush_json();
    auto parsed = json::parse(raw, nullptr, false);
    std::ostringstream ss;
    if (parsed.is_object()) {
        auto promoted = parsed.value("promoted", json::array());
        auto demoted  = parsed.value("demoted",  json::array());
        ss << "executor_flush: promoted=" << promoted.size()
           << " demoted=" << demoted.size();
        if (auto store = parsed.find("store"); store != parsed.end())
            ss << " " << store->dump();
    } else {
        ss << raw;
    }
    return ToolResult::ok(ss.str(), {{"result", parsed.is_object() ? parsed : json::object()}});
}

ToolResult FieldRpcHandler::tool_list_policies(const json& params) {
    bool active_only = params.value("active_only", false);
    std::string raw  = field_store_->list_policies_json(active_only);
    auto parsed = json::parse(raw, nullptr, false);
    std::ostringstream ss;
    if (parsed.is_array()) {
        ss << "policies (" << parsed.size() << "):\n";
        for (const auto& p : parsed) {
            ss << "  [" << (p.value("active", false) ? "active" : "shadow") << "]"
               << " id=" << p.value("id", 0)
               << " rule=" << p.value("rule", 0)
               << " " << p.value("kind", "")
               << " shadow_events=" << p.value("shadow_events", 0)
               << " lift=" << p.value("lift", 0.0)
               << "\n";
        }
        if (parsed.empty()) ss << "  (none)\n";
    } else {
        ss << raw;
    }
    return ToolResult::ok(ss.str(), {{"policies", parsed.is_array() ? parsed : json::array()}});
}

ToolResult FieldRpcHandler::tool_recall_true_counterfactual(const json& params) {
    std::string tool   = params.value("tool",    "");
    std::string entity = params.value("entity",  "");
    uint8_t outcome    = static_cast<uint8_t>(params.value("outcome", 0));
    size_t k           = static_cast<size_t>(params.value("k", 5));
    if (tool.empty() || entity.empty())
        return ToolResult::error("tool and entity are required");
    std::string raw = field_store_->recall_true_counterfactual_json(tool, entity, outcome, k);
    auto parsed = json::parse(raw, nullptr, false);
    std::ostringstream ss;
    if (parsed.is_array() && !parsed.empty()) {
        ss << "True counterfactuals for " << tool << "(" << entity << ") — "
           << parsed.size() << " decision points where this was rejected:\n";
        for (const auto& h : parsed) {
            ss << "  " << h.value("content", "") << "\n";
        }
    } else {
        ss << "No decision-tape entries yet for " << tool << "(" << entity
           << ") — log decisions via log_event_ex to populate.";
    }
    return ToolResult::ok(ss.str(), {{"hits", parsed.is_array() ? parsed : json::array()}});
}

ToolResult FieldRpcHandler::tool_hypothesis_probes(const json& params) {
    size_t k = static_cast<size_t>(params.value("k", 10));
    std::string raw = field_store_->hypothesis_probes_json(k);
    auto parsed = json::parse(raw, nullptr, false);
    std::ostringstream ss;
    if (parsed.is_object()) {
        auto top = parsed.value("top_k", json::array());
        ss << "hypothesis_probes: total=" << parsed.value("total", 0)
           << " top_" << top.size() << "=[\n";
        for (const auto& h : top) {
            ss << "  rule_" << h.value("rule_id", 0)
               << ": p_hat=" << h.value("p_hat", 0.0)
               << " wilson=[" << h.value("wilson_lower", 0.0)
               << "," << h.value("wilson_upper", 1.0) << "]"
               << " probe_value=" << h.value("probe_value", 0.0) << "\n";
        }
        ss << "]";
    } else {
        ss << raw;
    }
    return ToolResult::ok(ss.str(), {{"result", parsed.is_object() ? parsed : json::object()}});
}

ToolResult FieldRpcHandler::tool_turiya_status(const json& /*params*/) {
    std::string raw = field_store_->turiya_status_json();
    auto parsed = json::parse(raw, nullptr, false);
    std::ostringstream ss;
    if (parsed.is_object()) {
        ss << "turiya_status: diagnosis=" << parsed.value("diagnosis", "unknown")
           << " trend=" << parsed.value("trend", "unknown")
           << " samples=" << parsed.value("samples", 0) << "\n";
        if (parsed.contains("latest")) {
            const auto& l = parsed["latest"];
            ss << "  cdawg_states=" << l.value("cdawg_states", 0)
               << " tape_events=" << l.value("tape_events", 0)
               << " tracked_rules=" << l.value("tracked_rules", 0)
               << " refuted_rules=" << l.value("refuted_rules", 0) << "\n"
               << "  hypotheses=" << l.value("hypotheses", 0)
               << " mean_probe=" << l.value("mean_probe_value", 0.0)
               << " q_variance=" << l.value("q_variance", 0.0) << "\n"
               << "  delta_states=" << l.value("delta_states", 0)
               << " delta_events=" << l.value("delta_events", 0);
        }
        if (parsed.contains("recent_diagnoses")) {
            ss << "\n  recent=" << parsed["recent_diagnoses"].dump();
        }
    } else {
        ss << raw;
    }
    return ToolResult::ok(ss.str(), {{"result", parsed.is_object() ? parsed : json::object()}});
}

ToolResult FieldRpcHandler::tool_tape_stats(const json& /*params*/) {
    std::string raw = field_store_->tape_stats_json();
    auto parsed = json::parse(raw, nullptr, false);
    std::ostringstream ss;
    if (parsed.is_object()) {
        long long events   = parsed.value("events", 0);
        long long tomb     = parsed.value("tombstoned_total", 0);
        long long total    = events + tomb;
        float ratio = total > 0 ? (float)tomb / total * 100.0f : 0.0f;
        ss << "tape_stats: events=" << events
           << " tombstoned=" << tomb
           << " compression=" << std::fixed << std::setprecision(1) << ratio << "%"
           << " sessions=" << parsed.value("sessions", 0)
           << " tools=" << parsed.value("tool_count", 0)
           << " entities=" << parsed.value("entity_count", 0)
           << " failures=" << parsed.value("failure_events", 0);
    } else {
        ss << raw;
    }
    return ToolResult::ok(ss.str(), {{"result", parsed.is_object() ? parsed : json::object()}});
}

ToolResult FieldRpcHandler::tool_verbalize_rules(const json& params) {
    size_t k = static_cast<size_t>(params.value("k", 10));
    std::string raw = field_store_->verbalize_rules_json(k);
    auto parsed = json::parse(raw, nullptr, false);
    std::ostringstream ss;
    if (parsed.is_object()) {
        auto rules = parsed.value("rules", json::array());
        ss << "verbalize_rules: " << rules.size() << " rules\n";
        for (const auto& r : rules) {
            ss << "  [rule_" << r.value("rule_id", 0)
               << " ×" << r.value("support", 0)
               << " " << r.value("avg_outcome", "?") << "]\n"
               << "  " << r.value("text", "") << "\n";
        }
    } else {
        ss << raw;
    }
    return ToolResult::ok(ss.str(), {{"result", parsed.is_object() ? parsed : json::object()}});
}

ToolResult FieldRpcHandler::tool_queue_experiments(const json& params) {
    size_t k = static_cast<size_t>(params.value("k", 5));
    std::string raw = field_store_->queue_experiments_json(k);
    auto parsed = json::parse(raw, nullptr, false);
    std::ostringstream ss;
    if (parsed.is_object()) {
        int queued   = parsed.value("queued", 0);
        int refuted  = parsed.value("skipped_refuted", 0);
        int certain  = parsed.value("skipped_certain", 0);
        ss << "queue_experiments: queued=" << queued
           << " skipped_refuted=" << refuted
           << " skipped_certain=" << certain;
        if (queued == 0) ss << " (no uncertain rules above threshold)";
    } else {
        ss << raw;
    }
    return ToolResult::ok(ss.str(), {{"result", parsed.is_object() ? parsed : json::object()}});
}

ToolResult FieldRpcHandler::tool_fep_status(const json& /*params*/) {
    std::string raw = field_store_->fep_status_json();
    auto parsed = json::parse(raw, nullptr, false);
    std::ostringstream ss;
    if (parsed.is_object()) {
        uint64_t obs    = parsed.value("obs_count",    (uint64_t)0);
        double drift    = parsed.value("ewma_drift",   0.0);
        double shock    = parsed.value("ewma_shock",   0.0);
        int    states   = parsed.value("states_modeled", 0);
        bool   cd       = parsed.value("context_drift",  false);
        bool   es       = parsed.value("emission_shock", false);
        ss << "FEP prior organ  obs=" << obs
           << "  states_modeled=" << states
           << "  drift=" << std::fixed << std::setprecision(4) << drift
           << "  shock=" << shock;
        if (cd) ss << "  [CONTEXT_DRIFT]";
        if (es) ss << "  [EMISSION_SHOCK]";
    } else {
        ss << raw;
    }
    return ToolResult::ok(ss.str(), {{"result", parsed.is_object() ? parsed : json::object()}});
}

ToolResult FieldRpcHandler::tool_routed_recall(const json& params) {
    // Build RecallRequest JSON from tool params.
    json req = json::object();
    if (params.contains("subject"))       req["subject"]       = params["subject"];
    if (params.contains("predicate"))     req["predicate"]     = params["predicate"];
    if (params.contains("freetext"))      req["freetext"]      = params["freetext"];
    if (params.contains("realm"))         req["realm"]         = params["realm"];
    if (params.contains("causal_tool"))   req["causal_tool"]   = params["causal_tool"];
    if (params.contains("causal_entity")) req["causal_entity"] = params["causal_entity"];
    if (params.contains("time_from_ms"))  req["time_from_ms"]  = params["time_from_ms"];
    if (params.contains("time_to_ms"))    req["time_to_ms"]    = params["time_to_ms"];
    req["k"] = params.value("k", 10);

    std::string raw = field_store_->routed_recall_json(req.dump());
    auto parsed = json::parse(raw, nullptr, false);
    std::ostringstream ss;
    if (parsed.is_object()) {
        std::string dispatch = parsed.value("dispatch", "unknown");
        int cost = parsed.value("token_cost", 0);
        auto hits = parsed.value("hits", json::array());
        ss << "routed_recall: dispatch=" << dispatch
           << "  token_cost=" << cost
           << "  hits=" << hits.size() << "\n";
        if (dispatch == "needs_disambiguation") {
            for (auto& slot : parsed.value("unbound_slots", json::array()))
                ss << "  unbound: " << slot.value("slot","?")
                   << " — " << slot.value("context","") << "\n";
        } else {
            int i = 0;
            for (auto& h : hits) {
                if (i++ >= 5) { ss << "  ...\n"; break; }
                if (h.contains("content"))
                    ss << "  • " << h.value("content","").substr(0,120) << "\n";
                else if (h.contains("object"))
                    ss << "  • " << h.value("subject","") << " → "
                       << h.value("predicate","") << " → " << h.value("object","") << "\n";
            }
        }
    } else {
        ss << raw;
    }
    return ToolResult::ok(ss.str(), {{"result", parsed.is_object() ? parsed : json::object()}});
}

ToolResult FieldRpcHandler::tool_witness_memory(const json& params) {
    uint64_t memory_id = params.value("memory_id", uint64_t(0));
    std::string witness_kind = params.value("witness_kind", "");
    if (memory_id == 0 || witness_kind.empty())
        return ToolResult::error("memory_id and witness_kind required");
    std::string raw = field_store_->witness_memory_json(memory_id, witness_kind);
    auto parsed = json::parse(raw, nullptr, false);
    bool ok = parsed.is_object() && parsed.value("ok", false);
    std::string status = parsed.is_object() ? parsed.value("status", "unknown") : raw;
    std::ostringstream ss;
    ss << (ok ? "witness_memory: " : "witness_memory error: ") << status
       << "  memory_id=" << memory_id << "  witness=" << witness_kind;
    return ToolResult::ok(ss.str(), {{"result", parsed.is_object() ? parsed : json::object()}});
}

ToolResult FieldRpcHandler::tool_reconcile_pass(const json& /*params*/) {
    std::string raw = field_store_->reconcile_pass_json();
    auto parsed = json::parse(raw, nullptr, false);
    std::ostringstream ss;
    if (parsed.is_object()) {
        ss << "reconcile_pass:"
           << "  illegal_edges=" << parsed.value("illegal_edges", 0)
           << "  contradictions=" << parsed.value("contradictions", 0)
           << "  unresolved="     << parsed.value("unresolved", 0);
    } else { ss << raw; }
    return ToolResult::ok(ss.str(), {{"result", parsed.is_object() ? parsed : json::object()}});
}

ToolResult FieldRpcHandler::tool_harvest_scope(const json& /*params*/) {
    std::string raw = field_store_->harvest_scope_json();
    auto parsed = json::parse(raw, nullptr, false);
    std::ostringstream ss;
    if (parsed.is_object()) {
        ss << "harvest_scope:"
           << "  diagnosis=" << parsed.value("turiya_diagnosis", "unknown")
           << "  total_misses=" << parsed.value("total_router_misses", 0)
           << "  budget_items=" << parsed.value("harvest_budget_items", 0)
           << "\n";
        for (auto& m : parsed.value("top_router_misses", json::array())) {
            ss << "  miss: " << m.value("pattern", "?")
               << " → corpus=" << m.value("suggested_corpus", "?") << "\n";
        }
    } else { ss << raw; }
    return ToolResult::ok(ss.str(), {{"result", parsed.is_object() ? parsed : json::object()}});
}

ToolResult FieldRpcHandler::tool_seed_hdc_geometry(const json& params) {
    std::string path = params.value("json_path", "");
    if (path.empty()) return ToolResult::error("json_path required");
    std::string raw = field_store_->seed_hdc_geometry_json(path);
    auto parsed = json::parse(raw, nullptr, false);
    if (!parsed.is_object() || !parsed.value("ok", false))
        return ToolResult::error(parsed.is_object() ? parsed.value("error", raw) : raw);
    std::ostringstream ss;
    ss << "seed_hdc_geometry: seeded " << parsed.value("seeded_tokens", 0)
       << " tokens, codebook_len=" << parsed.value("codebook_len", 0)
       << "  source=" << parsed.value("source", path);
    return ToolResult::ok(ss.str(), {{"result", parsed}});
}

// ── Interaction Ledger handlers ───────────────────────────────────────────────

ToolResult FieldRpcHandler::tool_ledger_append(const json& params) {
    std::string json_in = params.dump();
    uint64_t event_id = 0;
    try { event_id = field_store_->ledger_append(json_in); }
    catch (const std::exception& e) { return ToolResult::error(e.what()); }
    std::ostringstream ss;
    ss << "ledger_append: event_id=" << event_id;
    return ToolResult::ok(ss.str(), {{"event_id", event_id}});
}

ToolResult FieldRpcHandler::tool_ledger_query(const json& params) {
    std::string raw = field_store_->ledger_query_json(params.dump());
    auto parsed = json::parse(raw, nullptr, false);
    std::ostringstream ss;
    ss << "ledger_query: " << (parsed.is_array() ? parsed.size() : 0) << " events";
    return ToolResult::ok(ss.str(), {{"events", parsed.is_array() ? parsed : json::array()}});
}

ToolResult FieldRpcHandler::tool_ledger_compile(const json& /*params*/) {
    uint32_t count = field_store_->ledger_compile();
    std::ostringstream ss;
    ss << "ledger_compile: " << count << " new assertions";
    return ToolResult::ok(ss.str(), {{"new_assertions", count}});
}

ToolResult FieldRpcHandler::tool_ledger_contradictions(const json& /*params*/) {
    std::string raw = field_store_->ledger_contradictions_json();
    auto parsed = json::parse(raw, nullptr, false);
    std::ostringstream ss;
    ss << "ledger_contradictions: " << (parsed.is_array() ? parsed.size() : 0) << " contested pairs";
    return ToolResult::ok(ss.str(), {{"contested", parsed.is_array() ? parsed : json::array()}});
}

// ── Predicate (falsifiable memory) handlers ──────────────────────────────────

ToolResult FieldRpcHandler::tool_predicate_attach(const json& params) {
    uint64_t memory_id = params.value("memory_id", uint64_t{0});
    // check_cmd may arrive as boolean if CLI auto-converted "true"/"false" strings.
    std::string check_cmd;
    if (params.contains("check_cmd")) {
        const auto& v = params["check_cmd"];
        if (v.is_string()) check_cmd = v.get<std::string>();
        else if (v.is_boolean()) check_cmd = v.get<bool>() ? "true" : "false";
    }
    if (!memory_id || check_cmd.empty())
        return ToolResult::error("memory_id and check_cmd are required");
    int64_t pred_id = field_store_->predicate_attach(memory_id, check_cmd);
    if (pred_id < 0) return ToolResult::error("predicate_attach failed");
    std::ostringstream ss;
    ss << "predicate_attach: predicate_id=" << pred_id
       << " attached to memory=" << memory_id;
    return ToolResult::ok(ss.str(), {{"predicate_id", pred_id}, {"memory_id", memory_id}});
}

ToolResult FieldRpcHandler::tool_predicate_run(const json& params) {
    uint64_t memory_id = params.value("memory_id", uint64_t{0});
    if (!memory_id) return ToolResult::error("memory_id is required");
    std::string raw = field_store_->predicate_run_json(memory_id);
    auto parsed = json::parse(raw, nullptr, false);
    if (parsed.is_discarded()) return ToolResult::error("predicate_run returned invalid JSON");
    std::ostringstream ss;
    ss << "predicate_run: memory=" << memory_id
       << " passed=" << parsed.value("passed", 0)
       << " failed=" << parsed.value("failed", 0)
       << " status=" << parsed.value("epistemic_status", "?");
    return ToolResult::ok(ss.str(), parsed.is_object() ? parsed : json::object());
}

ToolResult FieldRpcHandler::tool_predicate_list(const json& params) {
    uint64_t memory_id = params.value("memory_id", uint64_t{0});
    if (!memory_id) return ToolResult::error("memory_id is required");
    std::string raw = field_store_->predicate_list_json(memory_id);
    auto parsed = json::parse(raw, nullptr, false);
    std::ostringstream ss;
    int n = (parsed.is_object() && parsed.contains("predicates")) ? (int)parsed["predicates"].size() : 0;
    ss << "predicate_list: memory=" << memory_id << " predicates=" << n
       << " epistemic_status=" << (parsed.is_object() ? parsed.value("epistemic_status", "?") : "?");
    return ToolResult::ok(ss.str(), parsed.is_object() ? parsed : json::object());
}

} // namespace chitta
