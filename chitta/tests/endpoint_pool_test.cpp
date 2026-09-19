// Real loopback HTTP servers exercise background probes and admission together.
#include <chitta/llm_http.hpp>
#include <cassert>
#include <atomic>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>

struct Server {
    int fd;
    std::string url;
    std::atomic<bool> stop{false}, down{false}, malformed{false}, tags_only{false};
    std::atomic<int> delay_ms{0}, probes{0}, generates{0}, waiting{0}, metrics_count{0};
    std::vector<std::string> models;
    std::thread worker;
    explicit Server(std::vector<std::string> names) : models(std::move(names)) {
        fd = socket(AF_INET, SOCK_STREAM, 0); assert(fd >= 0);
        sockaddr_in address{}; address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        assert(bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        socklen_t len = sizeof(address);
        assert(getsockname(fd, reinterpret_cast<sockaddr*>(&address), &len) == 0);
        assert(listen(fd, 8) == 0);
        url = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
        worker = std::thread([this] {
            while (!stop) {
                pollfd ready{fd, POLLIN, 0};
                if (poll(&ready, 1, 50) <= 0) continue;
                int client = accept(fd, nullptr, nullptr); if (client < 0) continue;
                char request[8192]{};
                auto count = recv(client, request, sizeof(request)-1, 0);
                const std::string text(request, count > 0 ? count : 0);
                ++probes;
                bool tags = text.find("/api/tags") != std::string::npos;
                bool ps = text.find("/api/ps") != std::string::npos;
                bool generate = text.find("/api/generate") != std::string::npos;
                bool metrics = text.find("/metrics") != std::string::npos;
                bool chat = text.find("/v1/chat/completions") != std::string::npos;
                if (metrics) ++metrics_count;
                if (generate || metrics) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms.load()));
                bool ok = !down && !(tags_only && text.find("/v1/models") != std::string::npos);
                nlohmann::json data = nlohmann::json::array();
                for (const auto& model : models) data.push_back({{tags || ps ? "name" : "id", model}, {"size", 100}});
                std::string body = nlohmann::json{{tags || ps ? "models" : "data", data}}.dump();
                if (generate) { ++generates; body = R"({"done":true,"response":"x","eval_count":1})"; }
                if (chat) body = R"({"choices":[{"message":{"content":"ok"}}],"usage":{"completion_tokens":2}})";
                if (metrics) body = "unrelated_metric{label=\"text with spaces\"} 1\n# HELP ignored\nvllm:num_requests_running{model=\"shared\"} 0\nvllm:num_requests_waiting{model=\"shared\"} " + std::to_string(waiting.load()) + "\nvllm:gpu_cache_usage_perc 0.2\n";
                if (malformed) body = "invalid JSON";
                std::string response = std::string("HTTP/1.1 ") + (ok ? "200 OK" : "503 Unavailable") +
                    "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
                    "\r\nConnection: close\r\n\r\n" + body;
                send(client, response.data(), response.size(), MSG_NOSIGNAL); close(client);
            }
        });
    }
    ~Server() { stop = true; worker.join(); close(fd); }
};

int main() {
    Server slurm({"teacher-model", "shared"}), rtx({"student-model", "shared"});
    std::string pattern = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/p15-pool-XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end()); name.push_back(0);
    auto temp = mkdtemp(name.data()); assert(temp); std::filesystem::path dir(temp);
    setenv("CHITTA_ENDPOINT_DIR", temp, 1);
    setenv("CHITTA_ENDPOINT_TTL_S", "60", 1);
    setenv("CHITTA_ROUTER_PROBE_S", "1", 1);
    setenv("CHITTA_ROUTER_BACKOFF_S", "1", 1);
    setenv("CHITTA_ROUTER_MAX_INFLIGHT_RTX", "2", 1);
    setenv("CHITTA_ROUTER_TPS_BUDGET_RTX", "0", 1);
    setenv("CHITTA_ROLE_STUDENT_MODEL", "student-model", 1);
    auto declare = [&](bool always, int priority, const std::string& kind = "ollama", std::vector<std::string> roles = {}) {
        std::ofstream(dir / "slurm.url") << slurm.url << "/v1/\n";
        std::ofstream(dir / "rtx.json") << nlohmann::json{{"url",rtx.url}, {"kind",kind},
            {"label","rtx"}, {"always_on",always}, {"priority",priority}, {"roles",roles}};
    };
    declare(true, 999);
    std::ofstream(dir / "rtx.url") << rtx.url << "/v1\n";
    assert(chitta::declared_endpoints().size() == 2);
    auto choose = [&](const std::string& role, const std::string& model) { return chitta::discover_gpu_endpoint(role, model, nullptr, false); };
    auto refresh = [&] { return chitta::endpoint_list(true, "teacher-model"); };
    auto row = [&](const nlohmann::json& report, const std::string& url) {
        for (const auto& ep : report["endpoints"]) if (ep["url"] == url) return ep;
        assert(false); return nlohmann::json();
    };
    auto report = refresh();
    assert(choose("teacher", "teacher-model") == slurm.url);
    assert(choose("student", "student-model") == rtx.url);
    assert(choose("teacher", "shared") == rtx.url); // Batch ties prefer always-on.
    assert(choose("student", "absent").empty());
    assert(choose("teacher", "teacher").empty());
    declare(false, 10); refresh();
    assert(choose("teacher", "shared") == rtx.url); // Priority breaks equal capacity.
    declare(true, 10, "ollama", {"student"}); refresh();
    assert(choose("teacher", "shared") == slurm.url);
    declare(true, 10); refresh();
    int before = rtx.probes + slurm.probes;
    assert(choose("student", "student-model") == rtx.url);
    assert(rtx.probes + slurm.probes == before); // Admission has no synchronous probes.
    assert(rtx.generates > 0); // Loaded Ollama model is probed with one token.
    {
        chitta::EndpointLease a("student", "student-model", true, 1, 0);
        chitta::EndpointLease b("student", "student-model", true, 1, 0);
        chitta::EndpointLease c("student", "student-model", true, 1, 0);
        assert(a.url == rtx.url && b.url == rtx.url && c.url.empty());
        assert(row(chitta::endpoint_list(), rtx.url)["inflight"] == 2);
    }
    // Busy signal must re-route an interactive request within a probe interval
    // plus the bounded HTTP sample. No request triggers a refresh here.
    rtx.delay_ms = 350;
    auto start = std::chrono::steady_clock::now(); bool busy = false;
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1800)) {
        if (row(chitta::endpoint_list(), rtx.url)["state"] == "busy") { busy = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    assert(busy);
    assert(choose("hint", "shared") == slurm.url);
    chitta::EndpointLease blocked("student", "student-model", true, 1, 0);
    assert(blocked.url.empty());
    // A waiting batch call stays blocked until two good samples and backoff expire.
    auto pending = std::async(std::launch::async, [&] {
        chitta::EndpointLease lease("student", "student-model", true, 1, 6);
        return lease.url;
    });
    assert(pending.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout);
    rtx.delay_ms = 0;
    assert(pending.get() == rtx.url);
    assert(row(chitta::endpoint_list(), rtx.url)["state"] == "idle");
    // Token debt rejects further admission, then actual usage refunds the reserve.
    setenv("CHITTA_ROUTER_TPS_BUDGET_RTX", "1", 1);
    {
        chitta::EndpointLease a("student", "student-model", true, 100, 0);
        chitta::EndpointLease b("student", "student-model", true, 1, 0);
        assert(a.url == rtx.url && b.url.empty()); a.used = 0;
    }
    setenv("CHITTA_ROUTER_TPS_BUDGET_RTX", "0", 1);
    assert(chitta::call_llm_http("", "student-model", "test", "", 2, .3f, 4, nullptr, std::nullopt, "student", true) == "ok");
    assert(row(chitta::endpoint_list(), rtx.url)["inflight"] == 0);
    chitta::endpoint_failed(rtx.url);
    assert(choose("hint", "shared") == slurm.url);
    rtx.down = true;
    refresh(); refresh(); report = refresh();
    assert(row(report, rtx.url)["state"] == "down");
    assert(choose("student", "student-model").empty());
    rtx.down = false; rtx.tags_only = true; refresh(); report = refresh();
    assert(row(report, rtx.url)["reachable"] == true); // /api/tags fallback.
    declare(true, 10, "vllm"); rtx.tags_only = false; rtx.waiting = 1;
    report = refresh(); assert(row(report, rtx.url)["state"] == "busy");
    rtx.waiting = 0; report = refresh(); assert(row(report, rtx.url)["state"] == "busy");
    report = refresh(); assert(row(report, rtx.url)["state"] == "idle");
    // Saturated vLLM must only use metrics, and all endpoints share a parallel deadline.
    int generations = rtx.generates;
    std::ofstream(dir / "slurm.json") << nlohmann::json{{"url",slurm.url}, {"kind","vllm"}, {"label","slurm"}};
    refresh();
    setenv("CHITTA_ROUTER_PROBE_TIMEOUT_MS", "250", 1);
    rtx.delay_ms = 1200; slurm.delay_ms = 1200;
    int metrics_before = rtx.metrics_count;
    auto stalled = std::async(std::launch::async, refresh);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (rtx.metrics_count == metrics_before && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    assert(rtx.metrics_count > metrics_before);
    auto inventory_start = std::chrono::steady_clock::now();
    chitta::endpoint_list();
    auto inventory_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - inventory_start).count();
    assert(inventory_ms < 100); // The probe cannot own the inventory mutex.
    report = stalled.get();
    for (int i = 0; i < 4; ++i) {
        auto probe_start = std::chrono::steady_clock::now();
        report = refresh();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - probe_start).count();
        assert(elapsed < 450); // Sequential probing would take at least 500 ms.
        assert(row(report, rtx.url)["state"] == "busy");
        assert(row(report, slurm.url)["state"] == "busy");
        std::cout << "stalled pair probe_ms=" << elapsed << " inventory_ms=" << inventory_ms << "\n";
    }
    assert(rtx.generates == generations);
    rtx.delay_ms = 0; slurm.delay_ms = 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    setenv("CHITTA_ROUTER_PROBE_TIMEOUT_MS", "3000", 1);
    refresh(); refresh();
    // Discovery can execute only this private stub, and only on exact opt-in.
    auto stub = dir / "chitta-gpu", marker = dir / "started";
    std::ofstream(stub) << "#!/bin/sh\nprintf started > '" << marker.string() << "'\n";
    std::filesystem::permissions(stub, std::filesystem::perms::owner_all);
    std::string path = dir.string() + ":" + std::getenv("PATH");
    setenv("PATH", path.c_str(), 1);
    unsetenv("CHITTA_GPU_AUTOSTART");
    chitta::discover_gpu_endpoint("teacher", "absent", nullptr, true);
    assert(!std::filesystem::exists(marker));
    setenv("CHITTA_GPU_AUTOSTART", "true", 1);
    chitta::discover_gpu_endpoint("teacher", "absent", nullptr, true);
    assert(!std::filesystem::exists(marker));
    setenv("CHITTA_GPU_AUTOSTART", "1", 1);
    chitta::discover_gpu_endpoint("teacher", "absent", nullptr, true);
    assert(std::filesystem::exists(marker));
    unsetenv("CHITTA_GPU_AUTOSTART");
    rtx.malformed = true; refresh(); refresh(); report = refresh();
    assert(row(report, rtx.url)["state"] == "down");
    std::cout << "endpoint_pool_test PASS: model/role, capacity/priority, background reroute, batch resume, cap, budget, hysteresis, failure, tags, metrics\n";
    // This directory was created by this test; router no longer needs declarations.
    std::filesystem::remove_all(dir);
}
