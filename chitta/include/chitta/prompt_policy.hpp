#pragma once

// Prompt admission is independent of retrieval/scoring and store mutation.
// Inputs are the hook's lane text and session-local state; the result includes
// the exact rendered block plus local writes to apply only after validation.
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace chitta::prompt_policy {
using json = nlohmann::json;

inline std::string prefix(const std::string& text, size_t chars) {
    size_t i = 0, count = 0;
    for (; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xc0) != 0x80) {
            if (count == chars) break;
            ++count;
        }
    }
    return text.substr(0, i);
}
inline std::string hash(const std::string& line) {
    const auto text = prefix(line, 80);
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned size = 0;
    if (EVP_Digest(text.data(), text.size(), digest, &size, EVP_md5(), nullptr) != 1)
        throw std::runtime_error("prompt content hash failed");
    std::ostringstream out;
    for (unsigned i = 0; i < 8; ++i)
        out << std::hex << std::setw(2) << std::setfill('0') << unsigned(digest[i]);
    return out.str();
}
inline std::set<std::string> words(const std::string& text) {
    std::istringstream in(text);
    std::set<std::string> out;
    for (std::string word; in >> word;) out.insert(word);
    return out;
}
inline std::set<std::string> tokens(std::string text) {
    for (char& c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    static const std::regex pattern("[a-z0-9][a-z0-9_>/-]{3,}");
    std::set<std::string> out;
    for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it)
        out.insert(it->str());
    return out;
}
inline bool shares(const std::set<std::string>& a, const std::set<std::string>& b) {
    for (const auto& word : a) if (b.count(word)) return true;
    return false;
}
inline bool matches(const std::string& text, const char* pattern, bool insensitive = false) {
    return std::regex_search(text, std::regex(pattern,
        insensitive ? std::regex::ECMAScript | std::regex::icase : std::regex::ECMAScript));
}

inline json admit(const json& state) {
    const auto query = state.value("query", "");
    const auto qt = words(state.value("query_tokens", ""));
    const auto distinct = words(state.value("distinct_tokens", ""));
    const auto ctx = words(state.value("context_tokens", ""));
    const auto seen = state.value("seen_hashes", "");
    const auto sid = state.value("session_id", "unknown");
    const bool measured = !state.value("c2_pct", "").empty();
    const int c2 = measured ? std::stoi(state.at("c2_pct").get<std::string>()) : 0;
    const bool small = state.value("small_realm", false);
    const bool unknown = state.value("unknown_silence", true) && measured && c2 < 81;
    const bool persist = !sid.empty() && sid != "unknown";
    size_t word_count = 0;
    std::istringstream query_stream(query);
    for (std::string word; query_stream >> word;) ++word_count;
    const bool negation = word_count <= 8 && matches(query,
        "(^|[[:space:]])(is|are|was|were)[[:space:]]+not([[:space:]]|$)|^(no|not)([[:punct:][:space:]]|$)", true);
    const std::vector<std::string> lanes = {"sem", "ctx", "hyb", "kw", "corr", "xr"};
    const std::vector<std::string> reasons = {"conf", "dup", "meta", "cap", "unk"};
    json admitted = json::object(), dropped = json::object();
    for (const auto& lane : lanes) admitted[lane] = 0;
    for (const auto& reason : reasons) dropped[reason] = 0;
    auto drop = [&](const char* reason) { dropped[reason] = dropped[reason].get<int>() + 1; };
    struct Candidate { std::string lane, line, key; bool selected = false; };
    std::vector<Candidate> candidates;
    json shadow = json::array(), hashes = json::array();
    std::string debug;
    std::istringstream memory_stream(state.value("memories", ""));
    static const std::regex confidence("\\[([0-9]+)%\\]");
    for (std::string line; std::getline(memory_stream, line);) {
        std::smatch match;
        if (!std::regex_search(line, match, confidence)) continue;
        std::string lane = "sem";
        for (const auto& name : lanes) {
            if (name == "sem") continue;
            const auto tag = "[" + name + "]";
            if (line.rfind(tag, 0) == 0) { lane = name; line.erase(0, tag.size()); break; }
        }
        if (negation) { drop("unk"); continue; }
        std::regex_search(line, match, confidence);
        const int conf = std::stoi(match[1]);
        if (state.value("debug", false)) {
            debug += "[admit-debug] lane=" + lane + " conf=" + std::to_string(conf)
                + " c2=" + (measured ? std::to_string(c2) : "none")
                + " qtok=" + std::to_string(qt.size()) + " sr=" + (small ? "1" : "0")
                + " | " + prefix(line, 90) + "\n";
        }
        if (conf < ((lane == "hyb" || lane == "kw") ? 1 : 30)) { drop("conf"); continue; }
        const auto content_tokens = tokens(line);
        if (unknown) {
            const bool shared = shares(distinct, content_tokens);
            if (((lane == "sem" || lane == "ctx") && !(small && shared))
                || lane == "corr"
                || ((lane == "kw" || lane == "hyb" || lane == "xr") && !shared)) {
                drop("unk"); continue;
            }
        }
        if (lane == "kw" && distinct.size() < 2 && conf < state.value("kw_single_token_min", 60)) {
            drop("conf"); continue;
        }
        if (matches(line, "\\[episode\\].*\\[thinking.block:")
            || line.find("[thought]") != std::string::npos
            || matches(line, "\\[session:[^\\]]+\\][[:space:]]+(working|complete|completed|failed|blocked)→[0-9]+[[:space:]]+turns|\\[cache:break\\]", true)
            || matches(line, "pre-compact[[:space:]]+context:|\\[pre-compact:", true)
            || matches(line, "you are ([*]{0,2})?(claude|codex)(:[^ ,*]+)?([*]{0,2})?.*multi-agent discussion", true)
            || (line.find('?') == std::string::npos && matches(line, "(DONE|complete|completed|launched|shipped).*Fable [0-9a-f]{6,}"))
            || (prefix(line, 89) == line && matches(line, "[A-Z][a-z]+_et_al_[0-9]{4}_[A-Za-z0-9]+"))) {
            drop("meta"); continue;
        }
        const auto key = hash(line);
        if (persist && seen.find(key) != std::string::npos) { drop("dup"); continue; }
        if ((lane == "sem" || lane == "ctx") && !qt.empty()) {
            const bool turn_share = shares(qt, content_tokens);
            shadow.push_back({{"h", key}, {"conf", conf}, {"shares", turn_share ? 1 : 0},
                {"sctx", shares(ctx, content_tokens) ? 1 : 0}, {"lane", lane},
                {"qn", qt.size()}, {"s", sid}});
            if (lane == "sem" && state.value("anchor_enforce", false) && !turn_share
                && qt.size() >= 3 && qt.size() <= 50) { drop("meta"); continue; }
        }
        candidates.push_back({lane, line, key});
    }
    std::string block;
    int count = 0;
    while (count < 3) {
        bool added = false;
        for (const char* lane : {"corr", "kw", "sem", "ctx", "hyb", "xr"}) {
            auto it = std::find_if(candidates.begin(), candidates.end(), [&](const Candidate& c) {
                return !c.selected && c.lane == lane;
            });
            if (it == candidates.end()) continue;
            it->selected = true;
            added = true;
            block += "[" + it->lane + "]" + prefix(it->line, 150) + "\n";
            admitted[lane] = admitted[lane].get<int>() + 1;
            if (persist) hashes.push_back(it->key);
            if (++count == 3) break;
        }
        if (!added) break;
    }
    dropped["cap"] = candidates.size() - static_cast<size_t>(count);
    std::string tag, phrase, calibrated;
    if (measured) {
        tag = c2 >= 81 ? "KNOWN" : "UNKNOWN";
        phrase = c2 >= 81
            ? " | relevant memory present — verify before fully trusting; recall() more if it matters"
            : " | boundary of known memory — do not lean on this; ask or recall explicitly";
        const double p = std::clamp(c2 / 100.0, .001, .999);
        const double probability = 1 / (1 + std::exp(-(1.8029 * std::log(p / (1 - p)) - 2.9263)));
        calibrated = std::to_string(static_cast<int>(probability * 100 + .5));
    }
    std::string in, out;
    for (const auto& lane : lanes)
        if (admitted[lane].get<int>() > 0) in += " " + lane + ":" + admitted[lane].dump();
    for (const auto& reason : reasons)
        if (dropped[reason].get<int>() > 0) out += " " + reason + ":" + dropped[reason].dump();
    std::string summary;
    if (count > 0 || !out.empty()) {
        const auto ablated = state.value("ablated", "");
        summary = "[admit]" + (ablated.empty() ? "" : " abl:" + ablated)
            + (tag.empty() ? "" : " C2:" + tag + "(" + calibrated + "%)")
            + (small ? " sr:on" : "") + (in.empty() ? " none" : in)
            + " | drop" + (out.empty() ? " none" : out);
    }
    std::string timing;
    const auto ms = state.value("lane_ms", json::object());
    const auto timeouts = state.value("lane_timeout", json::object());
    for (const char* lane : {"sem", "ctx", "hyb", "kw", "corr", "corrk", "xr"}) {
        if (!ms.contains(lane)) continue;
        if (!timing.empty()) timing += ",";
        timing += std::string(lane) + "=" + ms.at(lane).dump()
            + (timeouts.value(lane, false) ? "!" : "");
    }
    return {{"fused_block", block}, {"count", count}, {"admit_line", summary},
        {"c2_tag", tag}, {"c2_cal", calibrated}, {"c2_phrase", phrase},
        {"admitted", admitted}, {"dropped", dropped}, {"hashes", hashes},
        {"shadow", shadow}, {"debug_text", debug}, {"terse_negation", negation},
        {"timing_field", timing},
        {"admit_tail", " | t:" + timing + (timing.empty() ? "" : ",") + "total=@HOOK_MS@" + phrase}};
}
} // namespace chitta::prompt_policy
