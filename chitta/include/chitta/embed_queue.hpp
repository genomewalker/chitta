#pragma once
// EmbedQueue — cached query lane and opt-in bounded document workers.
//
// The default retains one embedding worker. CHITTA_EMBED_WRITE_WORKERS>0
// uses the existing thread-safe model-context pool through separate workers.
//   - One query worker, plus a bounded document lane when enabled
//   - Two lanes: READ jobs (await ≤2s, fall to BM25 on miss) and WRITE jobs
//     (fire-and-forget, warm cache for future reads)
//   - LRU cache keyed by text hash; checked synchronously before submit
//   - Inflight coalescing: identical texts share one future
//   - Deadline-skip for READ jobs only; WRITE jobs always run to warm cache

#include "vak.hpp"
#include <functional>
#include <future>
#include <queue>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <unordered_map>
#include <list>
#include <optional>
#include <chrono>
#include <atomic>
#include <cstdlib>
#include <stdexcept>

namespace chitta {

class EmbedQueue {
public:
    using Vec = std::vector<float>;
    using Clock = std::chrono::steady_clock;

    static constexpr size_t kReadQueueCap  = 8;
    static constexpr size_t kWriteQueueCap = 64;
    static constexpr size_t kCacheSize     = 512;

    explicit EmbedQueue(std::shared_ptr<VakYantra> inner)
        : inner_(std::move(inner)) {
        write_workers_count_ = env_size("CHITTA_EMBED_WRITE_WORKERS", 0, 32);
        // Leave one model context available to the query worker where possible.
        const auto contexts = env_size("CHITTA_EMBED_CONTEXTS", 4, 16);
        write_workers_count_ = std::min(write_workers_count_, std::max<size_t>(1, contexts - 1));
        write_cap_ = bounded_writes()
            ? env_size("CHITTA_EMBED_WRITE_DEPTH", kWriteQueueCap, 65536) : kWriteQueueCap;
        admission_wait_ = std::chrono::milliseconds(env_size("CHITTA_EMBED_WRITE_WAIT_MS", 1000, 60000));
        worker_ = std::thread([this]{ run(false); });
        for (size_t i = 0; i < write_workers_count_; ++i)
            write_workers_.emplace_back([this]{ run(true); });
    }

    ~EmbedQueue() {
        { std::lock_guard lk(mu_); stop_ = true; }
        cv_.notify_all();
        space_cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        for (auto& worker : write_workers_) if (worker.joinable()) worker.join();
    }

    // Synchronous cache lookup + async submit for READ path.
    // Returns empty vector if the slot is full or deadline passes.
    Vec query(const std::string& text, std::chrono::milliseconds wait = std::chrono::milliseconds(2000)) {
        auto key = hash(text);

        // 1. Cache hit (synchronous, ~0ms)
        if (auto v = cache_get(key)) return *v;

        // 2. Coalesce with in-flight job if one exists
        std::shared_future<Vec> fut;
        {
            std::lock_guard lk(mu_);
            auto it = inflight_.find(key);
            if (it != inflight_.end()) {
                fut = it->second;
                // A queued document with the same text must not make a query
                // wait behind the document backlog. Promote its existing job,
                // retaining the shared promise and bounded read-lane capacity.
                if (bounded_writes() && read_q_.size() < kReadQueueCap) {
                    auto queued = std::find_if(write_q_.begin(), write_q_.end(),
                        [&](const auto& job) { return job->key == key; });
                    if (queued != write_q_.end()) {
                        read_q_.push_front(*queued);
                        write_q_.erase(queued);
                        space_cv_.notify_all();
                        cv_.notify_all();
                    }
                }
            } else {
                if (read_q_.size() >= kReadQueueCap) return {};
                auto job = std::make_shared<Job>(text, key, Clock::now() + wait, /*write=*/false);
                fut = job->prom.get_future().share();
                inflight_[key] = fut;
                read_q_.push_back(std::move(job));
                cv_.notify_all();
            }
        }

        auto status = fut.wait_for(wait);
        if (status != std::future_status::ready) return {};
        return fut.get();
    }

    // Fire-and-forget enqueue for WRITE path (backfill, learn_codebase, etc.)
    void enqueue_write(const std::string& text) {
        auto key = hash(text);
        if (cache_get(key)) return; // already cached
        std::lock_guard lk(mu_);
        if (inflight_.count(key)) return; // already in-flight
        if (stop_ || write_q_.size() >= write_cap_) return;
        auto job = std::make_shared<Job>(text, key, Clock::time_point::max(), /*write=*/true);
        if (bounded_writes()) inflight_[key] = job->prom.get_future().share();
        write_q_.push_back(std::move(job));
        cv_.notify_all();
    }

    bool bounded_writes() const { return write_workers_count_ != 0; }
    void set_recall_pressure(bool active) { recall_pressure_.store(active); }

    // Actual write embedding: admission waits outside RPC/store locks. Cache
    // warming above stays nonblocking because legacy callers hold those locks.
    Vec write(const std::string& text) {
        if (!bounded_writes()) return query(text, std::chrono::seconds(30));
        const auto key = hash(text);
        if (auto v = cache_get(key)) return *v;
        std::shared_future<Vec> future;
        {
            std::unique_lock lk(mu_);
            if (!space_cv_.wait_for(lk, admission_wait_, [&] {
                    return stop_ || inflight_.count(key) || write_q_.size() < write_cap_;
                })) throw std::runtime_error("embedding write queue admission timed out");
            if (stop_) throw std::runtime_error("embedding queue stopped");
            auto it = inflight_.find(key);
            if (it != inflight_.end()) future = it->second;
            else {
                auto job = std::make_shared<Job>(text, key, Clock::time_point::max(), true);
                future = job->prom.get_future().share();
                inflight_[key] = future;
                write_q_.push_back(std::move(job));
            }
        }
        cv_.notify_all();
        return future.get();
    }

    size_t cache_size() const { std::lock_guard lk(cache_mu_); return cache_map_.size(); }
    size_t inflight_count() { std::lock_guard lk(mu_); return inflight_.size(); }

private:
    struct Job {
        std::string text;
        size_t key;
        Clock::time_point deadline;
        bool write;
        std::promise<Vec> prom;
        Job(std::string t, size_t k, Clock::time_point d, bool w)
            : text(std::move(t)), key(k), deadline(d), write(w) {}
    };

    static size_t env_size(const char* name, size_t fallback, size_t maximum) {
        const char* raw = std::getenv(name);
        if (!raw || !*raw) return fallback;
        char* end = nullptr;
        auto value = std::strtoul(raw, &end, 10);
        if (*end || value > maximum || (value == 0 && fallback != 0)) return fallback;
        return value;
    }

    void run(bool write_worker) {
        while (true) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock lk(mu_);
                auto available = [&] {
                    return write_worker ? !write_q_.empty()
                        : (!read_q_.empty() || (!bounded_writes() && !write_q_.empty()));
                };
                cv_.wait(lk, [&]{ return stop_ || available(); });
                if (!available()) return;
                if (!write_worker && !read_q_.empty()) {
                    job = read_q_.front(); read_q_.pop_front();
                } else {
                    job = write_q_.front(); write_q_.pop_front();
                }
                if (!bounded_writes()) inflight_.erase(job->key);
                space_cv_.notify_all();
            }

            // Skip stale READ jobs; WRITE jobs always run (cache warming)
            if (!job->write && Clock::now() > job->deadline) {
                job->prom.set_value({});
                if (bounded_writes()) { std::lock_guard lk(mu_); inflight_.erase(job->key); }
                space_cv_.notify_all();
                continue;
            }

            // Same bounded recall-pressure gate as queue writes. A dedicated
            // read worker remains available while writes yield (at most 100 ms).
            if (write_worker)
                for (int i = 0; i < 2 && recall_pressure_.load(); ++i) {
                    std::unique_lock lk(mu_);
                    if (cv_.wait_for(lk, std::chrono::milliseconds(50), [&]{ return stop_; })) break;
                }
            Vec vec;
            try {
                Artha a = inner_->transform(job->text);
                vec = (a.certainty > 0.0f) ? a.nu.data : Vec{};
            } catch (...) { vec = {}; }

            if (!vec.empty()) cache_put(job->key, vec);
            job->prom.set_value(std::move(vec));
            if (bounded_writes()) { std::lock_guard lk(mu_); inflight_.erase(job->key); }
            space_cv_.notify_all();
        }
    }

    // LRU cache (mutex-separate from job queue for read concurrency)
    std::optional<Vec> cache_get(size_t key) const {
        std::lock_guard lk(cache_mu_);
        auto it = cache_map_.find(key);
        if (it == cache_map_.end()) return std::nullopt;
        cache_lru_.splice(cache_lru_.begin(), cache_lru_, it->second.lru_it);
        return it->second.vec;
    }

    void cache_put(size_t key, Vec vec) {
        std::lock_guard lk(cache_mu_);
        if (auto old = cache_map_.find(key); bounded_writes() && old != cache_map_.end()) {
            cache_lru_.erase(old->second.lru_it);
            cache_map_.erase(old);
        }
        if (cache_map_.size() >= kCacheSize) {
            cache_map_.erase(cache_lru_.back());
            cache_lru_.pop_back();
        }
        auto lit = cache_lru_.insert(cache_lru_.begin(), key);
        cache_map_[key] = {std::move(vec), lit};
    }

    static size_t hash(const std::string& s) {
        return std::hash<std::string>{}(s);
    }

    std::shared_ptr<VakYantra> inner_;
    std::thread worker_;
    std::vector<std::thread> write_workers_;
    size_t write_workers_count_ = 0;
    size_t write_cap_ = kWriteQueueCap;
    std::chrono::milliseconds admission_wait_{1000};
    std::condition_variable space_cv_;
    std::atomic<bool> recall_pressure_{false};

    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::deque<std::shared_ptr<Job>> read_q_;
    std::deque<std::shared_ptr<Job>> write_q_;
    std::unordered_map<size_t, std::shared_future<Vec>> inflight_;

    mutable std::mutex cache_mu_;
    struct CacheEntry { Vec vec; std::list<size_t>::iterator lru_it; };
    mutable std::unordered_map<size_t, CacheEntry> cache_map_;
    mutable std::list<size_t> cache_lru_;
};

// Routes synchronous document inference (distillation/queue/maintenance) to
// the same bounded workers. Query inference keeps the context pool's read path.
class QueuedWriteYantra : public VakYantra {
public:
    QueuedWriteYantra(std::shared_ptr<VakYantra> inner, EmbedQueue& queue)
        : inner_(std::move(inner)), queue_(queue) {}
    Artha transform(const std::string& text) override {
        auto vec = queue_.write(text);
        Artha result;
        result.certainty = vec.empty() ? 0.0f : 1.0f;
        result.nu.data = std::move(vec);
        result.source = text;
        return result;
    }
    Artha transform(const std::string& text, EmbedMode mode) override {
        return mode == EmbedMode::Query ? inner_->transform(text, mode) : transform(text);
    }
    size_t dimension() const override { return inner_->dimension(); }
    bool ready() const override { return inner_->ready(); }
    std::string execution_provider_name() const override { return inner_->execution_provider_name(); }
private:
    std::shared_ptr<VakYantra> inner_;
    EmbedQueue& queue_;
};

} // namespace chitta
