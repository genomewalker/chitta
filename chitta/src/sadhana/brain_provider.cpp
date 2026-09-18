// BrainProvider: LLM interface implementations
//
// Uses fork/exec to run CLI tools with timeout handling.

#include <chitta/sadhana/brain_provider.hpp>
#include <nlohmann/json.hpp>
#include <iostream>
#include <sstream>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <chrono>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>

namespace chitta {

namespace {

// Execute a command with timeout, capturing stdout/stderr
BrainResult execute_with_timeout(
    const std::vector<std::string>& args,
    const std::string& stdin_data,
    int timeout_ms,
    const std::string& working_dir = "")
{
    BrainResult result;
    auto start_time = std::chrono::steady_clock::now();

    // Create pipes for stdin, stdout, stderr
    int stdin_pipe[2], stdout_pipe[2], stderr_pipe[2];
    if (pipe(stdin_pipe) < 0 || pipe(stdout_pipe) < 0 || pipe(stderr_pipe) < 0) {
        result.error = "Failed to create pipes: " + std::string(strerror(errno));
        return result;
    }

    pid_t pid = fork();
    if (pid < 0) {
        result.error = "Fork failed: " + std::string(strerror(errno));
        close(stdin_pipe[0]); close(stdin_pipe[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        return result;
    }

    if (pid == 0) {
        // Child process
        close(stdin_pipe[1]);  // Close write end of stdin
        close(stdout_pipe[0]); // Close read end of stdout
        close(stderr_pipe[0]); // Close read end of stderr

        // Redirect stdin/stdout/stderr
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);

        close(stdin_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        // Change directory if specified
        if (!working_dir.empty()) {
            if (chdir(working_dir.c_str()) < 0) {
                std::cerr << "chdir failed: " << strerror(errno) << "\n";
                _exit(1);
            }
        }

        // Unset CLAUDECODE to allow nested Claude Code sessions
        unsetenv("CLAUDECODE");

        // Build argv
        std::vector<char*> argv;
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        // Execute
        execvp(argv[0], argv.data());
        std::cerr << "execvp failed: " << strerror(errno) << "\n";
        _exit(1);
    }

    // Parent process
    close(stdin_pipe[0]);  // Close read end of stdin
    close(stdout_pipe[1]); // Close write end of stdout
    close(stderr_pipe[1]); // Close write end of stderr

    // Write stdin data
    if (!stdin_data.empty()) {
        ssize_t written = write(stdin_pipe[1], stdin_data.c_str(), stdin_data.size());
        if (written < 0) {
            std::cerr << "[brain] stdin write error: " << strerror(errno) << "\n";
        }
    }
    close(stdin_pipe[1]);  // Signal EOF

    // Set non-blocking on stdout/stderr
    fcntl(stdout_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(stderr_pipe[0], F_SETFL, O_NONBLOCK);

    // Read output with timeout
    std::string stdout_data, stderr_data;
    char buffer[4096];
    bool timed_out = false;

    struct pollfd fds[2] = {
        {stdout_pipe[0], POLLIN, 0},
        {stderr_pipe[0], POLLIN, 0}
    };

    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time).count();

        if (elapsed >= timeout_ms) {
            timed_out = true;
            break;
        }

        int remaining = timeout_ms - static_cast<int>(elapsed);
        int ret = poll(fds, 2, remaining);

        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (ret == 0) {
            timed_out = true;
            break;
        }

        // Read stdout
        if (fds[0].revents & POLLIN) {
            ssize_t n = read(stdout_pipe[0], buffer, sizeof(buffer) - 1);
            if (n > 0) {
                buffer[n] = '\0';
                stdout_data += buffer;
            }
        }

        // Read stderr
        if (fds[1].revents & POLLIN) {
            ssize_t n = read(stderr_pipe[0], buffer, sizeof(buffer) - 1);
            if (n > 0) {
                buffer[n] = '\0';
                stderr_data += buffer;
            }
        }

        // Check if both pipes closed
        if ((fds[0].revents & POLLHUP) && (fds[1].revents & POLLHUP)) {
            // Drain remaining data
            while (true) {
                ssize_t n = read(stdout_pipe[0], buffer, sizeof(buffer) - 1);
                if (n <= 0) break;
                buffer[n] = '\0';
                stdout_data += buffer;
            }
            while (true) {
                ssize_t n = read(stderr_pipe[0], buffer, sizeof(buffer) - 1);
                if (n <= 0) break;
                buffer[n] = '\0';
                stderr_data += buffer;
            }
            break;
        }
    }

    close(stdout_pipe[0]);
    close(stderr_pipe[0]);

    // Handle process termination
    int status = 0;
    if (timed_out) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        result.error = "Timeout after " + std::to_string(timeout_ms) + "ms";
        result.exit_code = -1;
    } else {
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            result.exit_code = WEXITSTATUS(status);
            result.success = (result.exit_code == 0 ||
                              result.exit_code == 10 ||   // achieved
                              result.exit_code == 20);    // blocked (not an error)
        } else if (WIFSIGNALED(status)) {
            result.exit_code = -WTERMSIG(status);
            result.error = "Killed by signal " + std::to_string(WTERMSIG(status));
        }
    }

    result.output = stdout_data;
    if (!stderr_data.empty() && result.error.empty()) {
        result.error = stderr_data;
    }

    auto end_time = std::chrono::steady_clock::now();
    result.duration_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count()
    );

    return result;
}

}  // anonymous namespace

// ClaudeBrain implementation
// Runs: claude --allowedTools Bash,Edit,Write,Read,Glob,Grep --model <model> --max-turns N [--system <sys>] -p <prompt>
BrainResult ClaudeBrain::think(const std::string& prompt, const BrainConfig& config) {
    int turns = config.max_turns > 0 ? config.max_turns : 20;

    std::vector<std::string> args = {
        claude_path_,
        "--allowedTools", "Bash,Edit,Write,Read,Glob,Grep",
        "--model", model_,
        "--max-turns", std::to_string(turns),
        "--output-format", "json"
    };

    if (!config.system_prompt.empty()) {
        args.push_back("--system-prompt");
        args.push_back(config.system_prompt);
    }

    for (const auto& arg : config.extra_args) {
        args.push_back(arg);
    }

    // -p triggers non-interactive print mode with full tool access
    args.push_back("-p");
    args.push_back(prompt);

    auto result = execute_with_timeout(args, "", config.timeout_ms, config.working_dir);

    // Parse the JSON wrapper from --output-format json to extract cost/turns,
    // then replace output with the inner result text for downstream parsing.
    if (!result.output.empty()) {
        try {
            auto j = nlohmann::json::parse(result.output);
            result.cost_usd  = j.value("cost_usd",  0.0);
            result.num_turns = j.value("num_turns",  0);
            if (j.contains("result") && j["result"].is_string()) {
                result.output = j["result"].get<std::string>();
            }
            if (j.value("is_error", false) && result.exit_code == 0) {
                result.exit_code = 1;
            }
        } catch (...) {
            // Not a JSON wrapper (old claude version or crash) — use raw output as-is
        }
    }

    return result;
}

// LocalBrain implementation (HTTP to Ollama/vLLM with tool calling)
BrainResult LocalBrain::think(const std::string& prompt, const BrainConfig& config) {
    BrainResult result;
    auto start_time = std::chrono::steady_clock::now();

    {
        if (const char* model = std::getenv("CHITTA_ROLE_JUDGE_MODEL")) {
            if (*model) model_ = model;
        }
        cached_endpoint_ = discover_gpu_endpoint("judge", model_);
    }

    if (cached_endpoint_.empty()) {
        result.error = "No GPU endpoint found";
        return result;
    }

    int timeout_secs = config.timeout_ms / 1000;
    int max_turns    = config.max_turns > 0 ? config.max_turns : 20;

    auto log_fn = [](const std::string& msg) { std::cerr << msg << "\n"; };
    std::string output = call_llm_http_with_tools(
        "", model_, prompt,
        config.system_prompt, max_turns, timeout_secs, log_fn, "judge");

    auto end_time = std::chrono::steady_clock::now();
    result.duration_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count());

    if (!output.empty()) {
        result.success = true;
        result.output = output;
        result.exit_code = 0;
    } else {
        result.error = "Empty response from " + cached_endpoint_;
    }

    return result;
}

// BridgeBrain: routes dream through chitta-bridge room API with local/gemma participant.
// Falls back to LocalBrain transparently if bridge is not reachable.
BrainResult BridgeBrain::think(const std::string& prompt, const BrainConfig& config) {
    // 1. Read bridge port + token from ~/.chitta-bridge/http.ports
    const char* home_c = std::getenv("HOME");
    int mcp_port = 0;
    std::string token;
    if (home_c) {
        std::ifstream pf(std::string(home_c) + "/.chitta-bridge/http.ports");
        std::string line;
        while (std::getline(pf, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("mcp=", 0) == 0) mcp_port = std::stoi(line.substr(4));
            if (line.rfind("token=", 0) == 0) token = line.substr(6);
        }
    }
    if (mcp_port == 0 || token.empty()) {
        // Bridge not configured — fall back to direct Ollama
        LocalBrain local(model_);
        return local.think(prompt, config);
    }

    std::string base_url = "http://127.0.0.1:" + std::to_string(mcp_port) + "/mcp";
    std::string auth_hdr = "Authorization: Bearer " + token;

    // Unique room ID per invocation
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string room_id = "dream-" + std::to_string(now_ms) + "-" + std::to_string(getpid());

    std::string parts_json = "[{\"name\":\"gemma\",\"backend\":\"local\",\"model\":\"" + model_ + "\"}]";
    std::string synth_json = "{\"name\":\"gemma\",\"backend\":\"local\",\"model\":\"" + model_ + "\"}";

    int timeout_secs = std::max(60, config.timeout_ms / 1000);
    std::string tmp = "/tmp/chitta-bridge-" + std::to_string(getpid()) + ".json";

    // POST a tools/call JSON-RPC to bridge MCP SSE endpoint, return result object
    auto bridge_call = [&](const std::string& tool, const nlohmann::json& args) -> nlohmann::json {
        nlohmann::json rpc = {{"jsonrpc","2.0"},{"id",1},{"method","tools/call"},
                    {"params",{{"name",tool},{"arguments",args}}}};
        { std::ofstream tf(tmp); tf << rpc.dump(); }

        std::string resp = fork_exec_capture({
            "curl", "-sf", "--max-time", std::to_string(timeout_secs),
            "-X", "POST", base_url,
            "-H", auth_hdr,
            "-H", "Content-Type: application/json",
            "-H", "Accept: application/json, text/event-stream",
            "-d", "@" + tmp
        }, timeout_secs + 5);
        std::remove(tmp.c_str());

        // Parse SSE: look for "data: {...}" lines
        std::istringstream iss(resp);
        std::string ln;
        while (std::getline(iss, ln)) {
            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
            if (ln.rfind("data: ", 0) == 0) {
                try {
                    auto j = nlohmann::json::parse(ln.substr(6));
                    if (j.contains("result")) return j["result"];
                } catch (...) {}
            }
        }
        return {};
    };

    // 2. Create room
    auto cr = bridge_call("room_create", {
        {"room_id", room_id}, {"topic", prompt}, {"participants", parts_json}
    });
    if (cr.empty()) {
        // Bridge unreachable — fall back
        std::remove(tmp.c_str());
        LocalBrain local(model_);
        return local.think(prompt, config);
    }

    // 3. Run 1 round
    bridge_call("room_run", {{"room_id", room_id}, {"rounds", 1}});

    // 4. Synthesize with local gemma
    auto sr = bridge_call("room_synthesize", {{"room_id", room_id}, {"synthesizer", synth_json}});

    // Extract text from MCP content array
    std::string synthesis;
    if (!sr.empty()) {
        try {
            if (sr.contains("content") && sr["content"].is_array()) {
                for (const auto& c : sr["content"])
                    if (c.value("type","") == "text") synthesis += c.value("text","");
            }
        } catch (...) { synthesis = sr.dump(); }
    }

    BrainResult result;
    if (!synthesis.empty()) {
        result.success   = true;
        result.output    = synthesis;
    } else {
        result.error = "Bridge room returned empty synthesis for room " + room_id;
    }
    return result;
}

}  // namespace chitta
