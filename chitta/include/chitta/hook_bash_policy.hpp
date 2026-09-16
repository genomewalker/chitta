#pragma once
#include <chitta/hook_ledger_policy.hpp>
#include <functional>

namespace chitta::hook_policy {
using json = nlohmann::json;
using hook_ledger::str;
using Invoke = std::function<json(const std::string&, const json&)>;
inline bool match(const std::string& s, const char* re, bool icase = false) {
    return prompt_policy::matches(s, re, icase);
}
inline std::string bytes(std::string s, size_t n) {
    s.resize(std::min(s.size(), n));
    while (!s.empty() && s.back() == '\n')
        s.pop_back();
    return json::parse(json(s).dump(-1, ' ', false, json::error_handler_t::replace))
        .get<std::string>();
}
inline std::string found(const std::string& s, const char* re, bool last = false,
                         size_t group = 0) {
    const std::regex r(re);
    std::string out;
    for (auto it = std::sregex_iterator(s.begin(), s.end(), r); it != std::sregex_iterator();
         ++it) {
        out = (*it)[group];
        if (!last) break;
    }
    return out;
}
inline std::string first_line(const std::string& s, const char* pattern, size_t limit,
                              bool icase = false) {
    std::istringstream stream(s);
    for (std::string line; std::getline(stream, line);)
        if (match(line, pattern, icase)) return bytes(line + "\n", limit);
    return "";
}
inline json bash(const json& a, const Invoke& invoke) {
    const auto input = a.value("input", json::object()), local = a.value("local", json::object());
    const auto command = str(input.value("tool_input", json::object()), "command");
    json result        = {
        {"queue", json::array()}, {"local", json::object()}, {"stdout", ""}, {"stderr", ""}};
    auto enqueue = [&](const char* tool, const json& args) {
        result["queue"].push_back({{"tool", tool}, {"args", args}});
    };
    std::string out, err;
    int exit   = 0;
    bool known = true, likely = false;
    if (str(input, "hook_event_name") == "PostToolUseFailure") {
        const auto raw = str(input, "error");
        auto code      = found(raw, "^Exit code ([0-9]+)", false, 1);
        exit           = code.empty() ? 1 : std::stoi(code);
        auto nl        = raw.find('\n');
        err            = nl == std::string::npos ? "" : bytes(raw.substr(nl + 1), 500);
    } else if (input.contains("tool_response") && input["tool_response"].is_string()) {
        out    = bytes(str(input, "tool_response") + "\n", 500);
        known  = false;
        likely = match(out,
                       "No such file|command not found|Permission denied|Traceback \\(most "
                       "recent|^Error[: ]|FAILED|fatal:|Exit code [1-9]",
                       true);
    } else {
        auto response = input.value("tool_response", input.value("tool_result", json::object()));
        if (!response.is_object()) response = json::object();
        exit = response.value("exit_code", 0);
        out  = bytes(str(response, "stdout") + "\n", 500);
        err  = bytes(str(response, "stderr") + "\n", 500);
    }
    if (command.empty()) return result;
    auto evidence = err.empty() ? (str(input, "error").empty() ? out : str(input, "error")) : err;
    result["outcome"] = {{"event", "bash_outcome"},
                         {"exit_code", known ? json(exit) : json()},
                         {"cmd_head", prompt_policy::prefix(command, 80)},
                         {"stderr_head", exit || likely ? bytes(evidence, 160) : ""}};
    if (likely) result["outcome"]["likely_fail"] = true;
    const auto first   = found(command, "[^\\s]+");
    const auto current = first.substr(
        first.find_last_of('/') == std::string::npos ? 0 : first.find_last_of('/') + 1);
    if (exit == 0) {
        const auto cwd = str(a, "cwd"), realm = found(cwd, "/repos/([^/]+)", false, 1);
        const bool job =
            match(command,
                  "(^|\\s)(nohup|sbatch|srun|qsub|bsub)\\s|parallel\\s.*--sshlogin|parallel\\s.*-"
                  "S\\s|gnu parallel|screen\\s+-dm|tmux\\s+new(-session)?\\s+-d") ||
            (match(command, "\\s*&\\s*$") &&
             !match(command, "^\\s*(sleep|echo|true|false|wait)\\s"));
        const bool analysis =
            match(command, "(^|\\s)(python3?|Rscript|julia|perl|snakemake|nextflow)\\s+\\S+\\.(py|"
                           "R|jl|pl|nf|smk)|(^|\\s)bash\\s+\\S+\\.sh(\\s|$)|(^|\\s)\\./"
                           "\\S+\\.(sh|py|R)(\\s|$)|(^|\\s)(snakemake|nextflow)\\s") ||
            (match(command, "(^|\\s)(python3?|Rscript)\\s") &&
             !match(command, "^\\s*(pip|conda|which|python.*--version)"));
        result["provenance"] = job || analysis;
        if (job) {
            auto output = found(command, "(--(out_dir|output|outdir|out)\\s+\\S+|-o\\s+\\S+)");
            output      = found(output, "\\S+$");
            auto log    = found(command, "(--joblog\\s+\\S+|--log\\s+\\S+)");
            log         = found(log, "\\S+$");
            if (log.empty()) log = found(found(command, ">\\s*\\S+\\.log"), "\\S+\\.log");
            std::string paths;
            if (!output.empty()) paths += "out:" + output + " ";
            if (!log.empty()) paths += "log:" + log + " ";
            enqueue("remember", {{"content", "[job:auto] cmd: " + bytes(command + "\n", 300) +
                                                 " | " + paths + "| cwd: " + cwd},
                                 {"kind", "signal"},
                                 {"realm", realm},
                                 {"tags", {"long-running-job"}},
                                 {"visibility", 1}});
        }
        std::string title, content;
        const auto combined = out + err;
        if (match(command, "(^|&&|;|\\|\\|)\\s*git\\s+(-C\\s+\\S+\\s+)?commit(\\s|$)")) {
            auto msg = found(command, "-m\\s+['\"]?[^'\"]+['\"]?");
            msg      = std::regex_replace(msg, std::regex("^-m\\s*['\"]|['\"]$"), "");
            msg      = bytes(msg, 120);
            if (msg.empty()) msg = found(out, "\\[.+\\]");
            title   = "git commit: " + (msg.empty() ? "committed" : msg);
            content = "branch:" + str(a, "git_branch") + " commit:" + str(a, "git_commit") +
                      " cmd:" + prompt_policy::prefix(command, 200);
        } else if (match(command, "cargo\\s+(build|test|install)|cmake\\s+--build|make\\b") &&
                   !match(combined, "error\\[E[0-9]|undefined reference|FAILED|ld: ", true)) {
            const auto project = cwd.substr(
                cwd.find_last_of('/') == std::string::npos ? 0 : cwd.find_last_of('/') + 1);
            title   = match(command, "test")
                          ? "tests passed: " + project + " " + found(combined, "[0-9]+ passed", true)
                          : "build succeeded: " + project;
            content = "cmd:" + prompt_policy::prefix(command, 200) + " cwd:" + cwd;
        } else if (match(command, "^\\s*(sbatch|srun)\\s") &&
                   match(combined, "Submitted batch job [0-9]+")) {
            const auto id = found(combined, "Submitted batch job ([0-9]+)", false, 1);
            title         = "SLURM job submitted: job_id=" + id;
            content =
                "cmd:" + prompt_policy::prefix(command, 200) + " job_id:" + id + " cwd:" + cwd;
        } else if (match(command, "pytest|python.*-m\\s+pytest|Rscript.*test") &&
                   match(combined, "[0-9]+ passed")) {
            title   = "tests passed: " + found(combined, "[0-9]+ passed", true);
            content = "cmd:" + prompt_policy::prefix(command, 200) + " cwd:" + cwd;
        }
        if (!title.empty()) {
            enqueue("remember", {{"title", title},
                                 {"content", content},
                                 {"category", "milestone"},
                                 {"realm", realm},
                                 {"tags", {"auto-milestone"}},
                                 {"visibility", 1}});
            result["local"][".last_auto_store_ts"] =
                std::to_string(a.value("now", int64_t(0))) + "\n";
        }
        const auto previous = str(local, ".last_bash_cmd");
        if (!previous.empty() && previous != current)
            enqueue("habit_observe",
                    {{"trigger", "bash:" + previous}, {"response", "bash:" + current}});
        result["local"][".last_bash_cmd"] = current + "\n";
        return result;
    }
    const auto combined = out + err;
    std::string query   = command;
    if (match(combined, "FAILED|PASSED|test result:|not ok|ok [0-9]+ test")) {
        auto name = first_line(combined, "FAILED|not ok", 100, true);
        name      = std::regex_replace(name, std::regex(".*FAILED\\s*|.*not ok\\s*[0-9]*\\s*"), "");
        query     = "test failure " + name + " " +
                found(command, "pytest|cargo test|jest|mocha|rspec|go test|npm test");
    } else if (match(combined, "error\\[E[0-9]|undefined reference|ld: |make\\[|CMake Error")) {
        query = "build error " +
                found(command, "cargo|cmake|make|gcc|g\\+\\+|rustc|javac|go build|npm run build") +
                " " + first_line(combined, "error", 120);
    } else if (match(combined, "^[0-9]{4}-[0-9]{2}-[0-9]{2}|\\[(INFO|WARN|ERROR)\\]")) {
        auto message = first_line(combined, "ERROR", 120, true);
        message      = std::regex_replace(message, std::regex(".*ERROR[\\]\\s:]*"), "");
        query        = "error " + current + " " + message;
    } else if (!err.empty()) {
        const std::regex re("error|failed|not found|permission denied|no such|cannot|invalid",
                            std::regex::icase);
        int n = 0;
        std::string terms;
        for (auto it = std::sregex_iterator(err.begin(), err.end(), re);
             it != std::sregex_iterator() && n < 3; ++it, ++n)
            terms += it->str() + " ";
        if (!terms.empty()) query += " " + terms;
    }
    auto memories = bytes(str(invoke("recall", {{"query", query}, {"limit", 2}}), "text"), 400);
    if (!memories.empty() && memories.find("No memories") == std::string::npos)
        result["stdout"] =
            json({{"hookSpecificOutput",
                   {{"hookEventName", "PostToolUse"},
                    {"additionalContext", "🔴 COMMAND FAILED (exit " + std::to_string(exit) +
                                              ") - Related memories:\n" + memories}}}})
                .dump() +
            "\n";
    return result;
}
} // namespace chitta::hook_policy
