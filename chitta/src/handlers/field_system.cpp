// field_system RPC handlers — bodies for declarations in
// chitta/include/chitta/rpc/handlers/field_system.hpp.

#include "../../include/chitta/rpc/field_handler.hpp"

#include <fstream>
#include <sstream>

namespace chitta {

namespace {
// Shared cache for memory_stats() — full O(N) scan, expensive under writer contention.
// Used by tool_health_check, tool_soul_context, tool_hygiene_stats. 30s TTL.
//
// Single-flight refresh: the recompute runs OUTSIDE the protecting mutex so
// concurrent readers don't block on the scan (which itself contends with
// FieldStore writers). One leader recomputes; followers return the prior
// cached value, even if stale, rather than queue.
std::string get_memory_stats_cached(FieldStore* store) {
    struct CacheEntry {
        std::string cache;
        std::chrono::steady_clock::time_point ts{};
        bool in_flight = false;
    };
    static std::mutex mu;
    static std::unordered_map<FieldStore*, CacheEntry> entries;

    bool i_am_leader = false;
    std::string snapshot;
    {
        std::lock_guard<std::mutex> lk(mu);
        auto& e = entries[store];
        auto now = std::chrono::steady_clock::now();
        bool expired = e.cache.empty() ||
            std::chrono::duration_cast<std::chrono::seconds>(now - e.ts).count() > 30;
        if (expired && !e.in_flight) {
            e.in_flight = true;
            i_am_leader = true;
        }
        snapshot = e.cache;
    }

    if (i_am_leader) {
        std::string fresh;
        try { fresh = store->memory_stats(); } catch (...) {}
        std::lock_guard<std::mutex> lk(mu);
        auto& e = entries[store];
        if (!fresh.empty()) {
            e.cache = std::move(fresh);
            e.ts = std::chrono::steady_clock::now();
        }
        e.in_flight = false;
        return e.cache;
    }
    // Followers: return whatever we have (possibly stale, possibly empty on
    // very first call before the leader finishes).
    return snapshot;
}

// Same single-flight pattern for spectral stats (60s TTL).
std::string get_spectral_stats_cached(FieldStore* store) {
    struct CacheEntry {
        std::string cache;
        std::chrono::steady_clock::time_point ts{};
        bool in_flight = false;
    };
    static std::mutex mu;
    static std::unordered_map<FieldStore*, CacheEntry> entries;

    bool i_am_leader = false;
    std::string snapshot;
    {
        std::lock_guard<std::mutex> lk(mu);
        auto& e = entries[store];
        auto now = std::chrono::steady_clock::now();
        bool expired = e.cache.empty() ||
            std::chrono::duration_cast<std::chrono::seconds>(now - e.ts).count() > 60;
        if (expired && !e.in_flight) {
            e.in_flight = true;
            i_am_leader = true;
        }
        snapshot = e.cache;
    }

    if (i_am_leader) {
        std::string fresh;
        try { fresh = store->spectral_stats_by_realm(); } catch (...) {}
        std::lock_guard<std::mutex> lk(mu);
        auto& e = entries[store];
        if (!fresh.empty()) {
            e.cache = std::move(fresh);
            e.ts = std::chrono::steady_clock::now();
        }
        e.in_flight = false;
        return e.cache;
    }
    return snapshot;
}
}

ToolResult FieldRpcHandler::tool_health_check(const json& params) {
    if (!field_store_) return ToolResult::error("chitta-field store unavailable");

    // Fast path (default): only emit cheap fields. This is what hooks poll
    // every few seconds — full O(N) state scan + spectral stats can take
    // 15-30s under writer contention and stacks concurrent clients, which
    // has been a heap-corruption trigger. Pass `details=true` for the
    // expensive metrics.
    bool details = params.contains("arguments")
                       ? params["arguments"].value("details", false)
                       : params.value("details", false);

    std::ostringstream ss;
    ss << "Status: ok\n"
       << "chitta-field daemon healthy\n"
       << "  yantra   : " << (yantra_ ? "loaded" : "unavailable") << "\n"
       << "  backend  : chitta-field\n";

    ss << "  rpc over budget : " << rpc_budget_.over_budget_count() << "\n";

    if (!details) {
        // Fast path: O(1) only — raw_memory_count() returns payloads.len() (HashMap.len()),
        // avoiding O(N) states.read() that contends with encode_memory's write locks.
        size_t raw_count = field_store_->raw_memory_count();
        size_t pending   = field_store_->raw_pending_count();
        ss << "  memories : ~" << raw_count << "\n";
        if (pending > 0)
            ss << "  pending  : " << pending << " (awaiting embed)\n";
        json out = {
            {"status",           "ok"},
            {"backend",          "chitta-field"},
            {"yantra",           yantra_ ? "loaded" : "unavailable"},
            {"software_version", CHITTA_VERSION},
            {"protocol_major",   CHITTA_PROTOCOL_VERSION_MAJOR},
            {"protocol_minor",   CHITTA_PROTOCOL_VERSION_MINOR},
            {"pid",              static_cast<int>(getpid())},
            {"memory_count",     raw_count},
            {"pending_count",    pending},
            {"rpc_over_budget", rpc_budget_.over_budget_count()},
        };
        return ToolResult::ok(ss.str(), out);
    }

    // Details path: O(N) scans — only when explicitly requested.
    size_t mem_count = field_store_->memory_count();
    size_t sym_count = field_store_->symbol_count();

    ss << "  memories : " << mem_count << "\n"
       << "  symbols  : " << sym_count << "\n";

    json out = {
        {"status",           "ok"},
        {"backend",          "chitta-field"},
        {"yantra",           yantra_ ? "loaded" : "unavailable"},
        {"software_version", CHITTA_VERSION},
        {"protocol_major",   CHITTA_PROTOCOL_VERSION_MAJOR},
        {"protocol_minor",   CHITTA_PROTOCOL_VERSION_MINOR},
        {"pid",              static_cast<int>(getpid())},
        {"memory_count",     mem_count},
        {"symbol_count",     sym_count},
        {"rpc_over_budget",  rpc_budget_.over_budget_count()},
    };

    json stats_j;
    try {
        stats_j = json::parse(get_memory_stats_cached(field_store_));
    } catch (...) {
        stats_j = json::object();
    }
    if (!stats_j.is_null() && stats_j.contains("count_by_kind")) {
        out["count_by_kind"]  = stats_j["count_by_kind"];
        out["avg_confidence"] = stats_j.value("avg_confidence", 0.0f);
    }

    std::string chain = field_store_->chain_head();
    out["chain_head"] = chain.empty() ? "none (v1 data)" : chain;

    // Per-realm/kind embedding geometry — single-flight cached (60s TTL)
    try {
        std::string _spec_str = get_spectral_stats_cached(field_store_);
        if (_spec_str.empty()) throw std::runtime_error("spectral cache cold");
        auto spectral_j = json::parse(_spec_str);
        auto& by_realm = spectral_j["by_realm"];
        auto& by_kind  = spectral_j["by_kind"];
        auto& anomalies = spectral_j["anomalies"];

        if (by_realm.is_array() && !by_realm.empty()) {
            // Top 5 largest realms
            std::vector<json> sorted(by_realm.begin(), by_realm.end());
            std::sort(sorted.begin(), sorted.end(), [](const json& a, const json& b) {
                return a.value("count", 0) > b.value("count", 0);
            });
            ss << "\nTop realms (embedding geometry):\n";
            size_t shown = std::min(sorted.size(), size_t(5));
            for (size_t i = 0; i < shown; ++i) {
                ss << "  " << sorted[i].value("group", "?")
                   << " (" << sorted[i].value("count", 0) << ")"
                   << "  dim=" << std::fixed << std::setprecision(1)
                   << sorted[i].value("effective_dim", 0.0)
                   << "  iso=" << std::setprecision(3)
                   << sorted[i].value("isotropy", 0.0)
                   << "  cos=" << std::setprecision(3)
                   << sorted[i].value("mean_cosine_sim", 0.0) << "\n";
            }
            out["spectral_by_realm"] = by_realm;
        }
        if (by_kind.is_array() && !by_kind.empty()) {
            ss << "\nBy kind:\n";
            for (const auto& k : by_kind) {
                ss << "  " << k.value("group", "?")
                   << " (" << k.value("count", 0) << ")"
                   << "  dim=" << std::fixed << std::setprecision(1)
                   << k.value("effective_dim", 0.0)
                   << "  iso=" << std::setprecision(3)
                   << k.value("isotropy", 0.0)
                   << "  cos=" << std::setprecision(3)
                   << k.value("mean_cosine_sim", 0.0) << "\n";
            }
            out["spectral_by_kind"] = by_kind;
        }
        if (anomalies.is_array() && !anomalies.empty()) {
            ss << "\nAnomalies:\n";
            for (const auto& a : anomalies) {
                ss << "  ! " << a.value("group", "?")
                   << ": " << a.value("detail", "") << "\n";
            }
            out["anomalies"] = anomalies;
        }
    } catch (...) {}

    return ToolResult::ok(ss.str(), out);
}

ToolResult FieldRpcHandler::tool_version_check() {
    std::ostringstream ss;
    ss << "chitta " << CHITTA_VERSION << "\n"
       << "backend: chitta-field\n"
       << "protocol: " << CHITTA_PROTOCOL_VERSION_MAJOR
       << "." << CHITTA_PROTOCOL_VERSION_MINOR << "\n";

    json out = {
        {"version",  CHITTA_VERSION},
        {"backend",  "chitta-field"},
        {"protocol", {
            {"major", CHITTA_PROTOCOL_VERSION_MAJOR},
            {"minor", CHITTA_PROTOCOL_VERSION_MINOR},
        }},
    };
    return ToolResult::ok(ss.str(), out);
}

ToolResult FieldRpcHandler::tool_cycle(const json&) {
    if (!field_store_) return ToolResult::error("chitta-field store unavailable");

    field_store_->flush();

    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    auto [promoted, demoted] = field_store_->run_demotion(now_ms);

    std::ostringstream ss;
    ss << "Maintenance cycle complete\n"
       << "  flushed  : yes\n"
       << "  promoted : " << promoted << "\n"
       << "  demoted  : " << demoted  << "\n";

    return ToolResult::ok(ss.str(), {
        {"flushed",  true},
        {"promoted", promoted},
        {"demoted",  demoted},
    });
}

ToolResult FieldRpcHandler::tool_cleanup(const json& params) {
    if (!field_store_) return ToolResult::error("chitta-field store unavailable");

    std::string mode = params.value("mode", "confidence");

    if (mode == "dedupe") {
        // Delegate to consolidate_similar which has access to the embedding
        // index and correctly handles the Supersede lifecycle.
        float sim_threshold = params.value("threshold", 0.88f);
        json dedupe_params = {
            {"threshold",  sim_threshold},
            {"limit",      params.value("limit", 500)},
            {"dry_run",    params.value("dry_run", false)},
        };
        return tool_consolidate_similar(dedupe_params);
    }

    // Default mode: remove low-confidence memories
    float threshold = params.value("threshold", 0.05f);
    auto candidates = field_store_->recall_by_kind("wisdom", 1000);
    size_t removed = 0;
    for (const auto& hit : candidates) {
        if (hit.confidence < threshold) {
            field_store_->forget(hit.memory_id);
            if (field_store_->get_content(hit.memory_id).empty()) ++removed;
        }
    }

    std::ostringstream ss;
    ss << "Cleanup complete: removed " << removed << " memories below confidence "
       << threshold << "\n";

    return ToolResult::ok(ss.str(), {
        {"removed",   removed},
        {"threshold", threshold},
    });
}

ToolResult FieldRpcHandler::tool_soul_context(const json&) {
    if (!field_store_) return ToolResult::error("chitta-field store unavailable");

    json stats_j;
    try {
        stats_j = json::parse(get_memory_stats_cached(field_store_));
    } catch (...) {
        stats_j = json::object();
    }

    size_t total_memories = field_store_->memory_count();
    size_t wisdom_nodes   = 0;
    size_t beliefs        = 0;
    size_t episodes       = 0;

    if (stats_j.contains("count_by_kind") && stats_j["count_by_kind"].is_object()) {
        const auto& cbk = stats_j["count_by_kind"];
        wisdom_nodes = cbk.value("wisdom",    size_t(0));
        beliefs      = cbk.value("belief",    size_t(0));
        episodes     = cbk.value("episode",   size_t(0));
    }
    float avg_conf = stats_j.value("avg_confidence", 0.0f);

    size_t corrections_total = 0;
    size_t preferences_total = 0;
    if (stats_j.contains("count_by_kind") && stats_j["count_by_kind"].is_object()) {
        const auto& cbk = stats_j["count_by_kind"];
        corrections_total = cbk.value("correction", size_t(0));
        preferences_total = cbk.value("preference", size_t(0));
    }
    // Keep preview bounded to avoid large O(N) reads on hot path.
    size_t preview_limit = 100;
    auto corrections = field_store_->recall_by_kind("correction", preview_limit);
    auto preferences = field_store_->recall_by_kind("preference", preview_limit);

    std::ostringstream ss;
    ss << "Soul State Overview\n"
       << "  total memories : " << total_memories  << "\n"
       << "  wisdom nodes   : " << wisdom_nodes    << "\n"
       << "  beliefs        : " << beliefs         << "\n"
       << "  episodes       : " << episodes        << "\n"
       << "  corrections    : " << corrections_total << "\n"
       << "  preferences    : " << preferences_total << "\n"
       << "  avg confidence : " << std::fixed << std::setprecision(3) << avg_conf << "\n";

    if (!corrections.empty()) {
        ss << "\nTop corrections:\n";
        size_t shown = std::min(corrections.size(), size_t(5));
        for (size_t i = 0; i < shown; ++i) {
            ss << "  - " << utf8_trunc(corrections[i].content, 120) << "\n";
        }
    }
    if (!preferences.empty()) {
        ss << "\nTop preferences:\n";
        size_t shown = std::min(preferences.size(), size_t(5));
        for (size_t i = 0; i < shown; ++i) {
            ss << "  - " << utf8_trunc(preferences[i].content, 120) << "\n";
        }
    }

    // Spectral stats — single-flight cached (60s TTL) to avoid per-call SVD cost
    std::string spectral_str = get_spectral_stats_cached(field_store_);

    json out = {
        {"version",        CHITTA_VERSION},
        {"total_memories", total_memories},
        {"wisdom_nodes",   wisdom_nodes},
        {"beliefs",        beliefs},
        {"episodes",       episodes},
        {"corrections",    corrections_total},
        {"preferences",    preferences_total},
        {"avg_confidence", avg_conf},
    };
    if (stats_j.contains("count_by_kind"))
        out["count_by_kind"] = stats_j["count_by_kind"];

    try {
        auto spectral_j = json::parse(spectral_str);
        auto& by_realm  = spectral_j["by_realm"];
        auto& by_kind   = spectral_j["by_kind"];
        auto& anomalies = spectral_j["anomalies"];

        if (by_realm.is_array() && !by_realm.empty()) {
            std::vector<json> sorted(by_realm.begin(), by_realm.end());
            std::sort(sorted.begin(), sorted.end(), [](const json& a, const json& b) {
                return a.value("count", 0) > b.value("count", 0);
            });
            ss << "\nTop realms:\n";
            size_t shown = std::min(sorted.size(), size_t(5));
            for (size_t i = 0; i < shown; ++i) {
                ss << "  " << sorted[i].value("group", "?")
                   << " (" << sorted[i].value("count", 0) << ")"
                   << "  dim=" << std::fixed << std::setprecision(1)
                   << sorted[i].value("effective_dim", 0.0)
                   << "  iso=" << std::setprecision(3)
                   << sorted[i].value("isotropy", 0.0)
                   << "  cos=" << std::setprecision(3)
                   << sorted[i].value("mean_cosine_sim", 0.0) << "\n";
            }
            out["spectral_by_realm"] = by_realm;
        }
        if (by_kind.is_array() && !by_kind.empty()) {
            ss << "\nBy kind:\n";
            for (const auto& k : by_kind) {
                ss << "  " << k.value("group", "?")
                   << " (" << k.value("count", 0) << ")"
                   << "  dim=" << std::fixed << std::setprecision(1)
                   << k.value("effective_dim", 0.0)
                   << "  iso=" << std::setprecision(3)
                   << k.value("isotropy", 0.0)
                   << "  cos=" << std::setprecision(3)
                   << k.value("mean_cosine_sim", 0.0) << "\n";
            }
            out["spectral_by_kind"] = by_kind;
        }
        if (anomalies.is_array() && !anomalies.empty()) {
            ss << "\nAnomalies:\n";
            for (const auto& a : anomalies) {
                ss << "  ! " << a.value("group", "?")
                   << ": " << a.value("detail", "") << "\n";
            }
            out["anomalies"] = anomalies;
        }
    } catch (...) {}

    return ToolResult::ok(ss.str(), out);
}

ToolResult FieldRpcHandler::tool_resonance_stats(const json&) {
    return ToolResult::ok(
        "Resonance learner not available in chitta-field backend",
        {
            {"status",  "ok"},
            {"message", "Resonance learner not available in chitta-field backend"},
        });
}

ToolResult FieldRpcHandler::tool_subconscious_stats(const json&) {
    if (!field_store_) return ToolResult::error("chitta-field store unavailable");

    if (subconscious_) {
        const auto& st = subconscious_->stats();
        const auto& cfg = subconscious_->config();

        int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        int64_t started = st.started_at.load();
        int64_t uptime_s = started > 0 ? (now_ms - started) / 1000 : 0;

        std::ostringstream ss;
        ss << "Subconscious Stats\n"
           << "  running         : " << (subconscious_->is_running() ? "yes" : "no") << "\n"
           << "  uptime (s)      : " << uptime_s << "\n"
           << "  events_processed: " << st.events_processed.load() << "\n"
           << "  corrections     : " << st.corrections_detected.load() << "\n"
           << "  preferences     : " << st.preferences_detected.load() << "\n"
           << "  hygiene_runs    : " << st.hygiene_runs.load() << "\n"
           << "  demotion_runs   : " << st.demotion_runs.load() << "\n"
           << "  field_demoted   : " << st.field_demoted.load() << "\n"
           << "  field_deleted   : " << st.field_deleted.load() << "\n"
           << "  sleep_consolidation_runs: " << st.sleep_consolidation_runs.load() << "\n";

        json out = {
            {"running",                  subconscious_->is_running()},
            {"uptime_s",                 uptime_s},
            {"events_processed",         static_cast<size_t>(st.events_processed.load())},
            {"corrections_detected",     static_cast<size_t>(st.corrections_detected.load())},
            {"preferences_detected",     static_cast<size_t>(st.preferences_detected.load())},
            {"hygiene_runs",             static_cast<size_t>(st.hygiene_runs.load())},
            {"demotion_runs",            static_cast<size_t>(st.demotion_runs.load())},
            {"field_demoted",            static_cast<size_t>(st.field_demoted.load())},
            {"field_deleted",            static_cast<size_t>(st.field_deleted.load())},
            {"sleep_consolidation_runs", static_cast<size_t>(st.sleep_consolidation_runs.load())},
            {"hygiene_interval_min",     cfg.hygiene_interval.count()},
            {"demotion_interval_min",    cfg.demotion_interval.count()},
        };
        return ToolResult::ok(ss.str(), out);
    }

    size_t mem_count = field_store_->memory_count();
    size_t sym_count = field_store_->symbol_count();

    std::ostringstream ss;
    ss << "Subconscious not running\n"
       << "FieldStore stats:\n"
       << "  memories : " << mem_count << "\n"
       << "  symbols  : " << sym_count << "\n";

    return ToolResult::ok(ss.str(), {
        {"running",      false},
        {"memory_count", mem_count},
        {"symbol_count", sym_count},
    });
}

ToolResult FieldRpcHandler::tool_reembed_memories(const json& params) {
    if (!field_store_) return ToolResult::error("chitta-field store unavailable");

    size_t limit = static_cast<size_t>(params.value("limit", 500));

    field_store_->emit_event("admin", "reembed_request", "wisdom",
        "{\"limit\":" + std::to_string(limit) + "}");

    std::ostringstream ss;
    ss << "Reembed request queued for up to " << limit << " memories\n";

    return ToolResult::ok(ss.str(), {
        {"queued", true},
        {"limit",  limit},
    });
}

ToolResult FieldRpcHandler::tool_rebuild_fts_index(const json&) {
    if (!field_store_) return ToolResult::error("chitta-field store unavailable");

    field_store_->emit_event("admin", "rebuild_fts_request", "fts", "{}");

    return ToolResult::ok("FTS rebuild requested", {
        {"status", "requested"},
    });
}

ToolResult FieldRpcHandler::tool_hygiene_stats(const json&) {
    if (!field_store_) return ToolResult::error("chitta-field store unavailable");

    json stats_j;
    try {
        stats_j = json::parse(get_memory_stats_cached(field_store_));
    } catch (...) {
        stats_j = json::object();
    }

    size_t total     = field_store_->memory_count();
    float avg_conf   = stats_j.value("avg_confidence", 0.0f);

    std::ostringstream ss;
    ss << "Memory Hygiene Stats\n"
       << "  total memories : " << total    << "\n"
       << "  avg confidence : " << std::fixed << std::setprecision(3) << avg_conf << "\n";

    if (stats_j.contains("count_by_kind") && stats_j["count_by_kind"].is_object()) {
        ss << "  by kind:\n";
        for (auto& [kind, cnt] : stats_j["count_by_kind"].items()) {
            ss << "    " << kind << ": " << cnt << "\n";
        }
    }

    json out = {
        {"total_memories", total},
        {"avg_confidence", avg_conf},
    };
    if (stats_j.contains("count_by_kind"))
        out["count_by_kind"] = stats_j["count_by_kind"];

    return ToolResult::ok(ss.str(), out);
}

ToolResult FieldRpcHandler::tool_hygiene_run(const json& params) {
    if (!field_store_) return ToolResult::error("chitta-field store unavailable");

    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    auto [promoted, demoted] = field_store_->run_demotion(now_ms);

    float threshold = params.value("threshold", 0.05f);
    auto candidates = field_store_->recall_by_kind("wisdom", 1000);
    size_t removed = 0;
    for (const auto& hit : candidates) {
        if (hit.confidence < threshold) {
            field_store_->forget(hit.memory_id);
            if (field_store_->get_content(hit.memory_id).empty()) ++removed;
        }
    }

    // Purge corrupt nodes: iterates all payloads directly in Rust (not by kind)
    size_t purged = field_store_->purge_corrupt();

    size_t trimmed = field_store_->trim_realm_names();
    size_t requeued = field_store_->requeue_ghost_embeddings();
    field_store_->flush();

    std::ostringstream ss;
    ss << "Hygiene run complete\n"
       << "  promoted : " << promoted  << "\n"
       << "  demoted  : " << demoted   << "\n"
       << "  removed  : " << removed   << " (confidence < " << threshold << ")\n"
       << "  purged   : " << purged    << " corrupt/empty nodes\n"
       << "  trimmed  : " << trimmed   << " dirty realm names\n"
       << "  requeued : " << requeued  << " ghost embeddings\n";

    return ToolResult::ok(ss.str(), {
        {"promoted",  promoted},
        {"demoted",   demoted},
        {"removed",   removed},
        {"purged",    purged},
        {"trimmed",   trimmed},
        {"requeued",  requeued},
        {"threshold", threshold},
    });
}

ToolResult FieldRpcHandler::tool_import_soul(const json& params) {
    if (!field_store_) return ToolResult::error("chitta-field store unavailable");

    std::string content;
    if (params.contains("file") && params["file"].is_string()) {
        std::ifstream f(params["file"].get<std::string>());
        if (!f.is_open()) {
            return ToolResult::error("Cannot open file: " + params["file"].get<std::string>());
        }
        content = std::string(std::istreambuf_iterator<char>(f),
                              std::istreambuf_iterator<char>());
    } else if (params.contains("content") && params["content"].is_string()) {
        content = params["content"].get<std::string>();
    } else {
        return ToolResult::error("file or content parameter required");
    }

    std::string realm = params.value("realm", "brahman");
    std::string source_session = params.value("source_session", "");

    size_t imported = 0;
    std::istringstream iss(content);
    std::string line;
    std::string current_tag;
    std::string current_title;
    std::ostringstream current_body;
    bool in_entry = false;

    auto flush_entry = [&]() {
        if (!in_entry) return;
        std::string body = current_body.str();
        while (!body.empty() && (body.back() == '\n' || body.back() == ' '))
            body.pop_back();
        std::string full;
        if (!current_tag.empty()) {
            full = "[" + current_tag + "] " + current_title;
            if (!body.empty()) full += "\n" + body;
        } else {
            full = current_title;
            if (!body.empty()) full += "\n" + body;
        }
        if (!full.empty()) {
            float conf = params.value("confidence", 0.8f);
            auto emb   = embed_ssl_aware(full);
            uint64_t id = field_store_->remember("wisdom", realm, full, emb, conf, 0.001f);
            if (id > 0 && !source_session.empty())
                field_store_->set_source_session(id, source_session);
            ++imported;
        }
        current_tag.clear();
        current_title.clear();
        current_body.str("");
        current_body.clear();
        in_entry = false;
    };

    while (std::getline(iss, line)) {
        if (!line.empty() && line[0] == '[') {
            size_t close = line.find(']');
            if (close != std::string::npos) {
                flush_entry();
                current_tag   = line.substr(1, close - 1);
                current_title = (close + 1 < line.size()) ? line.substr(close + 2) : "";
                in_entry = true;
                continue;
            }
        }
        if (in_entry) {
            current_body << line << "\n";
        }
    }
    flush_entry();

    std::ostringstream ss;
    ss << "Imported " << imported << " memories from soul content\n";

    return ToolResult::ok(ss.str(), {
        {"imported", imported},
        {"realm",    realm},
    });
}

ToolResult FieldRpcHandler::tool_export_soul(const json& params) {
    if (!field_store_) return ToolResult::error("chitta-field store unavailable");

    size_t limit = static_cast<size_t>(params.value("limit", 500));

    static const std::vector<std::string> KINDS = {
        "wisdom", "correction", "preference", "belief", "milestone"
    };

    std::ostringstream ssl;
    size_t total = 0;

    for (const auto& kind : KINDS) {
        auto hits = field_store_->recall_by_kind(kind, limit);
        for (const auto& h : hits) {
            if (h.content.empty()) continue;
            if (h.content[0] == '[') {
                ssl << h.content;
            } else {
                ssl << "[" << kind << "] " << h.content;
            }
            if (!h.content.empty() && h.content.back() != '\n') ssl << "\n";
            ssl << "\n";
            ++total;
        }
    }

    std::string output = ssl.str();

    if (params.contains("file") && params["file"].is_string()) {
        std::ofstream f(params["file"].get<std::string>());
        if (!f.is_open()) {
            return ToolResult::error("Cannot open file for writing: " +
                params["file"].get<std::string>());
        }
        f << output;
    }

    std::ostringstream ss;
    ss << "Exported " << total << " memories\n";

    return ToolResult::ok(ss.str(), {
        {"exported", total},
        {"content",  output},
    });
}

ToolResult FieldRpcHandler::tool_chitta_health(const json&) {
    if (!field_store_) return ToolResult::error("chitta-field store unavailable");

    size_t mem_count   = field_store_->memory_count();
    size_t sym_count   = field_store_->symbol_count();
    bool   store_ok    = field_store_->healthy();

    std::string yantra_status = "unavailable";
    std::string yantra_model;
    if (yantra_) {
        yantra_status = "loaded";
        yantra_model  = yantra_->execution_provider_name();
    }

    bool subconscious_ok = subconscious_ && subconscious_->is_running();

    std::ostringstream ss;
    ss << "Feedback Loop Diagnostics\n"
       << "  store         : " << (store_ok ? "ok" : "error") << "\n"
       << "  memory_count  : " << mem_count << "\n"
       << "  symbol_count  : " << sym_count << "\n"
       << "  yantra        : " << yantra_status << "\n";
    if (!yantra_model.empty()) ss << "  yantra_model  : " << yantra_model << "\n";
    ss << "  subconscious  : " << (subconscious_ok ? "running" : "not running") << "\n"
       << "  rpc_over_budget: " << rpc_budget_.over_budget_count() << "\n"
       << "  fts           : available (chitta-field BM25)\n"
       << "  backend       : chitta-field\n";

    json out = {
        {"store",         store_ok ? "ok" : "error"},
        {"memory_count",  mem_count},
        {"symbol_count",  sym_count},
        {"yantra",        yantra_status},
        {"subconscious",  subconscious_ok ? "running" : "not running"},
        {"fts",           "available"},
        {"backend",       "chitta-field"},
        {"rpc_over_budget", rpc_budget_.over_budget_count()},
    };
    if (!yantra_model.empty()) out["yantra_model"] = yantra_model;

    return ToolResult::ok(ss.str(), out);
}

ToolResult FieldRpcHandler::tool_theme_list(const json& params) {
    std::string realm = params.value("realm", "");
    size_t limit = params.value("limit", 20);
    auto hits = field_store_->recall_by_kind("theme", limit);
    json themes = hits_to_results_json(hits);
    return ToolResult::ok(std::to_string(themes.size()) + " theme(s)",
        {{"themes", themes}, {"count", themes.size()}});
}

ToolResult FieldRpcHandler::tool_theme_get(const json& params) {
    auto id = extract_id(params);
    if (!id) return ToolResult::error("id is required");
    auto content = field_store_->get_content(id);
    return ToolResult::ok(content.empty() ? "Theme not found" : content,
        {{"id", id}, {"content", content}});
}

ToolResult FieldRpcHandler::tool_theme_recall(const json& params) {
    std::string query = params.value("query", "");
    if (query.empty()) return ToolResult::error("query is required");
    auto emb = embed_query(query);
    auto hits = field_store_->recall(emb, params.value("limit", 10), params.value("realm", ""));
    return ToolResult::ok(std::to_string(hits.size()) + " theme result(s)",
        {{"results", hits_to_results_json(hits)}});
}

ToolResult FieldRpcHandler::tool_theme_stats(const json&) {
    auto hits = field_store_->recall_by_kind("theme", 1000);
    return ToolResult::ok("Theme stats", {{"total_themes", hits.size()}});
}

ToolResult FieldRpcHandler::tool_realm_list() {
    auto raw = field_store_->realm_list();
    auto realms = json::parse(raw, nullptr, false);
    if (realms.is_discarded()) realms = json::array();
    std::ostringstream ss;
    ss << realms.size() << " realm(s):\n";
    for (const auto& r : realms) ss << "  " << r << "\n";
    return ToolResult::ok(ss.str(), {{"realms", realms}});
}

ToolResult FieldRpcHandler::tool_realm_get(const json& params) {
    auto [id, id_str] = parse_id(params);
    if (id_str.empty()) return ToolResult::error("id is required");
    auto raw = field_store_->query_subject("memory:" + id_str);
    auto triplets = json::parse(raw, nullptr, false);
    json realm_list = json::array();
    if (!triplets.is_discarded() && triplets.is_array()) {
        for (const auto& t : triplets) {
            if (t.value("predicate", "") == "in_realm")
                realm_list.push_back(t.value("object", ""));
        }
    }
    return ToolResult::ok(std::to_string(realm_list.size()) + " realm(s)",
        {{"id", id_str}, {"realms", realm_list}});
}

ToolResult FieldRpcHandler::tool_realm_set(const json& params) {
    auto [id, id_str] = parse_id(params);
    std::string realm = params.value("realm", "");
    if (id_str.empty() || realm.empty()) return ToolResult::error("id and realm are required");
    if (!field_store_->set_realm(id, realm)) return ToolResult::error("memory not found");
    return ToolResult::ok("Realm set", {{"id", id_str}, {"realm", realm}});
}

ToolResult FieldRpcHandler::tool_realm_add(const json& params) {
    return tool_realm_set(params);
}

ToolResult FieldRpcHandler::tool_realm_remove(const json& params) {
    auto [id, id_str] = parse_id(params);
    std::string realm = params.value("realm", "");
    if (id_str.empty() || realm.empty()) return ToolResult::error("id and realm are required");
    field_store_->emit_event("realm", "remove", "memory:" + id_str, realm);
    return ToolResult::ok("Realm removed (event emitted)", {{"id", id_str}, {"realm", realm}});
}

ToolResult FieldRpcHandler::tool_realm_visibility(const json& params) {
    auto [id, id_str] = parse_id(params);
    int vis = params.value("visibility", 0);
    if (id_str.empty()) return ToolResult::error("id is required");
    field_store_->emit_event("realm", "visibility", "memory:" + id_str,
                             std::to_string(vis));
    return ToolResult::ok("Visibility set", {{"id", id_str}, {"visibility", vis}});
}

ToolResult FieldRpcHandler::tool_realm_detect() {
    const char* env = std::getenv("CHITTA_REALM");
    std::string realm = env ? env : "brahman";
    return ToolResult::ok("Realm: " + realm, {{"realm", realm}});
}

ToolResult FieldRpcHandler::tool_queue_status(const json&) {
    size_t processed = queue_count_ ? queue_count_->load() : 0;
    size_t distilled = queue_distill_count_ ? queue_distill_count_->load() : 0;
    size_t failed    = queue_fail_count_ ? queue_fail_count_->load() : 0;
    size_t in_file   = 0;
    if (!failed_queue_path_.empty()) {
        std::ifstream f(failed_queue_path_);
        std::string ln;
        while (std::getline(f, ln)) if (!ln.empty()) in_file++;
    }
    // Live depth: in-flight claimed batch (RAM counter) + unclaimed queue file lines.
    // The .processing file is NOT counted — its unprocessed tail IS batch_remaining.
    size_t in_flight = queue_batch_remaining_ ? queue_batch_remaining_->load() : 0;
    size_t unclaimed = 0;
    if (!queue_path_.empty()) {
        std::ifstream f(queue_path_);
        std::string ln;
        while (std::getline(f, ln)) if (!ln.empty()) unclaimed++;
    }
    size_t pending_embeds = field_store_ ? field_store_->raw_pending_count() : 0;
    // Slow lane (distill_trigger re-routes): unclaimed + unprocessed suffix of
    // its claimed batch (file-based — .ckpt watermark marks the done prefix).
    size_t slow_pending = 0;
    if (!queue_path_.empty()) {
        auto count_lines = [](const std::string& p) {
            size_t n = 0; std::ifstream f(p); std::string ln;
            while (std::getline(f, ln)) if (!ln.empty()) n++;
            return n;
        };
        std::string slow = queue_path_ + ".slow";
        slow_pending = count_lines(slow);
        size_t claimed = count_lines(slow + ".processing");
        if (claimed > 0) {
            size_t done = 0;
            std::ifstream ck(slow + ".processing.ckpt");
            if (ck.good()) ck >> done;
            slow_pending += (claimed > done) ? claimed - done : 0;
        }
    }
    json s;
    s["pending"]            = in_flight + unclaimed;
    s["pending_in_flight"]  = in_flight;
    s["pending_unclaimed"]  = unclaimed;
    s["pending_distill"]    = slow_pending;
    s["processed"]          = processed;
    s["distill_processed"]  = distilled;
    s["failed"]             = failed;
    s["pending_embeddings"] = pending_embeds;
    s["dead_letter_count"]  = in_file;
    s["dead_letter_path"]   = failed_queue_path_;
    std::ostringstream ss;
    ss << "Queue: " << (in_flight + unclaimed) << " pending (" << in_flight
       << " in-flight, " << unclaimed << " unclaimed), " << slow_pending
       << " distills queued, " << processed
       << " processed, " << distilled << " distilled, " << failed << " failed; "
       << pending_embeds << " embeddings pending";
    if (in_file > 0) ss << "; " << in_file << " dead-letters";
    return ToolResult::ok(ss.str(), s);
}

ToolResult FieldRpcHandler::tool_ledger_health(const json&) {
    // Queue stats
    size_t processed = queue_count_ ? queue_count_->load() : 0;
    size_t failed    = queue_fail_count_ ? queue_fail_count_->load() : 0;
    size_t in_file   = 0;
    if (!failed_queue_path_.empty()) {
        std::ifstream f(failed_queue_path_);
        std::string ln;
        while (std::getline(f, ln)) if (!ln.empty()) in_file++;
    }

    // Ledger event counts by kind
    std::string raw = field_store_->ledger_query_json("{}");
    auto events = json::parse(raw, nullptr, false);
    std::unordered_map<std::string, int> kind_counts;
    int total_events = 0;
    if (!events.is_discarded() && events.is_array()) {
        total_events = static_cast<int>(events.size());
        for (const auto& ev : events) {
            std::string kind = ev.value("kind", "unknown");
            kind_counts[kind]++;
        }
    }
    json counts = json::object();
    for (const auto& [k, v] : kind_counts) counts[k] = v;

    json s;
    s["queue_processed"]   = processed;
    s["queue_failed"]      = failed;
    s["dead_letter_count"] = in_file;
    s["ledger_total"]      = total_events;
    s["ledger_by_kind"]    = counts;

    std::ostringstream ss;
    ss << "Ledger: " << total_events << " events";
    for (const auto& [k, v] : kind_counts) ss << ", " << k << "=" << v;
    ss << "; Queue: " << processed << " processed, " << failed << " failed";
    return ToolResult::ok(ss.str(), s);
}

ToolResult FieldRpcHandler::tool_memory_provenance(const json& params) {
    auto [id, id_str] = parse_id(params);
    if (id_str.empty()) return ToolResult::error("id is required");

    // 1. Basic metadata
    auto meta_raw = field_store_->get_memory_metadata(id);
    json meta = json::parse(meta_raw, nullptr, false);

    // 2. Outgoing triplets (what this memory claims)
    auto out_raw = field_store_->query_subject(id_str);
    json out_trips = json::parse(out_raw, nullptr, false);

    // 3. Incoming triplets (what claims about this memory)
    auto in_raw = field_store_->query_object(id_str);
    json in_trips = json::parse(in_raw, nullptr, false);

    // 4. Source/evidence triplets
    json source, evidence, superseded_by, supersedes;
    if (!out_trips.is_discarded() && out_trips.is_array()) {
        for (auto& t : out_trips) {
            auto pred = t.value("predicate", "");
            if (pred == "supersedes") supersedes.push_back(t.value("object",""));
            if (pred == "source")     source = t.value("object","");
            if (pred == "evidence")   evidence = t.value("object","");
        }
    }
    if (!in_trips.is_discarded() && in_trips.is_array()) {
        for (auto& t : in_trips) {
            if (t.value("predicate","") == "supersedes")
                superseded_by.push_back(t.value("subject",""));
        }
    }

    // 5. Memory status
    auto ms_params = json{{"id", id_str}};
    auto ms = tool_memory_status(ms_params);

    // Build output
    std::ostringstream ss;
    ss << "Memory #" << id_str << " provenance:\n";
    if (meta.is_object()) {
        ss << "  kind:       " << meta.value("kind","?") << "\n";
        ss << "  confidence: " << meta.value("confidence", 0.0f) << "\n";
        ss << "  strength:   " << meta.value("strength", 0.0f) << "\n";
        ss << "  access_count: " << meta.value("access_count", 0) << "\n";
        ss << "  created_at: " << meta.value("created_at_ms", 0) << "\n";
    }
    ss << "  status:     " << ms.structured.value("status","active") << "\n";
    if (!source.is_null())   ss << "  source:     " << source.dump() << "\n";
    if (!evidence.is_null()) ss << "  evidence:   " << evidence.dump() << "\n";
    if (!superseded_by.empty()) ss << "  superseded_by: " << superseded_by.dump() << "\n";
    if (!supersedes.empty())    ss << "  supersedes:    " << supersedes.dump() << "\n";

    json result = {
        {"id", id_str},
        {"status", ms.structured.value("status","active")},
        {"source", source},
        {"evidence", evidence},
        {"superseded_by", superseded_by},
        {"supersedes", supersedes},
        {"meta", meta},
    };
    return ToolResult::ok(ss.str(), result);
}

ToolResult FieldRpcHandler::tool_memory_status(const json& params) {
    auto [id, id_str] = parse_id(params);
    if (id_str.empty()) return ToolResult::error("id is required");

    // Check if any other memory supersedes this one
    auto raw = field_store_->query_object(std::to_string(id));
    json triplets = json::parse(raw, nullptr, false);
    std::string status = "active";
    uint64_t superseded_by = 0;
    if (!triplets.is_discarded() && triplets.is_array()) {
        for (const auto& t : triplets) {
            if (t.value("predicate", "") == "supersedes") {
                status = "superseded";
                try { superseded_by = std::stoull(t.value("subject", "0")); } catch (...) {}
                break;
            }
        }
    }
    // Check if this memory supersedes others (it's a correction)
    auto raw2 = field_store_->query_subject(std::to_string(id));
    json out_trips = json::parse(raw2, nullptr, false);
    std::vector<uint64_t> supersedes_ids;
    if (!out_trips.is_discarded() && out_trips.is_array()) {
        for (const auto& t : out_trips) {
            if (t.value("predicate", "") == "supersedes") {
                try { supersedes_ids.push_back(std::stoull(t.value("object", "0"))); } catch (...) {}
            }
        }
    }
    json result;
    result["id"]            = std::to_string(id);
    result["status"]        = status;
    result["superseded_by"] = superseded_by;
    result["supersedes"]    = supersedes_ids;
    std::string text = "Memory " + std::to_string(id) + ": " + status;
    if (superseded_by > 0) text += " (by " + std::to_string(superseded_by) + ")";
    return ToolResult::ok(text, result);
}

ToolResult FieldRpcHandler::tool_trim_realm_names(const json&) {
    if (!field_store_) return ToolResult::error("field store unavailable");
    auto count = field_store_->trim_realm_names();
    std::string msg = "Trimmed " + std::to_string(count) + " realm name(s)";
    return ToolResult::ok(msg, {{"trimmed", count}});
}

ToolResult FieldRpcHandler::tool_remap_realms(const json& p) {
    if (!field_store_) return ToolResult::error("field store unavailable");
    std::string mapping_json;
    if (p.contains("mapping_file")) {
        std::ifstream f(p["mapping_file"].get<std::string>());
        if (!f) return ToolResult::error("cannot open mapping_file");
        std::stringstream ss; ss << f.rdbuf(); mapping_json = ss.str();
    } else if (p.contains("mapping")) {
        mapping_json = p["mapping"].is_string()
            ? p["mapping"].get<std::string>() : p["mapping"].dump();
    } else {
        return ToolResult::error("require 'mapping' (object) or 'mapping_file' (path)");
    }
    bool dry_run = p.value("dry_run", true);  // safe default: never mutate unless asked
    std::string res = field_store_->remap_realms(mapping_json, dry_run);
    json data = json::parse(res, nullptr, false);
    if (data.is_discarded()) return ToolResult::error("remap returned invalid JSON");
    std::string msg = (dry_run ? "[dry-run] would move " : "moved ")
        + std::to_string(data.value("moved", 0)) + " memories -> "
        + std::to_string(data.value("final_realms", 0)) + " realms";
    return ToolResult::ok(msg, data);
}

ToolResult FieldRpcHandler::tool_save_spectral_snapshot(const json&) {
    if (!field_store_) return ToolResult::error("field store unavailable");
    auto filename = field_store_->save_spectral_snapshot();
    if (filename.empty()) return ToolResult::error("failed to save snapshot");
    return ToolResult::ok("Saved: " + filename, {{"filename", filename}});
}

ToolResult FieldRpcHandler::tool_spectral_drift(const json&) {
    if (!field_store_) return ToolResult::error("field store unavailable");
    auto drift_json = field_store_->spectral_drift();
    json drift_j;
    try { drift_j = json::parse(drift_json); } catch (...) { drift_j = json::object(); }

    if (drift_j.contains("error")) {
        return ToolResult::ok(drift_j.value("error", "unknown"), drift_j);
    }

    std::ostringstream ss;
    double age = drift_j.value("snapshot_age_hours", 0.0);
    size_t total = drift_j.value("total_drifted", size_t(0));
    ss << "Drift since " << std::fixed << std::setprecision(1) << age << "h ago: "
       << total << " group(s) changed\n";

    if (drift_j.contains("drifts") && drift_j["drifts"].is_array()) {
        for (const auto& d : drift_j["drifts"]) {
            double iso_d = d.value("isotropy_delta", 0.0);
            double cos_d = d.value("cosine_delta", 0.0);
            ss << "  " << d.value("group", "?")
               << "  iso " << (iso_d >= 0 ? "+" : "") << std::setprecision(3) << iso_d
               << "  cos " << (cos_d >= 0 ? "+" : "") << std::setprecision(3) << cos_d
               << "\n";
        }
    }

    return ToolResult::ok(ss.str(), drift_j);
}

ToolResult FieldRpcHandler::tool_compact_wal(const json&) {
    if (!field_store_) return ToolResult::error("field store unavailable");
    // Saves a full snapshot, then deletes the WAL segments it covers.
    auto raw = field_store_->compact_wal();
    if (raw < 0) return ToolResult::error("compaction failed");
    std::string msg = "WAL compacted: snapshot saved, " + std::to_string(raw) + " segment(s) deleted";
    return ToolResult::ok(msg, {{"segments_deleted", raw}});
}

} // namespace chitta
