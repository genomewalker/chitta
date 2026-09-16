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

// Lightweight and event checkpoints share the same bounded Stop input.
inline json stop_progress(const json& input) {
    const auto response = str(input, "response"), sid = str(input, "session_id");
    auto clip = [](std::string text, size_t n) {
        text.resize(std::min(text.size(), n));
        while (!text.empty() && text.back() == '\n') text.pop_back();
        return json::parse(json(text).dump(-1, ' ', false, json::error_handler_t::replace)).get<std::string>();
    };
    auto selected = [&](const std::string& pattern, size_t count, size_t bytes) {
        std::istringstream lines(response);
        std::string result;
        for (std::string line; std::getline(lines, line);) {
            if (!prompt_policy::matches(line, pattern.c_str(), true)) continue;
            result += line + "\n";
            if (--count == 0) break;
        }
        return clip(result, bytes);
    };
    std::string snapshot;
    const auto user = clip(str(input, "last_user") + "\n", 300);
    const auto next = selected("next[: ].{10,}|TODO[: ].{10,}", 1, 150);
    if (!user.empty()) snapshot = "Goal: " + user;
    if (!next.empty()) snapshot += "\\nNext: " + next;
    if (!str(input, "saddle").empty()) snapshot += "\n" + str(input, "saddle");
    json writes = json::array({{{"session_id", sid}, {"project", str(input, "realm")},
        {"mood", "in_progress"}, {"snapshot", snapshot}, {"updated_at", str(input, "updated_at")}}});
    std::string mood, event, diagnostics;
    if (input.value("has_error", false)) {
        mood = "debugging";
        event = selected("error|failed|exception", 3, 500);
        diagnostics += "[ledger] error checkpoint triggered\n";
    }
    if (prompt_policy::matches(response, "✓|✅|success|complete|done|shipped|released|merged|tests pass|build succeed", true)) {
        mood = "confident";
        event = selected("success|complete|done|shipped|released|merged|tests pass|build", 3, 500);
        diagnostics += "[ledger] milestone checkpoint triggered\n";
    }
    if (!mood.empty()) writes.push_back({{"session_id", sid}, {"project", str(input, "realm")},
        {"transcript_path", str(input, "transcript_path")}, {"mood", mood}, {"snapshot", event}});
    return {{"writes", writes}, {"diagnostics", diagnostics}};
}

// Stop supplies its bounded visible transcript; assembly is daemon policy.
inline json stop_checkpoint(const json& input) {
    const auto response = str(input, "response"), sid = str(input, "session_id");
    const auto snapshot = input.value("snapshot", json::object());
    const auto files = snapshot.value("files", json::array());
    const auto tools = snapshot.value("tools", json::array());
    int turns = snapshot.value("counts", json::object()).value("assistant", 0);
    if (turns <= 0) turns = (input.value("turn_index", 0) + 1) / 2;
    json decisions = json::array(), blockers = json::array(), discoveries = json::array();
    std::vector<std::string> nonempty, all;
    std::istringstream stream(response);
    for (std::string line; std::getline(stream, line);) {
        all.push_back(line);
        if (!line.empty()) nonempty.push_back(line);
        auto marker = [&](const char* tag, json& destination, size_t limit, bool strip) {
            const auto offset = line.find(tag);
            if (offset == std::string::npos || destination.size() >= limit) return;
            auto value = line.substr(offset + (strip ? std::string(tag).size() : 0));
            if (strip) value.erase(0, value.find_first_not_of(" \t\r\n\f\v"));
            destination.push_back(value);
        };
        marker("[DECISION]", decisions, 10, true);
        marker("[BLOCKER]", blockers, 5, true);
        marker(line.find("[SOLUTION]") < line.find("[GOTCHA]") ? "[SOLUTION]" : "[GOTCHA]", discoveries, 10, false);
    }
    // Command substitution removed trailing blank marker rows before jq -R.
    for (auto* rows : {&decisions, &blockers})
        while (!rows->empty() && rows->back() == "") rows->erase(rows->end() - 1);
    auto trimmed_bytes = [](std::string text, size_t size) {
        // Match jq --arg after head -c cuts a UTF-8 sequence at the byte cap.
        text = json::parse(json(text.substr(0, size)).dump(
            -1, ' ', false, json::error_handler_t::replace)).get<std::string>();
        while (!text.empty() && text.back() == '\n') text.pop_back();
        return text;
    };
    std::string excerpt;
    for (size_t i = nonempty.size() > 20 ? nonempty.size() - 20 : 0; i < nonempty.size(); ++i)
        excerpt += nonempty[i] + "\n";
    excerpt = trimmed_bytes(excerpt, 1000);
    if (excerpt.empty()) {
        for (size_t i = 0; i < std::min<size_t>(3, all.size()); ++i) excerpt += all[i] + "\n";
        excerpt = trimmed_bytes(excerpt, 300);
    }
    std::string mood = "working";
    if (prompt_policy::matches(response, "error|failed|bug|stuck", true)) mood = "debugging";
    else if (prompt_policy::matches(response, "complete|done|finished|shipped|success", true)) mood = "confident";
    else if (input.value("learned", 0) > 0) mood = "learning";
    std::string used;
    for (size_t i = 0; i < tools.size(); ++i) {
        if (i) used += (i % 2 ? "," : " "); // preserve paste -sd ', '
        used += tools[i].get<std::string>();
    }
    used = trimmed_bytes(used + "\n", 200);
    std::string summary, summary_log;
    if (turns >= 3) {
        summary = "[session:" + sid + "] " + mood + "→" + std::to_string(turns) + " turns";
        if (!used.empty()) summary += " | tools: " + prompt_policy::prefix(used, 100);
        const auto saddle = str(input, "saddle");
        if (!saddle.empty()) summary += "\n" + saddle;
        summary_log = "[soul] +session-summary: " + prompt_policy::prefix(summary, 60);
    } else summary_log = "[soul] skip session-summary: too few turns (" + std::to_string(turns) + "<3)";
    json ledger = {{"session_id", sid}, {"project", str(input, "realm")},
        {"transcript_path", str(input, "transcript_path")}, {"mood", mood}, {"active_files", files},
        {"decisions", decisions}, {"todos", json::array()}, {"blockers", blockers},
        {"discoveries", discoveries}, {"snapshot", excerpt}};
    return {{"ledger", ledger}, {"summary", summary}, {"summary_log", summary_log},
        {"ledger_log", "[ledger] queued: " + sid + " (" + mood + ", files="
            + std::to_string(files.size()) + " decisions=" + std::to_string(decisions.size()) + " todos=0)"}};
}
} // namespace chitta::hook_ledger
