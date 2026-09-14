#include "chitta/rpc/field_handler.hpp"
#include <cassert>
#include <future>
#include <stdexcept>

using chitta::QueryEmbeddingCache;
int main() {
    unsetenv("CHITTA_EMBED_CACHE");
    assert(QueryEmbeddingCache::configured_capacity() == 512);
    setenv("CHITTA_EMBED_CACHE", "0", 1);
    assert(QueryEmbeddingCache::configured_capacity() == 0);
    setenv("CHITTA_EMBED_CACHE", "invalid", 1);
    assert(QueryEmbeddingCache::configured_capacity() == 512);
    int calls = 0;
    auto compute = [&] { return std::vector<float>{float(++calls)}; };
    QueryEmbeddingCache cache(2);
    assert(cache.get("", compute).empty());
    assert(cache.get("a", compute)[0] == 1);
    assert(cache.get("b", compute)[0] == 2);
    assert(cache.get("a", compute)[0] == 1); // promotes a
    assert(cache.get("c", compute)[0] == 3); // evicts b
    assert(cache.get("a", compute)[0] == 1);
    assert(cache.get("b", compute)[0] == 4);
    assert(cache.get("A", compute)[0] == 5); // exact case
    assert(cache.get("A ", compute)[0] == 6); // exact whitespace
    assert(cache.stats()["hits"] == 2);
    assert(cache.stats()["misses"] == 6);
    QueryEmbeddingCache disabled(0);
    disabled.get("a", compute); disabled.get("a", compute);
    assert(calls == 8 && disabled.stats()["misses"] == 2);

    QueryEmbeddingCache concurrent(2);
    std::promise<void> started, release;
    auto released = release.get_future().share();
    auto first = std::async(std::launch::async, [&] {
        return concurrent.get("same", [&] {
            started.set_value(); released.wait(); return std::vector<float>{42};
        });
    });
    started.get_future().wait();
    std::vector<std::future<std::vector<float>>> others;
    for (int i = 0; i < 11; ++i)
        others.push_back(std::async(std::launch::async, [&] {
            return concurrent.get("same", [] { assert(false); return std::vector<float>{}; });
        }));
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (concurrent.stats()["coalesced"] != 11) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    // Unrelated queries proceed while the first computation is blocked.
    assert(concurrent.get("other", [] { return std::vector<float>{7}; })[0] == 7);
    release.set_value();
    assert(first.get()[0] == 42);
    for (auto& other : others) assert(other.get()[0] == 42);
    assert(concurrent.stats()["misses"] == 2);
    assert(concurrent.stats()["coalesced"] == 11);
    assert(concurrent.get("empty", [] { return std::vector<float>{}; }).empty());
    assert(concurrent.get("empty", [] { return std::vector<float>{9}; })[0] == 9);
    try {
        concurrent.get("throw", []() -> std::vector<float> { throw std::runtime_error("test"); });
        assert(false);
    } catch (const std::runtime_error&) {}
    assert(concurrent.get("throw", [] { return std::vector<float>{10}; })[0] == 10);
}
