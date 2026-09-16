#pragma once
// Subconscious: Background processor for autonomous learning
//
// Runs as a daemon thread, detecting learning opportunities from
// user/assistant messages and closing feedback loops automatically.
//
// Architecture:
//   FieldStore + VakYantra ← Subconscious (background thread) ← Event Queue ← Hooks/RPC
//
// Capabilities:
//   - Pattern detection (correction, preference, frustration, milestone)
//   - Suggestion tracking with outcome verification
//   - Anticipation pattern learning
//   - Sleep consolidation (chitta-field encode + snapshot + demotion)

#include <chitta/vak.hpp>
#include <chitta/maintenance_jitter.hpp>
#include <string>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <regex>
#include <chrono>
#include <shared_mutex>
#include <filesystem>

namespace chitta {

// Forward declaration — full header in .cpp only (chitta_field.h needs special include path)
class FieldStore;

// Event types the subconscious processes
enum class SubconsciousEventType {
    UserMessage,       // For pattern detection (corrections, preferences, etc.)
    AssistantMessage,  // For suggestion tracking
    ToolResult,        // For anticipation verification
};

// Event pushed from RPC handlers
struct SubconsciousEvent {
    SubconsciousEventType type;
    std::string content;
    std::string realm;
    int64_t timestamp;

    SubconsciousEvent() = default;
    SubconsciousEvent(SubconsciousEventType t, std::string c, std::string r, int64_t ts)
        : type(t), content(std::move(c)), realm(std::move(r)), timestamp(ts) {}
};

// Configuration for subconscious behavior
struct SubconsciousConfig {
    std::chrono::seconds process_interval{1};
    std::chrono::minutes hygiene_interval{30};
    std::chrono::minutes theme_maintenance_interval{60};  // Theme maintenance every hour
    std::chrono::minutes distillation_interval{120};      // Auto-distillation every 2 hours
    std::chrono::seconds embedding_interval{30};  // Background embedding interval (30s to reduce CPU)
    std::chrono::seconds idle_threshold{30};      // Only embed when no queries for this long
    int embed_defer_ms{1500};                     // Recall-priority gate: pause embed_loop if a query
                                                  // arrived within this window (keeps recall off the
                                                  // embed cores under load; drains during idle gaps)
    std::chrono::minutes sleep_consolidation_interval{10};  // encode_all + save_snapshot every 10 min
    std::chrono::minutes demotion_interval{60};             // run_demotion pass every hour
    std::chrono::minutes belief_maintenance_interval{120};  // Belief maintenance every 2 hours
    bool enable_belief_maintenance{true};
    size_t max_queue_size{1000};
    size_t embedding_batch_size{256};             // Items per embed_loop cycle (OllamaYantra batches 16/HTTP call)
    float correction_confidence{0.8f};
    bool enable_hygiene{true};
    bool enable_theme_maintenance{true};          // xMemory theme maintenance
    bool enable_distillation{true};               // Auto-distill episodes into wisdom
    bool enable_anticipation{true};
    bool enable_pattern_detection{true};
    bool enable_suggestion_tracking{true};
    bool enable_background_embedding{true};       // On by default; disable with --no-embed-interval
    bool enable_habit_formation{true};            // Auto-detect tool patterns and form habits
    bool enable_cls_replay{true};                 // CLS offline replay consolidation
    bool enable_sleep_consolidation{true};        // chitta-field encode + snapshot + demotion
    bool enable_code_intel_staleness{true};       // Restore confidence of code intel memories
    int code_intel_staleness_interval_hours{24};
    float code_intel_min_confidence{0.5f};
    float code_intel_target_confidence{0.8f};
    std::chrono::minutes cls_replay_interval{60}; // CLS replay every hour
    size_t cls_replay_batch_size{20};             // Memories to sample per replay
    size_t cls_replay_min_cluster{3};             // Min cluster size to distill
    std::chrono::minutes learning_cycle_interval{30}; // Autonomous learning cycle
    bool enable_learning_cycle{true};
    bool enable_correction_promotion{true};       // Promote repeated cross-project corrections to brahman
    int correction_promotion_interval_hours{12};
    int correction_promotion_min_realms{2};       // Min distinct non-brahman realms to trigger promotion
    std::string quiesce_flag_path;                // Eval freeze: skip periodic passes while this file is fresh
};

// Eval quiesce: hooks/grade-recall.sh touches $MIND/.quiesce before a
// measurement run so back-to-back evals see a frozen store (periodic
// distill/consolidation passes swing golden nDCG by ±0.027 mid-eval).
// TTL 30 min via mtime — a crashed eval can't freeze the daemon forever.
inline bool quiesce_active(const std::string& flag_path) {
    if (flag_path.empty()) return false;
    std::error_code ec;
    auto mtime = std::filesystem::last_write_time(flag_path, ec);
    if (ec) return false;
    auto age = std::filesystem::file_time_type::clock::now() - mtime;
    return age < std::chrono::minutes(30);
}

// Queued embedding result (computed in background, flushed by main thread)
struct QueuedEmbedding {
    int64_t symbol_id;
    std::vector<float> embedding;
};

// Runtime statistics
struct SubconsciousStats {
    std::atomic<size_t> events_processed{0};
    std::atomic<size_t> corrections_detected{0};
    std::atomic<size_t> preferences_detected{0};
    std::atomic<size_t> frustrations_detected{0};
    std::atomic<size_t> milestones_detected{0};
    std::atomic<size_t> uncertainties_detected{0};
    std::atomic<size_t> suggestions_tracked{0};
    std::atomic<size_t> outcomes_verified{0};
    std::atomic<size_t> hygiene_runs{0};
    std::atomic<size_t> theme_maintenance_runs{0};    // xMemory theme maintenance
    std::atomic<size_t> themes_split{0};
    std::atomic<size_t> themes_merged{0};
    std::atomic<size_t> memories_reassigned{0};
    std::atomic<size_t> distillation_runs{0};         // Auto-distillation runs
    std::atomic<size_t> wisdom_created{0};            // Wisdom nodes created from distillation
    std::atomic<size_t> symbols_embedded{0};
    std::atomic<size_t> embeddings_queued{0};
    std::atomic<size_t> embedding_skips{0};       // Skipped due to busy state
    std::atomic<size_t> habits_formed{0};         // Habits auto-created from tool patterns
    std::atomic<size_t> habits_matched{0};        // Existing habits matched by patterns
    std::atomic<size_t> cls_replay_runs{0};       // CLS offline replay runs
    std::atomic<size_t> cls_memories_consolidated{0}; // Memories consolidated by CLS
    std::atomic<size_t> cls_wisdom_created{0};    // Wisdom nodes created by CLS replay
    std::atomic<size_t> sleep_consolidation_runs{0};  // encode_all + save_snapshot runs
    std::atomic<size_t> demotion_runs{0};             // run_demotion pass runs
    std::atomic<size_t> field_demoted{0};             // Total memories demoted across all passes
    std::atomic<size_t> field_deleted{0};             // Total memories deleted across all passes
    std::atomic<size_t> belief_maintenance_runs{0};
    std::atomic<size_t> learning_cycle_runs{0};
    std::atomic<size_t> debts_auto_resolved{0};
    std::atomic<size_t> wisdom_candidates_created{0};
    std::atomic<size_t> scorer_updates{0};
    std::atomic<size_t> interventions_auto_closed{0};
    std::atomic<size_t> tasks_auto_completed{0};
    // Code intel staleness
    std::atomic<size_t> code_intel_staleness_runs{0};
    std::atomic<size_t> code_intel_memories_restored{0};
    // Correction promotion
    std::atomic<size_t> correction_promotion_runs{0};
    std::atomic<size_t> correction_promotions{0};
    // Layer 9: Wisdom Homeostasis
    std::atomic<size_t> lineage_staleness_ticks{0};
    std::atomic<size_t> lineages_inflamed{0};
    std::atomic<size_t> lineages_demoted_ttl{0};
    std::atomic<int64_t> last_belief_maintenance_at{0};
    std::atomic<int64_t> last_learning_cycle_at{0};
    std::atomic<int64_t> last_hygiene_at{0};
    std::atomic<int64_t> last_theme_maintenance_at{0};
    std::atomic<int64_t> last_distillation_at{0};
    std::atomic<int64_t> last_embedding_at{0};
    std::atomic<int64_t> last_cls_replay_at{0};
    std::atomic<int64_t> last_sleep_consolidation_at{0};
    std::atomic<int64_t> last_compact_wal_at{0};
    std::atomic<int64_t> last_demotion_at{0};
    std::atomic<int64_t> last_query_at{0};        // Last RPC query timestamp
    std::atomic<int64_t> started_at{0};
};

// Tracked suggestion for outcome verification
struct TrackedSuggestion {
    std::string content;
    std::string context;
    std::string realm;
    int64_t suggested_at;
    int64_t db_id;  // ID in suggestions table
};

class Subconscious {
public:
    explicit Subconscious(FieldStore* field_store, VakYantra* embedder, SubconsciousConfig config = {});
    ~Subconscious();

    // Lifecycle
    void start();
    void stop();
    bool is_running() const { return running_.load(); }

    // Event interface (non-blocking)
    void push_event(SubconsciousEvent event);

    // Statistics
    const SubconsciousStats& stats() const { return stats_; }

    // Access to config (for RPC stats)
    const SubconsciousConfig& config() const { return config_; }

    // Embedding queue (call from main thread to flush queued embeddings to DB)
    size_t flush_embedding_queue();
    size_t embedding_queue_size() const;

    // Query notification (call from RPC handlers to signal daemon is busy)
    void notify_query();
    bool is_idle() const;

    // Recall-priority gate: true if a recall/read arrived within embed_defer_ms.
    // Background embedders (embed_loop AND the daemon backfill thread) consult this
    // to yield CPU *and* the exclusive rpc_mutex to live recalls under load, so a
    // large embed backlog draining can never starve recall. Drains resume the moment
    // recalls pause. Lock-free (atomic load) — safe to call from any thread.
    bool recall_pressured() const {
        auto lq = stats_.last_query_at.load();
        if (lq == 0) return false;
        auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return (now - lq) < config_.embed_defer_ms;
    }

    // Dream callback: called when the soul has been idle long enough to dream
    void set_dream_callback(std::function<void()> fn) {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        dream_callback_ = std::move(fn);
    }

    // Belief maintenance callback: periodic stale demotion + contradiction resolution
    void set_maintenance_callback(std::function<void()> fn) {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        maintenance_cb_ = std::move(fn);
    }
    void set_maintenance_load_probe(std::function<bool(const std::string&)> fn) {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        maintenance_load_probe_ = std::move(fn);
    }

    // Think callback: called hourly during idle for internal memory synthesis
    void set_think_callback(std::function<void()> fn) {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        think_callback_ = std::move(fn);
    }

    // Wire in FieldStore (may be initialized async after construction).
    // Pointer is not owned; must outlive this Subconscious instance.
    void set_field_store(FieldStore* fs) { field_store_ = fs; }

    // Wire in embedder (may be initialized after construction).
    void set_embedder(VakYantra* e) { embedder_ = e; }
    void set_rpc_mutex(std::shared_mutex* m) { rpc_mutex_ = m; }

private:
    FieldStore* field_store_;
    VakYantra* embedder_;
    std::shared_mutex* rpc_mutex_{nullptr};
    SubconsciousConfig config_;
    SubconsciousStats stats_;
    MaintenanceJitter maintenance_jitter_;
    std::function<bool(const std::string&)> maintenance_load_probe_;
    mutable std::mutex callback_mutex_;
    struct Callbacks {
        std::function<void()> maintenance, dream, think;
    };
    Callbacks callbacks() const {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        return {maintenance_cb_, dream_callback_, think_callback_};
    }

    // Threading
    std::thread process_thread_;
    std::thread embed_thread_;
    size_t embed_cycle_count_ = 0;
    std::atomic<bool> running_{false};
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<SubconsciousEvent> event_queue_;

    // Embedding queue (filled by background thread, flushed by main thread)
    mutable std::mutex embedding_queue_mutex_;
    std::vector<QueuedEmbedding> embedding_queue_;

    // Pattern matchers (compiled regexes)
    // Known limitation: std::regex is slow (~10-100x slower than RE2 or manual
    // string matching). Acceptable here because pattern detection runs on a
    // background thread with low frequency. If this becomes a hot path,
    // consider replacing with RE2 or simple string::find-based matching.
    std::regex correction_pattern_;
    std::regex preference_pattern_;
    std::regex frustration_pattern_;
    std::regex milestone_pattern_;
    std::regex suggestion_pattern_;
    std::regex uncertainty_pattern_;

    // Tracked suggestions for outcome verification
    std::mutex suggestions_mutex_;
    std::vector<TrackedSuggestion> pending_suggestions_;
    static constexpr size_t MAX_PENDING_SUGGESTIONS = 50;

    // Anticipation patterns (context -> predicted action)
    std::mutex anticipation_mutex_;
    std::string last_context_;
    std::string last_predicted_action_;

    // Habit formation from tool sequences
    std::deque<std::pair<std::string, std::string>> recent_tool_sequence_;
    static constexpr size_t MAX_TOOL_SEQUENCE = 10;
    std::mutex tool_sequence_mutex_;

    // Main processing loop
    std::unique_lock<std::shared_mutex> write_lock();
    void process_loop();

    // Event handlers
    void process_user_message(const SubconsciousEvent& event);
    void process_assistant_message(const SubconsciousEvent& event);
    void process_tool_result(const SubconsciousEvent& event);

    // Pattern detection
    void detect_correction(const std::string& content, const std::string& realm);
    void detect_preference(const std::string& content, const std::string& realm);
    void detect_frustration(const std::string& content, const std::string& realm);
    void detect_milestone(const std::string& content, const std::string& realm);
    void detect_uncertainty(const std::string& content, const std::string& realm);

    // Auto-learning (stores to FieldStore)
    void store_correction(const std::string& context, const std::string& correction,
                          const std::string& realm);
    void store_preference(const std::string& preference, const std::string& realm);
    void store_frustration(const std::string& context, const std::string& realm);
    void store_milestone(const std::string& achievement, const std::string& realm);
    void store_uncertainty(const std::string& context, const std::string& realm);

    // Suggestion tracking
    void track_suggestion(const std::string& content, const std::string& context,
                          const std::string& realm);
    void check_outcomes(const std::string& user_message, const std::string& realm);

    // Anticipation
    void observe_pattern(const std::string& context, const std::string& action,
                         const std::string& realm);
    void verify_prediction(const std::string& actual_action, const std::string& realm);

    // Habit formation from tool sequences
    void observe_tool_for_habit(const std::string& tool_name, const std::string& context,
                                 const std::string& realm);

    // Periodic tasks
    void run_theme_maintenance();
    bool maintenance_loaded(const std::string& task) const {
        std::function<bool(const std::string&)> probe;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            probe = maintenance_load_probe_;
        }
        return probe && probe(task);
    }
    bool time_for_theme_maintenance() const;
    void run_sleep_consolidation();
    bool time_for_wal_compact() const;
    bool time_for_sleep_consolidation() const;
    void run_demotion_pass();
    bool time_for_demotion() const;

    // Belief maintenance
    std::function<void()> maintenance_cb_;
    std::chrono::steady_clock::time_point last_belief_maintenance_{std::chrono::steady_clock::now()};
    bool time_for_belief_maintenance() const;

    // Autonomous learning cycle (Moves 3, 5, 6)
    std::chrono::steady_clock::time_point last_learning_cycle_{std::chrono::steady_clock::now()};
    void run_learning_cycle();
    bool time_for_learning_cycle() const;

    // Code intel staleness restoration
    std::chrono::steady_clock::time_point last_code_intel_staleness_{std::chrono::steady_clock::now()};
    void run_code_intel_staleness();
    bool time_for_code_intel_staleness() const;

    // Correction promotion: elevate repeated cross-project corrections to brahman
    std::chrono::steady_clock::time_point last_correction_promotion_{std::chrono::steady_clock::now()};
    void run_correction_promotion();
    bool time_for_correction_promotion() const;

    // Background embedding: embed pending memories using VakYantra
    void run_background_embedding();
    void embed_loop();
    bool time_for_background_embedding() const;

    // Dream: autonomous curiosity-driven exploration when idle
    std::function<void()> dream_callback_;
    std::atomic<int64_t> last_dream_triggered_at_{0};
    bool time_for_dream() const;

    // Think: internal memory synthesis during idle (hourly, shorter idle threshold)
    std::function<void()> think_callback_;
    std::atomic<int64_t> last_think_triggered_at_{0};
    bool time_for_think() const;

    // Helpers
    static int64_t now_ms();
    std::optional<SubconsciousEvent> pop_event_with_timeout(std::chrono::seconds timeout);
    static float score_correction_quality(const std::string& correction,
                                          const std::string& context);

    // Embed text via VakYantra and return the float vector (empty on failure)
    std::vector<float> embed(const std::string& text);
};

}  // namespace chitta
