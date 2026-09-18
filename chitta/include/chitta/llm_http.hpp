#pragma once
// llm_http: Shared GPU endpoint discovery + HTTP LLM call utility
//
// Discovery chain (mirrors chitta-bridge gpu_serve.py):
// 1. Cached /tmp/ollama-server-*.url files (written by chitta-gpu start)
// 2. Probe SLURM GPU nodes (squeue → http://<node>:11434)
// 3. Probe localhost:11434
// 4. Invoke `chitta-gpu start <model>` as last resort
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
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <chrono>
#include <thread>

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
                                      const std::string& stdin_data = "") {
    int stdout_pipe[2];
    if (pipe(stdout_pipe) < 0) return "";

    int stdin_pipe[2] = {-1, -1};
    if (!stdin_data.empty() && pipe(stdin_pipe) < 0) {
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        return "";
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        if (stdin_pipe[0] >= 0) { close(stdin_pipe[0]); close(stdin_pipe[1]); }
        return "";
    }

    if (pid == 0) {
        close(stdout_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        close(stdout_pipe[1]);

        if (!stdin_data.empty()) {
            close(stdin_pipe[1]);
            dup2(stdin_pipe[0], STDIN_FILENO);
            close(stdin_pipe[0]);
        }

        std::vector<char*> argv;
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(1);
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
        if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= timeout_secs) {
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
            close(stdout_pipe[0]);
            return "";
        }
        int status;
        int result = waitpid(pid, &status, WNOHANG);
        if (result != 0) finished = true;

        ssize_t n;
        while ((n = read(stdout_pipe[0], buf.data(), buf.size())) > 0)
            output.append(buf.data(), n);

        if (!finished)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ssize_t n;
    while ((n = read(stdout_pipe[0], buf.data(), buf.size())) > 0)
        output.append(buf.data(), n);
    close(stdout_pipe[0]);
    return output;
}

// Probe an Ollama endpoint for /v1/models availability
inline bool probe_endpoint(const std::string& base_url) {
    std::string url = base_url + "/v1/models";
    std::string out = fork_exec_capture(
        {"curl", "-sL", "--max-time", "3", url}, 5);
    return !out.empty() && out.find("data") != std::string::npos;
}

// Discover GPU endpoint using the full chain from gpu_serve.py
// Returns base URL (e.g., "http://node:11434") or empty string
inline std::string discover_gpu_endpoint(const std::string& model = "gemma4:26b",
                                          LogFn log_fn = nullptr,
                                          bool allow_start = true) {
    auto log = [&](const std::string& msg) {
        if (log_fn) log_fn(msg);
    };

    // 1. Cached URL files: /tmp/ollama-server-*.url
    namespace fs = std::filesystem;
    for (auto& entry : fs::directory_iterator("/tmp")) {
        std::string fname = entry.path().filename().string();
        if (fname.find("ollama-server-") == 0 && fname.size() > 4 &&
            fname.substr(fname.size() - 4) == ".url") {
            std::ifstream ifs(entry.path());
            std::string url;
            std::getline(ifs, url);
            if (!url.empty()) {
                // Trim trailing whitespace/newline
                while (!url.empty() && (url.back() == '\n' || url.back() == '\r' || url.back() == ' '))
                    url.pop_back();
                if (probe_endpoint(url)) {
                    log("[llm] Found cached endpoint: " + url);
                    return url;
                }
            }
        }
    }

    // 2. Probe SLURM GPU nodes via squeue
    // Retry on empty output: squeue can be slow/unresponsive at daemon startup,
    // and an empty result here forces a CPU-GGUF fallback that serializes workers.
    std::string squeue_out;
    for (int attempt = 0; attempt < 3; ++attempt) {
        squeue_out = fork_exec_capture(
            {"squeue", "--me", "--noheader", "--format=%j %T %N"}, 5);
        if (!squeue_out.empty()) break;
        if (attempt < 2) std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    if (!squeue_out.empty()) {
        std::istringstream iss(squeue_out);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.find("ollama-") == std::string::npos) continue;
            // Parse: job_name state node
            std::istringstream parts(line);
            std::string name, state, node;
            parts >> name >> state >> node;
            if (state == "RUNNING" && !node.empty()) {
                std::string url = "http://" + node + ":11434";
                if (probe_endpoint(url)) {
                    log("[llm] Found SLURM endpoint: " + url + " (job " + name + ")");
                    return url;
                }
            }
        }
    }

    // 3. Probe localhost
    if (probe_endpoint("http://localhost:11434")) {
        log("[llm] Found local endpoint: http://localhost:11434");
        return "http://localhost:11434";
    }

    // 4. Last resort: invoke chitta-gpu start
    // Skipped when allow_start=false (fail-fast probes: a health check must
    // never spend 120s spinning up a GPU job).
    if (!allow_start) return "";
    log("[llm] No endpoint found, trying chitta-gpu start " + model + "...");
    std::string gpu_out = fork_exec_capture(
        {"chitta-gpu", "start", model}, 120);
    if (!gpu_out.empty()) {
        // chitta-gpu prints export lines; parse ANTHROPIC_BASE_URL
        std::istringstream iss(gpu_out);
        std::string line;
        while (std::getline(iss, line)) {
            const std::string prefix = "export ANTHROPIC_BASE_URL=";
            if (line.find(prefix) == 0) {
                std::string url = line.substr(prefix.size());
                while (!url.empty() && (url.back() == '\n' || url.back() == '\r'))
                    url.pop_back();
                if (probe_endpoint(url)) {
                    log("[llm] Started GPU endpoint: " + url);
                    return url;
                }
            }
        }
    }

    return "";
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
                                  std::optional<bool> think = std::nullopt) {
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

    std::string tmp_path = "/tmp/chitta-llm-" + std::to_string(getpid()) + ".json";
    {
        std::ofstream ofs(tmp_path, std::ios::binary);
        ofs << body;
    }

    std::string url = endpoint + (think.has_value() ? "/api/chat" : "/v1/chat/completions");
    std::string timeout_str = std::to_string(timeout_secs);
    std::string output = fork_exec_capture(
        {"curl", "-sL", "--max-time", timeout_str,
         "-H", "Content-Type: application/json",
         "-d", "@" + tmp_path, url},
        timeout_secs + 5);

    std::remove(tmp_path.c_str());

    try {
        auto resp = nlohmann::json::parse(output);
        if (think.has_value() && resp.contains("message") &&
            resp["message"].contains("content") && resp["message"]["content"].is_string()) {
            return resp["message"]["content"].get<std::string>();
        }
        if (resp.contains("choices") && !resp["choices"].empty()) {
            const auto& msg = resp["choices"][0]["message"];
            if (msg.contains("content")) {
                const auto& content = msg["content"];
                if (content.is_string()) {
                    return content.get<std::string>();
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
                                             LogFn log_fn = nullptr) {
    auto log = [&](const std::string& msg) { if (log_fn) log_fn(msg); };

    nlohmann::json messages = nlohmann::json::array();
    if (!system_prompt.empty())
        messages.push_back({{"role","system"},{"content",sanitize_utf8(system_prompt)}});
    messages.push_back({{"role","user"},{"content",sanitize_utf8(prompt)}});

    nlohmann::json tools = get_dream_tools();
    std::string last_content;
    int tool_calls_made = 0;

    for (int turn = 0; turn < max_turns; ++turn) {
        nlohmann::json req = {
            {"model", model},
            {"messages", messages},
            {"tools", tools},
            {"temperature", 0.4},
            {"max_tokens", 8192}
        };

        std::string tmp = "/tmp/chitta-llm-tools-" + std::to_string(getpid()) + "-" + std::to_string(turn) + ".json";
        { std::ofstream ofs(tmp, std::ios::binary); ofs << req.dump(-1, ' ', true); }

        std::string url = endpoint + "/v1/chat/completions";
        std::string raw = fork_exec_capture(
            {"curl", "-sL", "--max-time", std::to_string(timeout_secs),
             "-H", "Content-Type: application/json",
             "-d", "@" + tmp, url},
            timeout_secs + 5);
        std::remove(tmp.c_str());

        if (raw.empty()) { log("[llm] Empty response on turn " + std::to_string(turn)); break; }

        nlohmann::json resp;
        try { resp = nlohmann::json::parse(raw); } catch (...) { break; }

        if (!resp.contains("choices") || resp["choices"].empty()) break;

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
                std::string tmp2 = "/tmp/chitta-llm-summary-" + std::to_string(getpid()) + ".json";
                { std::ofstream ofs(tmp2, std::ios::binary); ofs << summary_req.dump(-1,' ',true); }
                std::string sraw = fork_exec_capture({"curl","-sL","--max-time","60","-H","Content-Type: application/json","-d","@"+tmp2,url}, 65);
                std::remove(tmp2.c_str());
                try {
                    auto sr = nlohmann::json::parse(sraw);
                    if (sr.contains("choices") && !sr["choices"].empty() && sr["choices"][0]["message"].contains("content"))
                        last_content = sr["choices"][0]["message"]["content"].get<std::string>();
                } catch (...) {}
            }
            break;
        }

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
    return last_content;
}

} // namespace chitta
