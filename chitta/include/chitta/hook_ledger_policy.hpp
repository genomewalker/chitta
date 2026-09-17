#pragma once
#include <chitta/prompt_policy.hpp>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <map>
#include <tuple>

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
// One lock covers capsule read/compare/WAL publication, including queued Stop writes.
// Ordinary session metadata updates continue to merge inside TaskLedger's lock.
inline std::recursive_mutex capsule_mutex;
inline std::string canonical_repository(std::string path) {
    if (path.rfind("/maps/projects/", 0) == 0) path.erase(0, 5);
    return std::filesystem::path(path).lexically_normal().string();
}
inline std::string first_line(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::string line;
    std::getline(file, line);
    return line;
}
// Read git metadata without starting a shell or executing repository configuration.
inline json git_identity(const std::string& project) {
    namespace fs = std::filesystem;
    fs::path root(project), dir = root / ".git";
    auto link = first_line(dir);
    if (link.rfind("gitdir: ", 0) == 0) {
        dir = link.substr(8);
        if (dir.is_relative()) dir = root / dir;
    }
    fs::path common = dir;
    const auto common_link = first_line(dir / "commondir");
    if (!common_link.empty()) {
        common = common_link;
        if (common.is_relative()) common = dir / common;
    }
    common = common.lexically_normal();
    std::string head = first_line(dir / "HEAD");
    if (head.rfind("ref: ", 0) == 0) {
        const auto ref = head.substr(5);
        head = first_line(common / ref);
        if (head.empty()) {
            std::ifstream packed(common / "packed-refs");
            for (std::string line; std::getline(packed, line);)
                if (line.size() > 41 && line.substr(41) == ref) head = line.substr(0, 40);
        }
    }
    if (head.size() != 40 && head.size() != 64) head.clear();
    std::string stream = root.filename().string();
    stream = stream.rfind("codex-wt-", 0) == 0 ? stream.substr(9) : "";
    return {{"repository", canonical_repository(common.string())}, {"code_head", head},
            {"stream_id", stream}};
}
inline std::string capsule_key(const json& cap) {
    return canonical_repository(str(cap, "repository")) + "\n" +
        (str(cap, "stream_id").empty() ? "session:" + str(cap, "session_id")
                                      : "stream:" + str(cap, "stream_id"));
}
// UTF-8 byte length is a conservative upper bound for byte-fallback tokenizers.
// Admit complete fields/rows; never cut JSON or a UTF-8 sequence.
inline json latest_by_key(const json& rows, const json& key) {
    json latest = json::object();
    for (const auto& row : rows) {
        const auto cap = metadata(row).value("handoff", json::object());
        if (cap.value("version", 0) == 2 && capsule_key(cap) == capsule_key(key) &&
            cap.value("revision", uint64_t(0)) > latest.value("revision", uint64_t(0))) latest = cap;
    }
    return latest;
}
inline json capsule_manifest(const json& rows, const std::string& repository) {
    std::map<std::string, json> latest;
    for (const auto& row : rows) {
        const auto cap = metadata(row).value("handoff", json::object());
        if (cap.value("version", 0) != 2 || str(cap, "repository") != canonical_repository(repository)) continue;
        const auto key = capsule_key(cap);
        if (!latest.count(key) || cap.value("revision", uint64_t(0)) > latest.at(key).value("revision", uint64_t(0)))
            latest[key] = cap;
    }
    json result = {{"streams", json::array()}, {"omitted", latest.size()}};
    for (const auto& [key, cap] : latest) {
        json row;
        for (auto field : {"stream_id", "session_id", "branch", "code_head", "state", "revision", "next_action"})
            row[field] = cap.at(field);
        auto candidate = result;
        candidate["streams"].push_back(row);
        candidate["omitted"] = result["omitted"].get<size_t>() - 1;
        if (candidate.dump().size() <= 450) result = std::move(candidate);
    }
    return result;
}
inline std::string capsule_card(const json& cap) {
    if (cap.empty()) return "";
    json card;
    for (auto field : {"stream_id", "session_id", "state", "revision", "code_head", "next_action"})
        card[field] = cap.at(field);
    // Detailed constraints, gates and jobs remain available through capsule_get.
    return "[handoff]\n" + card.dump() + "\n[/handoff]";
}
inline void bounded_text(const json& value, size_t bytes, const char* name) {
    if (!value.is_string() || value.get_ref<const std::string&>().size() > bytes)
        throw std::invalid_argument(std::string("capsule field limit: ") + name);
}
inline json capsule_v2(const json& input, uint64_t revision, double now) {
    json out = {{"version", 2}, {"revision", revision}, {"saved_at", now}};
    for (auto [field, limit] : std::vector<std::pair<const char*, size_t>>{
            {"objective", 384}, {"repository", 384}, {"project_dir", 384},
            {"session_id", 128}, {"stream_id", 96}, {"branch", 128},
            {"code_head", 64}, {"next_action", 400}}) {
        out[field] = input.value(field, json(""));
        bounded_text(out[field], limit, field);
    }
    out["repository"] = canonical_repository(str(out, "repository"));
    if (str(out, "repository").empty() ||
        (str(out, "stream_id").empty() && str(out, "session_id").empty()))
        throw std::invalid_argument("capsule repository and stream/session key required");
    out["state"] = input.value("state", "in_progress");
    if (out["state"] != "complete" && out["state"] != "in_progress" &&
        out["state"] != "missing" && out["state"] != "invalidated")
        throw std::invalid_argument("invalid capsule state");
    for (auto [field, count, length] : std::vector<std::tuple<const char*, size_t, size_t>>{
            {"constraints", 4, 128}, {"dirty_paths", 20, 128}, {"blockers", 3, 160}}) {
        out[field] = input.value(field, json::array());
        if (!out[field].is_array() || out[field].size() > count)
            throw std::invalid_argument(std::string("capsule array limit: ") + field);
        for (const auto& value : out[field]) bounded_text(value, length, field);
    }
    out["gates"] = input.value("gates", json::object());
    if (!out["gates"].is_object() || out["gates"].size() > 6)
        throw std::invalid_argument("capsule gate limit");
    for (const auto& [name, gate] : out["gates"].items()) {
        bounded_text(name, 48, "gate name");
        if (!gate.is_object() || gate.size() != 2 || !gate.contains("number") ||
            !gate["number"].is_number() || (str(gate, "status") != "pass" && str(gate, "status") != "fail"))
            throw std::invalid_argument("gate requires status pass/fail and one number");
    }
    out["jobs"] = input.value("jobs", json::array());
    if (!out["jobs"].is_array() || out["jobs"].size() > 4)
        throw std::invalid_argument("capsule job limit");
    for (const auto& job : out["jobs"]) {
        if (!job.is_object() || job.size() != 2) throw std::invalid_argument("invalid capsule job");
        bounded_text(job.at("id"), 64, "job id");
        bounded_text(job.at("result_path"), 192, "job result path");
    }
    // Check serialized UTF-8 including escaping and structure. Never truncate fields.
    if (out.dump().size() > 4096) throw std::invalid_argument("capsule exceeds 4096 bytes");
    return out;
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
        if (!capsule.is_object() || (capsule.value("version", 0) != 1 && capsule.value("version", 0) != 2)
            || str(capsule, "project_dir") != project || str(capsule, "branch") != branch) continue;
        if (latest.is_null() || capsule.value("saved_at", 0.0) >= latest.value("saved_at", 0.0)) latest = capsule;
    }
    if (!latest.is_null() && latest.value("version", 0) == 2) {
        if (str(latest, "state") != "in_progress") return "";
        return "[handoff]\n" + latest.dump() + "\n[/handoff]";
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
inline json prepare_capsule(const json& args, const json& session, const json& thread,
                            const json& old, double now) {
    const auto legacy = capsule(args, session, thread, now);
    auto input = git_identity(args.value("project_dir", ""));
    input["session_id"] = args.at("session_id");
    for (auto field : {"objective", "constraints", "jobs"})
        if (old.contains(field)) input[field] = old[field];
    input["project_dir"] = args.at("project_dir");
    input["branch"] = args.value("branch", "");
    input["next_action"] = legacy.at("next_action");
    input["dirty_paths"] = legacy.at("artifact_paths");
    input["blockers"] = json::array();
    if (!args.value("blocker", "").empty()) input["blockers"].push_back(args.at("blocker"));
    input["state"] = legacy.value("verified", false) ? "in_progress" : "invalidated";
    // A Stop cannot erase an explicit completed milestone at the same code state.
    if (str(old, "code_head") == str(input, "code_head") &&
        old.value("dirty_paths", json::array()) == input["dirty_paths"]) {
        if (old.contains("gates")) input["gates"] = old["gates"];
        if (str(old, "state") == "complete" && str(input, "next_action").empty())
            input["state"] = "complete";
    }
    const auto revision = old.value("revision", uint64_t(0));
    const auto cap = capsule_v2(input, revision + 1, now);
    return {{"op", "session_bind"}, {"args", {{"session_id", args.at("session_id")},
        {"project_dir", args.at("project_dir")}, {"expected_revision", revision},
        {"metadata", {{"handoff", cap}}}}}};
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
