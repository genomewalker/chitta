#include "chitta/vak_llama.hpp"
#include <cassert>
#include <cstring>
#include <future>
#include <iostream>

namespace chitta {
struct LlamaYantraTestAccess {
    static void check_config() {
        unsetenv("CHITTA_EMBED_CONTEXTS");
        assert(LlamaYantra::configured_contexts() == 4);
        for (const auto& [value, expected] : std::vector<std::pair<const char*, int>>{
                 {"0", 1}, {"-2", 1}, {"19", 16}, {"bad", 4}, {"4x", 4}, {"1", 1}}) {
            setenv("CHITTA_EMBED_CONTEXTS", value, 1);
            assert(LlamaYantra::configured_contexts() == expected);
        }
        setenv("CHITTA_EMBED_CONTEXTS", "4", 1);
    }
    static void check_pool(LlamaYantra& yantra) {
        assert(yantra.context_count_ == 4);
        const std::string query = "how does chitta semantic recall use the field store";
        const auto reference = yantra.transform(query, EmbedMode::Query);
        assert(reference.certainty == 1.0f);
        auto identical = [&](const Vector& v) {
            assert(v.data.size() == reference.nu.data.size());
            assert(std::memcmp(v.data.data(), reference.nu.data.data(),
                               v.data.size() * sizeof(float)) == 0);
        };
        // Force every context, including concurrent decode against the shared model.
        std::vector<std::future<Vector>> jobs;
        for (size_t i = 0; i < yantra.context_count_; ++i) {
            jobs.push_back(std::async(std::launch::async, [&, i] {
                std::lock_guard<std::mutex> lock(yantra.contexts_[i].mutex);
                return yantra.embed_context("search_query: " + query, yantra.contexts_[i].ctx);
            }));
        }
        for (auto& job : jobs) identical(job.get());
        // The public path must use a free slot even when the first is unavailable.
        {
            std::unique_lock<std::mutex> lock(yantra.contexts_[0].mutex);
            auto job = std::async(std::launch::async, [&] {
                return yantra.transform(query, EmbedMode::Query);
            });
            assert(job.wait_for(std::chrono::seconds(30)) == std::future_status::ready);
            identical(job.get().nu);
        }
        // More callers than permits; mixed priority traffic must drain and reuse slots.
        std::vector<std::future<Artha>> burst;
        for (int i = 0; i < 24; ++i)
            burst.push_back(std::async(std::launch::async, [&, i] {
                return yantra.transform(query, i % 3 ? EmbedMode::Query : EmbedMode::Document);
            }));
        for (size_t i = 0; i < burst.size(); ++i) {
            assert(burst[i].wait_for(std::chrono::seconds(30)) == std::future_status::ready);
            auto result = burst[i].get();
            assert(result.certainty == 1.0f);
            if (i % 3) identical(result.nu);
        }
        for (size_t i = 0; i < yantra.context_count_; ++i)
            assert(yantra.available_.try_acquire());
        assert(!yantra.available_.try_acquire());
        yantra.available_.release(yantra.context_count_);
    }
};
}
int main() {
    chitta::LlamaYantraTestAccess::check_config();
    const char* model = std::getenv("CHITTA_EMBED_MODEL");
    if (!model || !*model) {
        std::cerr << "Set CHITTA_EMBED_MODEL to a matching GGUF for pool integration test\n";
        return 77;
    }
    chitta::LlamaYantra yantra(model);
    assert(yantra.ready());
    chitta::LlamaYantraTestAccess::check_pool(yantra);
    std::cout << "four contexts bit-identical; first-free admission and 24 callers passed\n";
}
