// chitta_thinking — extract perception-change insights from Claude thinking blocks
//
// Streams a session JSONL transcript, finds thinking blocks that contain signals
// where Claude's understanding shifted, and stores them as [insight] memories via
// the running chitta daemon.
//
// Usage:
//   chitta_thinking --transcript PATH [--dry-run] [--verbose] [--min-turns N]
//
// Designed to be called periodically from the prompt hook (every N turns) so
// insights are captured during long sessions, not only at session end.

#include <chitta/socket_client.hpp>
#include <chitta/version.hpp>
#include <chitta/llm_http.hpp>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_set>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

static const char* USAGE = R"(chitta_thinking — extract perception-change insights from thinking blocks

Usage:
  chitta_thinking --transcript PATH [options]

Options:
  --transcript PATH   Session JSONL file to process
  --state-file PATH   Track last-processed offset (default: transcript + .thinking_pos)
  --dry-run           Print insights without storing
  --verbose, -v       Verbose output
  --min-confidence N  Minimum confidence 0-100 (default: 65)
  --help, -h          Show this help

Examples:
  chitta_thinking --transcript ~/.claude/projects/foo/session.jsonl
  chitta_thinking --transcript ... --dry-run --verbose
)";

// ── Perception-change signal patterns ─────────────────────────────────────────
struct Signal {
    std::regex  pattern;
    std::string kind;
    std::string label;
};

static std::vector<Signal> make_signals() {
    // {regex, kind, label}
    // Ordered: stronger signals first so we label with the most specific match.
    std::vector<std::tuple<std::string, std::string, std::string>> raw = {
        { R"(\bI was wrong\b)",                                "correction",  "self-correction"    },
        { R"(\bshould(?:n't| not) have\b)",                   "correction",  "regret-correction"  },
        { R"(\bwrong approach\b)",                             "correction",  "approach-change"    },
        { R"(\bI missed\b)",                                   "correction",  "oversight"          },
        { R"(\bturns? out\b)",                                 "realization", "turns-out"          },
        { R"(\brealized?\b)",                                  "realization", "realization"        },
        { R"(\bwait,?\s+(?:actually|no\b|this\b|but\b))",     "realization", "wait-correction"    },
        { R"(\bthe\s+(?:real|actual|root)\s+(?:issue|problem|cause|reason)\s+is\b)",
                                                               "root-cause",  "root-cause"         },
        { R"(\bnow I (?:understand|see|know)\b)",              "insight",     "understanding"      },
        { R"(\bkey insight\b)",                                "insight",     "key-insight"        },
        { R"(\bAha\b)",                                        "insight",     "aha"                },
        { R"(\binteresting[,:]?\s+\w+)",                       "insight",     "interesting"        },
        { R"(\bpattern\s+(?:here|is|emerges)\b)",              "pattern",     "pattern"            },
    };
    std::vector<Signal> signals;
    for (auto& [pat, kind, label] : raw) {
        signals.push_back({
            std::regex(pat, std::regex::icase | std::regex::optimize),
            kind,
            label
        });
    }
    return signals;
}

// ── Helpers ───────────────────────────────────────────────────────────────────

static const int CONTEXT_HALF = 300;   // chars each side of the signal
static const int MIN_BLOCK_LEN = 80;   // skip trivial navigation thoughts
static const int MAX_INSIGHT_LEN = 500; // cap stored text length

static std::string extract_snippet(const std::string& text, const std::smatch& m) {
    int start = std::max(0, (int)m.position() - CONTEXT_HALF);
    int end   = std::min((int)text.size(), (int)(m.position() + m.length()) + CONTEXT_HALF);
    std::string snippet = text.substr(start, end - start);
    // Trim to word boundary if over max
    if ((int)snippet.size() > MAX_INSIGHT_LEN) {
        snippet = snippet.substr(0, MAX_INSIGHT_LEN);
        auto pos = snippet.rfind(' ');
        if (pos != std::string::npos) snippet = snippet.substr(0, pos);
        snippet += "\u2026"; // ellipsis
    }
    return snippet;
}

static std::string dedup_key(const std::string& snippet) {
    // First 80 non-whitespace chars as dedup key
    std::string key;
    key.reserve(80);
    for (char c : snippet) {
        if (!std::isspace((unsigned char)c)) {
            key += c;
            if ((int)key.size() >= 80) break;
        }
    }
    return key;
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    std::string transcript_path;
    std::string state_file_path;
    std::string socket_path;
    bool dry_run  = false;
    bool verbose  = false;
    int  min_conf = 65;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h")       { std::cout << USAGE; return 0; }
        if (arg == "--dry-run")                   { dry_run = true;  continue; }
        if (arg == "--verbose" || arg == "-v")    { verbose = true;  continue; }
        if (arg == "--transcript"      && i+1<argc) { transcript_path  = argv[++i]; continue; }
        if (arg == "--state-file"      && i+1<argc) { state_file_path  = argv[++i]; continue; }
        if (arg == "--socket-path"     && i+1<argc) { socket_path      = argv[++i]; continue; }
        if (arg == "--min-confidence"  && i+1<argc) { min_conf         = std::stoi(argv[++i]); continue; }
        std::cerr << "Unknown argument: " << arg << "\n" << USAGE; return 1;
    }

    if (transcript_path.empty()) {
        std::cerr << "ERROR: --transcript is required\n" << USAGE; return 1;
    }

    // State file: tracks byte offset of last processed position
    if (state_file_path.empty()) state_file_path = transcript_path + ".thinking_pos";

    // Read last processed offset and turn count
    std::streampos last_pos = 0;
    long long last_turns = 0;
    {
        std::ifstream sf(state_file_path);
        if (sf) {
            long long p = 0;
            sf >> p;
            last_pos = p;
            sf >> last_turns; // second line — may be absent in old state files
        }
    }

    std::ifstream f(transcript_path);
    if (!f) {
        std::cerr << "ERROR: Cannot open transcript: " << transcript_path << "\n";
        return 1;
    }
    f.seekg(last_pos);

    auto signals = make_signals();
    std::unordered_set<std::string> seen;
    std::vector<std::pair<std::string, std::string>> insights; // {label, snippet}

    std::string line;
    std::streampos new_pos = last_pos;
    long long turn_count = last_turns;
    while (std::getline(f, line)) {
        new_pos = f.tellg();
        if (line.empty()) continue;

        json event;
        try { event = json::parse(line); } catch (...) { continue; }

        if (event.value("type", "") != "assistant") continue;
        ++turn_count;
        auto& content = event["message"]["content"];
        if (!content.is_array()) continue;

        for (auto& block : content) {
            if (block.value("type", "") != "thinking") continue;
            std::string text = block.value("thinking", "");
            if ((int)text.size() < MIN_BLOCK_LEN) continue;

            for (auto& sig : signals) {
                std::smatch m;
                if (!std::regex_search(text, m, sig.pattern)) continue;

                std::string snippet = extract_snippet(text, m);
                std::string key = dedup_key(snippet);
                if (!seen.insert(key).second) continue; // duplicate

                insights.push_back({sig.label, snippet});
                if (verbose) {
                    std::cerr << "  [" << sig.label << "] " << snippet.substr(0, 100) << "...\n";
                }
                break; // one signal per thinking block
            }
        }
    }

    if (verbose) {
        std::cerr << "[thinking] Scanned turns " << last_turns << "–" << turn_count
                  << " (+" << (turn_count - last_turns) << " new)\n";
    }

    if (insights.empty()) {
        if (verbose) std::cerr << "[thinking] No new perception-change moments found\n";
        if (!dry_run) {
            std::ofstream sf(state_file_path);
            sf << (long long)new_pos << "\n" << turn_count;
        }
        return 0;
    }

    std::cerr << "[thinking] Found " << insights.size() << " perception-change moments"
              << " (turns " << last_turns << "–" << turn_count << ")\n";

    if (dry_run) {
        for (auto& [label, snippet] : insights) {
            std::cout << "[" << label << "] " << snippet.substr(0, 200) << "\n---\n";
        }
        return 0;
    }

    // Store via daemon socket
    if (socket_path.empty()) socket_path = chitta::SocketClient::default_socket_path();
    chitta::SocketClient client(socket_path);
    if (!client.connect()) {
        std::cerr << "[thinking] Daemon unavailable — skipping store\n";
        return 0; // non-fatal: don't block the hook
    }

    int stored = 0;
    double confidence = min_conf / 100.0;

    // Discover GPU endpoint once for gemma4:26b distillation
    std::string gpu_endpoint = chitta::discover_gpu_endpoint("teacher", "gemma4:26b",
        [&](const std::string& m) { if (verbose) std::cerr << m << "\n"; });
    if (verbose) {
        std::cerr << "[thinking] Distillation endpoint: "
                  << (gpu_endpoint.empty() ? "none (storing verbatim)" : gpu_endpoint) << "\n";
    }

    auto trim_ws = [](std::string s) -> std::string {
        auto f = s.find_first_not_of(" \n\r\t");
        if (f == std::string::npos) return "";
        auto l = s.find_last_not_of(" \n\r\t");
        return s.substr(f, l - f + 1);
    };

    for (auto& [label, snippet] : insights) {
        std::string text = snippet;
        if (!gpu_endpoint.empty()) {
            std::string distilled = chitta::call_llm_http(
                "", "gemma4:26b",
                "Summarize this thinking-block in 1-2 sentences, capturing the core insight:\n\n" + snippet,
                "You are a concise knowledge distiller. Output only the 1-2 sentence summary.",
                30, 0.1f, 120, nullptr, std::nullopt, "teacher", true);
            distilled = trim_ws(distilled);
            if (!distilled.empty()) text = distilled;
        }
        std::string content = "[thinking-block:" + label + "] " + text;
        json req = {
            {"jsonrpc", "2.0"},
            {"id",      1},
            {"method",  "tools/call"},
            {"params",  {
                {"name",      "remember"},
                {"arguments", {
                    {"content",    content},
                    {"kind",       "insight"},
                    {"tags",       "thinking-block," + label + ",perception-change"},
                    {"confidence", confidence},
                }}
            }}
        };

        auto resp_opt = client.request(req.dump());
        if (resp_opt && !resp_opt->empty()) {
            try {
                auto resp = json::parse(*resp_opt);
                if (!resp.contains("error")) ++stored;
            } catch (...) {}
        }
    }

    std::cerr << "[thinking] Stored " << stored << "/" << insights.size() << "\n";

    // Save new offset and turn count
    std::ofstream sf(state_file_path);
    sf << (long long)new_pos << "\n" << turn_count;

    return 0;
}
