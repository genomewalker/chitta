#pragma once
// llm_http: Shared GPU endpoint discovery + HTTP LLM call utility
//
// Role/model-aware endpoint pool; declarations plus legacy GPU discovery.
//
// HTTP call: POST /v1/chat/completions (OpenAI-compatible)

#include <string>
#include <vector>
#include <functional>
#include <optional>
#include <nlohmann/json.hpp>
#include <array>
#include <fstream>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <spawn.h>
extern char** environ;
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <chrono>
#include <thread>
#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <cctype>
#include <condition_variable>
#include <future>
#include <memory>

namespace chitta {

// Keep thinking enabled until the frozen ablation satisfies the documented gate.
inline bool distill_think_enabled() {
    const char* value = std::getenv("CHITTA_DISTILL_THINK");
    if (!value) return true;
    const std::string setting(value);
    return setting != "0" && setting != "false" && setting != "off";
}

using LogFn = std::function<void(const std::string&)>;

// Sanitize invalid UTF-8 bytes for safe JSON transport
inline std::string sanitize_utf8(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ) {
        unsigned char c = static_cast<unsigned char>(input[i]);
        if (c < 0x80) {
            if (c < 0x20 && c != '\n' && c != '\r' && c != '\t')
                out += ' ';
            else
                out += static_cast<char>(c);
            ++i;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < input.size() &&
                   (static_cast<unsigned char>(input[i+1]) & 0xC0) == 0x80) {
            out += input[i]; out += input[i+1]; i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < input.size() &&
                   (static_cast<unsigned char>(input[i+1]) & 0xC0) == 0x80 &&
                   (static_cast<unsigned char>(input[i+2]) & 0xC0) == 0x80) {
            out += input[i]; out += input[i+1]; out += input[i+2]; i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < input.size() &&
                   (static_cast<unsigned char>(input[i+1]) & 0xC0) == 0x80 &&
                   (static_cast<unsigned char>(input[i+2]) & 0xC0) == 0x80 &&
                   (static_cast<unsigned char>(input[i+3]) & 0xC0) == 0x80) {
            out += input[i]; out += input[i+1]; out += input[i+2]; out += input[i+3]; i += 4;
        } else {
            out += ' ';
            ++i;
        }
    }
    return out;
}

// Fork/exec a command, capture stdout, return output (with timeout)
inline std::string fork_exec_capture(const std::vector<std::string>& args,
                                      int timeout_secs = 5,
                                      const std::string& stdin_data = "",
                                      int timeout_ms = 0, bool* timed_out = nullptr) {
    if (timed_out) *timed_out = false;
    int stdout_pipe[2];
    if (pipe(stdout_pipe) < 0) return "";

    int stdin_pipe[2] = {-1, -1};
    if (!stdin_data.empty() && pipe(stdin_pipe) < 0) {
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        return "";
    }

    // posix_spawn, never fork(): fork() runs the atfork handlers, and OpenBLAS's
    // joins its worker threads, which hung the daemon for good on 2026-09-19
    // (an endpoint probe under the ledger mutex; 23 ledger_ops queued behind
    // it, the queue processor starved every reader). posix_spawn runs none.
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, stdout_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&actions, stdout_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, stdout_pipe[1]);
    if (!stdin_data.empty()) {
        posix_spawn_file_actions_adddup2(&actions, stdin_pipe[0], STDIN_FILENO);
        posix_spawn_file_actions_addclose(&actions, stdin_pipe[0]);
        posix_spawn_file_actions_addclose(&actions, stdin_pipe[1]);
    }
    pid_t pid = -1;
    const int spawn_rc = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawn_rc != 0) {
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        if (stdin_pipe[0] >= 0) { close(stdin_pipe[0]); close(stdin_pipe[1]); }
        return "";
    }

    close(stdout_pipe[1]);
    if (!stdin_data.empty()) {
        close(stdin_pipe[0]);
        ssize_t written = 0;
        size_t total = stdin_data.size();
        const char* data = stdin_data.data();
        while (written < static_cast<ssize_t>(total)) {
            ssize_t n = write(stdin_pipe[1], data + written, total - written);
            if (n <= 0) break;
            written += n;
        }
        close(stdin_pipe[1]);
    }

    std::string output;
    std::array<char, 4096> buf;
    auto start = std::chrono::steady_clock::now();

    int flags = fcntl(stdout_pipe[0], F_GETFL, 0);
    fcntl(stdout_pipe[0], F_SETFL, flags | O_NONBLOCK);

    bool finished = false;
    while (!finished) {
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() >=
            (timeout_ms > 0 ? timeout_ms : int64_t(timeout_secs) * 1000)) {
            if (timed_out) *timed_out = true;
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            close(stdout_pipe[0]);
            return "";
        }
        int status;
        int result = waitpid(pid, &status, WNOHANG);
        if (result != 0) {
            finished = true;
            if (result > 0 && timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 28)
                *timed_out = true; // curl's deadline expired.
        }

        ssize_t n;
        while ((n = read(stdout_pipe[0], buf.data(), buf.size())) > 0) {
            output.append(buf.data(), n);
            if (std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(
                    timeout_ms > 0 ? timeout_ms : int64_t(timeout_secs) * 1000)) break;
        }

        if (!finished)
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms > 0 ? 5 : 50));
    }
    ssize_t n;
    while ((n = read(stdout_pipe[0], buf.data(), buf.size())) > 0)
        output.append(buf.data(), n);
    close(stdout_pipe[0]);
    return output;
}

struct LlmEndpoint {
    std::string url, kind = "ollama", label;
    bool always_on = false, reachable = false;
    int priority = 100;
    double latency_ms = 0;
    std::vector<std::string> roles, models;
};

inline const std::vector<std::string>& endpoint_roles() {
    static const std::vector<std::string> roles{"teacher", "hint", "embed", "rerank", "student", "judge"};
    return roles;
}

inline std::string endpoint_role_model(const std::string& role,
                                       const std::string& configured = "") {
    // A caller's configured model is authoritative: routing never changes it.
    if (!configured.empty()) return configured;
    std::string key = "CHITTA_ROLE_";
    for (unsigned char c : role) key += static_cast<char>(std::toupper(c));
    key += "_MODEL";
    if (const char* value = std::getenv(key.c_str())) return value;
    return role == "teacher" ? "gemma4:26b" : "";
}

inline int endpoint_ttl_s() {
    const char* value = std::getenv("CHITTA_ENDPOINT_TTL_S");
    if (!value) return 60;
    try { return std::max(0, std::stoi(value)); } catch (...) { return 60; }
}

inline std::vector<LlmEndpoint> declared_endpoints() {
    namespace fs = std::filesystem;
    std::map<std::string, LlmEndpoint> entries;
    auto add = [&](LlmEndpoint ep, bool declaration = false) {
        while (!ep.url.empty() && (std::isspace(static_cast<unsigned char>(ep.url.back())) || ep.url.back() == '/')) ep.url.pop_back();
        if (ep.url.size() >= 3 && ep.url.compare(ep.url.size() - 3, 3, "/v1") == 0) ep.url.resize(ep.url.size() - 3);
        if (ep.url.rfind("http://", 0) != 0 && ep.url.rfind("https://", 0) != 0) return;
        if (ep.label.empty()) ep.label = ep.url;
        if (declaration || !entries.count(ep.url)) entries[ep.url] = std::move(ep);
    };
    auto scan = [&](const fs::path& dir, bool local) {
        std::error_code ec;
        fs::directory_iterator it(dir, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            const auto path = it->path();
            const auto name = path.filename().string();
            if (local && name.rfind("ollama-server-", 0) != 0) continue;
            try {
                std::ifstream in(path);
                LlmEndpoint ep;
                if (!local && path.extension() == ".json") {
                    nlohmann::json j; in >> j;
                    ep.url = j.at("url").get<std::string>();
                    ep.kind = j.value("kind", "ollama");
                    if (ep.kind != "ollama" && ep.kind != "vllm" && ep.kind != "openai") continue;
                    ep.label = j.value("label", "");
                    ep.always_on = j.value("always_on", false);
                    ep.priority = j.value("priority", 100);
                    ep.roles = j.value("roles", std::vector<std::string>{});
                    add(ep, true);
                } else if (path.extension() == ".url") {
                    std::getline(in, ep.url);
                    if (name.find("vllm") != std::string::npos) ep.kind = "vllm";
                    add(ep);
                }
            } catch (...) { /* A malformed declaration must not break discovery. */ }
        }
    };
    // An override permits isolated tests and staging without editing shared home.
    if (const char* dir = std::getenv("CHITTA_ENDPOINT_DIR")) scan(dir, false);
    else {
        scan("/tmp", true);
        if (const char* home = std::getenv("HOME")) scan(fs::path(home) / ".chitta-bridge/endpoints", false);
        LlmEndpoint local; local.url = "http://localhost:11434"; add(local);
        auto jobs = fork_exec_capture({"squeue", "--me", "--noheader", "--format=%j %T %N"}, 5);
        std::istringstream lines(jobs); std::string line;
        while (std::getline(lines, line)) {
            std::istringstream fields(line); std::string name, state, node;
            fields >> name >> state >> node;
            if ((name.find("ollama-") != std::string::npos || name.find("vllm-") != std::string::npos) && state == "RUNNING" && !node.empty()) {
                LlmEndpoint ep; ep.url = "http://" + node + ":11434";
                if (name.find("vllm") != std::string::npos) ep.kind = "vllm";
                add(ep);
            }
        }
    }
    std::vector<LlmEndpoint> out;
    for (auto& entry : entries) out.push_back(std::move(entry.second));
    return out;
}

inline int router_probe_timeout_ms() {
    try { if (const char* v = std::getenv("CHITTA_ROUTER_PROBE_TIMEOUT_MS")) return std::max(1, std::stoi(v)); }
    catch (...) {}
    return 3000;
}

// One deadline covers model discovery and load sampling, including fallbacks.
struct RouterProbe {
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(router_probe_timeout_ms());
    bool timed_out = false;
    std::string fetch(const std::string& url, std::vector<std::string> extra = {}) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0 || timed_out) { timed_out = true; return ""; }
        std::vector<std::string> args{"curl", "-fsS", "--max-time", std::to_string(remaining / 1000.0)};
        args.insert(args.end(), extra.begin(), extra.end()); args.push_back(url);
        return fork_exec_capture(args, 0, "", static_cast<int>(remaining), &timed_out);
    }
};

inline void probe_endpoint_models(LlmEndpoint& ep, RouterProbe& probe) {
    auto previous = ep;
    ep.reachable = false; ep.models.clear();
    const auto start = std::chrono::steady_clock::now();
    auto fetch = [&](const std::string& path, const std::string& array, const std::string& key) {
        auto raw = probe.fetch(ep.url + path);
        try {
            auto j = nlohmann::json::parse(raw);
            if (!j.contains(array) || !j[array].is_array()) return false;
            for (const auto& item : j[array]) {
                if (item.contains(key) && item[key].is_string()) ep.models.push_back(item[key].get<std::string>());
            }
            return true;
        } catch (...) { return false; }
    };
    ep.reachable = fetch("/v1/models", "data", "id");
    if (!ep.reachable && ep.kind == "ollama") ep.reachable = fetch("/api/tags", "models", "name");
    if (probe.timed_out) { ep.reachable = previous.reachable; ep.models = std::move(previous.models); }
    ep.latency_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

inline void probe_endpoint_models(LlmEndpoint& ep) {
    RouterProbe probe; probe_endpoint_models(ep, probe);
}

inline bool probe_endpoint(const std::string& url) {
    LlmEndpoint ep; ep.url = url; probe_endpoint_models(ep); return ep.reachable;
}

inline bool endpoint_matches(const LlmEndpoint& ep, const std::string& role, const std::string& model) {
    return ep.reachable && !model.empty() &&
        (ep.roles.empty() || std::find(ep.roles.begin(), ep.roles.end(), role) != ep.roles.end()) &&
        std::find(ep.models.begin(), ep.models.end(), model) != ep.models.end();
}
inline bool endpoint_preferred(const LlmEndpoint& a, const LlmEndpoint& b) {
    if (a.always_on != b.always_on) return a.always_on;
    if (a.priority != b.priority) return a.priority < b.priority;
    if (a.latency_ms != b.latency_ms) return a.latency_ms < b.latency_ms;
    return a.url < b.url;
}

// Model discovery and load probes run only on this worker. Admission never does
// network I/O while holding the state lock. Reservations cover the entire call.
inline int router_setting(const std::string& key, int fallback, int minimum = 0) {
    try { if (const char* v = std::getenv(key.c_str())) return std::max(minimum, std::stoi(v)); }
    catch (...) {}
    return fallback;
}
inline std::string router_label_key(const std::string& prefix, const std::string& label) {
    std::string key = prefix;
    for (unsigned char c : label) key += std::isalnum(c) ? std::toupper(c) : '_';
    return key;
}
struct EndpointLoad {
    bool ok = false, timed_out = false;
    double latency = 0, running = 0, waiting = 0, gpu = 0;
    std::string mode;
};
inline EndpointLoad probe_endpoint_load(const LlmEndpoint& ep, RouterProbe& probe) {
    EndpointLoad out;
    auto start = std::chrono::steady_clock::now();
    auto fetch = [&](const std::string& path) {
        return probe.fetch(ep.url + path);
    };
    try {
        if (ep.kind == "vllm") {
            auto raw = fetch("/metrics");
            std::istringstream lines(raw); std::string line; bool seen = false;
            while (std::getline(lines, line)) {
                if (line.empty() || line[0] == '#') continue;
                auto split = line.find_first_of("{ ");
                auto name = line.substr(0, split);
                if (name != "vllm:num_requests_running" && name != "vllm:num_requests_waiting" &&
                    name != "vllm:gpu_cache_usage_perc" && name != "vllm:kv_cache_usage_perc") continue;
                auto end = line.find('}');
                auto value = std::stod(line.substr(end == std::string::npos ? split : end + 1));
                if (name == "vllm:num_requests_running") { out.running += value; seen = true; }
                if (name == "vllm:num_requests_waiting") { out.waiting += value; seen = true; }
                if (name == "vllm:gpu_cache_usage_perc" || name == "vllm:kv_cache_usage_perc") out.gpu = std::max(out.gpu, value);
            }
            out.ok = seen; out.mode = "metrics";
        } else if (ep.kind == "ollama") {
            auto ps = nlohmann::json::parse(fetch("/api/ps"));
            if (!ps.contains("models") || !ps["models"].is_array()) return out;
            std::string model; uint64_t size = UINT64_MAX;
            for (const auto& m : ps["models"]) {
                auto bytes = m.value("size", uint64_t(0));
                if (model.empty() || bytes < size) { model = m.value("name", m.value("model", "")); size = bytes; }
            }
            out.mode = model.empty() ? "ps" : "generate:" + model;
            if (!model.empty()) {
                nlohmann::json request = {{"model", model}, {"prompt", "Hi"}, {"stream", false},
                    {"think", false}, {"options", {{"num_predict", 1}, {"temperature", 0}}}};
                auto raw = probe.fetch(ep.url + "/api/generate", {"-H", "Content-Type: application/json",
                    "-d", request.dump()});
                auto response = nlohmann::json::parse(raw);
                out.ok = response.value("done", false) && !response.contains("error");
            } else out.ok = true;
        } else {
            // Generic OpenAI has no standard load metric. Report transport latency.
            auto response = nlohmann::json::parse(fetch("/v1/models"));
            out.ok = response.contains("data"); out.mode = "models";
        }
    } catch (...) {}
    out.timed_out = probe.timed_out;
    out.latency = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return out;
}

inline EndpointLoad probe_endpoint_load(const LlmEndpoint& ep) {
    RouterProbe probe; return probe_endpoint_load(ep, probe);
}

class EndpointRouter {
    using Clock = std::chrono::steady_clock;
    struct State {
        LlmEndpoint ep;
        EndpointLoad load;
        double ewma = 0, baseline = 0, tokens = 0;
        int failed = 0, good = 0, inflight = 0;
        bool busy = false;
        Clock::time_point backoff{}, failed_until{}, budget_at = Clock::now(), models_at{};
    };
    std::mutex mutex_;
    std::condition_variable changed_;
    std::map<std::string, State> states_;
    std::map<std::string, LlmEndpoint> pinned_;
    std::map<std::string, std::string> decisions_, role_models_;
    bool stop_ = false, refresh_ = true, probing_ = false;
    uint64_t generation_ = 0;
    std::thread worker_;

    static int cap(const State& s) {
        return router_setting(router_label_key("CHITTA_ROUTER_MAX_INFLIGHT_", s.ep.label),
            s.ep.always_on ? 4 : router_setting("OLLAMA_NUM_PARALLEL", 1, 1), 1);
    }
    static int budget(const State& s) {
        return router_setting(router_label_key("CHITTA_ROUTER_TPS_BUDGET_", s.ep.label), s.ep.always_on ? 60 : 0);
    }
    static void refill(State& s) {
        auto now = Clock::now(); auto tps = budget(s);
        if (tps) s.tokens = std::min(double(tps), s.tokens + std::chrono::duration<double>(now - s.budget_at).count() * tps);
        s.budget_at = now;
    }
    void run() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stop_) {
            auto started = Clock::now(); bool forced = refresh_; refresh_ = false;
            auto old = states_; auto pinned = pinned_; probing_ = true;
            lock.unlock();
            auto endpoints = declared_endpoints();
            for (auto& p : pinned) if (std::none_of(endpoints.begin(), endpoints.end(), [&](const LlmEndpoint& ep) { return ep.url == p.first; })) endpoints.push_back(p.second);
            std::vector<std::future<std::pair<LlmEndpoint, EndpointLoad>>> probes;
            for (auto ep : endpoints) {
                auto prior = old.find(ep.url);
                bool models_due = forced || prior == old.end() || prior->second.failed > 0 || started - prior->second.models_at >= std::chrono::seconds(endpoint_ttl_s());
                if (prior != old.end()) { ep.models = prior->second.ep.models; ep.reachable = prior->second.ep.reachable; ep.latency_ms = prior->second.ep.latency_ms; }
                probes.push_back(std::async(std::launch::async, [ep, models_due]() mutable {
                    RouterProbe probe;
                    if (models_due) probe_endpoint_models(ep, probe);
                    auto load = probe_endpoint_load(ep, probe);
                    return std::make_pair(ep, load);
                }));
            }
            std::vector<std::pair<LlmEndpoint, EndpointLoad>> results;
            for (auto& probe : probes) results.push_back(probe.get());
            lock.lock();
            std::set<std::string> present;
            for (auto& result : results) {
                auto& ep = result.first; auto& load = result.second; present.insert(ep.url);
                bool fresh = !states_.count(ep.url);
                auto& s = states_[ep.url];
                bool models_due = forced || fresh || started - s.models_at >= std::chrono::seconds(endpoint_ttl_s());
                s.ep = ep;
                if (models_due) s.models_at = started;
                if (fresh) s.tokens = budget(s);
                if (load.timed_out) {
                    s.failed = 0; s.good = 0; s.busy = true;
                } else if (!load.ok || !ep.reachable) {
                    ++s.failed; s.good = 0;
                    if (s.failed >= 3) s.ep.reachable = false;
                    // A timed-out generate is also a saturation signal immediately.
                    s.busy = true;
                } else {
                    s.failed = 0;
                    if (s.load.mode != load.mode || s.baseline == 0) {
                        s.baseline = std::max(1.0, load.latency); s.ewma = load.latency;
                    }
                    s.ewma = .3 * load.latency + .7 * s.ewma;
                    bool busy = load.waiting > 0 || load.gpu >= .95 ||
                        (ep.kind != "vllm" && load.latency > 3 * s.baseline);
                    if (busy) { s.busy = true; s.good = 0; }
                    else {
                        if (++s.good >= 2) s.busy = false;
                        if (!s.busy) s.baseline = std::min(s.baseline, .95 * s.baseline + .05 * load.latency);
                    }
                }
                if (s.busy) s.backoff = Clock::now() + std::chrono::seconds(router_setting("CHITTA_ROUTER_BACKOFF_S", 120));
                s.load = load;
            }
            for (auto it = states_.begin(); it != states_.end();) {
                if (!present.count(it->first) && it->second.inflight == 0) it = states_.erase(it); else ++it;
            }
            probing_ = false; ++generation_; changed_.notify_all();
            changed_.wait_until(lock, started + std::chrono::seconds(router_setting("CHITTA_ROUTER_PROBE_S", 20, 1)),
                [&] { return stop_ || refresh_; });
        }
    }
    void ready(std::unique_lock<std::mutex>& lock, bool force) {
        if (force || !generation_) {
            auto target = generation_ + (force && probing_ ? 2 : 1); refresh_ = true; changed_.notify_all();
            changed_.wait_for(lock, std::chrono::milliseconds(int64_t(router_probe_timeout_ms()) + 100),
                [&] { return stop_ || generation_ >= target; });
        }
    }
    State* select(const std::string& role, const std::string& model, bool batch, const std::string& pinned = "") {
        State* best = nullptr;
        auto rank = [&](State& s) {
            const bool busy = s.busy || s.inflight >= cap(s) || (budget(s) && s.tokens < 0);
            // Interactive uses latency first; batch maximizes available capacity.
            double spare = std::min(1.0 - double(s.inflight) / cap(s),
                std::min(1.0 - s.load.gpu, 1.0 / (1.0 + s.load.running)));
            return std::make_tuple(busy, batch ? -spare : s.ewma, !s.ep.always_on, s.ep.priority, s.ep.latency_ms, s.ep.url);
        };
        for (auto& entry : states_) {
            auto& s = entry.second; refill(s);
            if ((!pinned.empty() && pinned != s.ep.url) || !endpoint_matches(s.ep, role, model) || s.failed >= 3 || s.failed_until > Clock::now()) continue;
            if (s.inflight >= cap(s) || (budget(s) && s.tokens < 0)) continue;
            if (batch && (s.busy || s.backoff > Clock::now())) continue;
            if (!best || rank(s) < rank(*best)) best = &s;
        }
        return best;
    }
public:
    EndpointRouter() : worker_([this] { run(); }) {}
    ~EndpointRouter() {
        { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; changed_.notify_all(); }
        worker_.join();
    }
    std::string choose(const std::string& role, const std::string& model, bool force = false) {
        std::unique_lock<std::mutex> lock(mutex_); ready(lock, force);
        role_models_[role] = model;
        auto* s = select(role, model, role == "teacher" || role == "student");
        return s ? s->ep.url : "";
    }
    std::string acquire(const std::string& role, const std::string& model, bool batch, int tokens,
                        int wait_seconds, const std::string& pinned, LogFn log) {
        std::unique_lock<std::mutex> lock(mutex_); ready(lock, false);
        if (!pinned.empty() && !states_.count(pinned)) {
            LlmEndpoint ep; ep.url = pinned; ep.label = pinned; pinned_[pinned] = ep;
            ready(lock, true);
        }
        role_models_[role] = model;
        auto until = Clock::now() + std::chrono::seconds(wait_seconds);
        bool logged_wait = false;
        for (;;) {
            auto* s = select(role, model, batch, pinned);
            if (s) {
                ++s->inflight; if (budget(*s)) s->tokens -= tokens;
                auto reason = batch ? "most spare capacity" : (s->busy ? "busy fallback" : "fastest idle");
                auto decision = s->ep.url + " (" + reason + ")"; decisions_[role] = decision;
                auto url = s->ep.url; lock.unlock();
                if (log) log("[debug][router] " + role + " -> " + decision);
                return url;
            }
            decisions_[role] = batch ? "waiting: busy/backoff/capacity/token budget or model unavailable" : "unavailable: model/capacity/token budget";
            bool model_available = false;
            for (auto& entry : states_) {
                const auto& ep = entry.second.ep;
                if ((pinned.empty() || pinned == ep.url) && endpoint_matches(ep, role, model)) model_available = true;
            }
            if (!logged_wait && log) {
                auto decision = decisions_[role]; lock.unlock();
                log("[debug][router] " + role + " -> " + decision);
                lock.lock(); logged_wait = true;
            }
            if (!batch || !model_available || stop_ || Clock::now() >= until) return "";
            changed_.wait_until(lock, std::min(until, Clock::now() + std::chrono::seconds(1)));
        }
    }
    void release(const std::string& url, int reserved, int used) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = states_.find(url);
        if (found == states_.end()) return;
        auto& s = found->second; s.inflight = std::max(0, s.inflight - 1);
        refill(s); if (budget(s)) s.tokens = std::min(double(budget(s)), s.tokens + reserved - used);
        changed_.notify_all();
    }
    void failed(const std::string& url) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = states_.find(url);
        if (it != states_.end()) it->second.failed_until = Clock::now() + std::chrono::seconds(5);
        refresh_ = true; changed_.notify_all();
    }
    nlohmann::json report(bool force, const std::string& teacher) {
        std::unique_lock<std::mutex> lock(mutex_); ready(lock, force);
        nlohmann::json models = nlohmann::json::object(), winners = models, rows = nlohmann::json::array();
        for (auto& role : endpoint_roles()) {
            models[role] = endpoint_role_model(role, role == "teacher" && !teacher.empty() ? teacher : (role_models_.count(role) ? role_models_[role] : ""));
            auto* s = select(role, models[role], role == "teacher" || role == "student");
            winners[role] = s ? s->ep.url : "";
        }
        for (auto& entry : states_) {
            auto& s = entry.second; auto& ep = s.ep;
            std::vector<std::string> wins;
            for (auto& role : endpoint_roles()) if (winners[role] == ep.url) wins.push_back(role);
            rows.push_back({{"url", ep.url}, {"kind", ep.kind}, {"label", ep.label}, {"always_on", ep.always_on},
                {"priority", ep.priority}, {"roles", ep.roles}, {"reachable", ep.reachable}, {"models", ep.models},
                {"latency_ms", ep.latency_ms}, {"wins", wins}, {"state", s.failed >= 3 ? "down" : s.busy ? "busy" : "idle"},
                {"probe_latency_ms", s.load.latency}, {"ewma_ms", s.ewma}, {"baseline_ms", s.baseline},
                {"inflight", s.inflight}, {"max_inflight", cap(s)}, {"tps_budget", budget(s)}, {"token_balance", s.tokens},
                {"waiting", s.load.waiting}, {"running", s.load.running}, {"gpu_cache_usage", s.load.gpu}});
        }
        return {{"endpoints", rows}, {"role_models", models}, {"winners", winners}, {"last_decisions", decisions_}};
    }
};
inline EndpointRouter& endpoint_router() { static EndpointRouter router; return router; }
inline void endpoint_failed(const std::string& url) { endpoint_router().failed(url); }
inline std::string discover_gpu_endpoint(const std::string& role, const std::string& model,
                                         LogFn log_fn = nullptr, bool allow_start = true, bool force_probe = false) {
    auto required = endpoint_role_model(role, model);
    if (required.empty()) return "";
    auto choice = endpoint_router().choose(role, required, force_probe);
    // Busy pools must not provision extra jobs. Provision only if no model exists.
    if (choice.empty() && allow_start) {
        auto report = endpoint_router().report(false, ""); bool exists = false;
        for (const auto& ep : report["endpoints"]) for (const auto& m : ep["models"]) if (m == required) exists = true;
        if (!exists) {
            const char* setting = std::getenv("CHITTA_GPU_AUTOSTART");
            const bool autostart = setting && std::string(setting) == "1";
            const std::string decision = "[router] GPU autostart " + std::string(autostart ? "enabled: " : "disabled: ") + required;
            if (log_fn) log_fn(decision); else std::clog << decision << '\n';
            if (autostart) {
                fork_exec_capture({"chitta-gpu", "start", required}, 120);
                choice = endpoint_router().choose(role, required, true);
            }
        }
    }
    if (log_fn) log_fn("[debug][router] discover " + role + " -> " + (choice.empty() ? "unavailable" : choice));
    return choice;
}
inline nlohmann::json endpoint_list(bool force_probe = false, const std::string& teacher_model = "") {
    return endpoint_router().report(force_probe, teacher_model);
}
struct EndpointLease {
    std::string url;
    int reserved, used;
    EndpointLease(const std::string& role, const std::string& model, bool batch, int tokens,
                  int timeout, const std::string& pinned = "", LogFn log = nullptr)
        : reserved(tokens), used(tokens) {
        url = endpoint_router().acquire(role, model, batch, tokens, timeout, pinned, log);
    }
    EndpointLease(const EndpointLease&) = delete;
    ~EndpointLease() { if (!url.empty()) endpoint_router().release(url, reserved, used); }
    void usage(const nlohmann::json& response) {
        if (response.contains("eval_count")) used = response.value("eval_count", reserved);
        else if (response.contains("usage") && response["usage"].is_object()) used = response["usage"].value("completion_tokens", reserved);
    }
};

inline std::string endpoint_table(const nlohmann::json& report) {
    std::ostringstream out;
    out << "LABEL\tURL\tKIND\tALWAYS_ON\tPRIORITY\tREACHABLE\tLATENCY_MS\tSTATE\tPROBE_MS\tEWMA_MS\tBASELINE_MS\tINFLIGHT\tMODELS\tWINS\n";
    auto join = [](const nlohmann::json& values) {
        std::string text;
        for (const auto& value : values) { if (!text.empty()) text += ","; text += value.get<std::string>(); }
        return text.empty() ? "-" : text;
    };
    for (const auto& ep : report.at("endpoints")) {
        out << ep.at("label").get<std::string>() << '\t' << ep.at("url").get<std::string>() << '\t'
            << ep.at("kind").get<std::string>() << '\t' << (ep.at("always_on").get<bool>() ? "yes" : "no") << '\t'
            << ep.at("priority") << '\t' << (ep.at("reachable").get<bool>() ? "yes" : "no") << '\t'
            << static_cast<int>(ep.at("latency_ms").get<double>()) << '\t' << ep.at("state").get<std::string>() << '\t'
            << ep.at("probe_latency_ms") << '\t' << ep.at("ewma_ms") << '\t' << ep.at("baseline_ms") << '\t'
            << ep.at("inflight") << '\t' << join(ep.at("models")) << '\t' << join(ep.at("wins")) << '\n';
    }
    if (report.contains("last_decisions")) for (auto it = report["last_decisions"].begin(); it != report["last_decisions"].end(); ++it)
        out << "decision " << it.key() << ": " << it.value().get<std::string>() << '\n';
    return out.str();
}

inline std::string llm_request_file() {
    char path[] = "/tmp/chitta-llm-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) throw std::runtime_error("cannot create LLM request file");
    close(fd); return path;
}

// Call an OpenAI-compatible LLM via HTTP
// Returns the response content string, or empty on failure
inline std::string call_llm_http(const std::string& endpoint,
                                  const std::string& model,
                                  const std::string& prompt,
                                  const std::string& system_prompt = "",
                                  int timeout_secs = 180,
                                  float temperature = 0.3f,
                                  int max_tokens = 4096,
                                  LogFn log_fn = nullptr,
                                  std::optional<bool> think = std::nullopt,
                                  const std::string& role = "", bool batch = false) {
    std::unique_ptr<EndpointLease> lease;
    std::string selected = endpoint;
    if (!role.empty()) {
        lease = std::make_unique<EndpointLease>(role, model, batch, max_tokens, timeout_secs, endpoint, log_fn);
        selected = lease->url;
        if (selected.empty()) return "";
    }
    auto log = [&](const std::string& msg) {
        if (log_fn) log_fn(msg);
    };

    std::string safe_prompt = sanitize_utf8(prompt);
    nlohmann::json messages = nlohmann::json::array();
    if (!system_prompt.empty()) {
        messages.push_back({{"role", "system"}, {"content", system_prompt}});
    }
    messages.push_back({{"role", "user"}, {"content", safe_prompt}});

    nlohmann::json req = {
        {"model", model},
        {"messages", messages},
        {"temperature", temperature},
        {"max_tokens", max_tokens}
    };
    if (think.has_value()) {
        req = {{"model", model}, {"messages", messages}, {"stream", false},
               {"think", *think},
               {"options", {{"temperature", temperature}, {"num_predict", max_tokens}}}};
    }
    std::string body = req.dump(-1, ' ', true);

    std::string tmp_path = llm_request_file();
    {
        std::ofstream ofs(tmp_path, std::ios::binary);
        ofs << body;
    }

    std::string url = selected + (think.has_value() ? "/api/chat" : "/v1/chat/completions");
    std::string timeout_str = std::to_string(timeout_secs);
    std::string output = fork_exec_capture(
        {"curl", "-sL", "--max-time", timeout_str,
         "-H", "Content-Type: application/json",
         "-d", "@" + tmp_path, url},
        timeout_secs + 5);

    std::remove(tmp_path.c_str());

    try {
        auto resp = nlohmann::json::parse(output);
        if (lease) lease->usage(resp);
        if (think.has_value() && resp.contains("message") &&
            resp["message"].contains("content") && resp["message"]["content"].is_string()) {
            auto content = resp["message"]["content"].get<std::string>();
            if (content.empty()) endpoint_failed(selected);
            return content;
        }
        if (resp.contains("choices") && !resp["choices"].empty()) {
            const auto& msg = resp["choices"][0]["message"];
            if (msg.contains("content")) {
                const auto& content = msg["content"];
                if (content.is_string()) {
                    auto text = content.get<std::string>();
                    if (text.empty()) endpoint_failed(selected);
                    return text;
                }
                // Thinking models (e.g. gemma4) return content as an array of blocks.
                // Concatenate text blocks; skip thinking blocks so SSL parser sees clean output.
                if (content.is_array()) {
                    std::string out;
                    for (const auto& block : content) {
                        if (block.value("type", "") == "text") {
                            if (!out.empty()) out += "\n";
                            out += block.value("text", "");
                        }
                    }
                    if (out.empty()) endpoint_failed(selected);
                    return out;
                }
            }
        }
        if (resp.contains("error")) {
            log("[llm] Error: " + (resp["error"].is_string() ? resp["error"].get<std::string>() : resp["error"].value("message", "unknown")));
        }
    } catch (...) {
        log("[llm] Failed to parse response (" + std::to_string(output.size()) + " bytes)");
    }
    endpoint_failed(selected);
    return "";
}

// ============================================================================
// Tool-calling support for LocalBrain
// ============================================================================

inline std::string url_encode(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

inline std::string strip_html(const std::string& html, size_t max_chars = 6000) {
    std::string out;
    out.reserve(std::min(html.size(), max_chars));
    bool in_tag = false;
    bool in_script = false;
    size_t i = 0;
    while (i < html.size() && out.size() < max_chars) {
        char c = html[i];
        if (!in_tag && !in_script && html.compare(i, 7, "<script") == 0) {
            in_script = true; ++i; continue;
        }
        if (in_script) {
            if (html.compare(i, 9, "</script>") == 0) { in_script = false; i += 9; }
            else ++i;
            continue;
        }
        if (c == '<') { in_tag = true; ++i; continue; }
        if (c == '>') { in_tag = false; out += ' '; ++i; continue; }
        if (!in_tag) {
            if (c == '\n' || c == '\r' || c == '\t') out += ' ';
            else out += c;
        }
        ++i;
    }
    // Collapse whitespace
    std::string result;
    result.reserve(out.size());
    bool prev_space = false;
    for (char c : out) {
        if (c == ' ') { if (!prev_space) result += ' '; prev_space = true; }
        else { result += c; prev_space = false; }
    }
    return result;
}

inline std::string chitta_bin_path() {
    const char* home = getenv("HOME");
    if (home) {
        std::string path = std::string(home) + "/.claude/bin/chitta";
        if (access(path.c_str(), X_OK) == 0) return path;
    }
    return "chitta";
}

struct LocalToolResult {
    std::string tool_call_id;
    std::string content;
};

inline LocalToolResult execute_local_tool(const std::string& id,
                                           const std::string& name,
                                           const nlohmann::json& args) {
    LocalToolResult r;
    r.tool_call_id = id;

    if (name == "web_search") {
        std::string raw_q = args.value("query", "");
        std::string q = url_encode(raw_q);

        // Primary: arXiv API (academic, no bot-blocking, real content)
        std::string arxiv_url = "https://export.arxiv.org/api/query?search_query=all:"
            + q + "&max_results=5&sortBy=submittedDate&sortOrder=descending";
        std::string arxiv_out = fork_exec_capture(
            {"curl", "-sL", "--max-time", "20", arxiv_url}, 25);

        if (!arxiv_out.empty() && arxiv_out.find("<entry>") != std::string::npos) {
            // Parse arXiv Atom feed: extract titles, URLs, abstracts
            std::string result = "arXiv results for: " + raw_q + "\n\n";
            size_t pos = 0;
            int count = 0;
            while (count < 5) {
                auto entry_start = arxiv_out.find("<entry>", pos);
                if (entry_start == std::string::npos) break;
                auto entry_end = arxiv_out.find("</entry>", entry_start);
                if (entry_end == std::string::npos) break;
                std::string entry = arxiv_out.substr(entry_start, entry_end - entry_start);

                // title
                auto t1 = entry.find("<title>"); auto t2 = entry.find("</title>");
                std::string title = (t1 != std::string::npos && t2 != t1)
                    ? entry.substr(t1 + 7, t2 - t1 - 7) : "Unknown";

                // abstract
                auto s1 = entry.find("<summary>"); auto s2 = entry.find("</summary>");
                std::string summary = (s1 != std::string::npos && s2 != s1)
                    ? entry.substr(s1 + 9, s2 - s1 - 9) : "";
                if (summary.size() > 400) summary = summary.substr(0, 400) + "...";

                // URL (abs link)
                auto l1 = entry.find("rel=\"alternate\"");
                std::string url;
                if (l1 != std::string::npos) {
                    auto h1 = entry.rfind("href=\"", l1);
                    if (h1 != std::string::npos) {
                        h1 += 6;
                        auto h2 = entry.find('"', h1);
                        if (h2 != std::string::npos) url = entry.substr(h1, h2 - h1);
                    }
                }

                result += std::to_string(count + 1) + ". " + title + "\n";
                if (!url.empty()) result += "   URL: " + url + "\n";
                if (!summary.empty()) result += "   " + summary + "\n";
                result += "\n";
                pos = entry_end;
                count++;
            }
            r.content = result.size() > 3500 ? result.substr(0, 3500) : result;
        } else {
            // Fallback: web_fetch from a known source
            r.content = "arXiv search returned no results for: " + raw_q;
        }
    } else if (name == "web_fetch") {
        std::string url = args.value("url", "");
        if (url.empty()) { r.content = "Error: url required"; return r; }
        std::string out = fork_exec_capture(
            {"curl", "-sL", "--max-time", "25", "--max-filesize", "131072", url},
            30);
        r.content = out.empty() ? "Fetch failed or empty." : strip_html(out, 4000);
    } else if (name == "chitta_remember") {
        std::string content = args.value("content", "");
        if (content.empty()) { r.content = "Error: content required"; return r; }
        std::vector<std::string> cmd = {chitta_bin_path(), "remember", "--content", content};
        if (args.contains("tags") && args["tags"].is_array()) {
            cmd.push_back("--tags");
            for (const auto& t : args["tags"])
                if (t.is_string()) cmd.push_back(t.get<std::string>());
        }
        fork_exec_capture(cmd, 15);
        r.content = "Stored in memory.";
    } else if (name == "chitta_recall") {
        std::string query = args.value("query", "");
        int limit = args.value("limit", 5);
        std::string out = fork_exec_capture(
            {chitta_bin_path(), "recall", "--query", query,
             "--limit", std::to_string(limit)},
            15);
        r.content = out.empty() ? "No results." : out;
    } else if (name == "write_file") {
        std::string path = args.value("path", "");
        std::string content = args.value("content", "");
        if (path.empty() || content.empty()) {
            r.content = "Error: path and content are required";
            return r;
        }
        std::ofstream ofs(path);
        if (ofs) { ofs << content; r.content = "Written to " + path; }
        else r.content = "Error: could not open " + path + " for writing";
    } else {
        r.content = "Unknown tool: " + name;
    }
    return r;
}

inline nlohmann::json get_dream_tools() {
    return nlohmann::json::array({
        {{"type","function"},{"function",{
            {"name","web_search"},
            {"description","Search the web for information on a topic"},
            {"parameters",{
                {"type","object"},
                {"properties",{{"query",{{"type","string"},{"description","Search query"}}}}},
                {"required",nlohmann::json::array({"query"})}
            }}
        }}},
        {{"type","function"},{"function",{
            {"name","web_fetch"},
            {"description","Fetch content from a URL"},
            {"parameters",{
                {"type","object"},
                {"properties",{{"url",{{"type","string"},{"description","URL to fetch"}}}}},
                {"required",nlohmann::json::array({"url"})}
            }}
        }}},
        {{"type","function"},{"function",{
            {"name","chitta_remember"},
            {"description","Store an insight or finding in persistent memory"},
            {"parameters",{
                {"type","object"},
                {"properties",{
                    {"content",{{"type","string"},{"description","Content to remember"}}},
                    {"tags",{{"type","array"},{"items",{{"type","string"}}},{"description","Optional tags"}}}
                }},
                {"required",nlohmann::json::array({"content"})}
            }}
        }}},
        {{"type","function"},{"function",{
            {"name","chitta_recall"},
            {"description","Search existing memories for relevant context"},
            {"parameters",{
                {"type","object"},
                {"properties",{
                    {"query",{{"type","string"},{"description","Search query"}}},
                    {"limit",{{"type","integer"},{"description","Max results (default 5)"}}}
                }},
                {"required",nlohmann::json::array({"query"})}
            }}
        }}},
        {{"type","function"},{"function",{
            {"name","write_file"},
            {"description","Write content to a file on disk"},
            {"parameters",{
                {"type","object"},
                {"properties",{
                    {"path",{{"type","string"},{"description","Absolute file path"}}},
                    {"content",{{"type","string"},{"description","File content"}}}
                }},
                {"required",nlohmann::json::array({"path","content"})}
            }}
        }}}
    });
}

// Multi-turn tool-calling loop for LocalBrain
// Returns the final assistant text output (after all tool calls complete)
inline std::string call_llm_http_with_tools(const std::string& endpoint,
                                             const std::string& model,
                                             const std::string& prompt,
                                             const std::string& system_prompt,
                                             int max_turns = 20,
                                             int timeout_secs = 300,
                                             LogFn log_fn = nullptr,
                                             const std::string& role = "") {
    auto log = [&](const std::string& msg) { if (log_fn) log_fn(msg); };

    nlohmann::json messages = nlohmann::json::array();
    if (!system_prompt.empty())
        messages.push_back({{"role","system"},{"content",sanitize_utf8(system_prompt)}});
    messages.push_back({{"role","user"},{"content",sanitize_utf8(prompt)}});

    nlohmann::json tools = get_dream_tools();
    std::string last_content;
    int tool_calls_made = 0;

    for (int turn = 0; turn < max_turns; ++turn) {
        std::unique_ptr<EndpointLease> lease;
        std::string selected = endpoint;
        if (!role.empty()) {
            lease = std::make_unique<EndpointLease>(role, model, false, 8192, timeout_secs, endpoint, log_fn);
            selected = lease->url;
            if (selected.empty()) break;
        }
        nlohmann::json req = {
            {"model", model},
            {"messages", messages},
            {"tools", tools},
            {"temperature", 0.4},
            {"max_tokens", 8192}
        };

        std::string tmp = llm_request_file();
        { std::ofstream ofs(tmp, std::ios::binary); ofs << req.dump(-1, ' ', true); }

        std::string url = selected + "/v1/chat/completions";
        std::string raw = fork_exec_capture(
            {"curl", "-sL", "--max-time", std::to_string(timeout_secs),
             "-H", "Content-Type: application/json",
             "-d", "@" + tmp, url},
            timeout_secs + 5);
        std::remove(tmp.c_str());

        if (raw.empty()) { endpoint_failed(selected); log("[llm] Empty response on turn " + std::to_string(turn)); break; }

        nlohmann::json resp;
        try { resp = nlohmann::json::parse(raw); if (lease) lease->usage(resp); } catch (...) { endpoint_failed(selected); break; }

        if (!resp.contains("choices") || resp["choices"].empty()) { endpoint_failed(selected); break; }

        const auto& choice = resp["choices"][0];
        std::string finish = choice.value("finish_reason", "stop");
        const auto& msg = choice["message"];

        // Capture any text content
        if (msg.contains("content") && msg["content"].is_string())
            last_content = msg["content"].get<std::string>();

        // Add assistant message to history
        messages.push_back(msg);

        if (finish != "tool_calls" || !msg.contains("tool_calls") || msg["tool_calls"].empty()) {
            // If we finished but have no text content, ask for a summary
            if (last_content.empty() && messages.size() > 2) {
                messages.push_back({{"role","user"},{"content","Summarize what you explored and discovered. End with: {\"status\": \"achieved\", \"summary\": \"<one line>\"}"}});
                nlohmann::json summary_req = {{"model",model},{"messages",messages},{"temperature",0.3},{"max_tokens",512}};
                std::string tmp2 = llm_request_file();
                { std::ofstream ofs(tmp2, std::ios::binary); ofs << summary_req.dump(-1,' ',true); }
                std::string sraw = fork_exec_capture({"curl","-sL","--max-time","60","-H","Content-Type: application/json","-d","@"+tmp2,url}, 65);
                std::remove(tmp2.c_str());
                try {
                    auto sr = nlohmann::json::parse(sraw);
                    if (lease) lease->used += sr.value("usage", nlohmann::json::object()).value("completion_tokens", 512);
                    if (sr.contains("choices") && !sr["choices"].empty() && sr["choices"][0]["message"].contains("content"))
                        last_content = sr["choices"][0]["message"]["content"].get<std::string>();
                } catch (...) {}
            }
            break;
        }

        lease.reset();
        // Execute each tool call and append results
        for (const auto& tc : msg["tool_calls"]) {
            std::string tc_id   = tc.value("id", "");
            std::string tc_name = tc["function"].value("name", "");
            nlohmann::json tc_args;
            try { tc_args = nlohmann::json::parse(tc["function"].value("arguments", "{}")); }
            catch (...) { tc_args = nlohmann::json::object(); }

            log("[llm] tool_call: " + tc_name);
            ++tool_calls_made;
            auto result = execute_local_tool(tc_id, tc_name, tc_args);

            messages.push_back({
                {"role", "tool"},
                {"tool_call_id", tc_id},
                {"content", sanitize_utf8(result.content)}
            });
        }
    }

    if (last_content.empty() && tool_calls_made > 0)
        last_content = "{\"status\": \"achieved\", \"summary\": \"Completed " +
                       std::to_string(tool_calls_made) + " tool calls\"}";
    if (last_content.empty()) endpoint_failed(endpoint);
    return last_content;
}

} // namespace chitta
