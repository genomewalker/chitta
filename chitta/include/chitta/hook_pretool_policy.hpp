#pragma once
#include <chitta/hook_compact_policy.hpp>
#include <chitta/hook_saddle_policy.hpp>
#include <filesystem>

namespace chitta::hook_policy {
inline json pretool(const json& a, const Invoke& invoke) {
    auto result           = plan();
    result["state"]       = json::object();
    result["shadow"]      = json::array();
    const auto input      = a.value("input", json::object()),
               tool_input = input.value("tool_input", json::object()),
               state = a.value("state", json::object()), env = a.value("settings", json::object());
    const auto tool = str(a, "matcher"), sid = str(input, "session_id"), realm = str(a, "realm");
    auto opt     = [&](const char* key, const char* fallback) { return str(env, key, fallback); };
    auto integer = [&](const char* key, int fallback) {
        try {
            return std::stoi(opt(key, std::to_string(fallback).c_str()));
        } catch (...) {
            return fallback;
        }
    };
    auto count = [&](const std::string& key) {
        try {
            return std::stoi(state.value(key, std::string("0")));
        } catch (...) {
            return 0;
        }
    };
    auto value   = [&](const std::string& key) { return state.value(key, std::string()); };
    json output  = {{"hookEventName", "PreToolUse"}};
    auto context = [&](const std::string& text) {
        if (text.empty()) return;
        auto prior                  = str(output, "additionalContext");
        output["additionalContext"] = prior + (prior.empty() ? "" : "\n") + text;
    };
    auto finish = [&]() {
        if (output.size() > 1)
            result["stdout"] = json{{"hookSpecificOutput", output}}.dump() + "\n";
        return result;
    };
    // Hard stop: deny every tool call once the last request's context (input +
    // cache read + cache creation) crosses CHITTA_CONTEXT_HARD_STOP, until a
    // compaction or fresh session brings the transcript's last usage back down.
    // A small allowlist keeps the handoff path itself usable while denied.
    const auto hard_stop_limit = integer("CONTEXT_HARD_STOP", 0);
    if (hard_stop_limit > 0) {
        const auto usage = a.value("last_usage", json::object());
        const long long total = usage.value("input_tokens", 0LL) +
                                 usage.value("cache_read_input_tokens", 0LL) +
                                 usage.value("cache_creation_input_tokens", 0LL);
        bool allowlisted = tool == "mcp__chitta__checkpoint" || tool == "mcp__chitta__remember";
        if (!allowlisted && tool == "Agent")
            allowlisted = str(tool_input, "subagent_type") != "fork";
        if (!allowlisted && tool == "Bash")
            allowlisted = match(str(tool_input, "command"),
                                R"(^\s*chitta\s+(remember|checkpoint|ledger_op|msg_send|msg_inbox|msg_ack|session_list)(\s|$))");
        if (!allowlisted && total > hard_stop_limit) {
            output["permissionDecision"] = "deny";
            output["permissionDecisionReason"] =
                "[hard-stop] context " + std::to_string(total / 1000) + "k per request exceeds " +
                std::to_string(hard_stop_limit / 1000) +
                "k: write the handoff (chitta remember ... --tags handoff) and /compact or start "
                "a fresh session";
            return finish();
        }
    }
    if (tool == "Bash") {
        const auto command = str(tool_input, "command");
        if (command.empty()) return result;
        std::string advisory;
        if (match(command, R"((python3?|bash)\s+(/tmp/|/maps/[^\s]*/scratch/)[^\s]+\.(py|sh))"))
            advisory = "[code-intel] Temp patch script detected. Use the Edit tool directly — no "
                       "script needed.";
        else if (match(command, R"rx(python3?\s+-c\s+['"])rx") &&
                 match(command,
                       R"rx(open\([^)]*['"][wa]['"]|\.write_text\(|Path\([^)]*\)\.write\()rx"))
            advisory =
                "[code-intel] Inline Python file-write detected. Use the Edit tool directly.";
        if (match(command, R"((^|[;&|][\s]*)find[\s])") &&
            !match(command, R"(-maxdepth\s+[0-3]\b)") && opt("DEEP_SEARCH", "0") != "1" &&
            !match(command, R"((^|\s)(CHITTA|CC_SOUL)_DEEP_SEARCH=1(\s|$))")) {
            const auto target = found(command, R"(\bfind\s+(\S+))", true, 1);
            if (target.empty() || target[0] != '/' || target == "/") {
                if (opt("STRICT_MODE", "0") == "1" && match(command, R"(^\s*find\s+/(\s|$))")) {
                    output["permissionDecision"] = "deny";
                    output["permissionDecisionReason"] =
                        "[strict] Root-wide find is blocked. Scope to project/cwd (e.g., find . "
                        "-maxdepth 3 ...) or set CHITTA_DEEP_SEARCH=1 when intentional.";
                    return finish();
                }
                auto term = found(command, R"(-i?name\s+(\S+))", true, 1);
                term.erase(std::remove_if(term.begin(), term.end(),
                                          [](char c) {
                                              return std::string("\"'*?[]").find(c) !=
                                                     std::string::npos;
                                          }),
                           term.end());
                std::string directory = ".", why;
                if (!term.empty()) {
                    const auto symbols = invoke("find_symbol", {{"name", term}})
                                             .value("structured", json::object())
                                             .value("symbols", json::array());
                    if (!symbols.empty()) {
                        const auto file = str(symbols[0], "file");
                        std::error_code error;
                        if (!file.empty() && std::filesystem::exists(file, error)) {
                            output["permissionDecision"] = "deny";
                            output["permissionDecisionReason"] =
                                "[memory-first] chitta knows " + term + ": " + file +
                                " — use read_symbol/smart_context instead of find.";
                            return finish();
                        }
                        const auto parent = std::filesystem::path(file).parent_path();
                        if (!parent.empty() && std::filesystem::is_directory(parent, error)) {
                            directory = parent.string();
                            why =
                                "[memory-first] No exact match but memory hints at " + directory +
                                ". Scoped find there (maxdepth 3). CHITTA_DEEP_SEARCH=1 to expand.";
                        }
                    }
                }
                const auto replacement =
                    std::regex_replace(command, std::regex(R"(^(\s*)find\s+(\S+)(\s+.*)?$)"),
                                       "$1find " + directory + " -maxdepth 3$3");
                if (replacement != command) {
                    context(why.empty()
                                ? "[search-strategy] No memory hit for " +
                                      (term.empty() ? "(pattern)" : term) +
                                      ". Scoped to cwd -maxdepth 3. CHITTA_DEEP_SEARCH=1 to expand."
                                : why);
                    output["updatedInput"] = {{"command", replacement}};
                    return finish();
                }
            }
        }
        result["track_command"] = match(
            command,
            R"((^|\s)(sbatch|srun|bsub|qsub|nohup|screen|tmux\s+new|snakemake|nextflow)\s|(^|\s)(python3?|Rscript|julia|perl)\s+\S+\.(py|R|jl|pl)|(^|\s)bash\s+\S+\.sh(\s|$)|(^|\s)\./\S+\.(sh|py|R)(\s|$))");
        auto episode = hook_saddle::detect({{"session_id", sid},
                                            {"cmd", command},
                                            {"now_ms", a.value("now_ms", 0LL)},
                                            {"events", a.value("events", json::array())}});
        if (episode.is_object()) {
            const auto identity = str(episode, "saddle_id");
            const auto seen     = lines(value(".saddle_" + sid));
            if (std::find(seen.begin(), seen.end(), identity) == seen.end()) {
                result["notice"] = {{"name", ".saddle_" + sid}, {"identity", identity}};
                context(str(episode, "message"));
                context(advisory);
                return finish();
            }
        }
        context(advisory);
        if (!str(input, "agent_id").empty() && opt("SUBAGENT_BASH_RECALL", "0") != "1")
            return finish();
        if (!sid.empty()) {
            const auto sentinel =
                ".soul_injected_" + sid + "_" + std::to_string(count(".turn_index_" + sid));
            if (state.contains(sentinel)) return finish();
            result["state"][sentinel] = "";
        }
        std::string query;
        std::vector<std::string> tags;
        if (match(command, "Rscript|R --vanilla|R -e|module.*load.*R", true)) {
            query = "R environment conda activation";
            tags  = {"correction"};
        } else if (match(command, R"(python3?\s|pip\s|conda activate)", true)) {
            query = "python conda environment";
            tags  = {"correction"};
        } else if (match(command, "chittad|daemon", true)) {
            query = "chittad daemon startup";
            tags  = {"correction", "gotcha"};
        } else if (match(command, "git push|git commit|git rebase", true)) {
            query = "git push commit";
            tags  = {"gotcha"};
        } else if (match(command, "nohup|&$", true)) {
            query = "background daemon nohup";
            tags  = {"correction"};
        } else if (match(command, R"(release\.sh|version)", true)) {
            query = "release version bump";
            tags  = {"gotcha"};
        } else if (match(command, "cmake|make|build", true)) {
            query = "build cmake";
            tags  = {"solution"};
        }
        std::string memories;
        double minimum = 0.6;
        try {
            minimum = std::stod(opt("PRETOOL_MIN_SIM", "0.6"));
        } catch (...) {}
        if (!query.empty() && !realm.empty())
            for (const auto& tag : tags) {
                auto rows = invoke("recall",
                                   {{"query", query}, {"tag", tag}, {"realm", realm}, {"limit", 1}})
                                .value("structured", json::object())
                                .value("results", json::array());
                std::string text;
                for (const auto& row : rows)
                    if (row.value("similarity", 0.) >= minimum) text += str(row, "text") + "\n";
                text = bytes(text, 400);
                if (!text.empty()) memories += text + "\\n";
            }
        if (!memories.empty()) context("⚠️ BEFORE RUNNING: " + memories);
    } else if (tool == "Read") {
        const auto file = str(tool_input, "file_path");
        if (file.empty() || !a.value("file_exists", false)) return result;
        const auto parent  = std::filesystem::path(file).parent_path().string(),
                   base    = std::filesystem::path(file).filename().string();
        const auto symbols = invoke("code_context", {{"path", parent}})
                                 .value("structured", json::object())
                                 .value("dir_symbols", 0);
        const auto length = a.value("line_count", 0), offset = tool_input.value("offset", 0);
        const bool indexed = symbols > 0;
        std::string advisory, traces;
        if (indexed)
            advisory = "[code-intel] File is indexed in chitta (" + std::to_string(symbols) +
                       " symbols in dir). For large files prefer smart_context(task) → "
                       "read_symbol(file,symbol) over full Read. Whole-symbol rewrites: "
                       "symbol_patch(file,symbol,body).";
        const auto trace_name  = ".trace_cache_" + sid;
        const auto trace_cache = lines(value(trace_name));
        if (opt("FILE_TRACES", "1") != "0" && !sid.empty() &&
            std::find(trace_cache.begin(), trace_cache.end(), file) == trace_cache.end()) {
            result["state"][trace_name] =
                value(trace_name) + (value(trace_name).empty() ? "" : "\n") + file + "\n";
            if (!realm.empty()) {
                auto rows = invoke("recall", {{"query", base},
                                              {"strategy", "keyword"},
                                              {"realm", realm},
                                              {"limit", 6},
                                              {"no_learn", true}})
                                .value("structured", json::object())
                                .value("results", json::array());
                std::vector<std::string> matches;
                for (const auto& row : rows) {
                    auto type = str(row, "type"), text = str(row, "text");
                    if ((type != "correction" && type != "wisdom" && type != "signal" &&
                         type != "preference") ||
                        text.find(base) == std::string::npos)
                        continue;
                    for (auto& c : text)
                        if (c == '\n' || c == '"' || c == '\\') c = ' ';
                    const auto id = row.value("id", json());
                    matches.push_back("#" + (id.is_string() ? id.get<std::string>() : id.dump()) +
                                      " [" + type + "] " + prompt_policy::prefix(text, 160));
                    if (matches.size() == 2) break;
                }
                if (!matches.empty())
                    traces = " [traces] anchored on " + base + ": " + joined(matches, " | ");
            }
            advisory += traces;
        }
        const bool allow  = opt("ALLOW_READ", "0") == "1" || state.contains(".allow_read_" + sid);
        const bool system = match(
            file,
            R"(/site-packages/|/dist-packages/|/conda/envs/[^/]+/lib/|^/usr/lib/|^/opt/.*?/lib/)");
        auto shadow = [&](const char* decision, const char* reason, int enforced) {
            result["shadow"].push_back({{"tool", "Read"},
                                        {"file", file},
                                        {"lines", length},
                                        {"indexed", int(indexed)},
                                        {"decision", decision},
                                        {"reason", reason},
                                        {"enforced", enforced}});
        };
        const bool strict = opt("STRICT_MODE", "") == "1" ||
                            (opt("STRICT_MODE", "") != "0" && a.value("strict_marker", false));
        if (strict && indexed && !allow && !system) {
            shadow("advisory", "strict-indexed-read", 0);
            context(
                "[code-intel] Indexed file. Prefer mcp__chitta-bridge__read_symbol/smart_context "
                "over full Read for large files.");
        }
        if (length > 200 && !sid.empty() && !allow && !system) {
            const auto key = ".read_cache_" + sid, hash = str(a, "file_hash");
            if (value(key).find(hash) != std::string::npos && !hash.empty()) {
                const auto enforce_key = ".enforce_" + sid;
                bool enforce           = opt("HOOK_ENFORCE", "") == "1";
                if (opt("HOOK_ENFORCE", "").empty()) {
                    if (state.contains(enforce_key))
                        enforce = value(enforce_key) == "1";
                    else {
                        enforce =
                            a.value("shadow_count", 0) >= 100 &&
                            a.value("shadow_first_ts", 0LL) > 0 &&
                            a.value("now", 0LL) - a.value("shadow_first_ts", 0LL) >= 3 * 86400;
                        result["state"][enforce_key] = enforce ? "1\n" : "0\n";
                    }
                }
                if (enforce) {
                    shadow("truncate", "read-dedup", 1);
                    context("[read-dedup] " + base + " already read this session (" +
                            std::to_string(length) +
                            " lines) — returning 40. Use sqz_read_file for the full cached content "
                            "(13-token §ref§), or Read with offset for a specific range.");
                    output["updatedInput"] = {
                        {"file_path", file}, {"limit", 40}, {"offset", offset}};
                    return finish();
                }
                shadow("advisory", "read-dedup", 0);
                context("[read-dedup] " + base + " already read this session (" +
                        std::to_string(length) +
                        " lines). sqz_read_file returns a 13-token §ref§ for cached content.");
            }
            result["state"][key] = value(key) + (value(key).empty() ? "" : "\n") + hash + "\n";
        }
        if (length <= 200) {
            shadow("pass", "small-file", 0);
            context(advisory);
            return finish();
        }
        if (indexed && offset == 0) {
            shadow("advisory", "indexed-large-offset0", 0);
            advisory = "[code-intel] Large indexed file (" + std::to_string(length) +
                       " lines). Prefer read_symbol or smart_context for targeted extraction." +
                       traces;
        }
        shadow("truncate", "large-file", 0);
        context("[hook] Large file (" + std::to_string(length) +
                " lines). Reading first 150. Use offset for later sections." +
                (advisory.empty() ? "" : " " + advisory));
        output["updatedInput"] = {{"file_path", file}, {"limit", 150}, {"offset", offset}};
    } else if (tool == "Grep") {
        if (str(tool_input, "output_mode", "files_with_matches") != "content") return result;
        auto updated           = tool_input;
        updated["head_limit"]  = 50;
        output["updatedInput"] = updated;
        context("[hook] Grep content mode — capped at 50 matches.");
    } else if (tool == "Glob") {
        if (str(tool_input, "pattern").empty()) return result;
        auto updated           = tool_input;
        updated["head_limit"]  = 100;
        output["updatedInput"] = updated;
    } else if (tool == "Write") {
        const auto file = str(tool_input, "file_path");
        if (match(file, R"(\.(py|sh)$)") &&
            match(file, R"(^/tmp/|/scratch/|/tmp/|patch|fix_|edit_|_fix\.|_edit\.)") &&
            match(str(tool_input, "content"),
                  R"rx(open\([^)]*['"][wa]['"]|\.write_text\(|Path\([^)]*\)\.write\(|\.write\()rx"))
            context("[code-intel] Temp patch script detected. Use the Edit tool directly — no "
                    "script needed.");
        result["write_guard"] = true;
    } else if (tool == "Agent") {
        const auto subtype = str(tool_input, "subagent_type");
        if (str(tool_input, "model").empty() && subtype != "fork" &&
            opt("AGENT_NO_FORCE", "0") != "1" &&
            match(subtype + " " + str(tool_input, "description"),
                  "explore|search|find|research|grep|glob|read|locate|list|lookup|where|enumerate|"
                  "check if",
                  true)) {
            auto updated           = tool_input;
            updated["model"]       = "haiku";
            updated["prompt"]      = "Report in ≤200 words.\\n\\n" + str(tool_input, "prompt");
            output["updatedInput"] = updated;
            context("[token-route] lookup→haiku (4.5) + ≤200 word limit. Pass model=sonnet when "
                    "the agent must reason, model=opus for architecture, model=fable to match the "
                    "orchestrator; subagent_type=fork always inherits fable.");
            return finish();
        }
        if (!sid.empty()) {
            const auto n = count(".subagent_count_" + sid) + 1, limit = integer("AGENT_LIMIT", 50),
                       warn                           = integer("AGENT_WARN", 20);
            result["local"][".subagent_count_" + sid] = std::to_string(n) + "\n";
            if (n > limit)
                context("[agent-budget] " + std::to_string(n) + "/" + std::to_string(limit) +
                        " subagents. Each cold-starts a cache (~$5-50). Batch work or /recap for "
                        "fresh session.");
            else if (n > warn)
                context("[agent-budget] " + std::to_string(n) +
                        " subagents this session. Batch independent queries where possible.");
        }
    } else if (tool == "ScheduleWakeup" && !sid.empty()) {
        const auto n                            = count(".wakeup_count_" + sid) + 1;
        result["state"][".wakeup_count_" + sid] = std::to_string(n) + "\n";
        if (str(tool_input, "prompt") == "<<autonomous-loop-dynamic>>") {
            const auto loops = count(".loop_count_" + sid) + 1, limit = integer("LOOP_LIMIT", 20),
                       warn                       = integer("LOOP_WARN", 10);
            result["state"][".loop_count_" + sid] = std::to_string(loops) + "\n";
            if (loops > limit) {
                output["permissionDecision"] = "block";
                result["exit_code"]          = 2;
                context(
                    "[loop-budget] " + std::to_string(loops) +
                    " autonomous loop iterations this session (limit " + std::to_string(limit) +
                    "). Use /compact then restart the loop, or set CHITTA_LOOP_LIMIT=N to raise.");
            } else if (loops > warn)
                context("[loop-budget] " + std::to_string(loops) +
                        " autonomous loop iterations this session (warn at " +
                        std::to_string(warn) + ", limit " + std::to_string(limit) +
                        "). Consider /compact to reset context.");
        } else if (n > 30)
            context("[loop-budget] " + std::to_string(n) +
                    " total wakeups this session. Verify the loop has a termination condition.");
    }
    return finish();
}
} // namespace chitta::hook_policy
