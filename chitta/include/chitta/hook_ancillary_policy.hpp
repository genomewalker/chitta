#pragma once
#include <chitta/hook_compact_policy.hpp>
#include <chitta/hook_saddle_policy.hpp>
#include <filesystem>

namespace chitta::hook_policy {
// Ancillary decisions share the same read-only plan / durable client queue boundary.
inline json ancillary(const json& a, const Invoke& invoke) {
    auto p            = plan();
    const auto family = str(a, "family"), realm = str(a, "realm", "brahman");
    const auto input = a.value("input", json::object());
    auto err = [&](const std::string& value) { p["stderr"] = str(p, "stderr") + value + "\n"; };
    if (family == "post-commit") {
        p["stdout"] = "[cc-soul] Cleaning ephemeral dev:hot nodes...\n";
        const auto response =
            invoke("recall", {{"query", "dev:hot"}, {"tag", "dev:hot"}, {"limit", 100}});
        for (const auto& row : response.at("structured").value("results", json::array())) {
            if (!row.contains("id")) continue;
            const auto& id = row.at("id");
            queue(p, "forget", {{"id", id.is_string() ? id.get<std::string>() : id.dump()}});
        }
        if (a.value("bootstrap_exists", false) &&
            match(str(a, "changed_files"), R"(bootstrap/.*\.soul|chitta/include/|chitta/src/)")) {
            p["stdout"] = str(p, "stdout") + "[cc-soul] Crystallizing knowledge (full rewire)...\n";
            queue(p, "import_soul", {{"file", a.at("bootstrap_file")}});
        }
        p["stdout"] = str(p, "stdout") + "[cc-soul] Commit crystallized\n";
    } else if (family == "dream-select") {
        p["candidates"]     = json::array();
        p["short_sessions"] = json::array();
        const auto done     = a.value("processed", json::array());
        for (const auto& entry : a.value("entries", json::array())) {
            if (std::find(done.begin(), done.end(), entry.at("session_id")) != done.end()) continue;
            const auto turns = entry.value("message_rows", 0);
            if (turns < 4) {
                p["short_sessions"].push_back(entry.at("session_id"));
                continue;
            }
            auto candidate = entry;
            candidate.erase("message_rows");
            p["candidates"].push_back(candidate);
            if (p["candidates"].size() == 10) break;
        }
    } else if (family == "dream-synthesis") {
        const auto memories =
            str(invoke("recall", {{"query", "patterns solutions gotchas cross-session insights"},
                                  {"limit", 20}}),
                "text");
        p["rows"] = json::array();
        auto turn = [&](const char* role, const std::string& text) {
            p["rows"].push_back({{"type", role}, {"message", {{"role", role}, {"content", text}}}});
        };
        turn("user", "You are synthesizing cross-session learnings. " +
                         std::to_string(a.value("processed_count", 0)) +
                         " recent Claude sessions were just distilled. Here are the most relevant "
                         "memories across those sessions:\n\n" +
                         memories +
                         "\n\nWhat are the recurring patterns, cross-cutting insights, and "
                         "meta-level lessons?");
        turn("assistant",
             "Let me identify the cross-session patterns from these distilled memories.");
        turn("user", "Extract the highest-value meta-insights as SSL format learnings. Focus on "
                     "patterns that appear across multiple sessions, not single-session facts.");
        turn("assistant", "Based on the cross-session analysis, here are the meta-patterns:");
    } else if (family == "span-capture") {
        const auto snapshot = a.value("snapshot", json::object());
        auto sid            = std::regex_replace(str(snapshot, "session_id", "unknown"),
                                                 std::regex("[^A-Za-z0-9._-]"), "_");
        const auto now = a.value("now", 0LL), prior = a.value("prior_count", 0LL);
        const auto user = str(a, "last_user");
        int reward      = 0;
        if (match(user, "^(yes|perfect|great|thanks|exactly|good|nice|awesome|that works)", true))
            reward = 1;
        else if (match(user,
                       "^(no[,. ]|wrong|that.s not|incorrect|actually|fix|error|bug|doesn.t work)",
                       true))
            reward = -1;
        json spans = json::array();
        for (const auto& tool : snapshot.value("tool_spans", json::array())) {
            const bool failure = tool.value("is_error", false), success = !failure && reward != -1;
            const auto name   = str(tool, "tool"),
                       output = prompt_policy::prefix(str(tool, "output"), 500);
            const auto args   = tool.value("input", json::object());
            spans.push_back({{"type", "tool_span"},
                             {"timestamp", now},
                             {"session_id", sid},
                             {"tool", name},
                             {"input", args},
                             {"output", output},
                             {"success", success},
                             {"reward", reward},
                             {"is_error", failure}});
            if (!success)
                queue(p, "observe",
                      {{"category", "failure"},
                       {"content", "[tool:" + name + "] failed with: " +
                                       prompt_policy::prefix(output, 100) + "\n"}});
            if (success && reward == 1) {
                std::vector<std::string> entries;
                for (const auto& item : args.items())
                    entries.push_back(item.key() + "=" +
                                      prompt_policy::prefix(item.value().is_string()
                                                                ? item.value().get<std::string>()
                                                                : item.value().dump(),
                                                            30));
                queue(p, "observe",
                      {{"category", "solution"},
                       {"content", "[tool:" + name +
                                       "] success: " + bytes(joined(entries, ", "), 100) + "\n"}});
            }
        }
        const auto count = prior + spans.size();
        if (count) {
            spans.push_back({{"type", "exchange_span"},
                             {"timestamp", now},
                             {"session_id", sid},
                             {"tool_count", count},
                             {"reward", reward}});
            err("[spans] +" + std::to_string(count) + " tools (reward=" + std::to_string(reward) +
                ")");
        }
        p["spans"]     = spans;
        p["span_name"] = sid + "-" + std::to_string(now) + ".jsonl";
    } else if (family == "subagent-stop") {
        const auto id = str(input, "agent_id"), type = str(input, "agent_type", "general-purpose"),
                   text = str(input, "last_assistant_message");
        if (id.empty()) return p;
        if (hook_saddle::decode(text).size() > 50) {
            for (const auto& line : lines(text)) {
                std::smatch m;
                static const std::regex marker(
                    R"(^\[(SOLUTION|GOTCHA|DECISION|FAILURE|PATTERN)\])");
                if (!std::regex_search(line, m, marker)) continue;
                auto kind = m[1].str(), content = line;
                const auto tag = "[" + kind + "] ";
                if (content.rfind(tag, 0) == 0) content.erase(0, tag.size());
                for (auto& c : kind)
                    c = std::tolower(c);
                queue(p, "remember",
                      {{"content", "[" + realm + ":" + type + "] " + content + "\n"},
                       {"kind", kind},
                       {"tags", {"agent", type, id}}});
                err("[soul] +agent-" + kind + " from " + type + ": " +
                    prompt_policy::prefix(content, 60));
            }
            auto summary = bytes(text + "\n", 300);
            std::replace(summary.begin(), summary.end(), '\n', ' ');
            queue(p, "remember",
                  {{"content",
                    "[" + realm + ":agent-result] " + type + " (" + id + "): " + summary + "\n"},
                   {"kind", "episode"},
                   {"tags", {"agent-result", type}}});
            err("[soul] +agent-result from " + type + " (" + id + ")");
        }
        if (a.value("transcript_exists", false))
            queue(p, "transcript_register",
                  {{"session_id", id},
                   {"transcript_path", str(input, "agent_transcript_path")},
                   {"realm", realm}});
    } else if (family == "file-changed" || family == "codebase-learn") {
        const auto file = family == "file-changed"
                              ? str(input, "file_path")
                              : str(input.value("tool_input", json::object()), "file_path");
        if (file.empty() || (family == "codebase-learn" && !a.value("file_exists", false)))
            return p;
        const auto path = std::filesystem::path(file), dir = path.parent_path();
        if (family == "file-changed") {
            queue(p, "learn_codebase", {{"path", file}, {"project", realm}, {"incremental", true}});
            const auto event = str(input, "event", "change");
            if (event == "unlink" || event == "delete") return p;
            p["log_event"] = {{"tool", "edit"},
                              {"entity", file},
                              {"outcome", 0},
                              {"ts_ms", a.value("now", 0LL) * 1000}};
            if (a.value("now", 0LL) - a.value("last_index", 0LL) < a.value("rate_limit", 300LL))
                return p;
            p["index_stamp"] = a.value("now", 0LL);
            p["stdout"]      = "[reindex] " + path.filename().string() +
                          " changed → queued re-index for " + dir.filename().string() + "\n";
        } else
            p["stdout"] =
                json{{"hookSpecificOutput",
                      {{"hookEventName", "PostToolUse"},
                       {"additionalContext", "[codebase-learn] Indexed " + dir.filename().string() +
                                                 " after skill run"}}}}
                    .dump(2) +
                "\n";
        queue(p, "learn_codebase", {{"path", dir.string()}, {"project", realm}});
    } else if (family == "bash-history") {
        auto command = str(a, "command", str(input.value("tool_input", json::object()), "command"));
        if (!command.empty() &&
            !match(
                command,
                R"(^ls( |$)|^pwd$|^(echo|cat|head|tail|wc|file|which|type|test) |^command -v|^\[)"))
            p["history"] = command;
    } else if (family == "shepherd") {
        struct Pattern {
            const char* name;
            const char* regex;
            const char* severity;
        };
        const Pattern errors[] = {
            {"snakemake_error", "Error in rule", "critical"},
            {"snakemake_missing", "MissingInputException", "critical"},
            {"snakemake_failed", "WorkflowError", "critical"},
            {"snakemake_lock", "LockException", "warning"},
            {"nextflow_error", "ERROR ~ ", "critical"},
            {"nextflow_failed", "Process .* failed", "critical"},
            {"nextflow_abort", "Execution aborted", "critical"},
            {"slurm_failed", "FAILED", "warning"},
            {"slurm_timeout", "TIMEOUT", "warning"},
            {"slurm_oom", "OUT_OF_MEMORY", "critical"},
            {"slurm_cancelled", "CANCELLED", "info"},
            {"traceback", R"(Traceback \(most recent call last\))", "critical"},
            {"segfault", "Segmentation fault", "critical"},
            {"killed", "Killed", "warning"},
            {"oom", "Out of memory", "critical"},
            {"permission", "Permission denied", "warning"},
            {"disk_full", "No space left on device", "critical"},
            {"connection", "Connection refused", "warning"}};
        const Pattern completions[] = {{"snakemake_complete", "100% done", "info"},
                                       {"snakemake_finish", "Finished job", "info"},
                                       {"nextflow_complete", "Succeeded", "info"},
                                       {"nextflow_done", "Completed at:", "info"},
                                       {"slurm_complete", "COMPLETED", "info"}};
        for (const auto& pane : a.value("panes", json::array())) {
            const auto task = str(pane, "task_id"),
                       text = joined(take(lines(str(pane, "text")), 100, true), "\n");
            for (const auto& pattern : errors)
                if (match(text, pattern.regex)) {
                    auto context = bytes(joined(take(lines(text), 20, true), "\n"), 500);
                    std::replace(context.begin(), context.end(), '\n', ' ');
                    context += ' ';
                    queue(p, "long_task_event",
                          {{"task_id", task},
                           {"kind", "error"},
                           {"payload", json{{"pattern", pattern.name},
                                            {"severity", pattern.severity},
                                            {"context", context}}
                                           .dump()},
                           {"tags", {"shepherd", "poll", pattern.name}}});
                    if (std::string(pattern.severity) == "critical")
                        p["alerts"].push_back({{"recipient", "user"},
                                               {"message", "[SHEPHERD] Critical error in " + task +
                                                               ": " + pattern.name}});
                    break;
                }
            for (const auto& pattern : completions)
                if (match(text, pattern.regex)) {
                    queue(p, "long_task_event",
                          {{"task_id", task},
                           {"kind", "observation"},
                           {"payload", json{{"pattern", pattern.name}}.dump()},
                           {"tags", {"shepherd", "poll", "completion"}}});
                    break;
                }
        }
    } else if (family == "shepherd-stop") {
        auto text       = str(a, "pane_tail");
        const auto pane = str(a, "pane_name", "pipeline-main");
        if (match(text, "(Error|FAILED|Exception|Traceback|TIMEOUT)", true))
            p["stdout"] =
                "[shepherd:alert] Error detected in " + pane + " — run /shepherd status\n";
        else if (match(text, "(complete|finished|done|100%)", true))
            p["stdout"] =
                "[shepherd:done] Pipeline appears complete — run /shepherd status to confirm\n";
    } else if (family == "memory-intercept") {
        const auto tool = input.value("tool_input", json::object());
        const auto file = str(tool, "file_path"), content = str(tool, "content");
        if (file.size() < 17 || file.substr(file.size() - 17) != "/memory/MEMORY.md" ||
            content.empty())
            return p;
        p["memory_sync"]     = true;
        p["imported_notice"] = "[memory-intercept] Imported to chitta\n";
        auto name = std::filesystem::path(file).parent_path().parent_path().filename().string();
        name      = std::regex_replace(name, std::regex("^-Users-[^-]*-Downloads-"), "");
        name      = std::regex_replace(name, std::regex("^-Users-[^-]*-"), "");
        std::string imported;
        bool user = false;
        for (const auto& line : lines(content)) {
            if (line.empty()) continue;
            if (line.find("Chitta Soul Memories (auto-synced)") != std::string::npos) {
                user = false;
                continue;
            }
            if (line.rfind("*Last synced:", 0) == 0) continue;
            if (line == "---") {
                user = true;
                continue;
            }
            if ((user || line.find("auto-synced") == std::string::npos) && line.rfind("# ", 0) != 0)
                imported += line + "\n";
        }
        if (!a.value("render", false)) {
            p["stderr"] = "[memory-intercept] Importing MEMORY.md for project: " + name + "\n";
            if (!imported.empty() && imported.find("Chitta Soul") == std::string::npos)
                p["import"] = {{"content", "[memory:" + name + "] Claude Code MEMORY.md import\n" +
                                               bytes(imported, imported.size())},
                               {"tags", {"memory-import", name}}};
        } else {
            const auto reply = invoke("recall", {{"query", name}, {"limit", 10}}).at("structured");
            std::string memories;
            if (reply.contains("results") && reply["results"].is_array()) {
                for (const auto& row : reply["results"])
                    if (row.contains("text")) memories += str(row, "text") + "\n";
            } else if (reply.contains("text"))
                memories = str(reply, "text");
            memories = bytes(memories, memories.size());
            if (!memories.empty() && memories.find("No memories") == std::string::npos) {
                p["memory_file"] = "# " + name +
                                   " Memory\n\n## Chitta Soul Memories (auto-synced)\n\n" +
                                   memories + "\n\n---\n\n## Project Notes\n\n" +
                                   bytes(imported, imported.size()) +
                                   "\n\n---\n*Last synced: " + str(a, "timestamp") + "*\n";
                p["stderr"] = "[memory-intercept] Enhanced MEMORY.md with chitta memories\n";
            }
        }
    } else if (family == "persona") {
        auto task = str(a, "task");
        if (task.empty()) return p;
        const bool seed = task == "--seed";
        json args       = {{"query", "[persona]"}, {"tag", "persona"}, {"limit", seed ? 1 : 20}};
        if (!str(a, "persona_realm").empty()) args["realm"] = a["persona_realm"];
        const auto recalled = str(invoke("recall", args), "text");
        if (seed) {
            if (recalled.find("[persona]") != std::string::npos) {
                p["stdout"] = "persona catalog already seeded; skipping\n";
                return p;
            }
            const std::string catalog =
                R"catalog(skeptic|falsify before you trust|hypothesis,claim,prove,verify,validate,assume,confident,probably,likely,should work|Adopt a falsifier's stance. State the strongest evidence that would DISPROVE this hypothesis, then look for it first. Distrust confirming evidence. Name the assumption that, if wrong, collapses the whole conclusion.
analogist|map structure across domains|creative,brainstorm,idea,novel,design,invent,imagine,reframe,metaphor|Think by analogy. Find a well-understood system whose STRUCTURE matches this problem, transfer its solution, then list where the mapping breaks. Prefer distant analogies over near ones.
socratic|drill into the gap|why,unclear,gap,unknown,understand,explain,definition,what is,how does,root cause|Do not answer yet. Ask the 3 sharpest questions whose answers would close the knowledge gap. For each, state what you'd accept as a sufficient answer. Then attempt to answer them from evidence.
redteam|assume it will be attacked|risk,security,attack,exploit,threat,fail,break,abuse,vulnerab,what could go wrong|Be the adversary. Enumerate how this fails, is abused, or is attacked. Rank by likelihood x impact. For the top failure, write the exact trigger sequence. Assume the user is wrong about what's safe.
compositor|integrate into one whole|synthesi,integrate,combine,merge,unify,reconcile,consolidate,bring together,across|Synthesize, don't list. Find the single frame that holds these pieces together. Surface contradictions between sources and resolve or flag them explicitly. Output one coherent model, not a summary of parts.
provenance|check before you redo|have we,already,before,previously,redo,duplicate,reprocess,done this,prior,existing|Check history first. Search for prior work on this exact input/task before proposing new work. Cite the prior memory or state none exists. Treat path != identity; prefer content hashes. Never silently reprocess.
bridger|connect the unconnected|connect,bridge,relate,link,interdisciplin,cross-domain,unexpected,what if these,intersection|Seek the non-obvious connection. Take two unrelated elements and find the mechanism that could link them. Favour connections that predict something testable over ones that merely sound clever.
yagni|delete before you add|architecture,build,add,feature,abstraction,framework,refactor,should we,design decision,scale|Default to NOT building it. Walk the ladder: does it need to exist, does stdlib/an installed dep cover it, is it one line? Recommend the smallest thing that works. Deletion beats addition; boring beats clever.
inversion|solve it backwards|stuck,blocked,can't,approach,strategy,how to achieve,goal,plan,best way|Invert the problem. Instead of "how do I achieve X", ask "what guarantees I FAIL at X" and avoid those. Work backward from the desired end state to the present. Name the one constraint that dominates.
steelman|argue the other side|disagree,wrong,bad idea,oppose,critique,review,evaluate,decision,trade-off,versus|Before critiquing, state the strongest version of the position you're about to oppose — better than its author put it. Only then argue against it. Separate "I disagree" from "this is incoherent".)catalog";
            for (const auto& line : lines(catalog)) {
                auto fields = std::vector<std::string>();
                std::istringstream stream(line);
                for (std::string field; std::getline(stream, field, '|');)
                    fields.push_back(field);
                if (fields.size() != 4) continue;
                queue(p, "remember",
                      {{"content", "[persona] name:" + fields[0] + " style:" + fields[1] +
                                       " triggers:" + fields[2] + " injection:" + fields[3]},
                       {"type", "wisdom"},
                       {"tags", {"persona", fields[0]}},
                       {"realm", "brahman"},
                       {"visibility", 2}});
                p["stdout"] = str(p, "stdout") + "seeded persona:" + fields[0] + "\n";
            }
        } else {
            for (auto& c : task)
                if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            int best = 0, best_n = 9999;
            std::string injection;
            for (const auto& line : lines(recalled)) {
                if (line.find("[persona]") == std::string::npos) continue;
                auto begin = line.find("triggers:"), end = line.find(" injection:");
                if (begin == std::string::npos || end == std::string::npos) continue;
                std::istringstream stream(line.substr(begin + 9, end - begin - 9));
                int score = 0, n = 0;
                for (std::string trigger; std::getline(stream, trigger, ',');) {
                    trigger = hook_ledger::trim(trigger);
                    for (auto& c : trigger)
                        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
                    if (trigger.empty()) continue;
                    ++n;
                    if (task.find(trigger) != std::string::npos) ++score;
                }
                if (score > best || (score == best && score > 0 && score * best_n > best * n)) {
                    best      = score;
                    best_n    = n;
                    injection = line.substr(end + 11);
                }
            }
            if (best > 0 && !injection.empty()) p["stdout"] = "PERSONA: " + injection + "\n";
        }
    } else if (family == "run-ledger") {
        const auto records = invoke("recall", {{"query", "session realm"}, {"limit", 5}})
                                 .at("structured")
                                 .value("results", json::array());
        std::map<std::string, int> counts;
        int n = 0;
        for (const auto& row : records)
            if (!str(row, "realm").empty() && n++ < 5) ++counts[str(row, "realm")];
        std::string chosen = "unknown";
        int best           = 0;
        for (const auto& [name, count] : counts)
            if (count >= best) {
                best   = count;
                chosen = name;
            }
        const std::vector<std::pair<std::string, std::string>> rules = {
            {"coding",
             R"rx("name":"(Edit|Write|symbol_patch|file_patch)"|cargo|cmake|pytest|compile)rx"},
            {"research",
             R"rx("name":"(WebSearch|WebFetch|web_search|lit_search|paper_fetch)"|arxiv|biorxiv|literature)rx"},
            {"analysis", "analy[sz]|dataframe|plot|statistic|correlation|benchmark"},
            {"distillation", R"(distill|synthesi[sz]|\[wisdom\]|dream-sweep)"}};
        std::string type = "unknown";
        best             = 0;
        for (const auto& [name, pattern] : rules) {
            int count = 0;
            for (const auto& line : lines(str(a, "transcript")))
                if (match(line, pattern.c_str(), true)) ++count;
            if (count > best) {
                best = count;
                type = name;
            }
        }
        // The former query-less recall was rejected by the current RPC and
        // always produced zero. Keep that accounting placeholder until a
        // session-scoped wisdom count exists; do not issue an invalid request.
        constexpr size_t wisdoms = 0;
        const auto content       = "[run-ledger] session:" + str(input, "session_id") +
                             " realm:" + chosen + " task_type:" + type +
                             " genome_id:" + str(a, "genome", "none") +
                             " n_wisdoms:" + std::to_string(wisdoms) +
                             " g0_delta:0.0 ts:" + str(a, "timestamp") + " status:complete";
        queue(p, "remember",
              {{"content", content},
               {"type", "signal"},
               {"tags", {"run-ledger", "provenance"}},
               {"realm", "brahman"},
               {"visibility", 1}});
        if (a.value("has_entries", false)) {
            int count = 0;
            for (const auto& row :
                 invoke("recall", {{"query", "[run-ledger]"}, {"type", "signal"}, {"limit", 10}})
                     .at("structured")
                     .value("results", json::array()))
                if (str(row, "text").rfind("[run-ledger]", 0) == 0) ++count;
            p["stdout"] = count >= 5 ? "true\n" : "false\n";
        }
    } else
        throw std::invalid_argument("unknown ancillary family");
    return p;
}
} // namespace chitta::hook_policy
