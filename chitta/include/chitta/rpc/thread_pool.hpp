#pragma once
// Thread Pool: Async RPC request handling for daemon
//
// Allows main thread to stay responsive (poll/accept/write) while
// slow operations (semantic search, embedding) run on worker threads.
//
// Features:
// - Configurable worker count
// - Request tracing with timing
// - Watchdog for slow request detection with escalation callback
// - Thread-safe response queue integration

#include <thread>
#include <queue>
#include <mutex>
#include <optional>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <set>
#include <vector>
#include <string>
#include <iostream>
#include <nlohmann/json.hpp>

namespace chitta {

// Callback for watchdog escalation (method, duration_seconds)
using WatchdogCallback = std::function<void(const std::string&, int64_t)>;

struct RequestTrace {
    uint64_t id;
    std::string method;
    std::chrono::steady_clock::time_point start;
};

// Lock ordering: watchdog_mutex_ and queue_mutex_ must always be acquired
// before trace_mutex_ to prevent deadlocks. All code paths follow this order:
//   watchdog_loop(): locks watchdog_mutex_ first, then trace_mutex_
//   submit(): locks queue_mutex_ first, then trace_mutex_ (nested)
//   get_active()/active_count(): only locks trace_mutex_
// Never acquire watchdog_mutex_ or queue_mutex_ while holding trace_mutex_.
class ThreadPool {
public:
    explicit ThreadPool(size_t min_threads = 2, size_t max_threads = 16, size_t max_queue_depth = 256)
        : stop_(false), min_workers_(min_threads), max_workers_(max_threads), max_queue_depth_(max_queue_depth) {
        for (size_t i = 0; i < min_threads; ++i) {
            spawn_worker();
        }
        watchdog_ = std::thread([this] { watchdog_loop(); });
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            stop_ = true;
        }
        condition_.notify_all();
        watchdog_cv_.notify_all();

        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
        if (watchdog_.joinable()) watchdog_.join();
    }

    // Non-copyable, non-movable
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    // Submit work, returns request ID for tracking, or nullopt when the queue
    // is at max_queue_depth_. On rejection neither task nor on_complete is
    // invoked — the caller is responsible for answering the client.
    std::optional<uint64_t> submit(int client_fd, const std::string& method,
                    std::function<std::string()> task,
                    std::function<void(int, std::string)> on_complete) {
        uint64_t id = next_id_.fetch_add(1, std::memory_order_relaxed);

        size_t queue_size;
        size_t worker_count;
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            if (tasks_.size() >= max_queue_depth_) {
                return std::nullopt;
            }
            {
                std::lock_guard<std::mutex> tlock(trace_mutex_);
                active_[id] = {id, method, std::chrono::steady_clock::now()};
            }
            tasks_.push({id, client_fd, std::move(task), std::move(on_complete)});
            pending_tasks_.fetch_add(1, std::memory_order_relaxed);
            queue_size = tasks_.size();
            worker_count = num_workers_.load();
        }
        condition_.notify_one();

        // Scale up if queue is growing (more pending than workers)
        if (queue_size > worker_count && worker_count < max_workers_) {
            spawn_worker();
        }

        return id;
    }

    // Get active requests (for diagnostics)
    std::vector<std::pair<std::string, int64_t>> get_active() const {
        std::lock_guard<std::mutex> lock(trace_mutex_);
        std::vector<std::pair<std::string, int64_t>> result;
        auto now = std::chrono::steady_clock::now();
        for (const auto& [id, trace] : active_) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - trace.start).count();
            result.push_back({trace.method, ms});
        }
        return result;
    }

    // Stats
    size_t pending() const { return pending_tasks_.load(std::memory_order_relaxed); }

    size_t active_count() const {
        std::lock_guard<std::mutex> lock(trace_mutex_);
        return active_.size();
    }

    size_t worker_count() const {
        return num_workers_.load();
    }

    size_t max_workers() const {
        return max_workers_;
    }

    const std::atomic<size_t>& pending_counter() const { return pending_tasks_; }
    const std::atomic<size_t>& active_counter() const { return active_tasks_; }

    // Set callback for watchdog escalation (called when operation exceeds critical threshold)
    void set_watchdog_callback(WatchdogCallback cb) {
        std::lock_guard<std::mutex> lock(watchdog_mutex_);
        watchdog_callback_ = std::move(cb);
    }

    // Set escalation threshold (default: 60 seconds)
    void set_escalation_threshold(std::chrono::seconds threshold) {
        escalation_threshold_ = threshold;
    }

private:
    void spawn_worker() {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        if (num_workers_.load() >= max_workers_) return;

        workers_.emplace_back([this] { worker_loop(); });
        num_workers_.fetch_add(1);
    }
    struct Task {
        uint64_t id;
        int client_fd;
        std::function<std::string()> work;
        std::function<void(int, std::string)> on_complete;
    };

    void worker_loop() {
        while (true) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                condition_.wait(lock, [this] {
                    return stop_ || !tasks_.empty();
                });
                if (stop_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
                pending_tasks_.fetch_sub(1, std::memory_order_relaxed);
                active_tasks_.fetch_add(1, std::memory_order_relaxed);
            }

            // Execute work
            std::string result;
            try {
                result = task.work();
            } catch (const std::exception& e) {
                // Properly escape error message to prevent JSON injection
                nlohmann::json error_response = {
                    {"jsonrpc", "2.0"},
                    {"error", {{"code", -32603}, {"message", e.what()}}},
                    {"id", nullptr}
                };
                result = error_response.dump();
            }

            // Mark completed and remove from active
            {
                std::lock_guard<std::mutex> lock(trace_mutex_);
                active_.erase(task.id);
            }
            active_tasks_.fetch_sub(1, std::memory_order_relaxed);

            // Deliver result
            if (task.on_complete) {
                task.on_complete(task.client_fd, std::move(result));
            }
        }
    }

    void watchdog_loop() {
        while (true) {
            WatchdogCallback callback;
            {
                std::unique_lock<std::mutex> lock(watchdog_mutex_);
                watchdog_cv_.wait_for(lock, std::chrono::seconds(5), [this] {
                    return stop_.load();
                });
                if (stop_) return;
                callback = watchdog_callback_;  // Copy callback under lock
            }

            std::lock_guard<std::mutex> lock(trace_mutex_);
            auto now = std::chrono::steady_clock::now();
            auto threshold_secs = escalation_threshold_.count();

            for (const auto& [id, trace] : active_) {
                auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                    now - trace.start).count();

                // Critical threshold - escalate if callback is set
                if (secs > threshold_secs && callback) {
                    // Only escalate once per request
                    if (escalated_.find(id) == escalated_.end()) {
                        escalated_.insert(id);
                        std::cerr << "[watchdog] CRITICAL: " << trace.method
                                  << " stuck for " << secs << "s - escalating\n";
                        callback(trace.method, secs);
                    }
                } else if (secs > 10) {
                    std::cerr << "[watchdog] SLOW: " << trace.method
                              << " running for " << secs << "s\n";
                }
            }

            // Clean up escalated set for completed requests
            for (auto it = escalated_.begin(); it != escalated_.end(); ) {
                if (active_.find(*it) == active_.end()) {
                    it = escalated_.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    std::vector<std::thread> workers_;
    std::thread watchdog_;
    std::queue<Task> tasks_;
    mutable std::mutex queue_mutex_;
    mutable std::mutex workers_mutex_;
    std::condition_variable condition_;
    std::atomic<bool> stop_;
    std::atomic<uint64_t> next_id_{1};
    std::atomic<size_t> num_workers_{0};
    std::atomic<size_t> pending_tasks_{0};
    std::atomic<size_t> active_tasks_{0};
    size_t min_workers_;
    size_t max_workers_;
    size_t max_queue_depth_;

    mutable std::mutex trace_mutex_;
    std::unordered_map<uint64_t, RequestTrace> active_;

    std::mutex watchdog_mutex_;
    std::condition_variable watchdog_cv_;
    WatchdogCallback watchdog_callback_;
    std::chrono::seconds escalation_threshold_{60};  // Default 60s
    std::set<uint64_t> escalated_;  // Requests already escalated (prevent spam)
};

} // namespace chitta
