#pragma once
#include <chitta/hook_bash_policy.hpp>
#include <chitta/hook_unicode.hpp>

namespace chitta::hook_saddle {
using json = nlohmann::json;
using Text = std::vector<int>;
inline Text decode(const std::string& s) {
    Text out;
    for (size_t i = 0; i < s.size();) {
        const auto byte = static_cast<unsigned char>(s[i++]);
        int c = byte, n = 0;
        if (byte >= 0xf0) {
            c = byte & 7;
            n = 3;
        } else if (byte >= 0xe0) {
            c = byte & 15;
            n = 2;
        } else if (byte >= 0xc0) {
            c = byte & 31;
            n = 1;
        }
        while (n-- && i < s.size())
            c = (c << 6) | (static_cast<unsigned char>(s[i++]) & 63);
        out.push_back(c);
    }
    return out;
}
inline std::string encode(const Text& s) {
    std::string out;
    for (auto c : s) {
        if (c < 128)
            out += char(c);
        else if (c < 2048) {
            out += char(192 | (c >> 6));
            out += char(128 | (c & 63));
        } else if (c < 65536) {
            out += char(224 | (c >> 12));
            out += char(128 | ((c >> 6) & 63));
            out += char(128 | (c & 63));
        } else {
            out += char(240 | (c >> 18));
            out += char(128 | ((c >> 12) & 63));
            out += char(128 | ((c >> 6) & 63));
            out += char(128 | (c & 63));
        }
    }
    return out;
}
template <size_t N> inline bool member(int c, const std::array<int, 2> (&ranges)[N]) {
    for (const auto& r : ranges) {
        if (c < r[0]) return false;
        if (c <= r[1]) return true;
    }
    return false;
}
inline Text strip(Text s) {
    auto first = s.begin(), last = s.end();
    while (first != last && member(*first, hook_unicode::space))
        ++first;
    while (first != last && member(*(last - 1), hook_unicode::space))
        --last;
    return {first, last};
}
inline Text collapse(const Text& s) {
    Text out;
    bool pending = false;
    for (auto c : strip(s)) {
        if (member(c, hook_unicode::space)) {
            pending = true;
            continue;
        }
        if (pending && !out.empty()) out.push_back(' ');
        pending = false;
        out.push_back(c);
    }
    return out;
}
inline Text normalize(const std::string& raw) {
    const auto source = strip(decode(raw));
    Text lowered;
    for (size_t i = 0; i < source.size(); ++i) {
        auto c = source[i];
        if (c == 304) {
            lowered.push_back('i');
            lowered.push_back(775);
            continue;
        }
        if (c == 931) {
            bool before = false, after = false;
            for (size_t j = i; j > 0;) {
                auto p = source[--j];
                if (member(p, hook_unicode::ignorable)) continue;
                before = member(p, hook_unicode::cased);
                break;
            }
            for (size_t j = i + 1; j < source.size(); ++j) {
                auto p = source[j];
                if (member(p, hook_unicode::ignorable)) continue;
                after = member(p, hook_unicode::cased);
                break;
            }
            if (before && !after) {
                lowered.push_back(962);
                continue;
            }
        }
        for (const auto& r : hook_unicode::lowercase)
            if (c >= r[0] && c <= r[1] && (c - r[0]) % r[2] == 0) {
                c += r[3];
                break;
            }
        lowered.push_back(c);
    }
    Text digits;
    bool was_digit = false;
    for (auto c : lowered) {
        bool digit = member(c, hook_unicode::decimal);
        if (!digit)
            digits.push_back(c);
        else if (!was_digit)
            digits.push_back('#');
        was_digit = digit;
    }
    auto out = collapse(digits);
    if (out.size() > 60) out.resize(60);
    return out;
}
inline size_t matching(const Text& a, size_t a0, size_t a1, const Text& b, size_t b0, size_t b1) {
    size_t best = 0, ai = a0, bi = b0;
    std::vector<size_t> prev(b1 - b0 + 1), next(b1 - b0 + 1);
    for (size_t i = a0; i < a1; ++i) {
        std::fill(next.begin(), next.end(), 0);
        for (size_t j = b0; j < b1; ++j)
            if (a[i] == b[j]) {
                auto n           = prev[j - b0] + 1;
                next[j - b0 + 1] = n;
                if (n > best) {
                    best = n;
                    ai   = i + 1 - n;
                    bi   = j + 1 - n;
                }
            }
        prev.swap(next);
    }
    if (!best) return 0;
    return best + (ai > a0 && bi > b0 ? matching(a, a0, ai, b, b0, bi) : 0) +
           (ai + best < a1 && bi + best < b1 ? matching(a, ai + best, a1, b, bi + best, b1) : 0);
}
inline bool similar(const Text& a, const Text& b, double threshold) {
    if (a == b) return true;
    if (2. * std::min(a.size(), b.size()) / (a.size() + b.size()) < threshold) return false;
    return 2. * matching(a, 0, a.size(), b, 0, b.size()) / (a.size() + b.size()) >= threshold;
}
inline bool truth(const json& j) {
    if (j.is_null()) return false;
    if (j.is_boolean()) return j.get<bool>();
    if (j.is_number()) return j.get<double>() != 0;
    return !j.empty();
}
inline bool failed(const json& row) {
    const auto exit = row.value("exit_code", json());
    if (exit.is_null()) return truth(row.value("likely_fail", json()));
    if (exit.is_boolean()) return exit.get<bool>();
    if (exit.is_number()) return std::trunc(exit.get<double>()) != 0;
    if (!exit.is_string()) return false;
    auto text = strip(decode(exit.get<std::string>()));
    for (auto& c : text)
        for (const auto& r : hook_unicode::decimal)
            if (c >= r[0] && c <= r[1]) {
                c = '0' + (c - r[0]) % 10;
                break;
            }
    auto number = encode(text);
    if (!chitta::hook_policy::match(number, "^[+-]?[0-9]+(_[0-9]+)*$")) return false;
    return number.find_first_of("123456789") != std::string::npos;
}
inline json detect(const json& a) {
    const auto session = chitta::hook_ledger::str(a, "session_id");
    if (!chitta::hook_policy::match(session, "^[a-zA-Z0-9_-]+$")) return nullptr;
    const auto now = a.value("now_ms", 0.0), minutes = a.value("minutes", 7.0),
               threshold = a.value("similarity", 0.8);
    json events          = json::array();
    for (const auto& r : a.value("events", json::array())) {
        if (!r.is_object() || chitta::hook_ledger::str(r, "session_id") != session ||
            chitta::hook_ledger::str(r, "event") != "bash_outcome" || !r.contains("ts") ||
            !r["ts"].is_number() || !r.contains("cmd_head") || !r["cmd_head"].is_string())
            continue;
        const auto ts = r["ts"].get<double>();
        if (ts < now - minutes * 60000 || ts > now ||
            strip(decode(r["cmd_head"].get<std::string>())).empty())
            continue;
        events.push_back(r);
    }
    std::stable_sort(events.begin(), events.end(), [](const auto& x, const auto& y) {
        return x["ts"].template get<double>() < y["ts"].template get<double>();
    });
    struct Group {
        Text shape;
        json fails = json::array();
    };
    std::vector<Group> groups;
    for (const auto& r : events) {
        const auto shape = normalize(r["cmd_head"].get<std::string>());
        auto first       = std::find_if(groups.begin(), groups.end(), [&](const auto& g) {
            return similar(g.shape, shape, threshold);
        });
        if (failed(r)) {
            if (first == groups.end()) {
                groups.push_back({shape, json::array()});
                first = groups.end() - 1;
            }
            first->fails.push_back(r);
        } else {
            auto exit = r.value("exit_code", json());
            if ((exit.is_number() && exit.get<double>() == 0) ||
                (exit.is_boolean() && !exit.get<bool>()) || exit == "0")
                groups.erase(std::remove_if(
                                 groups.begin(), groups.end(),
                                 [&](const auto& g) { return similar(g.shape, shape, threshold); }),
                             groups.end());
        }
    }
    const bool has_cmd  = a.contains("cmd") && !a["cmd"].is_null();
    const auto cmd      = normalize(chitta::hook_ledger::str(a, "cmd"));
    const Group* winner = nullptr;
    for (const auto& g : groups) {
        if (g.fails.size() < a.value("min_fails", 3u) ||
            (has_cmd && !similar(g.shape, cmd, threshold)))
            continue;
        if (!winner ||
            g.fails.back()["ts"].get<double>() > winner->fails.back()["ts"].get<double>())
            winner = &g;
    }
    if (!winner) return nullptr;
    const auto first = winner->fails.front(), last = winner->fails.back();
    auto err = last.value("stderr_head", json());
    const auto raw =
        truth(err) ? (err.is_string() ? err.get<std::string>() : err.dump()) : "error unavailable";
    auto excerpt = collapse(decode(raw));
    if (excerpt.size() > 160) excerpt.resize(160);
    const auto error = encode(excerpt);
    std::ostringstream duration;
    duration << minutes;
    const auto identity = session + ":" + first["ts"].dump() + ":" + encode(winner->shape);
    return {{"saddle_id", identity},
            {"start_ts", first["ts"]},
            {"end_ts", last["ts"]},
            {"n_fails", winner->fails.size()},
            {"escaped", false},
            {"cmd_head", prompt_policy::prefix(first["cmd_head"].get<std::string>(), 80)},
            {"stderr_head", error},
            {"window_minutes", minutes},
            {"message", "[saddle] this command shape failed " +
                            std::to_string(winner->fails.size()) + "× in " + duration.str() +
                            " min (last: " + error +
                            "). Change approach or read the error before retrying."}};
}
} // namespace chitta::hook_saddle
