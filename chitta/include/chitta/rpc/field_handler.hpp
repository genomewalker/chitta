#pragma once
// FieldRpcHandler: RPC handler backed by FieldStore + VakYantra.

#include "protocol.hpp"
#include "../ssl_gloss.hpp"
#include "../field_store.hpp"
#include "../vak.hpp"
#include "../embed_queue.hpp"
#include "../code_intel.hpp"
#include "../mind/subconscious.hpp"
#include "../sadhana/sadhana_manager.hpp"
#include "../version.hpp"
#include "../daemon_config.hpp"
#include "../ingester.hpp"
#include "../wiki_export.hpp"
#include "../embedding_export.hpp"
#include "../query_intent.hpp"
#include "../text_utils.hpp"
#include "../transcript_parser.hpp"
#include "sandbox.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <optional>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <regex>
#include <array>
#include <numeric>
#include <unistd.h>
#include <glob.h>
#include <iostream>

namespace chitta {

using json = nlohmann::json;

struct ToolResult {
    bool is_error = false;
    std::string text;
    json structured;
    static ToolResult ok(const std::string& t, const json& s = json()) { return {false, t, s}; }
    static ToolResult error(const std::string& msg) { return {true, msg, json()}; }
};

inline std::string display_path(const std::string& file_path) {
    size_t last_slash = file_path.rfind('/');
    if (last_slash == std::string::npos) return file_path;
    std::string basename = file_path.substr(last_slash + 1);
    if (last_slash > 0) {
        size_t prev_slash = file_path.rfind('/', last_slash - 1);
        if (prev_slash != std::string::npos) return file_path.substr(prev_slash + 1);
    }
    return basename;
}

class FieldRpcHandler {
public:
    explicit FieldRpcHandler(FieldStore* fs, VakYantra* yantra)
        : field_store_(fs), yantra_(yantra) {
        register_tools();
    }

    void set_embed_queue(EmbedQueue* eq) { embed_queue_ = eq; }

    // Base mind dir (parent of chitta-field). Used by tool_consolidation_pass to
    // honor the .disable_consolidation marker at the same path the hooks check.
    void set_mind_path(const std::string& p) { mind_path_ = p; }

    void set_subconscious(Subconscious* s) { subconscious_ = s; }
    void set_sadhana_manager(SadhanaManager* sm) { sadhana_manager_ = sm; }
    void set_queue_stats(std::atomic<size_t>* count, std::atomic<size_t>* fails,
                         const std::string& failed_path) {
        queue_count_ = count; queue_fail_count_ = fails; failed_queue_path_ = failed_path;
    }
    using RecallCallback = std::function<void(const std::vector<uint64_t>&, int)>;
    void set_recall_callback(RecallCallback cb) { recall_callback_ = std::move(cb); }

    // Called after any write that stores a memory with empty embedding (pending backfill).
    // Used by the backfill thread to wake immediately rather than waiting 30s.
    using WriteNotifyCallback = std::function<void()>;
    void set_write_notify_callback(WriteNotifyCallback cb) { write_notify_fn_ = std::move(cb); }
    void fire_write_notify() const { if (write_notify_fn_) write_notify_fn_(); }
    FieldStore* get_field_store() const { return field_store_; }
    VakYantra* get_yantra() const { return yantra_; }

    std::string get_distill_model() const {
        std::lock_guard<std::mutex> lk(distill_mutex_);
        return distill_model_;
    }
    void set_distill_model(const std::string& m) {
        std::lock_guard<std::mutex> lk(distill_mutex_);
        distill_model_ = m;
    }
    bool get_distill_enabled() const { return distill_enabled_.load(); }
    void set_distill_enabled(bool e) { distill_enabled_.store(e); }

    // Serialize direct field_store access outside handle() (maintenance thread, queue processor).
    // Returns an exclusive lock — write-side use only.
    //
    // LOCK-DIRECTION INVARIANT (load-bearing): rpc_mutex_ is always OUTERMOST
    // relative to chitta-field's internal Rust locks — C++ acquires it, then
    // calls into Rust, and every cf_* call releases all Rust locks before
    // returning. The FFI has no Rust→C++ callbacks; adding one would let the
    // two lock domains interleave and deadlock across the language boundary.
    // See the matching note at the top of field_store.hpp.
    std::unique_lock<std::shared_mutex> acquire_lock() { return std::unique_lock<std::shared_mutex>(rpc_mutex_); }

    // Shared lock for maintenance ops whose mutations are protected by their
    // own internal Rust locks (parking_lot RwLock inside FieldStore). Using a
    // shared lock here means periodic background work (sync_foreign, flush,
    // demotion) does not starve concurrent reader RPCs. Writer RPCs still
    // serialize via the exclusive side.
    std::shared_lock<std::shared_mutex> acquire_shared_lock() { return std::shared_lock<std::shared_mutex>(rpc_mutex_); }
    std::shared_mutex& rpc_mutex() { return rpc_mutex_; }

    // Lock-free health_check fast path — bypasses both thread pool and rpc_mutex_.
    // Both raw_memory_count() and raw_pending_count() use AtomicUsize internally.
    // Called inline on the socket-loop thread when details=false (the default).
    std::string fast_health_check_json(const json& req_id, int pool_workers, int pool_active, int pool_pending) const {
        if (!field_store_) {
            json err = {{"jsonrpc","2.0"},{"id",req_id},
                        {"error",{{"code",-32000},{"message","store unavailable"}}}};
            return err.dump(-1, ' ', false, json::error_handler_t::replace);
        }
        size_t mem  = field_store_->raw_memory_count();
        size_t pend = field_store_->raw_pending_count();
        json out = {
            {"status",           "ok"},
            {"backend",          "chitta-field"},
            {"yantra",           yantra_ ? "loaded" : "unavailable"},
            {"software_version", CHITTA_VERSION},
            {"protocol_major",   CHITTA_PROTOCOL_VERSION_MAJOR},
            {"protocol_minor",   CHITTA_PROTOCOL_VERSION_MINOR},
            {"pid",              static_cast<int>(getpid())},
            {"memory_count",     mem},
            {"pending_count",    pend},
            {"pool_workers",     pool_workers},
            {"pool_active",      pool_active},
            {"pool_pending",     pool_pending},
        };
        std::string text = "Status: ok\nchitta-field daemon healthy\n  memories : ~"
                           + std::to_string(mem) + "\n";
        if (pend > 0) text += "  pending  : " + std::to_string(pend) + " (awaiting embed)\n";
        json resp = {
            {"jsonrpc", "2.0"},
            {"id",      req_id},
            {"result", {
                {"content", {{{"type","text"},{"text", text}}}},
                {"structured", out}
            }}
        };
        return resp.dump(-1, ' ', false, json::error_handler_t::replace);
    }

    void run_belief_maintenance(float stale_strength_threshold = 0.1f,
                                int stale_days = 30,
                                float dup_threshold = 0.97f,
                                size_t max_dups = 5) {
        std::unique_lock<std::shared_mutex> _lk(rpc_mutex_);
        size_t demoted = 0, contradictions_archived = 0, dups_merged = 0;

        // 1. Stale belief demotion: archive Active memories with decayed strength below threshold
        {
            std::string raw = field_store_->list_memories("", "", "recency", 2000, 0);
            try {
                auto arr = json::parse(raw);
                auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                for (const auto& m : arr) {
                    uint64_t id = m.value("id", uint64_t(0));
                    if (id == 0) continue;
                    std::string meta_json = field_store_->get_memory_metadata(id);
                    if (meta_json.empty()) continue;
                    auto meta = json::parse(meta_json, nullptr, false);
                    if (meta.is_discarded()) continue;
                    std::string status = meta.value("status", "Active");
                    if (status != "Active") continue;
                    float strength = meta.value("strength", 1.0f);
                    float decay_rate = meta.value("decay_rate", 0.001f);
                    int64_t last_ms = meta.value("last_strengthened_ms", int64_t(0));
                    if (last_ms == 0) last_ms = meta.value("created_at_ms", int64_t(0));
                    if (last_ms == 0) continue;
                    double age_days = static_cast<double>(now_ms - last_ms) / 86400000.0;
                    if (age_days < stale_days) continue;
                    double effective = strength * std::exp(-decay_rate * age_days);
                    if (effective < stale_strength_threshold) {
                        field_store_->set_memory_status(id, 3); // Archived
                        ++demoted;
                    }
                }
            } catch (...) {}
        }

        // 2. Contradiction resolution: archive Contradicted memories whose contradictors are gone
        {
            std::string raw = field_store_->list_memories("", "", "recency", 2000, 0);
            try {
                auto arr = json::parse(raw);
                for (const auto& m : arr) {
                    uint64_t id = m.value("id", uint64_t(0));
                    if (id == 0) continue;
                    std::string meta_json = field_store_->get_memory_metadata(id);
                    if (meta_json.empty()) continue;
                    auto meta = json::parse(meta_json, nullptr, false);
                    if (meta.is_discarded()) continue;
                    std::string status = meta.value("status", "Active");
                    if (status != "Contradicted") continue;
                    auto conflicts = field_store_->get_conflicts(id);
                    bool any_active = false;
                    for (uint64_t cid : conflicts) {
                        std::string cmeta_json = field_store_->get_memory_metadata(cid);
                        if (cmeta_json.empty()) continue;
                        auto cmeta = json::parse(cmeta_json, nullptr, false);
                        if (cmeta.is_discarded()) continue;
                        std::string cs = cmeta.value("status", "Active");
                        if (cs == "Active" || cs == "Verified") { any_active = true; break; }
                    }
                    if (!any_active) {
                        field_store_->set_memory_status(id, 3); // Archived
                        ++contradictions_archived;
                    }
                }
            } catch (...) {}
        }

        // 3. Duplicate consolidation: merge highest-similarity pairs
        {
            auto pairs = find_dup_pairs("", 200, dup_threshold);
            if (pairs.size() > max_dups) pairs.resize(max_dups);
            std::unordered_set<uint64_t> processed;
            for (const auto& p : pairs) {
                if (processed.count(p.a_id) || processed.count(p.b_id)) continue;
                uint64_t weaker_id = (p.a_score >= p.b_score) ? p.b_id : p.a_id;
                try {
                    field_store_->forget(weaker_id);
                    processed.insert(weaker_id);
                    ++dups_merged;
                } catch (...) {}
            }
        }

        std::cerr << "[belief_maintenance] demoted=" << demoted
                  << " contradictions_archived=" << contradictions_archived
                  << " dups_merged=" << dups_merged << "\n";
    }

    // Read-only tools that may run concurrently under a shared lock.
    // Anything not in this set takes an exclusive lock (safe default).
    // Tools that manage their own Rust-level RwLock synchronisation and must NOT
    // hold rpc_mutex_ during subprocess execution (popen in predicate_run takes
    // up to 5s and would starve every other RPC for its duration).
    static bool is_subprocess_tool(const std::string& name) {
        static const std::unordered_set<std::string> kSubprocess = {
            "predicate_run",
            "consolidation_pass",   // long-running Sequitur+FEP rebuild; manages own Rust RwLocks
        };
        return kSubprocess.count(name) > 0;
    }

    static bool is_read_only_tool(const std::string& name) {
        static const std::unordered_set<std::string> kReads = {
            "health_check", "version_check", "soul_context", "hygiene_stats",
            "recall", "smart_recall", "hybrid_recall",
            "search_memories", "list_memories", "list_memories_brief",
            "expand_memory", "get_memory_metadata", "explain_fact", "memory_stats",
            "find_symbol", "read_symbol", "read_function", "describe_symbol",
            "code_context", "codebase_overview", "smart_context",
            "symbol_callers", "symbol_callees", "search_symbols",
            // Read-only event/queue queries — without these, hook-driven calls
            // like msg_inbox take the exclusive lock and starve every other
            // RPC for the duration of their event scan.
            "msg_inbox", "msg_history",
            "queue_status", "distill_status", "enrichment_status",
            "probe_status", "ledger_query", "ledger_contradictions",
            "agent_list", "agent_get", "agent_protocol_stats",
            "dream_list", "dream_status",
            "file_at_time", "file_timeline", "file_dependents", "file_imports",
        };
        return kReads.count(name) > 0;
    }

    // Writes whose Rust side is fully self-synchronized (per-component RwLocks) and that
    // mutate ONLY structures the read path never consumes — the CEC event tape, CDAWG,
    // decision tape, episode HDC, refutation ledger. No cross-component atomicity with
    // readers is required, so these must NOT take the global exclusive rpc_mutex_: a slow
    // or stuck one (e.g. log_event blocked on the CDAWG write lock held by a consolidation
    // pass) would otherwise hold the exclusive lock and starve EVERY reader/recall despite
    // sharing no data with them — the deadlock-reads-on-stuck-write gap. Index-mutating
    // writes (put_memory) still take the exclusive lock: they mutate semantic_idx +
    // payloads + time_idx together and a reader must not observe a half-applied write
    // (a hit whose payload is not yet inserted).
    static bool is_lockfree_write(const std::string& name) {
        static const std::unordered_set<std::string> kLockFreeWrites = {
            "log_event", "log_event_ex", "log_decision",
            // compact_wal only touches Rust state (per-component RwLocks) and writes
            // files to NFS — no C++ state is modified.  Holding rpc_mutex_ exclusively
            // for a 5-min NFS write blocks all reads/recall for that entire duration.
            "compact_wal",
        };
        return kLockFreeWrites.count(name) > 0;
    }

    // Hot read path that takes NO global rpc_mutex_ — so an index-mutating write
    // (put_memory holds the exclusive lock ~270ms for observer/event_tape/hdc work)
    // can never block recall. Safety rests entirely on the Rust store's per-component
    // RwLocks plus publish ordering, NOT on this C++ lock:
    //   * insert: payload + state are written BEFORE the semantic_idx upsert, so a
    //     semantic hit always has its payload and state (store.rs put_memory);
    //   * delete: state.deleted is set BEFORE semantic_idx.remove and the payload is
    //     never removed (soft delete), so recall either filters the hit by .deleted or
    //     never sees it — no torn read (store.rs forget);
    //   * recall waits only on the brief per-upsert semantic_idx.read(), never a writer's
    //     whole-handler hold.
    // Precedent: consolidation_pass (is_subprocess_tool) already runs lock-free
    // concurrent with recall on exactly these Rust locks — this extends that model to
    // put_memory. Other reads keep the shared lock (not yet audited for this guarantee).
    static bool is_lockfree_read(const std::string& name) {
        static const std::unordered_set<std::string> kLockFreeReads = {
            "recall", "smart_recall", "hybrid_recall",
        };
        return kLockFreeReads.count(name) > 0;
    }

    // Lock-hold profiler (diagnostic only, no behaviour change). Logs an exclusive
    // rpc_mutex_ hold (readers/recall blocked for that long) or a long shared-lock wait
    // (recall blocked waiting), over CHITTA_LOCKPROF_MS (default 250). Set =0 to disable.
    static long lockprof_threshold_ms() {
        static const long t = [] {
            const char* e = std::getenv("CHITTA_LOCKPROF_MS");
            return e ? std::atol(e) : 250L;
        }();
        return t;
    }
    static long ms_since(std::chrono::steady_clock::time_point t0) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
    }

    json handle(const json& request) {
        std::string method = request.value("method", "");
        json params = request.value("params", json::object());
        auto id = request.value("id", json());

        if (method == "tools/list") {
            std::shared_lock<std::shared_mutex> _lk(rpc_mutex_);
            return make_response(id, tool_list());
        }
        if (method == "tools/call") {
            std::string name = params.value("name", "");
            json args = params.value("arguments", json::object());
            auto it = handlers_.find(name);
            if (it == handlers_.end()) {
                return rpc::make_error(id, -32601, "Unknown tool: " + name);
            }

            // Write tools: skip synchronous embedding here — the backfill thread
            // will embed pending memories asynchronously via backfill_embedding().
            // Embedding inside the write lock caused 96% CPU blocks starving all readers.
            // Read tools still pre-embed below (shared lock, not exclusive).

            // Pre-embed query via EmbedQueue (cache-only, ≤50ms budget).
            // If the vector is cached it arrives instantly; if not, we fall through to BM25
            // immediately and enqueue a write to warm the cache for the next call.
            // ceiling: 2000ms wait caused all 16 pool workers to park on the single-GPU
            //   embed queue simultaneously, saturating the pool under any recall burst.
            //   upgrade: per-query dedup map (share one GPU call across concurrent waiters).
            if (is_read_only_tool(name) && !args.contains("_preembedding")) {
                std::string q = args.value("query", "");
                if (!q.empty()) {
                    std::vector<float> emb;
                    if (embed_queue_) {
                        emb = embed_queue_->query(q, std::chrono::milliseconds(50));
                        if (emb.empty()) embed_queue_->enqueue_write(q); // async cache warm
                    } else if (yantra_) {
                        Artha a = yantra_->transform(q, EmbedMode::Query);
                        if (a.certainty > 0.0f) emb = a.nu.data;
                    }
                    if (!emb.empty()) args["_preembedding"] = emb;
                }
            }

            ToolResult result;
            bool _was_write = false;
            const long _lp_thr = lockprof_threshold_ms();
            if (is_subprocess_tool(name) || is_lockfree_write(name) || is_lockfree_read(name)) {
                // No rpc_mutex_ held. The Rust side fully self-synchronizes via per-component
                // RwLocks: lock-free writes touch no read-path structure (is_lockfree_write),
                // and the hot recall path relies on publish ordering rather than this lock so
                // an index-mutating write can never block it (is_lockfree_read).
                result = it->second(args);
            } else if (is_read_only_tool(name)) {
                auto _lp_w0 = std::chrono::steady_clock::now();
                std::shared_lock<std::shared_mutex> _lk(rpc_mutex_);
                if (_lp_thr > 0) {
                    long _wait = ms_since(_lp_w0);
                    if (_wait >= _lp_thr)
                        std::cerr << "[lockprof] SHARED " << name << " waited=" << _wait
                                  << "ms (recall/reader blocked by a writer)\n";
                }
                result = it->second(args);
            } else {
                _was_write = true;
                auto _lp_w0 = std::chrono::steady_clock::now();
                std::unique_lock<std::shared_mutex> _lk(rpc_mutex_);
                auto _lp_h0 = std::chrono::steady_clock::now();
                result = it->second(args);
                if (_lp_thr > 0) {
                    long _held = ms_since(_lp_h0);
                    if (_held >= _lp_thr)
                        std::cerr << "[lockprof] EXCLUSIVE " << name << " held=" << _held
                                  << "ms wait=" << (ms_since(_lp_w0) - _held)
                                  << "ms (blocks all readers/recall while held)\n";
                }
            }
            // Durable WAL fdatasync OFF the rpc_mutex: put_memory only flush_buf()s the
            // append under the lock now, so we fsync here after it is released — recall is
            // never blocked by the per-write disk sync (was ~200-330ms held). See
            // store.rs put_memory / cf_sync / FieldStore::sync.
            if (_was_write && field_store_) {
                field_store_->sync();
            }
            return make_tool_response(id, result);
        }
        return rpc::make_error(id, -32601, "Unknown method: " + method);
    }

private:
    FieldStore* field_store_;
    VakYantra* yantra_;
    Subconscious* subconscious_ = nullptr;
    SadhanaManager* sadhana_manager_ = nullptr;
    std::atomic<size_t>* queue_count_ = nullptr;
    std::atomic<size_t>* queue_fail_count_ = nullptr;
    std::string failed_queue_path_;
    RecallCallback recall_callback_;
    WriteNotifyCallback write_notify_fn_;
    EmbedQueue* embed_queue_ = nullptr;
    std::string mind_path_;                  // base mind dir (parent of chitta-field)

    mutable std::shared_mutex rpc_mutex_;    // Reads share, writes exclusive; see is_read_only_tool()
    mutable std::mutex distill_mutex_;
    std::string distill_model_ = "github-copilot/gpt-5-mini";
    std::atomic<bool> distill_enabled_{true};

    std::vector<json> tools_;
    std::unordered_map<std::string, std::function<ToolResult(const json&)>> handlers_;
    std::unordered_map<std::string, std::string> tool_visibility_;

    // ── Embedding helpers ───────────────────────────────────────────────────

    // Write-path embed: fire-and-forget via queue (warms cache, doesn't block).
    // Falls back to direct yantra if no queue; returns empty if both unavailable.
    std::vector<float> embed_text(const std::string& text) {
        if (text.empty()) return {};
        if (embed_queue_) {
            embed_queue_->enqueue_write(text);
            return {};  // caller must handle empty (store with pending embed)
        }
        if (!yantra_) return {};
        Artha a = yantra_->transform(text);
        return (a.certainty > 0.0f) ? a.nu.data : std::vector<float>{};
    }

    // Read-path embed: await via queue (≤50ms), fall to BM25 on miss.
    // ceiling: 2000ms caused pool workers to block 2s each on cold cache —
    // 16 workers × 2s = 32s of wasted pool capacity per recall burst.
    // upgrade: per-query dedup map so N concurrent callers share one GPU call.
    std::vector<float> embed_query(const std::string& query) {
        if (query.empty()) return {};
        if (embed_queue_)
            return embed_queue_->query(query, std::chrono::milliseconds(50));
        if (!yantra_) return {};
        Artha a = yantra_->transform(query, EmbedMode::Query);
        return (a.certainty > 0.0f) ? a.nu.data : std::vector<float>{};
    }

    // ── ID extraction helpers ───────────────────────────────────────────────

    static uint64_t extract_id(const json& params, const std::string& key = "id") {
        if (!params.contains(key)) return 0;
        const auto& v = params[key];
        if (v.is_number_integer()) return static_cast<uint64_t>(v.get<int64_t>());
        if (v.is_string()) {
            try { return std::stoull(v.get<std::string>()); } catch (...) {}
        }
        return 0;
    }

    static std::pair<int64_t, std::string> parse_id(const json& params, const std::string& key = "id") {
        int64_t db_id = 0;
        std::string id_str;
        if (params.contains(key)) {
            const auto& val = params[key];
            if (val.is_number_integer()) {
                db_id = val.get<int64_t>();
                id_str = std::to_string(db_id);
            } else if (val.is_string()) {
                id_str = val.get<std::string>();
                try { db_id = std::stoll(id_str); } catch (...) { db_id = 0; }
            }
        }
        return {db_id, id_str};
    }

    // ── Result conversion helpers ───────────────────────────────────────────

    static json hits_to_results_json(const std::vector<FieldRecallHit>& hits, bool explain = false) {
        json arr = json::array();
        for (const auto& h : hits) {
            json entry = {
                {"id",         std::to_string(h.memory_id)},
                {"relevance",  h.score},
                {"similarity", h.semantic_score},
                {"type",       h.kind.empty() ? "episode" : h.kind},
                {"text",       h.content},
                {"realm",      h.realm},
                {"confidence", h.confidence},
                {"ts_ms",      h.ts_ms},
            };
            if (h.affect_valence != 0.0f || h.affect_arousal != 0.0f) {
                entry["affect"] = {
                    {"valence", h.affect_valence},
                    {"arousal", h.affect_arousal},
                };
            }
            if (explain) {
                entry["explain"] = {
                    {"semantic_weight",     h.semantic_weight},
                    {"status_mul",          h.status_mul},
                    {"epistemic_mul",       h.epistemic_mul},
                    {"strength_factor",     h.strength_factor},
                    {"actr_activation",     h.actr_activation},
                    {"access_count",        h.access_count},
                    {"interference_factor", h.interference_factor},
                    {"spacing_boost",       h.spacing_boost},
                    {"surprise_boost",      h.surprise_boost},
                    {"arousal_boost",       h.arousal_boost},
                    {"mood_congruence",     h.mood_congruence},
                    {"frustration_boost",   h.frustration_boost},
                };
            }
            arr.push_back(std::move(entry));
        }
        return arr;
    }

    // Human-facing confidence percent. Prefers the raw semantic cosine
    // ("similarity") — a true 0-1 relevance — and falls back to the composite
    // "relevance" only for keyword-only hits that carry no cosine. The composite
    // score is a product of ~20 cognitive factors, so its magnitude caps around
    // 0.03 even for a cos-0.97 match; displaying it tripped the prompt-injection
    // confidence gate and dropped every strong semantic hit.
    static int display_pct(const nlohmann::json& r) {
        float sim = r.value("similarity", 0.0f);
        float rel = r.value("relevance", 0.0f);
        float v = sim > 0.0f ? sim : rel;
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        return static_cast<int>(v * 100);
    }

    // True when query likely contains entity tokens (caps, hyphens, domain keywords, length>15).
    // Used to gate SSL query expansion — short/generic queries don't benefit.
    static bool query_has_entities(const std::string& q) {
        if (q.size() < 8) return false;
        static const std::vector<std::string> domain_kw = {
            "cluster", "node", "ssh", "kerberos", "slurm", "cmake", "build",
            "compile", "chaos", "kerberos", "bge", "embed", "chitta",
        };
        std::string ql = q;
        std::transform(ql.begin(), ql.end(), ql.begin(), ::tolower);
        for (auto& kw : domain_kw)
            if (ql.find(kw) != std::string::npos) return true;
        // contains uppercase letter (entity name) or hyphen (SSL token)
        for (char c : q)
            if (std::isupper(c) || c == '-') return true;
        return q.size() > 15;
    }

    // Embed content with NL gloss baked in (for SSL memories).
    // Keeps canonical content unchanged; gloss only affects the embedding vector.
    std::vector<float> embed_ssl_aware(const std::string& content) {
        static const std::string arrow = "\xe2\x86\x92"; // UTF-8 →
        bool is_ssl = content.find(arrow) != std::string::npos
                   || content.find("[SOLUTION]")    != std::string::npos
                   || content.find("[OPERATIONAL]") != std::string::npos
                   || content.find("[DECISION]")    != std::string::npos
                   || content.find("[PREFERENCE]")  != std::string::npos
                   || content.find("[GOTCHA]")      != std::string::npos
                   || content.find("[PATTERN]")     != std::string::npos;
        if (!is_ssl) return embed_text(content);
        return embed_text(chitta::ssl::retrieval_text(content));
    }

    // Reciprocal Rank Fusion — scale-invariant across semantic (cosine) and BM25 scores.
    // RRF(d) = sum_i 1/(k + rank_i(d)), k=60 per standard practice.
    // Each document gets contributions from every list it appears in; union of both lists.
    static json merge_results(const json& a, const json& b) {
        constexpr float kRRF = 60.0f;
        std::unordered_map<std::string, float> rrf_scores;
        std::unordered_map<std::string, json> entries;

        auto process = [&](const json& arr) {
            int rank = 1;
            for (const auto& entry : arr) {
                std::string id = entry.value("id", "");
                if (id.empty()) { rank++; continue; }
                rrf_scores[id] += 1.0f / (kRRF + rank);
                if (entries.find(id) == entries.end()) entries[id] = entry;
                rank++;
            }
        };
        process(a);
        process(b);

        json merged = json::array();
        for (auto& [id, entry] : entries) {
            entry["relevance"] = rrf_scores[id];
            merged.push_back(entry);
        }
        std::sort(merged.begin(), merged.end(), [](const json& x, const json& y) {
            return x.value("relevance", 0.0f) > y.value("relevance", 0.0f);
        });
        return merged;
    }

    // ── Query helpers ───────────────────────────────────────────────────────

    static float category_to_confidence(const std::string& category) {
        if (category == "correction") return 0.95f;
        if (category == "preference") return 0.90f;
        if (category == "solution")   return 0.90f;
        if (category == "milestone")  return 0.90f;
        if (category == "decision")   return 0.85f;
        if (category == "failure")    return 0.85f;
        if (category == "gotcha")     return 0.85f;
        if (category == "episode")    return 0.70f;
        return 0.80f;
    }

    static std::string_view trim_view(std::string_view s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
        return s;
    }

    static bool starts_with_ci(std::string_view s, std::string_view prefix) {
        if (s.size() < prefix.size()) return false;
        for (size_t i = 0; i < prefix.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(s[i])) !=
                std::tolower(static_cast<unsigned char>(prefix[i]))) return false;
        }
        return true;
    }

    static bool looks_like_code_query(const std::string& q) {
        std::string_view s = trim_view(q);
        if (s.empty()) return false;
        if (starts_with_ci(s, "how ") || starts_with_ci(s, "why ") || starts_with_ci(s, "what ") ||
            starts_with_ci(s, "where ") || starts_with_ci(s, "explain ") || starts_with_ci(s, "describe ") ||
            starts_with_ci(s, "find ") || starts_with_ci(s, "show ")) return false;
        if (s.find("::") != std::string_view::npos || s.find("->") != std::string_view::npos ||
            s.find('(') != std::string_view::npos || s.find(')') != std::string_view::npos ||
            s.find('_') != std::string_view::npos || s.find('/') != std::string_view::npos ||
            s.find('\\') != std::string_view::npos || s.find('#') != std::string_view::npos ||
            s.find('.') != std::string_view::npos) return true;
        bool has_space = s.find_first_of(" \t\n") != std::string_view::npos;
        if (!has_space && s.size() <= 80) {
            size_t ok = 0;
            for (char c : s) {
                unsigned char uc = static_cast<unsigned char>(c);
                if (std::isalnum(uc) || c == ':' || c == '.' || c == '-') ok++;
            }
            if (ok == s.size()) return true;
        }
        return false;
    }

    struct ExpandedQuery {
        std::string lex;
        std::string vec;
        std::string hyde;
    };

    static ExpandedQuery expand_query(const std::string& query) {
        ExpandedQuery eq;
        eq.vec = query;
        eq.hyde = "[memory about] " + query;

        static const std::unordered_set<std::string> STOP_WORDS = {
            "a","an","the","is","are","was","were","be","been","being","have","has","had",
            "do","does","did","will","would","could","should","may","might","shall","can",
            "to","of","in","for","on","with","at","by","from","as","into","through",
            "i","me","my","we","our","you","your","he","him","his","she","her","it","its",
            "they","them","their","what","which","who","this","that","these","those",
            "not","only","just","about","so","than","too","very","all","both","each",
            "more","most","other","some","such","no","own","same","here","there",
            "when","where","why","how","then","once","am","out","off","over","under"
        };

        std::ostringstream lex_ss;
        std::istringstream iss(query);
        std::string word;
        bool first = true;
        while (iss >> word) {
            std::string lower;
            lower.reserve(word.size());
            for (char c : word) {
                unsigned char uc = static_cast<unsigned char>(c);
                if (std::isalnum(uc)) lower += static_cast<char>(std::tolower(uc));
            }
            if (!lower.empty() && STOP_WORDS.find(lower) == STOP_WORDS.end()) {
                if (!first) lex_ss << " ";
                lex_ss << lower;
                first = false;
            }
        }
        eq.lex = lex_ss.str().empty() ? query : lex_ss.str();
        return eq;
    }

    // ── SSL helpers ─────────────────────────────────────────────────────────

    static bool is_ssl_format(const std::string& content) {
        if (content.empty()) return false;
        if (content[0] == '[' && content.find(']') != std::string::npos) return true;
        if (content.rfind("[LEARN]", 0) == 0) return true;
        if (content.rfind("[ε]", 0) == 0) return true;
        if (content.find("\xe2\x86\x92") != std::string::npos) return true;  // UTF-8 →
        // SSL v0.4: G:N granularity annotation
        if (content.find(" G:0") != std::string::npos ||
            content.find(" G:1") != std::string::npos ||
            content.find(" G:2") != std::string::npos ||
            content.find(" G:3") != std::string::npos ||
            content.find(" G:4") != std::string::npos) return true;
        return false;
    }

    static std::string to_ssl_format(const std::string& content,
                                      const std::string& domain = "note",
                                      const std::string& location = "") {
        if (is_ssl_format(content)) return content;
        std::string result = "[" + domain + "] ";
        size_t newline = content.find('\n');
        if (newline != std::string::npos && newline < 80) {
            result += content.substr(0, newline);
            if (!location.empty()) result += " @" + location;
            result += "\n" + content.substr(newline + 1);
        } else if (content.size() > 80) {
            result += content.substr(0, 80) + "...";
            if (!location.empty()) result += " @" + location;
            result += "\n" + content;
        } else {
            result += content;
            if (!location.empty()) result += " @" + location;
        }
        return result;
    }

    // ── Timestamp parsing ───────────────────────────────────────────────────

    std::optional<int64_t> parse_timestamp_str(const std::string& ts) {
        if (ts.empty()) return std::nullopt;
        std::tm tm = {};
        int year, month, day, hour = 0, min = 0, sec = 0;
        if (std::sscanf(ts.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &min, &sec) >= 3 ||
            std::sscanf(ts.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &min, &sec) >= 3 ||
            std::sscanf(ts.c_str(), "%d-%d-%d", &year, &month, &day) == 3) {
            tm.tm_year = year - 1900;
            tm.tm_mon = month - 1;
            tm.tm_mday = day;
            tm.tm_hour = hour;
            tm.tm_min = min;
            tm.tm_sec = sec;
            tm.tm_isdst = -1;
            std::time_t time = std::mktime(&tm);
            if (time == -1) return std::nullopt;
            return static_cast<int64_t>(time) * 1000;
        }
        try {
            int64_t val = std::stoll(ts);
            if (val < 946684800000LL) val *= 1000;
            return val;
        } catch (...) {
            return std::nullopt;
        }
    }

    // ── Misc helpers ────────────────────────────────────────────────────────

    static std::vector<std::string> extract_terms(const std::string& query) {
        std::vector<std::string> terms;
        std::istringstream iss(query);
        std::string word;
        while (iss >> word) {
            std::string clean;
            for (char c : word) {
                if (std::isalnum(c)) clean += std::tolower(c);
            }
            if (clean.length() >= 3 &&
                clean != "the" && clean != "and" && clean != "for" &&
                clean != "that" && clean != "with" && clean != "how" &&
                clean != "what" && clean != "does" && clean != "can") {
                terms.push_back(clean);
            }
        }
        return terms;
    }

    static std::string get_session_id() {
        const char* env = std::getenv("CLAUDE_SESSION_ID");
        return env ? env : "";
    }

    static std::string get_session_id(const json& params) {
        if (params.contains("session_id") && params["session_id"].is_string()) {
            std::string sid = params["session_id"].get<std::string>();
            if (!sid.empty()) return sid;
        }
        return get_session_id();
    }

    // Aspect to kind mapping
    static inline const std::unordered_map<std::string, std::vector<std::string>> ASPECT_KINDS = {
        {"preferences", {"preference"}},
        {"corrections", {"correction"}},
        {"insights", {"insight", "wisdom"}},
        {"failures", {"failure"}},
        {"decisions", {"decision"}},
        {"approaches", {"approach"}},
        {"milestones", {"milestone"}},
        {"goals", {"goal"}},
        {"habits", {"habit"}},
        {"beliefs", {"belief", "invariant"}},
        {"wisdom", {"wisdom", "insight"}},
        {"code", {"symbol", "function", "class", "file", "dependency"}},
        {"gaps", {"gap", "question"}},
    };

    // ── RPC response helpers ────────────────────────────────────────────────

    json tool_list() {
        json filtered = json::array();
        for (const auto& tool : tools_) {
            auto name = tool["name"].get<std::string>();
            auto it = tool_visibility_.find(name);
            std::string vis = (it != tool_visibility_.end()) ? it->second : "default";
            if (vis != "internal") filtered.push_back(tool);
        }
        return {{"tools", filtered}};
    }

    json make_response(const json& id, const json& result) {
        return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
    }

    // JSON-RPC error envelopes come from the single definition in
    // protocol.hpp (chitta::rpc::make_error) — do not re-implement here.

    json make_tool_response(const json& id, const ToolResult& result) {
        json content = json::array();
        content.push_back({{"type", "text"}, {"text", result.text}});
        return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {
            {"content", content}, {"isError", result.is_error}, {"structured", result.structured}
        }}};
    }

    // ═══════════════════════════════════════════════════════════════════════
    // Handler file includes — tool implementations (must be before register_tools)
    // ═══════════════════════════════════════════════════════════════════════

    #include "handlers/field_memory_recall.hpp"
    #include "handlers/field_memory_ops.hpp"
    #include "handlers/field_memory_structured.hpp"
    #include "handlers/field_code_intel.hpp"
    #include "handlers/field_system.hpp"
    #include "handlers/field_session.hpp"
    #include "handlers/field_distill.hpp"
    #include "handlers/field_misc.hpp"
    #include "handlers/field_contradiction.hpp"
    #include "handlers/field_write_gate.hpp"
    #include "handlers/field_symbol_events.hpp"
    #include "handlers/field_introspection.hpp"
    #include "handlers/field_operator.hpp"
    #include "handlers/ledger.hpp"
    #include "handlers/long_task.hpp"
    #include "handlers/compact.hpp"
    #include "handlers/drift_recon.hpp"
    #include "handlers/drift_5w.hpp"
    #include "handlers/drift_consolidation.hpp"
    #include "handlers/drift_recall.hpp"
    #include "handlers/drift_probe.hpp"
    #include "handlers/field_skill.hpp"
    #include "handlers/field_agent.hpp"
    #include "handlers/trajectory_compact.hpp"
    #include "handlers/constraint.hpp"
    #include "handlers/meta_memory.hpp"
    #include "handlers/learning.hpp"
    #include "handlers/intervention.hpp"
    #include "handlers/agent_protocol.hpp"
    #include "handlers/wisdom_lineage.hpp"
    #include "handlers/field_lookup.hpp"
    #include "handlers/repl_sessions.hpp"

    // ═══════════════════════════════════════════════════════════════════════
    // register_tools() — all tool schemas and handler bindings
    // ═══════════════════════════════════════════════════════════════════════

    void register_tools();
    void register_memory_core_tools();
    void register_code_intel_tools();
    void register_distill_drift_tools();
    void register_session_transcript_tools();
    void register_system_tools();
    void register_misc_tools();
    void register_protocol_tools();
    void classify_tools();

};

} // namespace chitta
