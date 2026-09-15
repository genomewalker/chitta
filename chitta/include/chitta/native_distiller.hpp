#pragma once
// NativeDistiller: C++ distillation
//
// Extracts learnings from conversation transcripts using LLM + SSL format:
// 1. Parse JSONL transcript (streaming, handles any file size)
// 2. Build SSL prompt
// 3. Call LLM via HTTP (Ollama/vLLM endpoint, auto-discovered)
// 4. Parse SSL output
// 5. Store via FieldStore
//    - Dedup: if a near-identical memory exists (cosine > threshold), strengthen
//      the existing one instead of creating a duplicate

#include "transcript_parser.hpp"
#include "ssl_parser.hpp"
#include "field_store.hpp"
#include "llm_http.hpp"
#include "value_fact_extractor.hpp"
#include <string>
#include <functional>
#include <vector>

namespace chitta {

struct TranscriptState {
    std::string session_id;
    std::string transcript_path;
    std::string realm = "brahman";
    int64_t last_processed_line = 0;
};

struct NativeDistillConfig {
    std::string model = "gemma4:26b";         // LLM model — overridden by --distill-model
    std::string endpoint = "";                // HTTP endpoint (auto-discovered if empty)
    int timeout_secs = 180;                   // Timeout for HTTP call
    int min_turns = 5;                        // Minimum turns for distillation
    bool verbose = false;                     // Enable verbose logging
    float dedup_threshold = 0.92f;            // Cosine similarity above which we strengthen
                                              // instead of storing a duplicate
    size_t max_context_chars = 0;             // 0 = no limit (pass full transcript to LLM)
    int max_tokens = 8192;                    // LLM output token limit
    // BOUND: cap lines parsed per distill pass so a 108MB transcript is chunked
    // across incremental passes (progress event carries last_line forward) rather
    // than parsed+distilled in one unbounded shot. 0 = no cap.
    int64_t max_lines_per_pass = 20000;
    // BOUND: cap bytes fed to the deterministic value-fact extractor. Independent
    // of the LLM context cap; keeps the lock-free precompute pass finite. 0 = no cap.
    size_t value_fact_max_bytes = 4u * 1024 * 1024;
    // Reserved compatibility field for callers; no longer used by distillation.
    std::string mind_path;
};

struct DistillResult {
    int learnings_stored = 0;
    int learnings_deduped = 0;  // Strengthened existing instead of storing new
    int triplets_created = 0;
    int citations_linked = 0;   // Code citations (memory→file:line)
    int value_facts_stored = 0; // Deterministic (identifier,value) atoms stored
    int value_facts_deduped = 0;// Value-facts a strict holder already covered
    int64_t episode_id = 0;
    int64_t last_line = 0;      // Last JSONL line processed (for progress tracking)
    bool success = false;
    std::string error;
};

// Per-learning result from the lock-free dedup precompute phase.
// Holds the embedding and any matching existing memory, so commit_distillation
// can skip field_store_->recall() (an O(N) embedding scan) while holding the lock.
struct LearningPrep {
    std::vector<float> embedding;
    bool is_dup = false;
    uint64_t dup_mem_id = 0;
    float dup_confidence = 0.0f;
};

// Per-value-fact result from the lock-free precompute phase — mirrors LearningPrep
// so commit_distillation writes value-facts without any embed/recall under the lock
// (the fix for the daemon-wide hang: extraction + embedding + dedup recall all run
// lock-free in prepare_distillation, only remember()/edges run under the write lock).
struct ValueFactPrep {
    ValueFact fact;
    std::vector<float> embedding;
    bool is_dup = false;
};

// Output of the lock-free preparation phase (transcript parse + LLM call + SSL parse
// + dedup embedding recalls).  Pass to commit_distillation() under the write lock.
struct PreparedDistillation {
    std::string session_id;
    std::string realm;
    int start_turn = 0;
    int end_turn = 0;
    int64_t last_line = 0;
    std::string ep_content;
    std::vector<float> ep_embedding;
    SSLParser::Result ssl_result;
    std::vector<LearningPrep> learning_preps;  // indexed 1:1 with ssl_result.learnings
    std::string conversation;                  // raw text — fed to the value-fact extractor
    std::vector<ValueFactPrep> value_fact_preps; // precomputed value-facts (embed+dedup done lock-free)
    bool valid = false;
    std::string error;
};

// Embedder function type — takes text, returns 768-dim float vector (or empty on failure)
using EmbedFn = std::function<std::vector<float>(const std::string&)>;

class NativeDistiller {
public:
    NativeDistiller(FieldStore& field, EmbedFn embedder, const NativeDistillConfig& config = {});

    // Phase 1 (lock-free): parse transcript, call LLM, parse SSL output.
    // No field_store writes — safe to call without holding the RPC mutex.
    PreparedDistillation prepare_distillation(
        const std::string& session_id,
        const std::string& transcript_path,
        const std::string& realm,
        int64_t skip_lines = 0,
        bool queue_triggered = false
    );

    // Phase 2 (needs write lock): create episode + store learnings/triplets.
    // Must be called while holding the exclusive RPC mutex.
    DistillResult commit_distillation(const PreparedDistillation& prep);

    // Combined convenience wrapper — holds lock across both phases.
    // Use only when the caller cannot split (e.g. manual distill command).
    DistillResult distill_session(
        const std::string& session_id,
        const std::string& transcript_path,
        const std::string& realm,
        int64_t skip_lines = 0,
        bool queue_triggered = false
    );

    // Set callback for progress messages
    using LogCallback = std::function<void(const std::string&)>;
    void set_log_callback(LogCallback cb) { log_callback_ = cb; }

    // Set cancellation check callback - return true to abort distillation
    using CancelCallback = std::function<bool()>;
    void set_cancel_callback(CancelCallback cb) { cancel_callback_ = cb; }

private:
    FieldStore* field_store_ = nullptr;
    EmbedFn embedder_;

    NativeDistillConfig config_;
    TranscriptParser parser_;
    SSLParser ssl_parser_;
    LogCallback log_callback_;
    CancelCallback cancel_callback_;
    std::string cached_endpoint_;

    // Call LLM via HTTP (Ollama/vLLM)
    std::string call_llm(const std::string& prompt);

    // Phase 1b (lock-free): embed all learnings + run recall-based dedup checks.
    // Populates prep.learning_preps so commit_distillation needs no recall calls.
    void precompute_dedup(PreparedDistillation& prep);

    // Store learnings using precomputed dedup results — no field_store reads, writes only.
    void store_learnings(
        const SSLParser::Result& ssl_result,
        const std::string& realm,
        uint64_t episode_mem_id,
        const std::vector<LearningPrep>& learning_preps,
        DistillResult& result
    );

    // Phase 1c (lock-free): extract value-facts from prep.conversation, embed each,
    // and run recall-based dedup — populates prep.value_fact_preps. No field_store
    // writes, so this heavy pass (extract + N embeds + 2N recalls) runs OUTSIDE the
    // RPC lock. This is the core hang fix: previously all of this ran under the lock.
    void precompute_value_facts(PreparedDistillation& prep);

    // Phase 2b (needs write lock): write the precomputed value-facts — remember() +
    // episode edges only, no embed/recall. Gated by env CHITTA_VALUE_FACTS (default on).
    void store_value_facts(
        const std::vector<ValueFactPrep>& value_fact_preps,
        const std::string& realm,
        uint64_t episode_mem_id,
        DistillResult& result
    );

    static bool value_facts_enabled();

    static float category_to_confidence(const std::string& category);
    static float category_to_decay(const std::string& category);
    void log(const std::string& msg);
};

} // namespace chitta
