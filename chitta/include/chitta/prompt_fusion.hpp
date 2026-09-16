#pragma once
#include <chitta/prompt_policy.hpp>

namespace chitta::prompt_policy {
inline std::string trim_newlines(std::string text) {
    while (!text.empty() && text.back() == '\n') text.pop_back();
    return text;
}
inline std::string c2_from_text(const std::string& text) {
    std::istringstream lines(text);
    std::smatch match;
    const std::regex signal("^Found .*\\(maxrel ([0-9]+)%\\)");
    for (std::string line; std::getline(lines, line);)
        if (std::regex_search(line, match, signal)) return match[1];
    if (matches(text, "(^|\\n)(No (memories|messages|results) found|Found 0 results)")) return "0";
    return "";
}
inline std::string lane_rows(const std::string& text, const std::string& lane,
                             bool keyword = false) {
    std::string result;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        if (!matches(line, "\\[[0-9]+%\\]") || line.find("[thought]") != std::string::npos) continue;
        if (keyword && (!matches(line, "^(#[0-9]+ )?\\[[0-9]+%\\]")
            || !matches(line, "\\] \\[(wisdom|insight|research|data|milestone|convergence)\\]"))) continue;
        result += "[" + lane + "]" + line + "\n";
    }
    return trim_newlines(result);
}
// This is text policy, deliberately downstream of unchanged recall handlers.
inline json fuse(const json& lanes, const json& options) {
    auto text = [&](const char* lane) {
        return lanes.contains(lane) ? trim_newlines(lanes.at(lane).value("text", "")) : "";
    };
    const auto hyb = text("hyb");
    auto sem = text("sem");
    if (matches(sem.substr(0, sem.find('\n')), "^Smart recall \\(keyword,")) {
        // Legacy sem retagging keeps thought rows for admission to reject/count.
        std::istringstream lines(sem);
        sem.clear();
        for (std::string line; std::getline(lines, line);)
            if (matches(line, "\\[[0-9]+%\\]")) sem += "[kw]" + line + "\n";
        sem = trim_newlines(sem);
    }
    std::string memories = trim_newlines(sem + "\n" + lane_rows(text("ctx"), "ctx") + "\n"
        + lane_rows(hyb, "hyb") + "\n" + lane_rows(text("kw"), "kw", true) + "\n"
        + lane_rows(text("corr"), "corr"));
    if (memories.find("No memories") != std::string::npos || !matches(memories, "\\[[0-9]+%\\]")) memories.clear();
    bool small = false;
    std::smatch realm, top;
    const std::regex realm_pattern("^Found ([0-9]+) results in realm '([^']+)'");
    const std::regex pct_pattern("\\[([0-9]+)%\\]");
    if (options.value("small_realm_enabled", true) && std::regex_search(hyb, realm, realm_pattern)
        && std::regex_search(hyb, top, pct_pattern)) {
        const auto count = std::stoi(realm[1]);
        small = realm[2] != "brahman" && count < options.value("hyb_limit", 5)
            && count <= options.value("small_realm_maxn", 3)
            && std::stoi(top[1]) >= options.value("small_realm_minpct", 50);
    }
    return {{"memories", memories}, {"c2_pct", c2_from_text(hyb)}, {"small_realm", small}};
}
} // namespace chitta::prompt_policy
