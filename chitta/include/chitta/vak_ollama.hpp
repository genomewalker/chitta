#pragma once
// OllamaYantra — VakYantra implementation backed by the Ollama embeddings API.
// Model: nomic-embed-text:v1.5  Dimension: 768  Batch: 64 × PARALLEL_CALLS concurrent
// Replaces AntahkaranaYantra (ONNX) entirely.
#include "chitta/vak.hpp"
#include "chitta/llm_http.hpp"
#include <nlohmann/json.hpp>
#include <chitta/httplib.h>
#include <cmath>
#include <atomic>
#include <mutex>
#include <future>
#include <chrono>
#include <thread>

namespace chitta {

class OllamaYantra : public VakYantra {
public:
    static constexpr size_t BATCH          = 64;  // items per HTTP call
    static constexpr size_t PARALLEL_CALLS = 2;   // concurrent HTTP calls (one per GPU)
    static constexpr int    MAX_RETRIES    = 3;
    static constexpr int    TIMEOUT_MS     = 60000;

    explicit OllamaYantra(
        std::string model     = "nomic-embed-text:v1.5",
        std::string base_url  = ""
    ) : model_(std::move(model)), base_url_(std::move(base_url)), ready_(false) {
        if (base_url_.empty()) {
            base_url_ = discover_gpu_endpoint("embed", model_);
        }
        ready_ = !base_url_.empty() && probe();
        if (ready_) {
            log("[ollama-embed] ready at " + base_url_ + " model=" + model_);
        } else {
            log("[ollama-embed] WARNING: Ollama not reachable — memories will queue as embed_pending");
        }
    }

    size_t dimension() const override { return EMBED_DIM; }
    bool   ready()     const override { return ready_; }

    Artha transform(const std::string& vak) override {
        return transform(vak, EmbedMode::Document);
    }

    Artha transform(const std::string& vak, EmbedMode mode) override {
        auto batch = transform_batch({vak}, mode);
        return batch.empty() ? Artha{} : batch[0];
    }

    std::vector<Artha> transform_batch(const std::vector<std::string>& vaks) override {
        return transform_batch(vaks, EmbedMode::Document);
    }

    std::vector<Artha> transform_batch(
        const std::vector<std::string>& vaks,
        EmbedMode mode
    ) {
        std::vector<Artha> results(vaks.size());

        // Collect all sub-batches, then fire PARALLEL_CALLS concurrently so both GPUs work.
        struct SubBatch { size_t offset; std::vector<std::string> texts; };
        std::vector<SubBatch> sub_batches;
        for (size_t offset = 0; offset < vaks.size(); offset += BATCH) {
            size_t end = std::min(offset + BATCH, vaks.size());
            SubBatch sb; sb.offset = offset;
            sb.texts.reserve(end - offset);
            for (size_t i = offset; i < end; ++i)
                sb.texts.push_back(add_prefix(vaks[i], mode));
            sub_batches.push_back(std::move(sb));
        }

        for (size_t bi = 0; bi < sub_batches.size(); ) {
            size_t wave_end = std::min(bi + PARALLEL_CALLS, sub_batches.size());
            std::vector<std::future<std::vector<Vector>>> futures;
            for (size_t wi = bi; wi < wave_end; ++wi) {
                auto& sb = sub_batches[wi];
                futures.push_back(std::async(std::launch::async,
                    [this, texts = sb.texts]() mutable { return embed_batch(texts); }));
            }
            for (size_t wi = bi; wi < wave_end; ++wi) {
                auto batch_vecs = futures[wi - bi].get();
                size_t off = sub_batches[wi].offset;
                for (size_t i = 0; i < batch_vecs.size(); ++i)
                    results[off + i] = Artha{std::move(batch_vecs[i]), 1.0f, vaks[off + i]};
            }
            bi = wave_end;
        }
        return results;
    }

private:
    std::string        model_;
    std::string        base_url_;
    std::atomic<bool>  ready_;
    mutable std::mutex mtx_;

    std::string add_prefix(const std::string& text, EmbedMode mode) const {
        // nomic-embed-text v1.5 prefixes
        if (mode == EmbedMode::Query)    return "search_query: "    + text;
        return                                  "search_document: " + text;
    }

    bool probe() {
        try {
            auto [host, port, path] = parse_url(base_url_ + "/api/tags");
            httplib::Client cli(host, port);
            cli.set_connection_timeout(2, 0);
            auto res = cli.Get(path.c_str());
            return res && res->status == 200;
        } catch (...) { return false; }
    }

    // Embed a single input with progressive truncation on HTTP 400 (context overflow).
    // Tries full text first, then halves down to 500 bytes before giving up.
    Vector embed_one_safe(const std::string& input) {
        static const size_t TRUNCATION_STEPS[] = {
            std::string::npos, 4000, 2000, 1000, 500
        };
        for (size_t max_bytes : TRUNCATION_STEPS) {
            std::string text = (max_bytes == std::string::npos || input.size() <= max_bytes)
                               ? input : input.substr(0, max_bytes);
            nlohmann::json body;
            body["model"] = model_;
            body["input"] = nlohmann::json::array({text});
            std::string body_str = body.dump();
            try {
                auto [host, port, path] = parse_url(base_url_ + "/api/embed");
                httplib::Client cli(host, port);
                cli.set_connection_timeout(2, 0);  // dead node: fail fast, not 300s default
                cli.set_read_timeout(TIMEOUT_MS / 1000, 0);
                cli.set_write_timeout(5, 0);
                auto res = cli.Post(path.c_str(), body_str, "application/json");
                if (!res) continue;
                if (res->status == 400) continue;  // truncate and retry
                if (res->status != 200) { ready_ = false; return Vector{}; }
                auto j = nlohmann::json::parse(res->body);
                auto& emb_field = j.contains("embeddings") ? j["embeddings"] : j["embedding"];
                if (emb_field.empty()) continue;
                auto& row = emb_field[0];
                if (row.size() != EMBED_DIM) continue;
                Vector v;
                v.data.resize(EMBED_DIM);
                for (size_t d = 0; d < EMBED_DIM; ++d) v.data[d] = row[d].get<float>();
                l2_normalize(v);
                return v;
            } catch (...) {}
        }
        log("[ollama-embed] WARNING: could not embed even at 500 bytes — zero-vec");
        return Vector{};
    }

    std::vector<Vector> embed_batch(const std::vector<std::string>& inputs) {
        std::vector<Vector> out(inputs.size());  // zero-vectors as fallback

        nlohmann::json body;
        body["model"] = model_;
        body["input"] = inputs;
        std::string body_str = body.dump();

        for (int attempt = 0; attempt < MAX_RETRIES; ++attempt) {
            try {
                auto [host, port, path] = parse_url(base_url_ + "/api/embed");
                httplib::Client cli(host, port);
                cli.set_connection_timeout(2, 0);  // dead node: fail fast, not 300s default
                cli.set_read_timeout(TIMEOUT_MS / 1000, 0);
                cli.set_write_timeout(5, 0);

                auto res = cli.Post(path.c_str(), body_str, "application/json");
                if (!res || res->status != 200) {
                    if (attempt + 1 < MAX_RETRIES) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(200 * (1 << attempt)));
                        continue;
                    }
                    // On persistent 400 (context overflow), retry each item individually
                    // with progressive truncation so no memory is silently skipped.
                    if (res && res->status == 400) {
                        for (size_t i = 0; i < inputs.size(); ++i)
                            out[i] = embed_one_safe(inputs[i]);
                        ready_ = true;
                        return out;
                    }
                    log("[ollama-embed] ERROR: HTTP " +
                        std::to_string(res ? res->status : -1) + " after " +
                        std::to_string(MAX_RETRIES) + " retries — returning zero-vecs");
                    if (res) log("[ollama-embed] response body: " + res->body.substr(0, 200));
                    // Don't permanently poison ready_: ollama may be briefly busy (LLM load).
                    // Re-probe on next call via ready() → re-enable automatically.
                    ready_ = probe();
                    return out;
                }

                auto j = nlohmann::json::parse(res->body);
                // Ollama returns either "embeddings" (array) or "embedding" (single)
                auto& emb_field = j.contains("embeddings") ? j["embeddings"] : j["embedding"];

                for (size_t i = 0; i < inputs.size() && i < emb_field.size(); ++i) {
                    auto& row = emb_field[i];
                    if (row.size() != EMBED_DIM) {
                        log("[ollama-embed] WARNING: expected " +
                            std::to_string(EMBED_DIM) + " dims, got " +
                            std::to_string(row.size()) + " — zero-vec");
                        continue;
                    }
                    Vector v;
                    v.data.resize(EMBED_DIM);
                    for (size_t d = 0; d < EMBED_DIM; ++d) {
                        v.data[d] = row[d].get<float>();
                    }
                    l2_normalize(v);
                    out[i] = std::move(v);
                }
                ready_ = true;
                return out;
            } catch (const std::exception& e) {
                if (attempt + 1 < MAX_RETRIES) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(200 * (1 << attempt)));
                    continue;
                }
                log(std::string("[ollama-embed] exception: ") + e.what());
                ready_ = false;
            }
        }
        return out;
    }

    static void l2_normalize(Vector& v) {
        float norm = 0.0f;
        for (float x : v.data) norm += x * x;
        norm = std::sqrt(norm);
        if (norm < 1e-8f) return;
        for (float& x : v.data) x /= norm;
    }

    // Parse "http://host:port/path" → {host, port, "/path"}
    static std::tuple<std::string, int, std::string>
    parse_url(const std::string& url) {
        // strip scheme
        auto s = url;
        if (s.substr(0, 7) == "http://")  s = s.substr(7);
        if (s.substr(0, 8) == "https://") s = s.substr(8);
        auto slash = s.find('/');
        std::string hostport = (slash == std::string::npos) ? s : s.substr(0, slash);
        std::string path     = (slash == std::string::npos) ? "/" : s.substr(slash);
        auto colon = hostport.rfind(':');
        std::string host = (colon == std::string::npos) ? hostport : hostport.substr(0, colon);
        int port = 11434;
        if (colon != std::string::npos) port = std::stoi(hostport.substr(colon + 1));
        return {host, port, path};
    }

    static void log(const std::string& msg) {
        fprintf(stderr, "%s\n", msg.c_str());
    }
};

} // namespace chitta
