#pragma once
#include <chitta/hook_compact_policy.hpp>
#include <filesystem>

namespace chitta::hook_policy {
inline std::string scalar(const json& value) {
    return value.is_string() ? value.get<std::string>() : value.dump();
}
// Preserve the CLI --text-only contract used by the former envelope.
inline std::string text_only(const json& response) {
    const auto data = response.at("structured");
    std::string text;
    if (data.contains("results") && data["results"].is_array()) {
        for (const auto& item : data["results"])
            if (item.contains("text")) text += str(item, "text") + "\n";
    } else if (data.contains("text"))
        text = str(data, "text") + "\n";
    while (!text.empty() && text.back() == '\n')
        text.pop_back();
    return text;
}
inline json session_start(const json& a, const Invoke& invoke) {
    auto p           = plan();
    p["state"]       = json::object();
    const auto input = a.value("input", json::object()), local = a.value("local", json::object());
    const auto sid = str(input, "session_id"), realm = str(a, "realm", "brahman"),
               project = str(a, "project_dir"), source = str(input, "source", "startup");
    const auto now = a.value("now", 0LL);
    std::string output;
    const bool subagent = source == "startup" && a.value("subagent_age", 999LL) < 30;
    if (source != "compact") {
        p["local"][".session_active"]        = nullptr;
        p["local"][".gaps_surfaced"]         = nullptr;
        p["local"][".subagent_count_" + sid] = nullptr;
        p["reset_patterns"]                  = true;
    }
    const auto strict = str(a, "strict_default", "1");
    if (strict == "1") p["local"][".strict_claude_style"] = "enabled " + str(a, "utc") + "\n";
    if (strict == "0") p["local"][".strict_claude_style"] = nullptr;
    if (!sid.empty()) {
        p["state"][".last_store_turn_" + sid] = str(a, "turn", "0") + "\n";
        auto tid                              = str(input, "thread_id");
        if (tid.empty())
            tid = str(invoke("ledger_op", {{"op", "session_get"}, {"args", {{"session_id", sid}}}})
                          .at("structured")
                          .value("value", json::object()),
                      "thread_id");
        queue(p, "session_register",
              {{"session_id", sid},
               {"realm", realm},
               {"pid", a.value("pid", 0)},
               {"project_dir", project.empty() ? str(a, "cwd") : project},
               {"transcript_path", str(input, "transcript_path")},
               {"metadata",
                {{"client", "claude"},
                 {"model", str(input, "model", str(a, "model"))},
                 {"host", str(a, "host")},
                 {"thread_id", tid},
                 {"hook_source", source}}}});
        if (!tid.empty())
            queue(p, "ledger_op",
                  {{"op", "lease_claim"}, {"args", {{"session_id", sid}, {"thread_id", tid}}}});
        if (!str(input, "transcript_path").empty())
            queue(p, "transcript_register",
                  {{"session_id", sid},
                   {"transcript_path", str(input, "transcript_path")},
                   {"realm", realm}});
    }
    if (subagent) return p;
    if (a.value("has_transcript", false)) queue(p, "distill_trigger", {{"session_id", sid}});
    for (const auto& failed : a.value("failed_observations", json::array()))
        if (!str(failed, "content").empty())
            queue(p, "observe",
                  {{"category", str(failed, "category", "general")},
                   {"content", str(failed, "content")}});
    p["retry_ack"] = true;
    using Future   = std::future<json>;
    std::map<std::string, Future> pending;
    auto launch = [&](const std::string& key, const std::string& tool, json args) {
        pending.emplace(
            key, std::async(std::launch::async, [&, tool, args] { return invoke(tool, args); }));
    };
    auto get = [&](const std::string& key) { return pending.at(key).get(); };
    launch("soul", "soul_context", json::object());
    launch("corrections", "recall", {{"query", "correction"}, {"tag", "correction"}, {"limit", 5}});
    launch("compliance", "recall", {{"query", "compliance:auto user correction"}, {"limit", 2}});
    launch("cache", "recall", {{"query", "cache:break session cache_hit_ratio"}, {"limit", 1}});
    launch("ledger", "ledger_op",
           {{"op", "hook_session_context"},
            {"args", {{"project", realm}, {"source", source}, {"now", now}}}});
    launch("tasks", "ledger_op", {{"op", "hook_task_context"}, {"args", {{"realm", realm}}}});
    if (a.value("project_exists", false)) {
        json args = {{"project_dir", project}, {"limit", 100}, {"branch", str(a, "branch")}};
        if (!str(input, "thread_id").empty()) args["thread_id"] = input["thread_id"];
        launch("handoff", "ledger_op", {{"op", "hook_handoff_context"}, {"args", args}});
    }
    auto loaded         = get("ledger").at("structured");
    p["ledger_profile"] = loaded;
    const auto card = loaded.at("value"), ledger = card.at("ledger");
    if (pending.count("handoff")) {
        auto text = str(get("handoff").at("structured").at("value"), "text");
        if (!text.empty()) output += text + "\n";
    }
    if (card.at("post_compact").get<bool>() || card.at("post_clear").get<bool>())
        output += str(card, "card");
    else {
        if (!realm.empty() && realm != "brahman") {
            auto query = bytes(str(ledger, "snapshot"), 120);
            query      = query.substr(0, query.find('\n'));
            if (query.empty()) query = realm;
            launch("scoped", "recall", {{"query", query}, {"realm", realm}, {"limit", 6}});
            launch("fallback", "recall",
                   {{"query",
                     std::filesystem::path(project.empty() ? realm : project).filename().string() +
                         " " + query},
                    {"limit", 6}});
        }
        auto tasks = str(get("tasks").at("structured").at("value"), "text");
        if (!tasks.empty()) output += tasks + "\n";
        auto soul     = str(get("soul"), "text");
        auto memories = found(soul, "Memory: ([0-9]+)", false, 1),
             triplets = found(soul, "([0-9]+) triplets", false, 1);
        if (!soul.empty() && !memories.empty() && memories != "0")
            output += "[soul] m=" + memories + " t=" + (triplets.empty() ? "0" : triplets) + "\n";
        output += str(card, "card");
        if (pending.count("scoped")) {
            auto body = [&](const json& response) {
                std::vector<std::string> rows;
                for (const auto& line : lines(text_only(response)))
                    if (match(line, R"(^#[0-9]+ \[[0-9]+%\] \[[^\]]+\]\s+\S)") &&
                        !match(line, R"(^#[0-9]+ \[[0-9]+%\] \[episode\])"))
                        rows.push_back(line);
                return joined(rows, "\n");
            };
            auto text = body(get("scoped"));
            if (text.empty()) text = body(get("fallback"));
            if (!text.empty())
                output +=
                    "\n[recall:" + realm + "]\n" + bytes(text + "\n", 1200) +
                    (text.size() + 1 <= 1200 ? "\n" : "") + "\n[/recall:" + realm +
                    "]\n[soul] If context above is sparse for the current task, call "
                    "mcp__chitta__recall or mcp__chitta__smart_context for deeper retrieval.\n";
        }
        std::vector<std::string> corrections;
        auto surfaces = lines(str(local, ".correction_surfaces"));
        for (const auto& row :
             get("corrections").at("structured").value("results", json::array())) {
            auto text = str(row, "text"), state = str(row, "correction_state", "emitted");
            if (match(text, "verified", true) || state == "verified" || state == "applied")
                continue;
            auto id = scalar(row.value("id", json("")));
            if (id.empty() || text.empty()) continue;
            const auto tags = invoke("triplet_history", {{"subject", id}, {"predicate", "tagged"}})
                                  .at("structured")
                                  .value("history", json::array());
            bool suppressed = false;
            for (const auto& tag : tags)
                if (match(str(tag, "object"), "wontfix|verified")) suppressed = true;
            if (suppressed || std::count(surfaces.begin(), surfaces.end(), id) >= 5) continue;
            surfaces.push_back(id);
            corrections.push_back(prompt_policy::prefix(text, 120));
        }
        p["local"][".correction_surfaces"] =
            joined(surfaces, "\n") + (surfaces.empty() ? "" : "\n");
        if (!corrections.empty())
            output += "\n[recent-corrections]\n" +
                      joined(take(lines(joined(corrections, "\n")), 5), "\n") +
                      "\n[/recent-corrections]\n";
        auto compliance = bytes(text_only(get("compliance")), 300);
        if (!compliance.empty() && compliance.find("No memories") == std::string::npos)
            output += "\n[compliance] missed corrections\n";
        auto cache = bytes(text_only(get("cache")), 400);
        if (!cache.empty() && cache.find("No memories") == std::string::npos &&
            cache.find("0 memories") == std::string::npos)
            output += "\n⚠️ BEFORE RUNNING: [cache] Recent cache break detected:\n" +
                      joined(take(lines(cache), 3), "\n") + "\n";
        const auto memory = str(a, "memory_file");
        std::vector<std::string> user;
        bool notes = memory.find("## Project Notes") != std::string::npos, inside = false;
        if (notes)
            for (const auto& line : lines(memory)) {
                if (line.find("## Project Notes") != std::string::npos) inside = true;
                if (inside && line.rfind("##", 0) != 0 && line.rfind("---", 0) != 0)
                    user.push_back(line);
                if (inside && line.find("---") != std::string::npos) inside = false;
            }
        else if (memory.find("Chitta Soul") == std::string::npos)
            for (const auto& line : lines(memory))
                if (line.rfind("#", 0) != 0) user.push_back(line);
        auto text = joined(take(user, 20), "\n");
        while (!text.empty() && text.back() == '\n')
            text.pop_back();
        if (text.size() > 10) {
            const auto name = std::filesystem::path(project).filename().string();
            queue(
                p, "remember",
                {{"content", "[memory:" + name + "] Claude Code MEMORY.md import\n" + text + "\n"},
                 {"tags", {"memory-import", name}}});
            p["stderr"] = "[soul] imported MEMORY.md content\n";
        }
    }
    p["maintenance"] = true;
    p["stdout"]      = output;
    return p;
}
} // namespace chitta::hook_policy
