#include <chitta/rpc/field_handler.hpp>
#include <cassert>
#include <thread>

// This fixture exercises C++ publication without a store, tools, or daemon.
namespace chitta {
void FieldRpcHandler::load_task_ledger() {}
void FieldRpcHandler::register_tools() {}
}

int main() {
    chitta::FieldRpcHandler handler(nullptr, nullptr);
    std::atomic<size_t> first{1}, second{2};
    handler.set_queue_stats(&first, &first, "1");
    std::atomic<bool> go{false};
    std::vector<std::thread> clients;
    for (int i = 0; i < 2; ++i) clients.emplace_back([&, i] {
        while (!go.load()) std::this_thread::yield();
        auto* counter = i ? &second : &first;
        for (int j = 0; j < 10000; ++j)
            handler.set_queue_stats(counter, counter, i ? "2" : "1");
    });
    for (int i = 0; i < 4; ++i) clients.emplace_back([&] {
        while (!go.load()) std::this_thread::yield();
        for (int j = 0; j < 10000; ++j) {
            const auto queue = handler.queue_snapshot();
            assert(queue.count == queue.fail_count);
            assert(queue.failed_path == std::to_string(queue.count->load()));
        }
    });
    go = true;
    for (auto& client : clients) client.join();

    // A notification can replace itself: invoking under the publication mutex
    // would deadlock, and invoking an empty callback would throw.
    int calls = 0;
    handler.set_write_notify_callback([&] {
        ++calls;
        handler.set_write_notify_callback({});
    });
    handler.fire_write_notify();
    handler.fire_write_notify();
    assert(calls == 1);
}
