// Distillation implementation.
// Extracted from simple_cli.cpp.

#include <chitta/distillation.hpp>
#include <chitta/native_distiller.hpp>
#include <chitta/rpc/field_handler.hpp>
#include <chitta/version.hpp>
#include <iostream>
#include <sstream>
#include <atomic>
#include <shared_mutex>

// Global flags shared with simple_cli.cpp
extern std::atomic<bool> daemon_running;
extern std::atomic<bool> verbose_mode;

namespace chitta {

bool run_distillation(
    FieldStore& field_store,
    VakYantra* yantra,
    const TranscriptState& state,
    const DistillConfig& config,
    FieldRpcHandler* handler,
    bool queue_triggered,
    DistillResult* out
) {
    NativeDistillConfig native_config;
    if (handler) native_config.mind_path = handler->mind_path();
    native_config.model = handler ? handler->get_distill_model() : config.model;
    native_config.endpoint = config.endpoint;  // pre-probed by slow lane; empty = discover
    native_config.timeout_secs = 180;
    native_config.min_turns = config.min_turns;
    native_config.verbose = verbose_mode;
    native_config.max_context_chars = config.max_context_chars;
    native_config.max_tokens = config.max_tokens;
    native_config.max_lines_per_pass = config.max_lines_per_pass;

    auto embed_fn = [yantra](const std::string& text) -> std::vector<float> {
        if (!yantra) return {};
        try { return yantra->transform(text).nu.data; } catch (...) {}
        return {};
    };
    auto distiller = std::make_unique<NativeDistiller>(field_store, embed_fn, native_config);

    distiller->set_cancel_callback([]() {
        return !daemon_running.load();
    });

    if (verbose_mode) {
        distiller->set_log_callback([](const std::string& msg) {
            std::cerr << msg << "\n";
        });
    }

    // Phase 1 (lock-free): parse transcript + LLM call + SSL parse.
    // The LLM HTTP call takes 10-30s — running it without the lock keeps the
    // daemon responsive to health_check and other read queries during distillation.
    auto prep = distiller->prepare_distillation(
        state.session_id,
        state.transcript_path,
        state.realm,
        state.last_processed_line,
        queue_triggered
    );

    if (!prep.valid) {
        if (!prep.error.empty())
            std::cerr << "[distill] " << state.session_id << ": " << prep.error << "\n";
        return false;
    }

    // Phase 2 (exclusive lock): episode + learnings + triplets writes.
    // Lock scope is now milliseconds, not 10-30s.
    DistillResult result;
    {
        std::unique_lock<std::shared_mutex> _lk;
        if (handler) _lk = handler->acquire_lock();
        result = distiller->commit_distillation(prep);
        if (out) *out = result;

        if (!result.success) {
            if (!result.error.empty())
                std::cerr << "[distill] " << state.session_id << ": " << result.error << "\n";
            return false;
        }

        field_store.emit_event("transcript", "progress",
                               state.session_id,
                               "{\"last_line\":" + std::to_string(result.last_line) + "}");
        field_store.emit_event("transcript", "distilled", state.session_id, "{}");
    }

    if (verbose_mode) {
        std::cerr << "[distill] Completed " << state.session_id
                  << " (line " << result.last_line << ", +"
                  << result.learnings_stored << " new, "
                  << result.learnings_deduped << " deduped, "
                  << result.triplets_created << " triplets)\n";
    }
    return true;
}

std::string generate_stats(FieldStore& field_store, VakYantra* yantra) {
    std::ostringstream oss;
    oss << "{"
        << "\"version\":\"" << CHITTA_VERSION << "\","
        << "\"memories\":" << field_store.memory_count() << ","
        << "\"symbols\":" << field_store.symbol_count() << ","
        << "\"yantra\":" << (yantra ? "true" : "false")
        << "}";
    return oss.str();
}

} // namespace chitta
