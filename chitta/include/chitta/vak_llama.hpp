#pragma once
// LlamaYantra — VakYantra backed by llama.cpp running a GGUF embedding model in-process.
// Public default model: bge-large-en-v1.5 (1024-d, BERT mean-pooled). Personal builds use
// ssl_distiller_dpo (1536-d) via the CHITTA_EMBED_* build override. No external server.
// Compile-time guard: only active when CHITTA_WITH_LLAMA_CPP is defined.
#include "chitta/vak.hpp"
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <mutex>
#include <array>
#include <semaphore>
#include <atomic>
#include <thread>
#include <chrono>
#include <string>
#include <vector>
#include <filesystem>

#ifdef CHITTA_WITH_LLAMA_CPP
#include <llama.h>

namespace chitta {

class LlamaYantra : public VakYantra {
public:
    static constexpr int N_CTX      = 8192;   // upper bound; clamped to the model's trained ctx
    static constexpr int MAX_TOKENS = 8000;   // upper bound; clamped to the model's trained ctx
    static constexpr int N_GPU_LAYERS = 99;   // offload all if CUDA present; no-op on CPU build

    // model_path: if empty, discover via env/home. Pass mind_path to enable third search location.
    explicit LlamaYantra(std::string model_path = "", const std::string& mind_path = "")
        : model_path_(model_path.empty() ? discover_model_path(mind_path) : std::move(model_path)) {
        if (model_path_.empty()) {
            log("[llama-embed] no GGUF model found — will fall back to Ollama");
            return;
        }
        if (!load()) {
            log("[llama-embed] WARNING: failed to load model at " + model_path_);
            cleanup();
            return;
        }
        ready_ = true;
        log("[llama-embed] ready: " + model_path_ +
            " (n_embd=" + std::to_string(n_embd_) +
            ", gpu=" + (gpu_ ? "yes" : "no") + ")");
    }

    ~LlamaYantra() override { cleanup(); }

    LlamaYantra(const LlamaYantra&) = delete;
    LlamaYantra& operator=(const LlamaYantra&) = delete;

    size_t dimension() const override { return EMBED_DIM; }
    bool   ready()     const override { return ready_; }

    Artha transform(const std::string& vak) override {
        return transform(vak, EmbedMode::Document);
    }

    Artha transform(const std::string& vak, EmbedMode mode) override {
        Vector v = embed_one(add_prefix(vak, mode), mode == EmbedMode::Query);
        const bool valid = std::any_of(v.data.begin(), v.data.end(), [](float x) { return x != 0.0f; });
        return Artha{std::move(v), valid ? 1.0f : 0.0f, vak};
    }

    std::vector<Artha> transform_batch(const std::vector<std::string>& vaks) override {
        return transform_batch(vaks, EmbedMode::Document);
    }

    std::vector<Artha> transform_batch(const std::vector<std::string>& vaks, EmbedMode mode) {
        std::vector<Artha> out;
        out.reserve(vaks.size());
        for (const auto& vak : vaks) out.push_back(transform(vak, mode));
        return out;
    }

private:
    friend struct LlamaYantraTestAccess;
    std::string    model_path_;
    bool           ready_  = false;
    bool           gpu_    = false;
    int            n_embd_ = 0;
    int            n_ctx_eff_ = N_CTX;  // effective context = min(N_CTX, model trained ctx)
    llama_model*   model_  = nullptr;
    struct Context {
        llama_context* ctx = nullptr;
        std::mutex mutex;  // A llama_context is never used by two callers at once.
    };
    std::array<Context, 16> contexts_;
    std::counting_semaphore<16> available_{0};
    size_t context_count_ = 0;
    std::atomic<int> query_waiters_{0};

    static int configured_contexts() {
        const char* env = std::getenv("CHITTA_EMBED_CONTEXTS");
        if (!env || !*env) return 4;
        char* end = nullptr;
        long n = std::strtol(env, &end, 10);
        return end == env || *end ? 4 : static_cast<int>(std::clamp(n, 1L, 16L));
    }

    // Return the context before its permit, including on decode failure/exception.
    struct Lease {
        std::unique_lock<std::mutex> lock;
        std::counting_semaphore<16>& available;
        ~Lease() { lock.unlock(); available.release(); }
    };

    static std::string add_prefix(const std::string& text, EmbedMode mode) {
        if (mode == EmbedMode::Query) return "search_query: "    + text;
        return                               "search_document: " + text;
    }

    static std::string discover_model_path(const std::string& mind_path = "") {
        namespace fs = std::filesystem;
        static constexpr auto MODEL_FILE = "bge-large-en-v1.5.gguf";

        // 1) env override
        if (const char* env = std::getenv("CHITTA_EMBED_MODEL"))
            if (env[0] && fs::exists(env)) return env;

        // 2) ~/.claude/models/
        if (const char* home = std::getenv("HOME")) {
            fs::path p = fs::path(home) / ".claude" / "models" / MODEL_FILE;
            if (fs::exists(p)) return p.string();
        }

        // 3) $mind_path/../../models/ (sibling of the mind dir)
        if (!mind_path.empty()) {
            fs::path p = fs::path(mind_path) / ".." / ".." / "models" / MODEL_FILE;
            if (fs::exists(p)) return fs::canonical(p).string();
        }

        return "";
    }

    bool load() {
        llama_backend_init();

        llama_model_params mparams = llama_model_default_params();
        mparams.n_gpu_layers = N_GPU_LAYERS;
        model_ = llama_model_load_from_file(model_path_.c_str(), mparams);
        if (!model_) return false;

        n_embd_ = llama_model_n_embd(model_);
        if (static_cast<size_t>(n_embd_) != EMBED_DIM) {
            log("[llama-embed] ERROR: model n_embd=" + std::to_string(n_embd_) +
                " != EMBED_DIM=" + std::to_string(EMBED_DIM) + " — rejecting model");
            return false;
        }

        // Clamp the context to the model's trained context. BERT-style embedders (e.g.
        // bge-large-en-v1.5) have only 512 learned position embeddings; feeding a longer
        // sequence overflows the position-embedding gather (GGML_ASSERT i01 < ne01). Large
        // decoder embedders (nomic 8192, ssl_distiller 32k) keep the full N_CTX.
        {
            int nct = llama_model_n_ctx_train(model_);
            n_ctx_eff_ = (nct > 0 && nct < N_CTX) ? nct : N_CTX;
        }

        llama_context_params cparams = llama_context_default_params();
        cparams.n_ctx        = n_ctx_eff_;
        cparams.n_batch      = n_ctx_eff_;
        cparams.n_ubatch     = n_ctx_eff_;  // non-causal embedding requires n_ubatch >= n_tokens
        cparams.embeddings   = true;
        cparams.pooling_type = LLAMA_POOLING_TYPE_MEAN;
        // CHITTA_EMBED_THREADS overrides llama's conservative default thread count. The CPU
        // embedder is compute-bound; on a many-core host this is a large throughput win
        // (e.g. bulk re_embed). Unset → llama default (unchanged behavior).
        if (const char* t = std::getenv("CHITTA_EMBED_THREADS")) {
            int nt = std::atoi(t);
            if (nt > 0) { cparams.n_threads = nt; cparams.n_threads_batch = nt; }
        }
        for (int i = 0; i < configured_contexts(); ++i) {
            auto* ctx = llama_init_from_model(model_, cparams);
            if (!ctx) return false;  // Constructor cleanup frees even a partial pool.
            contexts_[context_count_++].ctx = ctx;
        }
        available_.release(context_count_);
        log("[llama-embed] contexts=" + std::to_string(context_count_) +
            " threads/context=" + std::to_string(cparams.n_threads));

        gpu_ = llama_supports_gpu_offload() && (N_GPU_LAYERS > 0);
        return true;
    }

    void cleanup() {
        for (auto& context : contexts_) {
            if (context.ctx) { llama_free(context.ctx); context.ctx = nullptr; }
        }
        context_count_ = 0;
        if (model_) { llama_model_free(model_); model_ = nullptr; }
        // Do NOT call llama_backend_free() — it's process-global; freeing here risks
        // double-free if the yantra is replaced/reconstructed during the daemon lifetime.
        ready_ = false;
    }

    // Query waiters claim permits ahead of background work. Sustained recall can
    // defer documents indefinitely; background queues drain between recall bursts.
    Vector embed_one(const std::string& text, bool high_prio) {
        if (!ready_ || !model_) return {};
        if (high_prio) {
            query_waiters_.fetch_add(1, std::memory_order_acq_rel);
            available_.acquire();
            query_waiters_.fetch_sub(1, std::memory_order_acq_rel);
        } else {
            for (;;) {
                if (query_waiters_.load(std::memory_order_acquire) == 0
                    && available_.try_acquire()) {
                    if (query_waiters_.load(std::memory_order_acquire) == 0) break;
                    available_.release();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        // A permit guarantees a free context, but another admitted caller may
        // win a try_lock while we scan. Retry the scan rather than queue on it.
        for (;;) {
            for (size_t i = 0; i < context_count_; ++i) {
                std::unique_lock<std::mutex> lock(contexts_[i].mutex, std::try_to_lock);
                if (!lock.owns_lock()) continue;
                Lease lease{std::move(lock), available_};
                return embed_context(text, contexts_[i].ctx);
            }
            std::this_thread::yield();
        }
    }

    Vector embed_context(const std::string& text, llama_context* ctx) {
        Vector v;
        const llama_vocab* vocab = llama_model_get_vocab(model_);

        // Tokenize — add_special=true so BOS/EOS are added per model config.
        std::vector<llama_token> toks(N_CTX);
        int n = llama_tokenize(vocab, text.c_str(), (int)text.size(),
                               toks.data(), (int)toks.size(),
                               /*add_special=*/true, /*parse_special=*/false);
        if (n < 0) {
            // Buffer too small: -n is required size; resize and retry once.
            toks.resize(-n);
            n = llama_tokenize(vocab, text.c_str(), (int)text.size(),
                               toks.data(), (int)toks.size(), true, false);
        }
        if (n <= 0) return v;
        // Truncate to the effective context (= model trained ctx). Essential for BERT
        // embedders whose position-embedding table is the hard upper bound on n_tokens.
        int max_toks = std::min(MAX_TOKENS, n_ctx_eff_);
        if (n > max_toks) n = max_toks;
        toks.resize(n);

        // Clear KV cache between sequences — essential for pooled embeddings.
        llama_memory_clear(llama_get_memory(ctx), false);

        llama_batch batch = llama_batch_get_one(toks.data(), (int)toks.size());
        if (llama_decode(ctx, batch) != 0) {
            // Edge-case input (e.g. empty/oversized batch): drop this embedding and
            // return the zero vector. Do NOT rebuild the context here — a per-item
            // context reinit rebuilt the whole llama ctx under the embed mutex and
            // pegged multiple cores across the distill/backfill queue (the c20bc907
            // recall-hang + CPU-storm regression). The shared context stays valid
            // across a single-item decode failure; the next item embeds normally.
            log("[llama-embed] decode failed (len=" + std::to_string(text.size()) +
                ") — dropping embedding");
            return v;
        }

        // Prefer pooled sequence embedding; fall back to per-token mean if unavailable.
        const float* emb = llama_get_embeddings_seq(ctx, 0);
        if (!emb) emb = llama_get_embeddings(ctx);
        if (!emb) return v;

        v.data.assign(emb, emb + EMBED_DIM);
        l2_normalize(v);
        return v;
    }

    static void l2_normalize(Vector& v) {
        float norm = 0.0f;
        for (float x : v.data) norm += x * x;
        norm = std::sqrt(norm);
        if (norm < 1e-8f) return;
        for (float& x : v.data) x /= norm;
    }

    static void log(const std::string& msg) { fprintf(stderr, "%s\n", msg.c_str()); }
};

} // namespace chitta

#endif // CHITTA_WITH_LLAMA_CPP
