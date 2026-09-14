// field_memory_recall RPC handlers — bodies for declarations in
// chitta/include/chitta/rpc/handlers/field_memory_recall.hpp.
#include <ctime>
#include <iomanip>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <future>
#include "chitta/hit_line.hpp"
#include "chitta/recall_lanes.hpp"
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
// Span Lane class (u8) → human label. UUID/HEX classes exist in the enum but are
// intentionally not extracted; kept here so labels stay aligned if that changes.
const char* span_class_label(int c) {
    switch (c) {
        case 0: return "path";
        case 1: return "url";
        case 2: return "uuid";
        case 3: return "hex";
        case 4: return "issue";
        case 5: return "file:line";
        case 6: return "bash";
        case 7: return "error";
        default: return "atom";
    }
}

// Tokenizer shared by set_lexical() below — kept identical to the rank-boost
// tokens() lambda in tool_recall (glued form + '-'/'_' sub-parts, stopwords,
// >=3 chars) so the displayed lexical overlap and the ranking overlap agree on
// what a token is.
std::unordered_set<std::string> lex_tokens(const std::string& s) {
    static const std::unordered_set<std::string> stop = {
        "the", "and", "for", "with", "what", "how", "did", "does", "was",
        "were", "are", "that", "this", "when", "where", "who", "why"};
    std::unordered_set<std::string> out;
    auto emit = [&](const std::string& t) {
        if (t.size() >= 3 && !stop.count(t)) out.insert(t);
    };
    auto emit_parts = [&](const std::string& t) {
        emit(t);
        if (t.find('-') == std::string::npos && t.find('_') == std::string::npos) return;
        std::string part;
        for (char c : t) {
            if (c == '-' || c == '_') { emit(part); part.clear(); }
            else part.push_back(c);
        }
        emit(part);
    };
    std::string cur;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') {
            cur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else if (!cur.empty()) {
            emit_parts(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) emit_parts(cur);
    return out;
}

// Honest bounded per-hit relevance for DISPLAY. Keyword-only hits carry
// semantic_score 0, so display_pct would otherwise fall back to the composite
// activation (>=1 for keyword hits) and clamp to a flat 100% for every result.
// Sets entry["lexical"] = |q_toks ∩ content_toks| / |q_toks| in [0,1] so
// display_pct can show max(cosine, lexical). Empty q_toks (query is all short/
// stopword tokens, e.g. "up") -> 0 for every hit -> honest low confidence.
void set_lexical(nlohmann::json& results, const std::string& query) {
    auto q_toks = lex_tokens(query);
    if (q_toks.empty()) {
        for (auto& r : results) r["lexical"] = 0.0f;
        return;
    }
    for (auto& r : results) {
        auto c_toks = lex_tokens(r.value("text", ""));
        size_t inter = 0;
        for (const auto& t : q_toks) if (c_toks.count(t)) ++inter;
        r["lexical"] = static_cast<float>(inter) / static_cast<float>(q_toks.size());
    }
}

// ── Recall-biased pre-filter (EMem, arXiv:2511.17208) ───────────────────────
// Evidence (GOLDEN_SET 2026-09-01, n=30, reranker off): nDCG@20 = 0.425 with a
// pool of 20 and many golds simply ABSENT from the returned page; at pool 60 the
// same retriever scores 0.503 with 29/30 golds in-pool — observed gold union
// ranks 27, 35, 38, 43, 51, 52, 57. A cross-encoder over the pool-20 head buys
// only +0.016. So the top-20 CUT loses golds the retriever already found;
// ranking precision is not the bottleneck. EMem reports the same shape: a cheap
// recall-biased over-selecting filter ahead of precision reranking beats graph
// machinery.
//
// Contract: fetch a WIDE pool (CHITTA_RECALL_POOL, default 60), then over-select
// down to a rerank budget (CHITTA_RERANK_BUDGET, default 24) with scalar-only
// rules — no embeddings, no per-candidate store round-trips, O(pool). A
// candidate survives if ANY rule fires; that asymmetry is the point.
// PREFILTER_BEGIN — self-contained (scalars + <vector>/<string> only) so the
// keep-rule check can compile this block verbatim without the daemon.
struct PrefilterCand {
    float       score;     // fused score after the term-overlap rescore
    bool        has_lex;   // >=1 distinctive query token literally present
    bool        neighbor;  // 1-hop assoc neighbour of a top-3 hit
    std::string realm;
    std::string kind;
};

// `c` is in RANK order (the order the caller would return). Keep rules:
//   (a) rank < limit                       — never drop what we'd have returned
//   (b) has_lex                            — literal query-term evidence
//   (c) realm+kind of a top-5 hit AND score >= 0.5 * max   — same answer class
//   (d) neighbor                           — graph-adjacent to the head
// The budget caps the survivor count; because we walk in rank order the ones
// dropped at the cap are the lowest-ranked survivors. budget is floored at
// `limit` so the filter can never return fewer hits than the response limit.
std::vector<char> prefilter_keep(const std::vector<PrefilterCand>& c,
                                 size_t limit, size_t budget) {
    std::vector<char> keep(c.size(), 0);
    if (c.empty()) return keep;
    if (budget < limit) budget = limit;

    float max_score = c[0].score;
    for (const auto& x : c) max_score = std::max(max_score, x.score);
    const float half = 0.5f * max_score;

    std::vector<std::pair<std::string, std::string>> head_class;
    for (size_t i = 0; i < c.size() && i < 5; ++i) {
        std::pair<std::string, std::string> k{c[i].realm, c[i].kind};
        bool seen = false;
        for (const auto& h : head_class) if (h == k) { seen = true; break; }
        if (!seen) head_class.push_back(std::move(k));
    }
    auto in_head_class = [&](const PrefilterCand& x) {
        for (const auto& h : head_class)
            if (h.first == x.realm && h.second == x.kind) return true;
        return false;
    };

    size_t kept = 0;
    for (size_t i = 0; i < c.size(); ++i) {
        const bool hit = (i < limit)
                      || c[i].has_lex
                      || c[i].neighbor
                      || (max_score > 0.0f && c[i].score >= half && in_head_class(c[i]));
        if (!hit) continue;
        if (kept >= budget) break;
        keep[i] = 1;
        ++kept;
    }
    return keep;
}
// PREFILTER_END
} // namespace

namespace chitta {

// Single emission point for the `#<id> [pct%] …` hit line. See the contract on
// FieldRpcHandler::HitLineOpts in rpc/handlers/field_memory_recall.hpp — hooks
// parse this format, so it is frozen; only the per-lane flags vary.
void FieldRpcHandler::format_hits(std::ostringstream& ss, json& results,
                                  const std::string& query, const HitLineOpts& opts) {
    set_lexical(results, query);

    float lex_max = 0.0f;
    if (opts.norm_lexical)
        for (const auto& r : results) lex_max = std::max(lex_max, r.value("lexical", 0.0f));

    for (auto& r : results) {
        int pct = display_pct(r);
        if (opts.norm_lexical && lex_max > 0.0f)
            pct = static_cast<int>(100.0f * r.value("lexical", 0.0f) / lex_max);
        if (opts.record_pct) r["display_pct"] = pct;

        const std::string id = r.value("id", "0");
        ss << hit_line(id, pct, r.value("type", "?"), r.value("ts_ms", int64_t(0)),
                       r.value("text", ""), opts.show_type, opts.show_date);

        if (!opts.link_atoms) continue;
        // Memory→span forward edge: hyperlink this belief to the exact atoms its
        // text references (paths/commands/ids), un-paraphrased. Directly fixes the
        // ellesmere class — the distilled belief now carries the verbatim path.
        uint64_t mid = 0;
        try { mid = std::stoull(id); } catch (...) {}
        if (mid == 0) continue;
        auto linked = json::parse(field_store_->span_for_memory_json(mid, 4), nullptr, false);
        if (!linked.is_array() || linked.empty()) continue;
        r["atoms"] = linked;
        ss << "    ↳ ";
        bool first = true;
        for (const auto& a : linked) {
            if (!first) ss << " · ";
            first = false;
            ss << a.value("text", "");
        }
        ss << "\n";
    }
}

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
    // Measurement mode: skip co-retrieval strengthening so an eval/diagnostic
    // recall never mutates the store. "no_learn":true or "strengthen":false.
    bool no_learn        = params.value("no_learn", false) || !params.value("strengthen", true);

    // Temporal window (parsed from the query at the pre-embed hook, e.g.
    // "last week" / "in June"). The window GATES candidates by authored ts;
    // semantic relevance still RANKS — never recency-sorted (recency-ordered
    // temporal recall floods context with operationally-fresh noise).
    int64_t win_from = 0, win_to = 0;
    if (params.contains("_twindow") && params["_twindow"].is_array()
        && params["_twindow"].size() == 2) {
        win_from = params["_twindow"][0].get<int64_t>();
        win_to   = params["_twindow"][1].get<int64_t>();
    }
    const bool windowed = win_to > 0;
    auto window_gate = [&](std::vector<FieldRecallHit> v) {
        if (windowed) {
            v.erase(std::remove_if(v.begin(), v.end(),
                [&](const FieldRecallHit& h) { return h.ts_ms < win_from || h.ts_ms >= win_to; }),
                v.end());
        }
        return v;
    };

    // Field-RAG / Modern Hopfield mode: bypass RRF, run DAM relaxation.
    // Tagged requests must reach the common tag/realm selection below.
    std::vector<FieldRecallHit> tagged_field_hits;
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
            if (tag.empty()) {
                json results = hits_to_results_json(hits, false);
                return ToolResult::ok(std::to_string(hits.size()) + " field memories", {{"results", results}});
            }
            tagged_field_hits = std::move(hits);
        }
    }

    // Fetch more than needed so tag filtering has candidates to work with
    size_t fetch_limit = tag.empty() ? limit : limit * 8;
    // Lanes fetch deeper than the response limit so the rescore stage (drift +
    // term-overlap) picks from a wider pool — golds at union-rank 21-40 were
    // unreachable when lanes capped at 20 and truncation ran before rescoring
    // (GOLDEN_SET v6: 4/30 misses attributed to exactly this). Capped at 160
    // to bound HNSW/BM25 cost per lane.
    size_t lane_depth = std::min(std::max((size_t)20, 2 * limit), (size_t)160);

    // Recall-biased pre-filter (see prefilter_keep above). ON by default;
    // CHITTA_RECALL_PREFILTER=0 (or "prefilter":false) restores the exact prior
    // path — pool width, filter, and all — so the golden eval can A/B in place.
    static const bool kPrefilterEnv = [] {
        const char* e = std::getenv("CHITTA_RECALL_PREFILTER");
        return !e || e[0] != '0';
    }();
    auto env_pos = [](const char* k, size_t d) {
        const char* e = std::getenv(k);
        if (!e) return d;
        try { size_t v = static_cast<size_t>(std::stoul(e)); return v ? v : d; }
        catch (...) { return d; }
    };
    static const size_t kPoolDefault   = env_pos("CHITTA_RECALL_POOL", 60);
    static const size_t kRerankBudget  = env_pos("CHITTA_RERANK_BUDGET", 24);
    const bool   prefilter_on = kPrefilterEnv && params.value("prefilter", true);
    const size_t want_pool    = std::min(
        static_cast<size_t>(params.value("pool", static_cast<int>(kPoolDefault))), (size_t)160);

    // With the pre-filter on, the lanes must fetch at least as deep as the pool
    // we intend to over-select from — a gold at union rank 43 is unreachable if
    // no lane ever returned 43 rows. Still capped at 160 to bound HNSW/BM25 cost.
    if (prefilter_on) lane_depth = std::min(std::max(lane_depth, want_pool), (size_t)160);
    size_t pool_limit = std::max(fetch_limit, lane_depth);

    // Multi-lane RRF: original query (2×, weighted) + SSL-shaped variants + BM25
    // Works directly on FieldRecallHit to preserve Hebbian/temporal scoring metadata.
    std::vector<FieldRecallHit> hits;
    // Lane-0 atom bridge: id -> normalized saturating-IDF weight of a co-atom
    // partner of the recall anchor. Populated in the multi-lane block, consumed
    // as an additive atom-overlap boost in the rescore (identity-evidence analog
    // of the term-overlap lever). Populated by default; empty iff CHITTA_BRIDGE_LANE0=0.
    std::unordered_map<uint64_t, float> bridge_boost;
    // strategy="keyword": BM25 only, realm-scoped. Previously this string fell
    // through to the fused path and the flag was a silent no-op, so callers who
    // asked for the keyword lane got whatever the fused path did — including,
    // when pre-embed missed, the unscoped fallback below (the cross-realm leak).
    if (!tagged_field_hits.empty()) {
        hits = window_gate(std::move(tagged_field_hits));
    } else if (strategy == "keyword") {
        hits = window_gate(field_store_->recall_keyword(query, pool_limit, realm, no_learn));
    } else if (expand && query_has_entities(query)) {
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
                rrf_lane(window_gate(field_store_->recall(emb, lane_depth, realm, no_learn)));
            }
        }
        // BM25 lane
        rrf_lane(window_gate(field_store_->recall_keyword(query, lane_depth, realm, no_learn)));
        // HDC lane (skipped when disable_hdc=true for ablation/benchmarking)
        if (!params.value("disable_hdc", false)) {
            rrf_lane(window_gate(field_store_->recall_hdc(query, lane_depth, realm)));
        }

        // Lane-0 atom bridge: record the RRF leader's rare co-atom partners with a
        // normalized saturating-IDF weight. The RRF position-based fusion above
        // buries a keyword-absent partner under the keyword-matching crowd; the
        // additive term-overlap rescore below then finishes the job. So instead of
        // a (negligible) RRF-mass nudge here, we stash the partner's IDF weight and
        // spend it in that same rescore stage as an additive atom-overlap boost —
        // the identity-evidence analog of shared query terms. Never fabricates a
        // hit: the boost only lands on partners the lanes already surfaced.
        // Default ON since the honest-gold A/B (v7, n=161): single_hop recall+nDCG
        // +0.02, overall recall 0.950->0.969, no material regression. Kill switch:
        // CHITTA_BRIDGE_LANE0=0.
        const char* lane0_env = std::getenv("CHITTA_BRIDGE_LANE0");
        const bool lane0_on = !lane0_env || lane0_env[0] != '0';
        if (lane0_on && !rrf_scores.empty()) {
            // Anchor on the top-K fused hits, not just the RRF leader: the leader
            // is often a pure keyword match that carries no rare identity atom,
            // while the memory that DOES (the query's real subject) sits a rank or
            // two down. Union the co-atom partners of the top-K, keeping the max
            // saturating-IDF boost per partner.
            std::vector<std::pair<uint64_t, float>> lead(rrf_scores.begin(), rrf_scores.end());
            const size_t kAnchors = std::min<size_t>(5, lead.size());
            std::partial_sort(lead.begin(), lead.begin() + kAnchors, lead.end(),
                              [](auto& a, auto& b) { return a.second > b.second; });
            const float norm = std::log(static_cast<float>(field_store_->memory_count()) + 1.0f);
            for (size_t a = 0; a < kAnchors; ++a) {
                auto br = field_store_->bridge_lane(lead[a].first, realm, 16);
                const auto& bids = br.first; const auto& bw = br.second;
                for (size_t i = 0; i < bids.size(); ++i) {
                    // Materialize a partner no lane surfaced (keyword-absent,
                    // cosine-cold): fetch it realm-scoped so it can enter the pool.
                    // A partner already present keeps its own hit; we only add the
                    // bridge's vote+boost to it.
                    if (!best_hit.count(bids[i])) {
                        std::string content = field_store_->get_content(bids[i]);
                        if (content.empty()) continue;
                        FieldRecallHit h;
                        h.memory_id = bids[i];
                        h.semantic_score = 0.0f;
                        h.content = content;
                        try {
                            auto m = json::parse(field_store_->get_memory_metadata(bids[i]));
                            h.ts_ms        = m.value("ts_ms", int64_t(0));
                            h.strength     = m.value("strength", 0.5f);
                            h.confidence   = m.value("confidence", 0.5f);
                            h.access_count = m.value("access_count", uint32_t(0));
                            h.kind         = m.value("kind", "episode");
                            h.realm        = m.value("realm", "");
                        } catch (...) {}
                        if (!realm.empty() && h.realm != realm) continue;
                        best_hit[bids[i]] = std::move(h);
                    }
                    // Bridge is a first-class RRF lane: partners come weight-sorted,
                    // so rank i+1 gives the rarest-atom partner a rank-1 vote (same
                    // magnitude as leading any other lane). Applied whether or not a
                    // weak dense hit already put the partner in the pool — otherwise
                    // that tiny natural RRF base can't be lifted by the boost alone.
                    rrf_scores[bids[i]] += 1.0f / (kRRF + static_cast<float>(i + 1));
                    float boost = norm > 0.0f ? std::min(1.0f, bw[i] / norm) : 0.0f;
                    auto it = bridge_boost.find(bids[i]);
                    if (it == bridge_boost.end() || boost > it->second)
                        bridge_boost[bids[i]] = boost;
                }
            }
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

        // Keep the full rescore pool here; final truncation to `limit` happens
        // AFTER drift + term-overlap rescoring so deep-pool golds can climb.
        for (auto& [score, id] : ranked) {
            if (hits.size() >= pool_limit) break;
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
            hits = window_gate(field_store_->recall(emb, pool_limit, realm, no_learn));
        } else {
            // Realm scoping: this is the pre-embed-miss fallback (embed_queue
            // timed out at 50ms, so no _preembedding), which fires for ordinary
            // recalls, not just diagnostics. It passed "" for realm, so every
            // cold-cache recall silently returned cross-realm memories while the
            // semantic leg two branches up was correctly scoped. Same `realm`.
            hits = window_gate(field_store_->recall_keyword(query, pool_limit, realm, no_learn));
        }
    }

    // Filter orphaned HNSW entries (deleted payloads with lingering vectors) and
    // quarantine [gap] memories: they're epistemic-gap markers for the dream
    // engine (which reads them via direct recall_keyword("[gap]") calls in
    // field_misc_sadhana.cpp, bypassing this tool), and they literally embed
    // past queries — so they trivially outrank real answers for any query
    // resembling a previously-graded gap (measured +0.049 nDCG@20 when removed).
    hits.erase(
        std::remove_if(hits.begin(), hits.end(),
            [](const FieldRecallHit& h) {
                return h.content.empty() || h.content.rfind("[gap]", 0) == 0;
            }),
        hits.end());

    // Windowed starvation backfill: the gate can leave the shallow (≤20-wide)
    // lanes short. The Rust windowed hybrid over-fetches inside the window and
    // ranks any remainder by cosine — window gates, semantic ranks.
    if (windowed && hits.size() < limit && params.contains("_preembedding")) {
        auto emb = params["_preembedding"].get<std::vector<float>>();
        std::unordered_set<uint64_t> have;
        for (const auto& h : hits) have.insert(h.memory_id);
        for (auto& h : field_store_->recall_with_fallback(emb, query, limit, realm,
                                                          win_from, win_to)) {
            if (hits.size() >= limit) break;
            if (h.content.empty() || have.count(h.memory_id)) continue;
            if (h.content.rfind("[gap]", 0) == 0) continue;  // gap quarantine (see above)
            hits.push_back(std::move(h));
        }
    }

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

        // Tag selection is a hard filter, including when no such tag exists.
        if (tagged_ids.empty()) hits.clear();
        if (!tagged_ids.empty()) {
            // Filter semantic hits to tagged set
            hits.erase(
                std::remove_if(hits.begin(), hits.end(),
                    [&](const FieldRecallHit& h) { return tagged_ids.find(h.memory_id) == tagged_ids.end()
                            || (!realm.empty() && h.realm != realm); }),
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
            // No early resize: the final post-rescore truncation caps to `limit`.
        }
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

    // Tier-1 rescore: exact-term-overlap fusion. RRF rank fusion discards raw
    // lexical evidence, so a memory literally containing the query's terms can
    // tie with a mere semantic neighbor (the high-k-recall / low-top-k-rank
    // gap). Additive bounded boost; weight validated on GOLDEN_SET v6 nDCG@10.
    if (hits.size() > 1) {
        auto tokens = [](const std::string& s) {
            static const std::unordered_set<std::string> stop = {
                "the", "and", "for", "with", "what", "how", "did", "does", "was",
                "were", "are", "that", "this", "when", "where", "who", "why"};
            std::unordered_set<std::string> out;
            auto emit = [&](const std::string& t) {
                if (t.size() >= 3 && !stop.count(t)) out.insert(t);
            };
            // Emit the glued form AND its sub-parts. BM25 splits on every non-alphanumeric
            // (chitta-field/src/organ/keyword.rs:213); keeping '-'/'_' as word characters here
            // made the two components disagree about what a token IS. `lca-optimization-revert`
            // stayed one opaque token, so it never intersected a natural-language query — and
            // the +0.4 boost below, the largest lever in the ranker, silently contributed 0.0 to
            // exactly the kebab/snake memories it was written to rescue. Emitting both forms can
            // only grow the intersection: an exact `ref_id` match still scores, and `lca` now
            // matches too.
            auto emit_parts = [&](const std::string& t) {
                emit(t);
                if (t.find('-') == std::string::npos && t.find('_') == std::string::npos) return;
                std::string part;
                for (char c : t) {
                    if (c == '-' || c == '_') { emit(part); part.clear(); }
                    else part.push_back(c);
                }
                emit(part);
            };
            std::string cur;
            for (char c : s) {
                if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') {
                    cur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
                } else if (!cur.empty()) {
                    emit_parts(cur);
                    cur.clear();
                }
            }
            if (!cur.empty()) emit_parts(cur);
            return out;
        };
        auto q_toks = tokens(query);
        if (!q_toks.empty()) {
            // w=0.4: knee of the GOLDEN_SET v6 offline sweep (0.312→0.385 nDCG@10;
            // higher w keeps climbing but overfits 30 queries / lexical notation).
            //
            // That sweep is not evidence. GOLDEN_SET queries were written FROM each
            // memory's own text, so they echo its vocabulary back — the sweep rewarded a
            // lexical signal it had itself planted, and w climbed to fit the leak. On
            // decontaminated gold (gold-v3) the same contamination is gone, and this term
            // is the only stage in the pipeline with no document-length normalization:
            // BM25 has b=0.75 (keyword.rs:130) and cosine is length-invariant, but here a
            // 57k-char episode transcript contains query tokens by accident and harvests
            // the boost while the short distilled memory that answers gets ~0. Measured:
            // 49/175 queries have the gold in the candidate pool and ranked >=20.
            //
            // Env-tunable so the weight can be ablated and re-fitted against honest gold
            // without a rebuild+10min store reload per point; per-request override so a
            // whole dose-response sweep costs one daemon boot instead of one boot per point.
            // Default 0.4 -> 0.8: dose-response on honest gold (v7, n=161, dev daemon,
            // env-toggled per boot) is monotone up through 0.8 then plateaus. The
            // non-artifact win — multihop bridge golds pulled INTO top-10 — saturates
            // exactly at 0.8: multihop recall 0.818->0.909 and recall@k 0.969->0.975
            // both hit their ceiling there and never move for higher w. multihop nDCG
            // 0.515->0.635 (+0.120); single_hop 0.879->0.898; mean 0.854->0.880. Above
            // 0.8 only single_hop/mean keep creeping (0.907/0.889 at w=1.4) with no
            // further recall gain — that tail is the contamination this comment warns
            // about (gold queries written from the memory's own text reward lexical
            // weight without bound), so we stop at the recall-saturation point, not the
            // eval max. Still env/param-tunable for re-fit against future honest gold.
            static const float kOverlapW = [] {
                const char* e = std::getenv("CHITTA_OVERLAP_W");
                return e ? std::strtof(e, nullptr) : 0.8f;
            }();
            const float overlap_w = params.value("_overlap_w", kOverlapW);
            // Lane-0 atom-overlap weight: symmetric with the term-overlap default
            // (0.4) — a rare shared atom is weighted like a full query-term match.
            // With the bridge RRF vote seeding the base, a maximally-rare atom
            // (saturating IDF -> ~0.8) then lifts a keyword-absent partner past the
            // keyword-only crowd but below the genuine query subject (measured on
            // the atombridge A/B: 0.4 -> partner #2, above the crowd, under the
            // anchor; 0.45 flips it over the anchor). Env/param-tunable (CHITTA_ATOM_W
            // / _atom_w) so it can be re-fit on honest gold. bridge_boost is populated
            // by default now (lane-0 default ON); empty iff CHITTA_BRIDGE_LANE0=0.
            static const float kAtomW = [] {
                const char* e = std::getenv("CHITTA_ATOM_W");
                return e ? std::strtof(e, nullptr) : 0.4f;
            }();
            const float atom_w = params.value("_atom_w", kAtomW);
            // NB: `frac` below is query-token COVERAGE with no doc-length normalization —
            // the one rescore stage that lacks it. A BM25-style length penalty
            // (frac / (1-b + b*dl/L0), b=0.75 L0=120) was A/B'd against honest gold
            // (v7, n=161) on the hypothesis that long transcripts steal the +0.4 boost
            // from short distilled answers: it REGRESSED (mean nDCG 0.854->0.846,
            // multihop 0.515->0.498, single_hop 0.879->0.871; recall@k flat). Demoting
            // long docs did not promote golds — recall@k unchanged means golds stay
            // in-pool, so the reshuffle only hurt the head; the effect shrinks toward 0
            // as L0 rises, never positive. Hypothesis falsified; not re-attempted.
            for (auto& h : hits) {
                auto c_toks = tokens(h.content);
                size_t inter = 0;
                for (const auto& t : q_toks) inter += c_toks.count(t);
                const float frac = static_cast<float>(inter) / q_toks.size();
                h.score += overlap_w * frac;
                // Remember the raw lexical overlap for the abstain gate: a hit that
                // literally contains the query's terms is high-confidence even when
                // its cosine is 0 (keyword/atom-only hit).
                h.lexical_score = frac;
                // Atom-overlap boost: a bridge partner sharing the anchor's rare
                // atom is high-confidence even with zero query-term overlap. The
                // boost is the same identity evidence the abstain gate trusts, so
                // fold it into lexical_score too.
                if (auto it = bridge_boost.find(h.memory_id); it != bridge_boost.end()) {
                    h.score += atom_w * it->second;
                    h.lexical_score = std::max(h.lexical_score, it->second);
                }
            }
            std::sort(hits.begin(), hits.end(),
                      [](const FieldRecallHit& a, const FieldRecallHit& b) {
                          return a.score > b.score;
                      });
        }
    }

    // Tier-2: SOTA Personalized-PageRank injection lane (HippoRAG-style). RRF and
    // the lexical rescore above can only reorder what the semantic/BM25/HDC lanes
    // already surfaced; a multi-hop query's bridge memory shares no query terms
    // and never enters the pool (measured: multihop nDCG@20 0.694 vs single_hop
    // 0.852). Seed a single PPR pass with the top-S fused hits (weighted by fused
    // score), walk the association graph (DerivedFrom strong, CoRetrieved
    // down-weighted), and RRF-merge the stationary ranking back in — INJECTING
    // graph-reachable memories, not merely boosting in-pool ones. Gated by
    // CHITTA_PPR_LANE (default on); =0 reproduces the pre-injection ranking
    // exactly. Injected nodes carry semantic_score 0, so the cosine abstain gate
    // below (a max over semantic_score) is unmoved. Mirrors store.rs ppr_inject.
    // Default OFF: the dev ablation (v7 golds, n=161) showed the PPR injection
    // regresses single_hop nDCG 0.859->0.802 for only a +0.004 (noise) multihop
    // gain — RRF-merged graph neighbors displace correct single-hop answers from
    // the top-k rerank pool. Net-negative, so it ships dormant. Enable for
    // experiments with CHITTA_PPR_LANE=1.
    // RE-MEASURED 2026-08 WITH the head fence below (v7, n=161): the fence cut the
    // single_hop damage (nDCG 0.879->0.872 at gate 0.55) but there was never a real
    // multihop gain to protect — multihop nDCG 0.515->0.519 (+0.004, inside CI), and
    // injection still knocks golds out of the top-10 (recall@k 0.969->0.950). Raising
    // the gate to 0.70 (fewer protected heads -> more injection) is uniformly worse
    // (single_hop -0.028, multihop -0.017); lowering it converges back to baseline.
    // No operating point lifts multihop without single_hop/recall cost. Stays dormant.
    bool ppr_on = [] {
        const char* e = std::getenv("CHITTA_PPR_LANE");
        return e && std::string(e) == "1";
    }();
    if (ppr_on && hits.size() > 1) {
        auto env_sz = [](const char* k, size_t d) {
            const char* e = std::getenv(k);
            if (!e) return d;
            try { return static_cast<size_t>(std::stoul(e)); } catch (...) { return d; }
        };
        auto env_f = [](const char* k, float d) {
            const char* e = std::getenv(k);
            if (!e) return d;
            try { return std::stof(e); } catch (...) { return d; }
        };
        const size_t seeds_n = env_sz("CHITTA_PPR_SEEDS", 5);
        const size_t top_g   = env_sz("CHITTA_PPR_G", 15);
        const float  lane_w  = env_f("CHITTA_PPR_LANE_W", 1.0f);
        const float  rel_gate = env_f("CHITTA_PPR_MAX_REL_GATE", 0.55f);

        // Head fence (rank-preserving injection). PPR always runs when ppr_on;
        // an original hit is "protected" if its own Platt-calibrated relevance
        // clears rel_gate. The merge emits protected hits first in their incoming
        // order (single-hop answers, never displaced), then the rest — unprotected
        // hits + injected graph nodes (semantic_score 0) — sorted fused-descending
        // below the head. Same Platt calibration as the abstain gate below.
        std::unordered_set<uint64_t> protected_ids;
        for (const auto& h : hits)
            if (1.0f / (1.0f + std::exp(-(3.27f * h.semantic_score - 0.85f))) >= rel_gate)
                protected_ids.insert(h.memory_id);
        std::vector<uint64_t> seed_ids;
        std::vector<float>    seed_w;
        for (size_t i = 0; i < hits.size() && seed_ids.size() < seeds_n; ++i) {
            seed_ids.push_back(hits[i].memory_id);
            seed_w.push_back(std::max(hits[i].score, 1e-6f));
        }
        auto lane = field_store_->ppr_lane(seed_ids, seed_w, top_g);
        if (!lane.empty()) {
            constexpr float kPPRRRF = 60.0f;
            std::unordered_map<uint64_t, float>          fused;
            std::unordered_map<uint64_t, FieldRecallHit> by_id;
            int rank = 1;
            for (auto& h : hits) {
                fused[h.memory_id] += 1.0f / (kPPRRRF + rank++);
                by_id.emplace(h.memory_id, h);
            }
            rank = 1;
            for (auto& [mid, s] : lane) {
                (void)s;
                float contrib = lane_w * (1.0f / (kPPRRRF + rank++));
                if (by_id.find(mid) != by_id.end()) { fused[mid] += contrib; continue; }
                // Inject: hydrate the graph-reached memory (semantic_score 0).
                std::string content = field_store_->get_content(mid);
                if (content.empty() || content.rfind("[gap]", 0) == 0) continue;
                FieldRecallHit h;
                h.memory_id      = mid;
                h.score          = 0.0f;
                h.semantic_score = 0.0f;
                h.content        = std::move(content);
                try {
                    auto m = json::parse(field_store_->get_memory_metadata(mid));
                    h.ts_ms        = m.value("ts_ms", int64_t(0));
                    h.strength     = m.value("strength", 0.5f);
                    h.confidence   = m.value("confidence", 0.5f);
                    h.access_count = m.value("access_count", uint32_t(0));
                    h.kind         = m.value("kind", "episode");
                    h.realm        = m.value("realm", "");
                } catch (...) {}
                if (!realm.empty() && h.realm != realm) continue;
                fused[mid] += contrib;
                by_id.emplace(mid, std::move(h));
            }
            // Head fence: protected originals first, in incoming order.
            std::vector<FieldRecallHit> merged;
            merged.reserve(fused.size());
            for (auto& h : hits) {
                if (protected_ids.find(h.memory_id) == protected_ids.end()) continue;
                FieldRecallHit ph = std::move(by_id[h.memory_id]);
                ph.score = fused[h.memory_id];
                merged.push_back(std::move(ph));
            }
            std::vector<std::pair<float, uint64_t>> order;
            order.reserve(fused.size());
            for (auto& [mid, s] : fused)
                if (by_id.find(mid) != by_id.end() &&
                    protected_ids.find(mid) == protected_ids.end())
                    order.emplace_back(s, mid);
            std::sort(order.begin(), order.end(), std::greater<>());
            for (auto& [s, mid] : order) {
                FieldRecallHit h = std::move(by_id[mid]);
                h.score = s;
                merged.push_back(std::move(h));
            }
            hits = std::move(merged);
        }
    }

    // ── Recall-biased pre-filter: wide pool → cheap over-selection ──────────
    // Runs AFTER every scoring stage and BEFORE the MMR/limit truncation (and
    // before the MCP's cross-encoder, which reranks whatever this returns), so
    // the reranker sees a budget of recall-biased survivors instead of the
    // arbitrary top-`limit` slice. See prefilter_keep() for the rules/evidence.
    if (prefilter_on && hits.size() > std::max(limit, kRerankBudget)) {
        // Rule (d) adjacency: ONE FFI call for the top-3 hits' 1-hop neighbours,
        // not one per candidate. Keeps the stage O(pool) with no embedding work.
        std::unordered_set<uint64_t> nbr;
        {
            std::vector<uint64_t> seeds;
            for (size_t i = 0; i < hits.size() && i < 3; ++i) seeds.push_back(hits[i].memory_id);
            for (const auto& n : field_store_->expand_associations(seeds, 1, 32))
                nbr.insert(n.memory_id);
        }
        auto q_toks = lex_tokens(query);
        std::vector<PrefilterCand> cands;
        cands.reserve(hits.size());
        for (const auto& h : hits) {
            bool has_lex = false;
            if (!q_toks.empty()) {
                auto c_toks = lex_tokens(h.content);
                for (const auto& t : q_toks)
                    if (c_toks.count(t)) { has_lex = true; break; }
            }
            cands.push_back({h.score, has_lex, nbr.count(h.memory_id) != 0, h.realm, h.kind});
        }
        auto keep = prefilter_keep(cands, limit, kRerankBudget);
        std::vector<FieldRecallHit> kept;
        kept.reserve(std::max(limit, kRerankBudget));
        for (size_t i = 0; i < hits.size(); ++i)
            if (keep[i]) kept.push_back(std::move(hits[i]));
        hits = std::move(kept);
    }

    // Optional MMR diversification (separation_mode). The corpus stores the same event
    // several times, so near-identical siblings fill the page and one cluster crowds out
    // the rest — the "good cosine, no margin" failure. Greedy re-selection penalizes a
    // candidate by its max content overlap (token Jaccard) with those already picked, so
    // one sibling represents its cluster and other clusters get a slot. Content-based on
    // purpose: cosine over-trusts the very vectors that make siblings look identical.
    // Gated off by default (the schema has advertised separation_mode all along but nothing
    // implemented it), so plain recall is byte-identical. Tune mix with CHITTA_MMR_LAMBDA.
    if (params.value("separation_mode", false) && hits.size() > 1) {
        auto ctoks = [](const std::string& s) {
            std::unordered_set<std::string> out;
            std::string cur;
            auto flush = [&] { if (cur.size() >= 3) out.insert(cur); cur.clear(); };
            for (char c : s) {
                if (std::isalnum(static_cast<unsigned char>(c)))
                    cur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
                else flush();
            }
            flush();
            return out;
        };
        const float lambda = [] {
            const char* e = std::getenv("CHITTA_MMR_LAMBDA");
            return e ? std::strtof(e, nullptr) : 0.7f;
        }();
        std::vector<std::unordered_set<std::string>> tok;
        tok.reserve(hits.size());
        for (auto& h : hits) tok.push_back(ctoks(h.content));
        float smax = hits.front().score;
        if (smax < 1e-6f) smax = 1.0f;
        std::vector<size_t> cand;
        cand.reserve(hits.size());
        for (size_t i = 0; i < hits.size(); ++i) cand.push_back(i);
        std::vector<FieldRecallHit> selected;
        std::vector<size_t> sel_idx;
        while (selected.size() < limit && !cand.empty()) {
            size_t best_pos = 0;
            float best_val = -1e30f;
            for (size_t p = 0; p < cand.size(); ++p) {
                const size_t i = cand[p];
                const float rel = hits[i].score / smax;
                float maxsim = 0.0f;
                for (size_t j : sel_idx) {
                    const auto& a = tok[i];
                    const auto& b = tok[j];
                    if (a.empty() || b.empty()) continue;
                    const auto& small = a.size() < b.size() ? a : b;
                    const auto& big   = a.size() < b.size() ? b : a;
                    size_t inter = 0;
                    for (const auto& t : small) inter += big.count(t);
                    const float uni = static_cast<float>(a.size() + b.size() - inter);
                    const float jac = uni > 0 ? static_cast<float>(inter) / uni : 0.0f;
                    if (jac > maxsim) maxsim = jac;
                }
                const float val = lambda * rel - (1.0f - lambda) * maxsim;
                if (val > best_val) { best_val = val; best_pos = p; }
            }
            const size_t chosen = cand[best_pos];
            sel_idx.push_back(chosen);
            selected.push_back(std::move(hits[chosen]));
            cand.erase(cand.begin() + static_cast<long>(best_pos));
        }
        hits = std::move(selected);
    }

    // Final truncation to the response limit — deliberately after both rescore
    // passes so a deep-pool candidate the rescorers rank highly makes the cut.
    if (hits.size() > limit) hits.resize(limit);

    // Hebbian co-occurrence: strengthen associations between co-retrieved
    // memories. After truncation on purpose — only memories actually returned
    // to the caller should co-strengthen, not the whole rescore pool.
    // Honor no_learn: recall must not mutate the assoc graph when the caller
    // asked for a read-only query (the lanes above already thread no_learn;
    // this Hebbian co-strengthen leaked past it). CHITTA_NO_ASSOC_LEARN freezes
    // it process-wide for reproducible paired evals on a fixed graph.
    static const bool assoc_frozen = std::getenv("CHITTA_NO_ASSOC_LEARN") != nullptr;
    if (!no_learn && !assoc_frozen && hits.size() >= 2) {
        std::vector<uint64_t> ids;
        ids.reserve(hits.size());
        for (const auto& h : hits) ids.push_back(h.memory_id);
        field_store_->record_co_retrieval(ids);
    }

    bool explain = params.value("explain", false);
    json results_json = hits_to_results_json(hits, explain);

    // Abstain signal: if no candidate clears the calibrated relevance bar, say so honestly
    // instead of presenting a weak best-of-a-bad-batch as if confident. relevance(cos) =
    // sigma(3.27*cos - 0.85) (the same Platt calibration the Rust scorer uses); tau=0.45 <=>
    // centered cosine 0.20 (~1.85 sigma above the random-pair spread). Room room-f366bf5a.
    // Per-lane abstain gate: a hit is "known" if EITHER lane is confident — the dense
    // Platt-calibrated cosine OR the Tier-1 lexical overlap (fraction of query content
    // tokens literally present). Previously max_rel maxed over semantic_score alone, so
    // a perfect keyword/atom hit (cosine 0) was capped at sigma(-0.85)~=0.30 and
    // mislabeled UNKNOWN — which tripped the prompt-hook cross-realm fallback into
    // injecting off-topic memories. Crediting the lexical lane fixes that class at the
    // source. (df<=4 exact-atom hard-override is a follow-on; needs atom-df from FFI.)
    float max_rel = 0.0f;
    for (const auto& h : hits) {
        const float dense = 1.0f / (1.0f + std::exp(-(3.27f * h.semantic_score - 0.85f)));
        max_rel = std::max(max_rel, std::max(dense, h.lexical_score));
    }
    bool weak = !hits.empty() && max_rel < 0.45f;

    std::ostringstream ss;
    if (weak)
        ss << "[weak: no strongly-relevant memory (max relevance "
           << static_cast<int>(max_rel * 100) << "%); results may be tangential]\n";
    ss << "Found " << hits.size() << " results";
    if (!realm.empty()) ss << " in realm '" << realm << "'";
    // Surface the calibrated confidence always, not only when weak: downstream
    // C2 self-monitoring (prompt-core.sh) bins this into KNOWN/THIN/UNKNOWN.
    if (!hits.empty()) ss << " (maxrel " << static_cast<int>(max_rel * 100) << "%)";
    ss << ":\n";
    format_hits(ss, results_json, query, {.show_date = true, .link_atoms = true});

    // Co-present the Span Lane: a separate, capped, realm-scoped block of verbatim
    // transcript atoms. Disjoint from distilled memories above — no RRF fusion, no
    // payload fetch, exact text shown inline. Realm-scoped by construction so a span
    // seen only in project A never surfaces in a project-B recall (case #2).
    json atoms_json = json::array();
    {
        std::string span_raw = field_store_->span_query_json(query, realm, 6);
        auto atoms = json::parse(span_raw, nullptr, false);
        if (atoms.is_array() && !atoms.empty()) {
            atoms_json = atoms;
            ss << "\n--- verbatim atoms (transcript-sourced, exact, no LLM) ---\n";
            for (const auto& a : atoms) {
                ss << "  [" << span_class_label(a.value("class", -1)) << "] "
                   << a.value("text", "");
                uint32_t cnt = a.value("count", 0u);
                if (cnt > 1) ss << "  (x" << cnt << ")";
                ss << "\n";
            }
        }
    }

    std::string recall_status = results_json.empty() ? "empty" : (weak ? "weak" : "ok");
    json meta = {{"results", results_json}, {"realm", realm},
                 {"status", recall_status},
                 {"atoms", atoms_json},
                 {"abstain", weak}, {"max_relevance", max_rel}};
    if (windowed) {
        // Surface the applied gate so a mis-parsed phrase is visible and correctable.
        auto iso = [](int64_t ms) {
            std::time_t t = static_cast<std::time_t>(ms / 1000);
            char buf[24];
            std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M", std::gmtime(&t));
            return std::string(buf);
        };
        meta["window"] = {{"from", iso(win_from)}, {"to", iso(win_to)},
                          {"from_ms", win_from}, {"to_ms", win_to}};
        ss << "\n[window: " << iso(win_from) << " → " << iso(win_to) << " UTC]\n";
    }
    auto result = ToolResult::ok(ss.str(), meta);
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
    // Realm scoping: the BM25 leg took no realm at all, so this tool (and any
    // caller routed to it) returned memories from every realm on the box while
    // the semantic tools were scoped. field_store_->recall_keyword() has always
    // accepted a realm — it was simply never passed. Empty realm = all visible,
    // matching tool_recall.
    std::string realm = params.value("realm", "");
    bool no_learn = params.value("no_learn", false) || !params.value("strengthen", true);

    auto hits = field_store_->recall_keyword(query, k, realm, no_learn);
    bool explain = params.value("explain", false);
    json results_json = hits_to_results_json(hits, explain);

    std::ostringstream ss;
    ss << "Found " << hits.size() << " keyword results for '" << query << "'";
    if (!realm.empty()) ss << " in realm '" << realm << "'";
    ss << ":\n";
    format_hits(ss, results_json, query, {.show_type = false});
    return ToolResult::ok(ss.str(), {{"results", results_json}});
}

// Deterministic anti-reprocessing check (keyed lane, capability #1). Resolves a
// prior `[done]` record by content-hash and/or input path via an exact O(1)
// lookup that NEVER touches the embedder or the fuzzy retriever — no rpc_mutex
// heavy op. Returns {found, memory_id, record} so a caller can skip reprocessing.
ToolResult FieldRpcHandler::tool_provenance_check(const json& params) {
    std::string sha   = params.value("sha", "");
    std::string input = params.value("input", "");
    if (sha.empty() && input.empty())
        return ToolResult::error("provenance_check requires 'sha' and/or 'input'");

    auto hit = field_store_->provenance_lookup(sha, input);
    json data{{"found", hit.found}};
    if (hit.found) {
        data["memory_id"] = hit.memory_id;
        data["record"]    = hit.content;
        return ToolResult::ok(
            "ALREADY PROCESSED (#" + std::to_string(hit.memory_id) + "): " + hit.content, data);
    }
    return ToolResult::ok("NOT PROCESSED — no prior [done] record for this key", data);
}

// Deterministic durable-correction check (keyed lane, capability #2). Given
// free turn text, returns any stored [correction] whose corrected-mistake
// trigger recurs — an exact-key bigram probe that RESERVES an injection slot
// for the correction instead of letting it lose on cosine similarity in fuzzy
// recall (the ~99% miss). No embedder, no fuzzy retriever, no rpc_mutex heavy op.
ToolResult FieldRpcHandler::tool_correction_check(const json& params) {
    std::string text = params.value("text", "");
    if (text.empty())
        return ToolResult::error("correction_check requires 'text'");

    auto hit = field_store_->correction_check(text);
    json data{{"found", hit.found}};
    if (hit.found) {
        data["memory_id"] = hit.memory_id;
        data["record"]    = hit.content;
        return ToolResult::ok(
            "CORRECTION FIRED (#" + std::to_string(hit.memory_id) + "):\n" + hit.content, data);
    }
    return ToolResult::ok("NO CORRECTION — no stored [correction] trigger matches this turn", data);
}

// Deterministic task-state check (keyed lane, capability #3). Given a task slug,
// returns the LATEST stored [task] record for that id — an exact-key HashMap
// read that BYPASSES the fuzzy retriever, so an agent resuming a discontinuous
// session gets the current durable status of task X without competing on cosine.
ToolResult FieldRpcHandler::tool_task_state(const json& params) {
    std::string id = params.value("id", "");
    if (id.empty())
        return ToolResult::error("task_state requires 'id'");

    auto hit = field_store_->task_state_lookup(id);
    json data{{"found", hit.found}};
    if (hit.found) {
        data["memory_id"] = hit.memory_id;
        data["record"]    = hit.content;
        return ToolResult::ok(
            "TASK STATE (#" + std::to_string(hit.memory_id) + "): " + hit.content, data);
    }
    return ToolResult::ok("NO TASK STATE — no stored [task] record for this id", data);
}

ToolResult FieldRpcHandler::tool_hybrid_recall(const json& params) {
    std::string query = params.value("query", "");
    if (query.empty()) return ToolResult::error("query is required");

    size_t limit      = static_cast<size_t>(params.value("limit", 10));
    std::string realm = params.value("realm", "");
    // Measurement mode: skip co-retrieval strengthening so an eval/diagnostic
    // recall never mutates the store it reads. Accepts either "strengthen":false
    // or "no_learn":true.
    bool no_learn = params.value("no_learn", false) || !params.value("strengthen", true);

    auto embedding = embed_query(query);

    std::vector<FieldRecallHit> semantic_hits;
    if (!embedding.empty()) {
        semantic_hits = field_store_->recall(embedding, limit, realm, no_learn);
        semantic_hits.erase(
            std::remove_if(semantic_hits.begin(), semantic_hits.end(),
                [](const FieldRecallHit& h) { return h.content.empty(); }),
            semantic_hits.end());
    }
    auto keyword_hits  = field_store_->recall_keyword(query, limit, realm, no_learn);

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

    // Relevance floor: drop a hit only when it HAS a semantic cosine
    // ("similarity") but that cosine is below CHITTA_RECALL_MIN_SIM (default
    // 0.10). A ~1%-cosine match is noise — returning such hits derailed
    // downstream consumers (e.g. a fusion room anchored on an unrelated memory).
    // Keyword-only hits carry no cosine (similarity==0) and are KEPT, so lexical
    // recall is unaffected. Set CHITTA_RECALL_MIN_SIM=0 to disable.
    {
        static const float kMinSim = [] {
            const char* e = std::getenv("CHITTA_RECALL_MIN_SIM");
            float v = e ? std::strtof(e, nullptr) : 0.10f;
            if (!(v >= 0.0f && v <= 1.0f)) v = 0.10f;
            return v;
        }();
        if (kMinSim > 0.0f) {
            merged.erase(std::remove_if(merged.begin(), merged.end(),
                [](const nlohmann::json& r) {
                    float sim = r.value("similarity", 0.0f);
                    return sim > 0.0f && sim < kMinSim;
                }), merged.end());
        }
    }

    if (merged.size() > limit) merged.erase(merged.begin() + static_cast<int>(limit), merged.end());

    std::ostringstream ss;
    ss << "Hybrid recall: " << merged.size() << " results\n";
    format_hits(ss, merged, query, {.show_type = false});
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
    // True when the results below came from the BM25 leg ALONE (route 1, or the
    // embed-failure fallback, which keeps the learner's route_name). Drives the
    // display normalization at the bottom — see the header contract there.
    bool keyword_lane = (route == 1);

    if (route == 1) {  // Keyword
        auto kw_hits = field_store_->recall_keyword(eq.lex, limit, realm);
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
            auto kw_hits = field_store_->recall_keyword(eq.lex, limit, realm);
            apply_drift_smart(kw_hits);
            results = hits_to_results_json(kw_hits);
            keyword_lane = true;  // BM25-only page, whatever the learner's route_name says
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
                auto kw_hits = field_store_->recall_keyword(eq.lex, limit, realm);
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

    // ── Header contract (parsed by hooks/prompt-core.sh) ───────────────────
    // Header: `Smart recall (<route>, ep=<episode_id>): <n> results`
    //   <route> ∈ semantic|keyword|temporal|artifact|hybrid|full — the route the
    //   learner picked; ep=<id> is the bandit episode for route_feedback.
    // Lines are the shared format_hits() contract, with norm_lexical on:
    //   The hook applies MIN_CONFIDENCE=30 to <pct>. That threshold assumes the
    //   number is comparable ACROSS routes, and it was not: semantic hits print
    //   a cosine (70-100%) while the keyword lane printed raw query-token
    //   coverage (5-11%), so every keyword-routed page was dropped wholesale
    //   before the reasoner saw it. Max-normalize the keyword lane the same way
    //   the hybrid path max-normalizes RRF: the best BM25 hit on the page reads
    //   100% and the rest are relative to it. Raw `lexical`/`relevance` stay
    //   untouched in the JSON; `display_pct` records what was printed.
    std::ostringstream ss;
    ss << "Smart recall (" << route_name << ", ep=" << episode_id << "): " << results.size() << " results\n";
    format_hits(ss, results, query, {.norm_lexical = keyword_lane, .record_pct = true});
    auto result = ToolResult::ok(ss.str(), {{"results", results}, {"intent", is_code ? "code" : "semantic"}});
    fire_recall_callback(results, 1);
    return result;
}

ToolResult FieldRpcHandler::tool_recall_lanes(const json& params) {
    const std::string query = params.value("query", "");
    if (query.empty()) return ToolResult::error("query is required");
    const std::string ctx_query = params.value("ctx_query", query);
    const std::string realm = params.value("realm", "");

    static const std::array<std::string, 6> kLaneOrder = {
        "sem", "ctx", "hyb", "kw", "corr", "corrk"};
    std::unordered_set<std::string> selected;
    if (params.contains("lanes")) {
        if (!params["lanes"].is_array())
            return ToolResult::error("lanes must be an array");
        for (const auto& lane : params["lanes"]) {
            if (!lane.is_string()) return ToolResult::error("lane names must be strings");
            const std::string name = lane.get<std::string>();
            if (std::find(kLaneOrder.begin(), kLaneOrder.end(), name) == kLaneOrder.end())
                return ToolResult::error("unknown recall lane: " + name);
            selected.insert(name);
        }
    } else {
        selected.insert(kLaneOrder.begin(), kLaneOrder.end());
    }

    const json limits = params.value("limits", json::object());
    if (!limits.is_object()) return ToolResult::error("limits must be an object");
    auto lane_limit = [&](const std::string& lane, int fallback) -> std::optional<size_t> {
        json args{{"limit", fallback}};
        auto it = limits.find(lane);
        if (it != limits.end()) {
            if (!it->is_number_integer() && !it->is_number_unsigned()) return std::nullopt;
            args["limit"] = *it;
        }
        // Compatibility for direct JSON callers that prefer sem_limit, etc.
        const std::string alias = lane + "_limit";
        auto alias_it = params.find(alias);
        if (alias_it != params.end()) {
            if (!alias_it->is_number_integer() && !alias_it->is_number_unsigned())
                return std::nullopt;
            args["limit"] = *alias_it;
        }
        args = rpc::clamp_read_arguments("recall_lanes." + lane, std::move(args));
        return static_cast<size_t>(args["limit"].get<int64_t>());
    };

    long budget_override = 0;
    if (params.contains("budget_ms")) {
        if (!params["budget_ms"].is_number_integer()
            && !params["budget_ms"].is_number_unsigned())
            return ToolResult::error("budget_ms must be a positive integer");
        if (params["budget_ms"].is_number_unsigned()) {
            const auto raw = params["budget_ms"].get<uint64_t>();
            budget_override = raw > static_cast<uint64_t>(LONG_MAX)
                ? LONG_MAX : static_cast<long>(raw);
        } else {
            budget_override = params["budget_ms"].get<long>();
        }
        if (budget_override <= 0)
            return ToolResult::error("budget_ms must be a positive integer");
    }

    struct LaneCall {
        std::string name;
        json args;
        long budget_ms;
    };
    std::vector<LaneCall> calls;
    calls.reserve(kLaneOrder.size());
    const long global_budget = rpc_budget_.budget_ms();
    auto add = [&](const std::string& name, json args, int default_limit,
                   long default_budget) -> bool {
        if (!selected.count(name)) return true;
        if (name != "corrk") {
            auto limit = lane_limit(name, default_limit);
            if (!limit) return false;
            args["limit"] = *limit;
        }
        // sem/ctx call smart_recall, which ignores _preembedding and embeds
        // expand_query(query).vec. Keep those semantics: that string may differ
        // from the dispatcher's time-stripped query; embed_query caches it exactly.
        if (params.contains("_preembedding") && name != "sem" && name != "ctx"
            && name != "corrk")
            args["_preembedding"] = params["_preembedding"];
        if (params.contains("_twindow") && name != "sem" && name != "ctx"
            && name != "corrk")
            args["_twindow"] = params["_twindow"];
        const long requested = budget_override > 0 ? budget_override : default_budget;
        calls.push_back({name, std::move(args), std::min(requested, global_budget)});
        return true;
    };

    if (!add("sem", {{"query", query}, {"realm", realm}}, 6, 2000)
        || !add("ctx", {{"query", ctx_query}, {"realm", realm}}, 4, 2000)
        || !add("hyb", {{"query", query}, {"realm", realm}, {"strategy", "hybrid"}}, 5, 3000)
        || !add("kw", {{"query", query}, {"realm", realm}, {"strategy", "keyword"}}, 3, 2000)
        || !add("corr", {{"query", query}, {"realm", realm}, {"tag", "correction"}, {"include_global", true}}, 3, 2000)
        || !add("corrk", {{"text", query}}, 0, 2000))
        return ToolResult::error("lane limits must be integers");

    const auto total_started = std::chrono::steady_clock::now();
    std::vector<std::future<RecallLaneOutput>> futures;
    futures.reserve(calls.size());
    // The dispatcher already permits these same handlers to overlap as separate
    // read RPCs. Keep one outer shared rpc_mutex_ lock while the Rust store's
    // component RwLocks synchronize the internal workers.
    for (const auto& call : calls) {
        futures.push_back(std::async(std::launch::async, [this, call] {
            const auto started = std::chrono::steady_clock::now();
            auto budget_scope = rpc_budget_.measure("recall_lanes." + call.name,
                                                    call.budget_ms);
            ToolResult result;
            if (call.name == "sem" || call.name == "ctx")
                result = tool_smart_recall(call.args);
            else if (call.name == "corrk")
                result = tool_correction_check(call.args);
            else
                result = tool_recall(call.args);
            const long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            return RecallLaneOutput{call.name, result.text, result.structured,
                                    elapsed, elapsed > call.budget_ms};
        }));
    }

    std::vector<RecallLaneOutput> outputs;
    outputs.reserve(futures.size());
    for (auto& future : futures) outputs.push_back(future.get());
    const long total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - total_started).count();
    json response = assemble_recall_lanes(outputs, total_ms);
    return ToolResult::ok("Recall lanes: " + std::to_string(outputs.size())
                              + " completed in " + std::to_string(total_ms) + "ms",
                          response);
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

    auto hits = field_store_->recall_spreading(query, limit, realm, 256, 64, 2);
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
    format_hits(ss, final_merged, query, {});
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

// Hypervector similarity is normalized Hamming: 0.5 is the orthogonal noise
// floor, not "half a match". Rescale so an unrelated candidate reads 0% and an
// identical signature reads 100% — printing the raw 0.5 as "50%" would invite
// the reader to trust pure noise.
static int analogy_pct(double score) {
    double norm = (score - 0.5) * 2.0;
    if (norm < 0.0) norm = 0.0;
    if (norm > 1.0) norm = 1.0;
    return static_cast<int>(norm * 100.0);
}

ToolResult FieldRpcHandler::tool_recall_analogy(const json& params) {
    const std::string mode = params.value("mode", "structural");
    json args = json::object();
    args["mode"]  = mode;
    args["limit"] = params.value("limit", 8);
    args["fact_limit"] = 10000;  // Internal serving-path bound; not a public RPC field.

    std::string header;
    if (mode == "proportional") {
        const std::string a = params.value("a", "");
        const std::string b = params.value("b", "");
        const std::string c = params.value("c", "");
        if (a.empty() || b.empty() || c.empty())
            return ToolResult::error("proportional mode needs a, b and c (a:b :: c:?)");
        args["a"] = a;
        args["b"] = b;
        args["c"] = c;
        header = a + ":" + b + " :: " + c + ":?";
    } else if (mode == "structural") {
        const uint64_t mid  = params.value("memory_id", uint64_t(0));
        const std::string q = params.value("text", "");
        if (mid == 0 && q.empty())
            return ToolResult::error("structural mode needs memory_id or text");
        if (mid != 0) {
            args["memory_id"] = mid;
            header = "memories shaped like #" + std::to_string(mid);
        } else {
            args["text"] = q;
            header = "memories shaped like \"" + q + "\"";
        }
        const std::string realm = params.value("realm", "");
        const std::string excl  = params.value("exclude_realm", "");
        if (!realm.empty()) args["realm"] = realm;
        if (!excl.empty())  args["exclude_realm"] = excl;
        if (params.value("cross_realm", false)) args["cross_realm"] = true;
        if (!realm.empty()) header += " [realm=" + realm + "]";
        if (!excl.empty())  header += " [excluding realm=" + excl + "]";
    } else {
        return ToolResult::error("mode must be 'proportional' or 'structural'");
    }

    auto parsed = json::parse(field_store_->recall_analogy_json(args.dump()), nullptr, false);
    if (!parsed.is_object() || !parsed.contains("results"))
        return ToolResult::error("analogy lane unavailable (no triplets indexed, or bad arguments)");
    const json& results = parsed["results"];
    const size_t indexed = parsed.value("indexed", size_t(0));

    std::ostringstream ss;
    if (!results.is_array() || results.empty()) {
        ss << "No analogy for " << header << " (" << indexed
           << (mode == "proportional" ? " facts" : " memories") << " indexed)."
              " The triplet lane needs the probed entities to appear in stored facts.";
        return ToolResult::ok(ss.str(), {{"results", json::array()}, {"indexed", indexed}});
    }

    ss << "Analogy [" << mode << "] " << header << " — " << results.size()
       << " of " << indexed << (mode == "proportional" ? " facts" : " memories indexed") << ":\n";
    for (const auto& r : results) {
        const uint64_t id = r.value("id", uint64_t(0));
        const int pct = analogy_pct(r.value("score", 0.0));
        // Line contract: `#<id> [pct%] ...` — the hooks' SUS exposure block and the
        // outcome-ledger tap parse `^#([0-9]+)`. A proportional answer with no
        // linking fact has no memory to credit, so it is indented instead of
        // prefixed with a fake `#0` that would book exposure against nothing.
        if (id != 0) ss << "#" << id << " [" << pct << "%]";
        else         ss << "   [" << pct << "%]";
        const std::string realm = r.value("realm", "");
        if (!realm.empty()) ss << " [" << realm << "]";
        if (mode == "proportional") {
            ss << " " << r.value("answer", "");
            const std::string pred = r.value("predicate", "");
            if (!pred.empty()) ss << "  (" << pred << ")";
            ss << "\n";
        } else {
            ss << " " << r.value("text", "").substr(0, 300) << "\n";
        }
    }
    return ToolResult::ok(ss.str(), {{"results", results}, {"indexed", indexed}, {"mode", mode}});
}

ToolResult FieldRpcHandler::tool_span_query(const json& params) {
    std::string query = params.value("query", "");
    std::string realm = params.value("realm", "");
    size_t k          = static_cast<size_t>(params.value("k", 6));
    if (query.empty())
        return ToolResult::error("query is required");
    std::string raw = field_store_->span_query_json(query, realm, k);
    auto parsed = json::parse(raw, nullptr, false);
    std::ostringstream ss;
    if (parsed.is_array() && !parsed.empty()) {
        ss << "Verbatim atoms" << (realm.empty() ? "" : " [realm=" + realm + "]")
           << " (" << parsed.size() << ", transcript-sourced, no LLM):\n";
        for (const auto& h : parsed) {
            ss << "  [" << span_class_label(h.value("class", -1)) << "] "
               << h.value("text", "");
            uint32_t count = h.value("count", 0u);
            if (count > 1) ss << "  (x" << count << ")";
            std::string sess = h.value("session", "");
            if (!sess.empty()) ss << "  @" << sess << ":" << h.value("line", 0u);
            ss << "\n";
        }
    } else {
        ss << "No verbatim atoms for \"" << query << "\""
           << (realm.empty() ? "" : " in realm " + realm) << ".";
    }
    return ToolResult::ok(ss.str(), {{"atoms", parsed.is_array() ? parsed : json::array()}});
}

ToolResult FieldRpcHandler::tool_span_backfill(const json& params) {
    std::string dir = params.value("projects_dir", "");
    if (dir.empty()) {
        const char* home = std::getenv("HOME");
        dir = std::string(home ? home : "") + "/.claude/projects";
    }
    std::string raw = field_store_->span_backfill_json(dir);
    auto parsed = json::parse(raw, nullptr, false);
    std::ostringstream ss;
    if (parsed.is_object()) {
        ss << "span_backfill: unique=" << parsed.value("unique", 0)
           << " new=" << parsed.value("new", 0)
           << " redacted=" << parsed.value("redacted", 0);
    } else {
        ss << raw;
    }
    return ToolResult::ok(ss.str(), {{"result", parsed.is_object() ? parsed : json::object()}});
}

ToolResult FieldRpcHandler::tool_span_backfill_memories(const json& /*params*/) {
    std::string raw = field_store_->span_backfill_memories_json();
    auto parsed = json::parse(raw, nullptr, false);
    std::ostringstream ss;
    if (parsed.is_object()) {
        ss << "span_backfill_memories: linked=" << parsed.value("linked", 0)
           << " new_spans=" << parsed.value("new_spans", 0);
    } else {
        ss << raw;
    }
    return ToolResult::ok(ss.str(), {{"result", parsed.is_object() ? parsed : json::object()}});
}

ToolResult FieldRpcHandler::tool_densify_backfill(const json& params) {
    bool apply = params.value("apply", false);
    std::string raw = field_store_->densify_backfill_json(apply);
    auto parsed = json::parse(raw, nullptr, false);
    std::ostringstream ss;
    if (parsed.is_object()) {
        auto h = parsed.value("histogram", json::object());
        ss << "densify_backfill apply=" << (apply ? "true" : "false")
           << " sessions=" << parsed.value("sessions", 0)
           << " memories_in_sessions=" << parsed.value("memories_in_sessions", 0)
           << " pairs=" << parsed.value("pairs", 0)
           << " directed_edges=" << parsed.value("directed_edges", 0)
           << " | group-size hist n1=" << h.value("n1", 0)
           << " n2=" << h.value("n2", 0) << " n3_5=" << h.value("n3_5", 0)
           << " n6_10=" << h.value("n6_10", 0) << " n11_50=" << h.value("n11_50", 0)
           << " n51+=" << h.value("n51plus", 0);
    } else {
        ss << raw;
    }
    return ToolResult::ok(ss.str(), {{"result", parsed.is_object() ? parsed : json::object()}});
}

ToolResult FieldRpcHandler::tool_semantic_backfill(const json& params) {
    bool apply = params.value("apply", false);
    size_t k = params.value("k", 6);
    float min_cos = params.value("min_cos", 0.6f);
    std::string raw = field_store_->semantic_backfill_json(apply, k, min_cos);
    auto parsed = json::parse(raw, nullptr, false);
    std::ostringstream ss;
    if (parsed.is_object()) {
        ss << "semantic_backfill apply=" << (apply ? "true" : "false")
           << " k=" << parsed.value("k", 0)
           << " min_cos=" << parsed.value("min_cos", 0.0)
           << " | memories_scanned=" << parsed.value("memories_scanned", 0)
           << " memories_with_neighbors=" << parsed.value("memories_with_neighbors", 0)
           << " directed_edges=" << parsed.value("directed_edges", 0);
    } else {
        ss << raw;
    }
    return ToolResult::ok(ss.str(), {{"result", parsed.is_object() ? parsed : json::object()}});
}

ToolResult FieldRpcHandler::tool_assoc_census(const json& /*params*/) {
    std::string raw = field_store_->assoc_census_json();
    auto parsed = json::parse(raw, nullptr, false);
    std::ostringstream ss;
    ss << "assoc_census (weight buckets <0.05 / 0.05-0.2 / 0.2-0.5 / 0.5-0.8 / >=0.8):\n";
    if (parsed.is_array()) {
        for (const auto& e : parsed) {
            ss << "  " << e.value("type", "?") << ": count=" << e.value("count", 0);
            if (e.contains("weights")) ss << " weights=" << e["weights"].dump();
            ss << "\n";
        }
    } else {
        ss << raw;
    }
    return ToolResult::ok(ss.str(), {{"census", parsed.is_array() ? parsed : json::array()}});
}

ToolResult FieldRpcHandler::tool_assoc_decay(const json& params) {
    bool apply = params.value("apply", false);
    int edge_type = params.value("edge_type", 3);  // wire numbering; default CoRetrieved
    float factor = params.value("factor", 1.0f);
    float prune_below = params.value("prune_below", 0.0f);
    if (edge_type < 0 || edge_type > 5)
        return ToolResult::error("edge_type must be 0-5 (wire numbering, see assoc_census)");
    if (factor <= 0.0f || factor > 1.0f)
        return ToolResult::error("factor must be in (0, 1]");
    std::string raw = field_store_->assoc_decay_json(
        static_cast<uint8_t>(edge_type), factor, prune_below, apply);
    auto parsed = json::parse(raw, nullptr, false);
    std::ostringstream ss;
    if (parsed.is_object()) {
        ss << "assoc_decay apply=" << (apply ? "true" : "false")
           << " edge_type=" << edge_type
           << " factor=" << factor
           << " prune_below=" << prune_below
           << " survivors=" << parsed.value("survivors", 0)
           << " pruned=" << parsed.value("pruned", 0);
    } else {
        ss << raw;
    }
    return ToolResult::ok(ss.str(), {{"result", parsed.is_object() ? parsed : json::object()}});
}

ToolResult FieldRpcHandler::tool_span_stats(const json& /*params*/) {
    std::string raw = field_store_->span_stats_json();
    auto parsed = json::parse(raw, nullptr, false);
    std::ostringstream ss;
    if (parsed.is_object()) {
        ss << "span_stats: unique=" << parsed.value("unique", 0)
           << " disk_bytes=" << parsed.value("disk_bytes", 0)
           << " redacted_total=" << parsed.value("redacted_total", 0);
    } else {
        ss << raw;
    }
    return ToolResult::ok(ss.str(), {{"result", parsed.is_object() ? parsed : json::object()}});
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
