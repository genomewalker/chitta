#pragma once
#include <chitta/prompt_policy.hpp>
#include <ctime>

namespace chitta::hook_ledger {
using json = nlohmann::json;
inline json metadata(const json& row) {
    if (!row.is_object()) return json::object();
    auto result = json::parse(row.value("metadata_json", "{}"), nullptr, false);
    return result.is_object() ? result : json::object();
}
inline std::string str(const json& object, const char* field, const std::string& fallback = "") {
    auto it = object.find(field);
    return it != object.end() && it->is_string() ? it->get<std::string>() : fallback;
}
inline std::string join(const json& values, const std::string& separator) {
    std::string out;
    bool first = true;
    for (const auto& value : values) {
        if (!first) out += separator;
        first = false;
        if (value.is_string()) out += value.get<std::string>();
    }
    return out;
}
inline std::string handoff_card(const json& rows, const std::string& project,
                                 const std::string& branch) {
    json latest;
    for (const auto& row : rows) {
        const auto meta = metadata(row);
        const auto capsule = meta.value("handoff", json::object());
        if (!capsule.is_object() || capsule.value("version", 0) != 1
            || str(capsule, "project_dir") != project || str(capsule, "branch") != branch) continue;
        if (latest.is_null() || capsule.value("saved_at", 0.0) >= latest.value("saved_at", 0.0)) latest = capsule;
    }
    if (latest.is_null() || !latest.value("verified", false) || str(latest, "next_action").empty()) return "";
    const auto source = latest.value("source", json::object());
    return "[handoff]\nNext action: " + str(latest, "next_action")
        + "\nBranch: " + branch + "\nArtifacts: " + join(latest.value("artifact_paths", json::array()), ", ")
        + "\nBlocker: " + (str(latest, "blocker").empty() ? "none recorded" : str(latest, "blocker"))
        + "\nSource: " + str(source, "kind", "null") + " (session " + str(source, "session_id", "null") + ")\n[/handoff]";
}
inline json capsule(const json& input, const json& session, const json& thread, double saved_at) {
    std::string action = str(input, "next_action"), source = "visible_plan", tid;
    if (action.empty()) {
        tid = str(session, "thread_id");
        if (!tid.empty()) {
            const auto meta = metadata(thread);
            auto next = meta.value("next_action", json());
            if (next.is_null() && meta.contains("next_steps") && meta["next_steps"].is_array() && !meta["next_steps"].empty())
                next = meta["next_steps"][0];
            if (next.is_string()) action = next.get<std::string>().substr(0, 400);
            source = "ledger_thread";
        }
    }
    auto paths = input.value("artifact_paths", json::array());
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    if (paths.size() > 20) paths.erase(paths.begin() + 20, paths.end());
    return {{"version", 1}, {"next_action", action}, {"verified", !action.empty()},
        {"branch", str(input, "branch")}, {"project_dir", str(input, "project_dir")},
        {"artifact_paths", paths}, {"blocker", str(input, "blocker")},
        {"source", {{"kind", source}, {"session_id", str(input, "session_id")}, {"thread_id", tid}}},
        {"saved_at", saved_at}};
}
inline std::string task_card(const json& inbox, const json& threads, const std::string& realm) {
    std::string out;
    if (!inbox.empty()) {
        out = "\n━━━ inbox (" + (realm.empty() ? "all" : realm) + ") ━━━";
        for (size_t i = 0; i < std::min<size_t>(5, inbox.size()); ++i) {
            const auto type = str(inbox[i], "event_type");
            out += "\n" + std::string(type == "completed" ? "✓" : type == "failed" || type == "failure" ? "✗" : "•")
                + " " + prompt_policy::prefix(str(inbox[i], "digest"), 110);
        }
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    if (!threads.empty()) {
        if (!out.empty()) out += "\n";
        out += "\n━━━ active threads ━━━";
        for (size_t i = 0; i < std::min<size_t>(3, threads.size()); ++i)
            out += "\n  ⟳  " + str(threads[i], "title", "?") + " [" + prompt_policy::prefix(str(threads[i], "thread_id"), 8) + "]";
    }
    return out;
}
inline std::string trim(std::string value) {
    const auto begin = value.find_first_not_of(" \t");
    if (begin == std::string::npos) return "";
    return value.substr(begin, value.find_last_not_of(" \t") - begin + 1);
}
inline json ledger_card(const json& ledger, const std::string& source, double now) {
    const auto mood = str(ledger, "mood");
    const bool compact = source == "compact" || mood == "pre-compact";
    bool clear = false;
    if (source == "clear" && (mood == "in_progress" || mood == "pre-compact")) {
        struct tm parsed {};
        const auto updated = str(ledger, "updated_at");
        const auto end = strptime(updated.c_str(), "%Y-%m-%dT%H:%M:%SZ", &parsed);
        if (end && !*end) clear = now - timegm(&parsed) < 14400;
    }
    std::string card;
    const auto snapshot = str(ledger, "snapshot");
    if (compact) {
        card = "\n[session-restored]\n";
        auto section = [&](const char* key, const char* heading, const char* bullet, bool leading = true) {
            const auto values = ledger.value(key, json::array());
            std::string body;
            for (const auto& value : values) {
                std::string raw = value.is_string() ? value.get<std::string>() : value.dump();
                if (std::string(key) == "todos") raw = "[" + str(value, "status", "null") + "] " + str(value, "content", "null");
                std::istringstream lines(raw);
                for (std::string line; std::getline(lines, line);) {
                    line = trim(line);
                    if (!line.empty()) body += std::string(bullet) + line + "\n";
                }
            }
            if (!body.empty()) card += std::string(leading ? "\n" : "") + heading + "\n" + body;
        };
        section("active_files", "Files in context:", "  - ", false);
        section("decisions", "Decisions made:", "  - ");
        section("todos", "Tasks:", "  ");
        section("blockers", "Blockers:", "  ! ");
        section("discoveries", "Discoveries:", "  * ");
        if (prompt_policy::prefix(snapshot, 20) != snapshot)
            card += "\nLast context:\n" + (snapshot + "\n").substr(0, 500) + "\n";
        card += "[/session-restored]\n\n";
    } else if (clear) {
        std::string goal = snapshot.substr(0, snapshot.find('\n'));
        std::istringstream lines(snapshot);
        for (std::string line; std::getline(lines, line);)
            if (line.rfind("Goal:", 0) == 0) { goal = trim(line.substr(5)); break; }
        goal = goal.substr(0, 200);
        const auto steps = ledger.value("next_steps", json::array());
        const auto next = !steps.empty() && steps[0].is_string() ? steps[0].get<std::string>().substr(0, 150) : "";
        auto files = ledger.value("active_files", json::array());
        if (files.size() > 5) files.erase(files.begin() + 5, files.end());
        card = "[last-session]";
        if (!goal.empty()) card += "\nPrevious task: " + goal;
        if (!next.empty()) card += "\nNext step: " + next;
        if (!files.empty()) card += "\nActive files: " + join(files, ", ");
        if (!str(ledger, "updated_at").empty()) card += "\nSaved: " + str(ledger, "updated_at");
        card += "\nRun /recap for full context. [/last-session]\n";
    } else if (!str(ledger, "session_id").empty()) {
        card = "[ledger] " + str(ledger, "session_id") + " (" + mood + ")\n";
    }
    return {{"card", card}, {"post_compact", compact}, {"post_clear", clear}, {"ledger", ledger}};
}
} // namespace chitta::hook_ledger
