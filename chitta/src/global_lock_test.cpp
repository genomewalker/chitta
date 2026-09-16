#include <chitta/rpc/field_handler.hpp>
#include <cassert>
#include <future>
#include <filesystem>

static std::atomic<int> sync_calls{0};
extern "C" int __wrap_cf_sync(CfHandle*) {
    ++sync_calls;
    return 0;
}

// Exercise the real dispatch and lock factories over a tiny scratch store. Tool
// bodies are deliberately trivial: the policy surrounding them is under test.
namespace chitta {
void FieldRpcHandler::load_task_ledger() {}
void Subconscious::notify_query() {}
void FieldRpcHandler::register_tools() {
    for (const auto* name : {"get", "recall", "remember", "observe", "ledger_op",
                             "log_event", "predicate_run", "distill_set_model"}) {
        handlers_[name] = [](const json&) { return ToolResult::ok("called"); };
    }
}
}

int main(int argc, char** argv) {
    assert(argc == 2);
    using namespace std::chrono_literals;
    using chitta::FieldRpcHandler;
    const bool enabled = std::string(argv[1]) == "1";
    assert(FieldRpcHandler::global_lock_enabled() == enabled);
    // Configuration is published once, never reread midway through a request.
    setenv("CHITTA_GLOBAL_LOCK", enabled ? "0" : "1", 1);
    assert(FieldRpcHandler::global_lock_enabled() == enabled);
    char temporary[] = "/tmp/chitta-global-lock-test-XXXXXX";
    assert(mkdtemp(temporary));
    const std::filesystem::path root(temporary);
    std::filesystem::create_directories(root / "locks");
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() { std::filesystem::remove_all(root); }
    } cleanup{root};
    chitta::FieldStore store((root / "field").string(), (root / "locks").string());
    FieldRpcHandler handler(&store, nullptr);
    auto check = [&](auto work, bool blocks) {
        std::unique_lock held(handler.rpc_mutex());
        auto result = std::async(std::launch::async, work);
        if (blocks) assert(result.wait_for(25ms) == std::future_status::timeout);
        else assert(result.wait_for(2s) == std::future_status::ready);
        held.unlock();
        assert(result.wait_for(2s) == std::future_status::ready);
        result.get();
    };
    check([&] { auto lock = handler.acquire_lock(); assert(lock.owns_lock() == enabled); }, enabled);
    check([&] { auto lock = handler.acquire_shared_lock(); assert(lock.owns_lock() == enabled); }, enabled);
    check([&] { handler.handle({{"method", "tools/list"}, {"id", 1}}); }, enabled);
    for (const auto* name : {"get", "recall", "remember", "observe", "ledger_op",
                             "log_event", "predicate_run", "distill_set_model"}) {
        for (const auto* op : {"counts", "thread_create", "hook_turn", "unknown_hook", "hook_apply",
                               "hook_handoff_prepare", "hook_handoff_context",
                               "hook_task_context", "hook_session_context",
                               "hook_stop_checkpoint", "hook_stop_progress", "hook_post_tool", "hook_pre_compact", "hook_compact_restore", "hook_saddle", "hook_pre_tool", "hook_ancillary", "hook_session_start"}) {
            const auto prior_syncs = sync_calls.load();
            const bool must_sync = std::string(name) == "remember"
                || std::string(name) == "observe" || std::string(name) == "distill_set_model"
                || (std::string(name) == "ledger_op"
                    && (std::string(op) == "thread_create" || std::string(op) == "hook_turn"
                        || std::string(op) == "unknown_hook" || std::string(op) == "hook_apply"));
            bool legacy_bypass = FieldRpcHandler::is_lockfree_read(name)
                || FieldRpcHandler::is_lockfree_write(name)
                || FieldRpcHandler::is_subprocess_tool(name);
            check([&] {
                auto response = handler.handle({{"method", "tools/call"}, {"id", 1},
                    {"params", {{"name", name}, {"arguments", {{"op", op}}}}}});
                assert(response.contains("result"));
            }, enabled && !legacy_bypass);
            assert(sync_calls.load() == prior_syncs + (must_sync ? 1 : 0));
        }
    }
}
