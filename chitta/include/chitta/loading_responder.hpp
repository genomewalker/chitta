#pragma once
#include <chitta/socket_server.hpp>
#include <nlohmann/json.hpp>
#include <future>
#include <thread>

namespace chitta {
// One thread owns bind, polling, the steady-state dispatcher and socket close.
// Initialization and joining happen on the caller, never on the I/O thread.
class LoadingResponder {
    SocketServer& server_;
    std::atomic<bool> stopping_{false};
    std::atomic<const char*> phase_{"field_store"};
    std::promise<std::function<void()>> dispatch_;
    std::promise<void> finished_;
    std::thread thread_;
public:
    explicit LoadingResponder(SocketServer& server) : server_(server) {}
    bool start() {
        std::promise<bool> bound;
        auto ready = bound.get_future();
        thread_ = std::thread([this, bound = std::move(bound)]() mutable {
            try {
                if (!server_.start()) { bound.set_value(false); return; }
                bound.set_value(true);
                auto dispatch = dispatch_.get_future();
                while (!stopping_.load()) {
                    if (dispatch.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                        dispatch.get()();
                        break;
                    }
                    for (const auto& req : server_.poll(50)) {
                        using json = nlohmann::json;
                        auto parsed = json::parse(req.data, nullptr, false);
                        json id = nullptr;
                        std::string method = req.data;
                        if (parsed.is_object()) {
                            id = parsed.value("id", json(nullptr));
                            method = parsed.contains("method") && parsed["method"].is_string()
                                ? parsed["method"].get<std::string>() : "";
                            if (method == "tools/call" && parsed.contains("params") && parsed["params"].is_object())
                                method = parsed["params"].contains("name") && parsed["params"]["name"].is_string()
                                    ? parsed["params"]["name"].get<std::string>() : "";
                        }
                        const bool health = method == "health_check" || method == "status";
                        json state = {{"status", "warming_up"}, {"loading", true},
                            {"phase", phase_.load()}, {"replayed_records", nullptr},
                            {"total", nullptr}, {"eta_s", nullptr}};
                        if (!health) { state["error"] = "loading"; state["retry_after_s"] = 1; }
                        json result = {{"text", "daemon loading, please retry"},
                            {"content", json::array({{{"type", "text"}, {"text", "daemon loading, please retry"}}})},
                            {"structured", state}};
                        if (!health) result["isError"] = true;
                        server_.respond(req.client_fd, json({{"jsonrpc", "2.0"},
                            {"id", id}, {"result", result}}).dump());
                    }
                }
                server_.stop();
                finished_.set_value();
            } catch (...) {
                try { bound.set_exception(std::current_exception()); } catch (...) {}
                server_.stop();
                finished_.set_exception(std::current_exception());
            }
        });
        return ready.get();
    }
    void phase(const char* value) { phase_.store(value); }
    void serve(std::function<void()> dispatch) {
        dispatch_.set_value(std::move(dispatch));
        thread_.join();
        finished_.get_future().get();
    }
    ~LoadingResponder() {
        stopping_.store(true);
        if (thread_.joinable()) thread_.join();
    }
};
} // namespace chitta
