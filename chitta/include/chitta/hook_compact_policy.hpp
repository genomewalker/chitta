#pragma once
#include <chitta/hook_bash_policy.hpp>
#include <set>
#include <future>

namespace chitta::hook_policy {
inline json plan() {
    return {{"queue", json::array()}, {"local", json::object()}, {"stdout", ""}, {"stderr", ""}};
}
inline void queue(json& p, const char* tool, const json& args) {
    p["queue"].push_back({{"tool", tool}, {"args", args}});
}
inline std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream stream(text);
    for (std::string line; std::getline(stream, line);)
        out.push_back(line);
    return out;
}
inline std::vector<std::string> take(std::vector<std::string> values, size_t n, bool tail = false) {
    if (values.size() > n) {
        if (tail)
            values.erase(values.begin(), values.end() - n);
        else
            values.resize(n);
    }
    return values;
}
inline std::string joined(const std::vector<std::string>& values, const std::string& separator) {
    return hook_ledger::join(json(values), separator);
}
inline std::string message_text(const json& content) {
    if (content.is_string()) return content.get<std::string>();
    std::vector<std::string> text;
    if (content.is_array())
        for (const auto& block : content)
            if (block.is_object() && block.contains("text")) text.push_back(str(block, "text"));
    return joined(text, " ");
}
inline std::vector<std::string> selected_lines(const std::vector<std::string>& source,
                                               const char* re, size_t n, bool tail = true) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto& line : source)
        if (!hook_ledger::trim(line).empty() && match(line, re, true)) {
            auto value = prompt_policy::prefix(line, 200);
            if (seen.insert(value).second) out.push_back(value);
        }
    return take(out, n, tail);
}
inline json precompact(const json& a, const Invoke& invoke) {
    auto result      = plan();
    const auto input = a.value("input", json::object());
    const auto realm = str(a, "realm"), sid = str(a, "checkpoint_id"),
               path = str(input, "transcript_path");
    const auto rows = a.value("transcript", json::array());
    std::vector<std::string> users, assistants, commands;
    std::set<std::string> tool_paths, all_paths;
    std::map<std::string, json> todos;
    json messages                            = json::array();
    std::function<void(const json&)> inspect = [&](const json& value) {
        if (value.is_object()) {
            for (const auto* key : {"filePath", "file_path", "transcript_path"})
                if (!str(value, key).empty()) {
                    tool_paths.insert(str(value, key));
                    break;
                }
            if (value.contains("subject") &&
                (str(value, "status") == "pending" || str(value, "status") == "in_progress")) {
                auto subject =
                    value["subject"].is_string() ? str(value, "subject") : value["subject"].dump();
                todos.emplace(subject, json{{"content", subject}, {"status", value["status"]}});
            }
            for (const auto& child : value.items())
                inspect(child.value());
        } else if (value.is_array())
            for (const auto& child : value)
                inspect(child);
    };
    for (const auto& row : rows) {
        inspect(row);
        const auto role    = str(row, "type");
        const auto message = row.value("message", json::object());
        if (!message.is_object()) continue;
        const auto content = message.value("content", json());
        const auto text    = message_text(content);
        if ((role == "user" || role == "assistant") && !text.empty())
            messages.push_back({{"role", role}, {"content", text}});
        if (role == "user")
            for (const auto& line : lines(text))
                if (!hook_ledger::trim(line).empty() &&
                    !match(line, "<(system-reminder|command-name|command-message|task-notification|"
                                 "local-command)|^\\[Request interrupted"))
                    users.push_back(line);
        if (role == "assistant") {
            if (content.is_array())
                for (const auto& block : content)
                    if (str(block, "type") == "text") {
                        auto text_lines = lines(str(block, "text"));
                        assistants.insert(assistants.end(), text_lines.begin(), text_lines.end());
                    }
            if (content.is_array())
                for (const auto& block : content)
                    if (str(block, "type") == "tool_use" && str(block, "name") == "Bash")
                        for (const auto& line :
                             lines(str(block.value("input", json::object()), "command")))
                            commands.push_back(line);
        }
    }
    users                        = take(users, 20, true);
    assistants                   = take(assistants, 5000, true);
    std::vector<std::string> all = users;
    all.insert(all.end(), assistants.begin(), assistants.end());
    const std::regex path_pattern(
        R"rx(/[^\s"'<>|]+\.(bam|cram|sam|vcf|bcf|fastq|fastq\.gz|fq|fq\.gz|tsv|csv|jsonl|parquet|mcaf|ktax|py|rs|sh|cpp|hpp|h|c|ts|js|toml|yaml|yml))rx");
    auto paths_in = [&](const std::vector<std::string>& source, size_t limit) {
        std::set<std::string> found_paths;
        for (const auto& line : source)
            for (auto i = std::sregex_iterator(line.begin(), line.end(), path_pattern);
                 i != std::sregex_iterator(); ++i)
                found_paths.insert(i->str());
        auto limited = take({found_paths.begin(), found_paths.end()}, limit, true);
        all_paths.insert(limited.begin(), limited.end());
    };
    auto limited_tools = take({tool_paths.begin(), tool_paths.end()}, 40, true);
    all_paths.insert(limited_tools.begin(), limited_tools.end());
    paths_in(all, 30);
    paths_in(commands, 20);
    std::vector<std::string> data, code;
    for (const auto& value : all_paths) {
        if (match(
                value,
                R"(\.(bam|cram|sam|vcf|bcf|fastq|fastq\.gz|fq|fq\.gz|tsv|csv|parquet|mcaf|ktax)$)",
                true))
            data.push_back(value);
        if (match(value, R"(\.(py|rs|sh|cpp|hpp|h|c|ts|js|toml|yaml|yml)$)", true))
            code.push_back(value);
    }
    data       = take(data, 20);
    code       = take(code, 20);
    auto files = data;
    files.insert(files.end(), code.begin(), code.end());
    files = take(files, 30);
    if (files.empty()) files = take({tool_paths.begin(), tool_paths.end()}, 20, true);
    std::vector<std::string> run;
    for (const auto& cmd : commands)
        if (!hook_ledger::trim(cmd).empty() && !match(cmd, "^(cat|echo|ls |pwd|cd )"))
            run.push_back(cmd);
    run             = selected_lines(take(run, 30, true), ".*", 15, true);
    const auto done = selected_lines(
        all,
        R"(\b(done|completed|finished|already (ran|tested|running|profiled|built|created)|passed|success(fully)?|works|working|resolved|confirmed|verified)\b)",
        10);
    const auto next = selected_lines(
        all, R"(\b(next|todo|remaining|follow[- ]?up|then we|plan to|need to|should|still need)\b)",
        10);
    const auto blockers = selected_lines(
        all,
        R"(\b(blocked|blocker|fail(ed|ure|s)|error|cannot|can't|stuck|timeout|broken|crash|missing)\b)",
        8);
    auto decisions = selected_lines(
        all,
        R"(\b(we (chose|decided|switched|went with|use)|decision|instead of|prefer|better to|approach:|strategy:)\b)",
        8);
    auto marked = selected_lines(assistants, R"(^\[(DECISION|SOLUTION|GOTCHA)\])", 5, false);
    decisions.insert(decisions.end(), marked.begin(), marked.end());
    decisions = take(decisions, 10);
    std::vector<std::string> discoveries, parts;
    if (!data.empty())
        discoveries.push_back("Real data files actively used: " + joined(take(data, 5), ",") + ",");
    if (!run.empty()) {
        discoveries.push_back("Commands executed this session (do NOT suggest re-running):");
        for (const auto& cmd : take(run, 8, true))
            discoveries.push_back("  ran: " + cmd);
    }
    for (const auto& line : take(done, 5))
        discoveries.push_back(line);
    auto intent = bytes(
        std::regex_replace(joined(take(users, 5, true), " ") + " ", std::regex("\\s+"), " "), 500);
    if (!intent.empty()) parts.push_back("Goal: " + intent);
    if (!data.empty()) parts.push_back("Data in active use: " + joined(take(data, 8), ",") + ",");
    if (!run.empty())
        parts.push_back("Already executed: " + bytes(joined(take(run, 8, true), ";") + ";", 600));
    if (!done.empty())
        parts.push_back("Completed: " + bytes(joined(take(done, 5), ";") + ";", 400));
    if (!next.empty()) parts.push_back("Pending: " + bytes(joined(take(next, 5), ";") + ";", 400));
    auto snapshot = rows.empty() ? "" : bytes(joined(parts, "\n") + "\n", 2000);
    if (!rows.empty() && prompt_policy::prefix(snapshot, 100) == snapshot && snapshot.size() < 100)
        snapshot = "Context compacted (" + str(input, "trigger", "auto") +
                   "). Files: " + joined(take(files, 5), ", ");
    auto facts = selected_lines(
        assistants,
        R"(use [a-z]+ (for|to|via|through)|is the (working|correct|proper|right)|works? (via|through|by|with)|connect (to|through|via)|host[: ][a-z]|server[: ][a-z]|proxy|socks|ssh [^$]|rclone|important:|note:|remember:)",
        10, false);
    std::sort(facts.begin(), facts.end());
    facts.erase(std::unique(facts.begin(), facts.end()), facts.end());
    if (!facts.empty())
        queue(result, "observe",
              {{"category", "wisdom"},
               {"title", "Pre-compact context: " + sid},
               {"content",
                "[pre-compact:" + realm + "] Key context from session\n" + joined(facts, "\n")},
               {"realm", realm},
               {"confidence", 0.85}});
    json tasks = json::array();
    for (const auto& pair : todos)
        tasks.push_back(pair.second);
    if (tasks.size() > 5) tasks.erase(tasks.begin(), tasks.end() - 5);
    queue(result, "ledger_save",
          {{"session_id", sid},
           {"project", realm},
           {"mood", "pre-compact"},
           {"active_files", files},
           {"decisions", decisions},
           {"todos", tasks},
           {"blockers", blockers},
           {"discoveries", discoveries},
           {"next_steps", take(next, 8)},
           {"snapshot", snapshot}});
    std::string diagnostics = "[checkpoint] " + sid + ": files=" + std::to_string(files.size()) +
                              " decisions=" + std::to_string(decisions.size()) +
                              " discoveries=" + std::to_string(discoveries.size()) +
                              " todos=" + std::to_string(tasks.size()) + "\n";
    const auto session = str(a, "session_id");
    if (!session.empty() && !path.empty()) {
        queue(result, "transcript_register",
              {{"session_id", session}, {"transcript_path", path}, {"realm", realm}});
        queue(result, "distill_trigger", {{"session_id", session}});
        diagnostics += "[distill] Triggered for " + session + "\n";
    }
    if (messages.size() > 60) messages.erase(messages.begin(), messages.end() - 60);
    if (!messages.empty()) {
        const auto compact =
            invoke("compact_context", {{"messages", messages},
                                       {"query", prompt_policy::prefix(snapshot, 300)},
                                       {"target_ratio", 0.4}})
                .value("structured", json::object());
        const auto stats  = compact.value("stats", json::object());
        const auto before = stats.value("before_tokens", 0), after = stats.value("after_tokens", 0);
        const auto dropped = stats.value("dropped_pct", json(0)).dump();
        diagnostics += "[compact_context] " + std::to_string(before) + "→" + std::to_string(after) +
                       " tok | " + dropped + "% memory-covered drops | embedding=" +
                       stats.value("embedding", json(false)).dump() + "\n";
        if (before > 0)
            queue(result, "observe",
                  {{"category", "episode"},
                   {"content", "[pre-compact:" + realm +
                                   "] Memory-aware compaction: " + std::to_string(before) + "→" +
                                   std::to_string(after) + " tokens (" + dropped +
                                   "% dropped as memory-covered). Session: " +
                                   (session.empty() ? sid : session)},
                   {"realm", realm},
                   {"confidence", 0.7}});
    }
    result["stderr"] = diagnostics;
    return result;
}
inline json compact_restore(const json& a, const Invoke& invoke) {
    auto result      = plan();
    const auto input = a.value("input", json::object());
    const auto realm = str(a, "realm"), session = str(input, "session_id"),
               path   = str(input, "transcript_path");
    const auto loaded = invoke("ledger_load", {{"project", realm}});
    auto ledger       = loaded.value("structured", json::object());
    if (!ledger.is_object()) ledger = json::object();
    if (!session.empty())
        queue(result, "session_register",
              {{"session_id", session}, {"realm", realm}, {"pid", a.value("pid", 0)}});
    if (a.value("has_transcript", false))
        queue(result, "transcript_register",
              {{"session_id", session}, {"realm", realm}, {"transcript_path", path}});
    const auto snapshot = str(ledger, "snapshot");
    auto values         = [&](const char* key, size_t n) {
        std::vector<std::string> v;
        for (const auto& value : ledger.value(key, json::array())) {
            auto entries = lines(value.is_string() ? value.get<std::string>() : value.dump());
            v.insert(v.end(), entries.begin(), entries.end());
        }
        return take(v, n);
    };
    const auto files = values("active_files", 15), decisions = values("decisions", 5),
               next = values("next_steps", 5), blockers = values("blockers", 3),
               discoveries = values("discoveries", 10);
    std::vector<std::string> parts;
    if (!ledger.empty()) {
        auto goal = first_line(snapshot, "^Goal:", 200);
        goal      = std::regex_replace(goal, std::regex("^Goal:\\s*"), "");
        if (goal.empty()) goal = bytes(snapshot.substr(0, snapshot.find('\n')) + "\n", 200);
        const auto step = next.empty() ? "" : bytes(next[0] + "\n", 150),
                   done = discoveries.empty() ? "" : bytes(discoveries[0] + "\n", 100);
        if (!goal.empty() || !step.empty()) {
            std::string directive = "[resume-directive] Session resumed after compaction. Start "
                                    "your FIRST reply with one sentence: \"";
            if (!goal.empty()) directive += "We were working on: " + goal + ".";
            if (!done.empty()) directive += " Done: " + done + ".";
            if (!step.empty()) directive += " Next: " + step + ".";
            directive += "\" Then continue from that next step immediately. If the user's message "
                         "clearly starts a new topic, skip the recap. [/resume-directive]";
            parts.push_back(directive);
        }
        std::string restored = "[session-state]";
        if (prompt_policy::prefix(snapshot, 20) != snapshot)
            restored += "\n" + prompt_policy::prefix(snapshot, 1500);
        if (!files.empty()) restored += "\nActive files: " + joined(files, ",") + ",";
        if (!decisions.empty()) restored += "\nDecisions: " + joined(decisions, ";") + ";";
        if (!next.empty()) restored += "\nNext steps: " + joined(next, ";") + ";";
        if (!blockers.empty()) restored += "\nBlockers: " + joined(blockers, ";") + ";";
        std::vector<std::string> todos;
        for (const auto& todo : ledger.value("todos", json::array()))
            todos.push_back("[" + str(todo, "status", "null") + "] " +
                            str(todo, "content", "null"));
        if (!todos.empty()) restored += "\nTasks: " + joined(take(todos, 5), ";") + ";";
        parts.push_back(restored + "\n[/session-state]");
        bool data = false;
        for (const auto& file : values("active_files", 10000))
            data |=
                match(file, R"(\.(bam|cram|sam|vcf|bcf|fastq|mcaf|ktax|tsv|csv|parquet)$)", true);
        if (!discoveries.empty() || data) {
            std::string guard =
                "[anti-confab] MANDATORY RULES FOR THIS RESUMED SESSION:\n1. [session-state] above "
                "is AUTHORITATIVE. Do not contradict it.\n2. Never propose as pending anything "
                "listed under 'Already done' or 'Already executed'.\n3. If uncertain, ask — do not "
                "guess or confabulate session history.";
            if (data)
                guard += "\n4. FORBIDDEN CLAIM: 'ready to test on real data' / 'when you have "
                         "files' — real data IS present and WAS used.";
            if (!discoveries.empty()) guard += "\nAlready done:\n- " + joined(discoveries, "\n- ");
            parts.push_back(guard + "\n[/anti-confab]");
        }
    }
    // Preserve the former sorted facet merge order. Every query is still an
    // existing smart_context call; this code does not implement recall scoring.
    std::vector<std::pair<std::string, int>> queries;
    const auto goal = first_line(snapshot, "^Goal:", 300), data = joined(take(files, 5), ", "),
               decision = joined(take(decisions, 3), "; ");
    if (!data.empty()) queries.emplace_back("data artifacts: " + data, 200);
    if (!decision.empty()) queries.emplace_back("decisions: " + decision, 200);
    if (goal.empty() && data.empty())
        queries.emplace_back("session continuation: " + realm + " project", 300);
    if (!goal.empty()) queries.emplace_back(goal, 200);
    std::vector<std::future<json>> futures;
    for (const auto& query : queries)
        futures.push_back(std::async(std::launch::async, [&, query] {
            return invoke("smart_context",
                          {{"task", query.first}, {"mode", "fast"}, {"limit", query.second}});
        }));
    std::vector<std::string> context;
    std::set<std::string> seen;
    for (auto& future : futures)
        for (const auto& line : lines(str(future.get(), "text")))
            if (!hook_ledger::trim(line).empty() && line.find("No memories") == std::string::npos &&
                seen.insert(line).second)
                context.push_back(line);
    const auto smart = bytes(joined(take(context, 25), "\n") + "\n", 800);
    if (!smart.empty()) parts.push_back("[soul-context]\n" + smart + "\n[/soul-context]");
    const auto agents = a.value("agent_count", 0);
    if (agents > 15)
        parts.push_back("[token-budget] " + std::to_string(agents) +
                        " subagents spawned pre-compact. Consider batching remaining work or "
                        "starting fresh with /recap.");
    json output = {{"hookEventName", "SessionStart"}};
    if (!parts.empty()) {
        output["additionalContext"] = joined(parts, "\n") + "\n";
        output["watchPaths"]        = a.value("watch_paths", json::array());
    }
    result["stdout"] = json{{"hookSpecificOutput", output}}.dump(2);
    return result;
}
} // namespace chitta::hook_policy
