#include <chitta/llm_http.hpp>
#include <chitta/output_cap.hpp>
#include <chitta/queue_path.hpp>
#include <chitta/prompt_policy.hpp>
// Chitta CLI - Multi-mode memory operations
// Command-line interface for soul integration
//
// Modes:
//   CLI mode:    chitta <tool> [args...]  - Direct tool invocation
//   Thin client: chitta                   - Forward JSON-RPC to daemon
//
// CLI Examples:
//   chitta recall "query"
//   chitta recall "query" --zoom sparse
//   chitta soul_context
//   chitta observe --category decision --title "..." --content "..."
//   chitta grow --type wisdom --title "..." --content "..."
//
// Options:
//   --socket-path PATH  Unix socket path
//   --json              CLI mode: output raw JSON instead of text
//   --toon              CLI mode: output TOON format (compact, shell-friendly)

#include <chitta/socket_client.hpp>
#include <chitta/rpc/cli_tools.hpp>
#include <chitta/rpc/sandbox.hpp>  // append_line_atomic
#include <unistd.h>  // getppid
#include <fstream>
#include <array>
#include <thread>
#include <chitta/version.hpp>
#include <set>
#include <iostream>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <nlohmann/json.hpp>

// TOON (Token-Oriented Object Notation) encoder
// Compact format for LLM inputs: ~40% fewer tokens than JSON
// Spec: https://github.com/toon-format/toon
namespace toon {

// Escape value for TOON (only escape commas and newlines in array values)
std::string escape_value(const std::string& s) {
    bool needs_quote = false;
    for (char c : s) {
        if (c == ',' || c == '\n' || c == '\r') {
            needs_quote = true;
            break;
        }
    }
    if (!needs_quote) return s;

    // Quote the value and escape internal quotes
    std::string result = "\"";
    for (char c : s) {
        if (c == '"') result += "\"\"";
        else if (c == '\n') result += "\\n";
        else if (c == '\r') result += "\\r";
        else result += c;
    }
    result += "\"";
    return result;
}

std::string value_to_string(const nlohmann::json& v) {
    if (v.is_null()) return "";
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_number_integer()) return std::to_string(v.get<int64_t>());
    if (v.is_number_float()) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.6g", v.get<double>());
        return buf;
    }
    if (v.is_string()) return escape_value(v.get<std::string>());
    // For nested objects/arrays, fall back to compact JSON
    return v.dump();
}

std::string encode(const nlohmann::json& j, int indent = 0) {
    std::string result;
    std::string prefix(indent, ' ');

    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            const auto& key = it.key();
            const auto& val = it.value();

            if (val.is_object()) {
                result += prefix + key + ":\n";
                result += encode(val, indent + 1);
            } else if (val.is_array() && !val.empty()) {
                // Check if uniform array of objects (tabular)
                bool is_tabular = true;
                std::vector<std::string> fields;
                for (const auto& item : val) {
                    if (!item.is_object()) {
                        is_tabular = false;
                        break;
                    }
                    if (fields.empty()) {
                        for (auto& [k, v] : item.items()) {
                            fields.push_back(k);
                        }
                    }
                }

                if (is_tabular && !fields.empty()) {
                    // Tabular format: array[N]{field1,field2}:\n val1,val2\n
                    result += prefix + key + "[" + std::to_string(val.size()) + "]{";
                    for (size_t i = 0; i < fields.size(); ++i) {
                        if (i > 0) result += ",";
                        result += fields[i];
                    }
                    result += "}:\n";
                    for (const auto& item : val) {
                        result += prefix + " ";
                        for (size_t i = 0; i < fields.size(); ++i) {
                            if (i > 0) result += ",";
                            if (item.contains(fields[i])) {
                                result += value_to_string(item[fields[i]]);
                            }
                        }
                        result += "\n";
                    }
                } else if (!val.empty() && val[0].is_primitive()) {
                    // Simple array: array[N]: val1,val2,val3
                    result += prefix + key + "[" + std::to_string(val.size()) + "]: ";
                    for (size_t i = 0; i < val.size(); ++i) {
                        if (i > 0) result += ",";
                        result += value_to_string(val[i]);
                    }
                    result += "\n";
                } else {
                    // Mixed array - fall back to JSON
                    result += prefix + key + ": " + val.dump() + "\n";
                }
            } else {
                result += prefix + key + ": " + value_to_string(val) + "\n";
            }
        }
    } else if (j.is_array()) {
        // Top-level array
        nlohmann::json wrapper;
        wrapper["results"] = j;
        return encode(wrapper, indent);
    } else {
        result = value_to_string(j);
    }

    return result;
}

}  // namespace toon

// Compatibility-only names: registered handlers historically callable from the
// CLI but absent from tools/list. Their argument validation belongs to the daemon;
// all advertised tools and schemas come exclusively from runtime discovery.
static const std::set<std::string> LEGACY_HANDLERS = {
    "cleanup",
    "cycle",
    "dedupe_symbols",
    "describe_symbol",
    "distill_status",
    "epiplexity_check",
    "export_soul",
    "export_training_pairs",
    "extract_symbols",
    "fep_status",
    "file_dependents",
    "file_imports",
    "harvest_scope",
    "health_check_start",
    "hygiene_run",
    "import_soul",
    "ingest_source",
    "ledger_append",
    "ledger_compile",
    "ledger_contradictions",
    "ledger_op",
    "ledger_query",
    "msg_ack",
    "predicate_attach",
    "predicate_list",
    "predicate_run",
    "queue_experiments",
    "recall_temporal_events",
    "reconcile_pass",
    "reembed_memories",
    "resolve_callsites",
    "routed_recall",
    "seed_hdc_geometry",
    "session_deregister",
    "session_heartbeat",
    "session_register",
    "tape_stats",
    "transcript_get",
    "transcript_list",
    "transcript_parse",
    "transcript_register",
    "transcript_remove",
    "transcript_update",
    "turiya_status",
    "type_hierarchy",
    "verbalize_rules",
    "version_check",
    "wiki_export",
    "witness_memory",
};

static void print_tool_help(const nlohmann::json& spec) {
    const auto name = spec.at("name").get<std::string>();
    std::cerr << "chitta " << name << " - " << spec.value("description", "") << "\n\n";
    if (!spec.contains("inputSchema")) {
        std::cerr << "  Legacy handler; parameters are validated by the daemon.\n";
        return;
    }
    const auto& schema = spec.at("inputSchema");
    const auto& properties = schema.at("properties");
    const auto required = schema.value("required", std::vector<std::string>{});
    if (properties.empty()) {
        std::cerr << "  No parameters required.\n";
        return;
    }
    std::cerr << "Parameters:\n";
    for (auto it = properties.begin(); it != properties.end(); ++it) {
        std::cerr << "  --" << it.key();
        if (std::find(required.begin(), required.end(), it.key()) != required.end()) {
            std::cerr << " (required)";
        } else if (it->contains("default")) {
            const auto& value = it->at("default");
            std::cerr << " [default: " << (value.is_string() ? value.get<std::string>() : value.dump()) << "]";
        }
        std::cerr << "\n";
        const auto description = it->value("description", "");
        if (!description.empty()) std::cerr << "      " << description;
        std::cerr << "\n";
    }
    std::cerr << "\nExample:\n  chitta " << name;
    for (const auto& param : required) std::cerr << " --" << param << " \"...\"";
    std::cerr << "\n";
}

// Unknown parameters remain accepted for handler extensions and historic aliases.
// Known parameters use schema types, so numeric-looking strings (notably IDs)
// remain strings and arrays/objects reach the daemon as structured JSON.
static nlohmann::json parse_cli_value(const std::string& value, const nlohmann::json& property) {
    using json = nlohmann::json;
    const auto type = property.value("type", "");
    if (type == "string") return value;
    auto parsed = json::parse(value, nullptr, false);
    if (type == "array") {
        if (parsed.is_array()) return parsed;
        // A JSON-looking value must be valid JSON; let daemon validation report
        // malformed structured input rather than silently splitting it.
        if (!value.empty() && (value.front() == '[' || value.front() == '{')) return value;
        json items = json::array();
        std::istringstream input(value);
        std::string item;
        const auto item_schema = property.value("items", json::object());
        while (std::getline(input, item, ',')) {
            const auto first = item.find_first_not_of(' ');
            const auto last = item.find_last_not_of(' ');
            if (first != std::string::npos)
                items.push_back(parse_cli_value(item.substr(first, last - first + 1), item_schema));
        }
        return items;
    }
    if (type == "boolean") return parsed.is_boolean() ? parsed : json(value);
    if (type == "object") return parsed.is_object() ? parsed : json(value);
    if ((type == "integer" && parsed.is_number_integer()) ||
        (type == "number" && parsed.is_number())) return parsed;
    if (type.empty() && (parsed.is_boolean() || parsed.is_object() || parsed.is_array())) return parsed;

    // Preserve legacy decimal spellings (e.g. 001 and .5), unsigned 64-bit IDs,
    // and untyped extension arguments. Do not unquote arbitrary JSON strings.
    bool numeric = !value.empty(), dot = false;
    for (size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (c == '-' && i == 0) continue;
        if (c == '.' && !dot) { dot = true; continue; }
        if (!std::isdigit(static_cast<unsigned char>(c))) { numeric = false; break; }
    }
    if (numeric && (type.empty() || type == "integer" || type == "number")) {
        try {
            if (dot) return std::stod(value);
            if (value.front() != '-') return std::stoull(value);
            return std::stoll(value);
        } catch (...) { /* Keep invalid or out-of-range values for daemon validation. */ }
    }
    return value;
}

static const char* prog_name(const char* path) {
    const char* last = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '/') last = p + 1;
    }
    return last;
}

void print_usage(const char* prog) {
    const char* name = prog_name(prog);
    std::cerr << "Usage:\n"
              << "  " << name << " <tool> --param value ...   Invoke tool\n"
              << "  " << name << " <tool> --help              Show tool parameters\n"
              << "  " << name << " [options]                  Interactive mode (JSON-RPC)\n"
              << "  " << name << " endpoints [--probe] [--json] [--local] [--distill-model MODEL]\n"
              << "\n"
              << "Examples:\n"
              << "  " << name << " recall --query \"search terms\"\n"
              << "  " << name << " soul_context\n"
              << "  " << name << " observe --title \"Decision\" --content \"Chose X over Y\"\n"
              << "  " << name << " grow --type wisdom --content \"Pattern discovered\"\n"
              << "  " << name << " learn_codebase --path /path/to/project\n"
              << "\n"
              << "Tool categories:\n"
              << "  Memory:      remember, recall, grow, get, update, forget, strengthen, weaken, tag\n"
              << "  Triplets:    connect, query, query_graph\n"
              << "  Hooks:       observe, full_resonate\n"
              << "  Explore:     explore_recall, explore_peek, explore_expand, explore_neighbors\n"
              << "  Context:     soul_context, health_check, version_check, lookup\n"
              << "  Maintenance: cycle, cleanup, hygiene_stats, hygiene_run\n"
              << "  Import/Export: import_soul, export_soul\n"
              << "  Code Intel:  extract_symbols, learn_codebase, find_symbol, search_symbols, code_context\n"
              << "  Call Graph:  symbol_callers, symbol_callees, resolve_callsites\n"
              << "  Type/Import: type_hierarchy, file_imports, file_dependents, read_symbol, read_function\n"
              << "  Realm:       realm_detect, realm_list, realm_get, realm_set, realm_add, realm_remove, realm_visibility\n"
              << "  Ledger:      ledger_save, ledger_load, ledger_list, ledger_get, ledger_delete\n"
              << "  Transcript:  read_transcript, transcript_register, transcript_get, transcript_list, transcript_update, transcript_remove\n"
              << "  Learning:    learn_outcome, episode_cluster_status, calibration_record, calibration_score\n"
              << "  Theme:       theme_list, theme_get, theme_recall, theme_stats, theme_maintain, theme_assign_orphans\n"
              << "  Bulk ops:    forget_kind, list_memories_brief\n"
              << "  CEC events:  log_event, log_event_ex, log_decision\n"
              << "  CEC recall:  recall_last_action, recall_causal_antecedent, recall_failure_pattern,\n"
              << "               recall_counterfactual, recall_true_counterfactual, recall_motif_value\n"
              << "  CEC market:  hypothesis_probes, refutation_stats\n"
              << "  CEC exec:    executor_flush, list_policies, consolidation_pass\n"
              << "  CEC witness: turiya_status, tape_stats, verbalize_rules\n"
              << "  CEC experiment: queue_experiments\n"
              << "  CEC phase15: fep_status\n"
              << "  CEC phase16: routed_recall\n"
              << "  CEC phase17: witness_memory, reconcile_pass, harvest_scope, seed_hdc_geometry\n"
              << "\n"
              << "Global options:\n"
              << "  --socket-path PATH  Unix socket path\n"
              << "  --json              Output raw JSON instead of text\n"
              << "  --toon              Output TOON format (compact, ~40% fewer tokens)\n"
              << "  --text-only         Output only text content (for piping/hooks)\n"
              << "  --help              Show this help message\n";
}

enum class OutputFormat { Text, Json, Toon, TextOnly };

// CLI mode: invoke tool directly
int run_cli(const std::string& socket_path, const std::string& tool,
            int argc, char* argv[], int arg_start, OutputFormat output_format,
            const nlohmann::json& spec) {
    using json = nlohmann::json;

    // Check for --help first
    for (int i = arg_start; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_tool_help(spec);
            return 0;
        }
    }

    // Build arguments JSON from command line (all named, no positional)
    json args = json::object();
    const auto schema = spec.value("inputSchema", json::object());
    const auto properties = schema.value("properties", json::object());

    for (int i = arg_start; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--json" || arg == "--toon" || arg == "--text-only") continue;
        if (arg == "--socket-path" && i + 1 < argc) { ++i; continue; }
        if (arg.rfind("--", 0) == 0) {
            // Named argument: --key value
            std::string key = arg.substr(2);
            // Normalize hyphens to underscores for consistency with JSON keys
            std::replace(key.begin(), key.end(), '-', '_');
            if (i + 1 < argc && (argv[i + 1][0] != '-' ||
                                   (argv[i + 1][0] == '-' && std::isdigit(static_cast<unsigned char>(argv[i + 1][1]))))) {
                std::string value = argv[++i];
                args[key] = parse_cli_value(value, properties.value(key, json::object()));
            } else {
                args[key] = true;  // Flag without value
            }
        } else {
            // Positional argument - show help instead of silently ignoring
            std::cerr << "Error: Unexpected positional argument: " << arg << "\n";
            std::cerr << "All arguments must be named (--param value).\n\n";
            print_tool_help(spec);
            return 1;
        }
    }

    // Keep the historical alias, now with the canonical schema type.
    if ((tool == "remember" || tool == "grow") && args.contains("kind") && !args.contains("type")) {
        args["type"] = args["kind"];
        args.erase("kind");
    }

    const auto required = schema.value("required", std::vector<std::string>{});
    std::vector<std::string> missing;
    for (const auto& name : required) {
        if (!args.contains(name)) missing.push_back(name);
    }
    if (!missing.empty()) {
        std::cerr << "Error: Missing required parameter(s): ";
        for (size_t i = 0; i < missing.size(); ++i) {
            if (i > 0) std::cerr << ", ";
            std::cerr << "--" << missing[i];
        }
        std::cerr << "\n\n";
        print_tool_help(spec);
        return 1;
    }
    for (auto it = properties.begin(); it != properties.end(); ++it) {
        if (!args.contains(it.key()) && it->contains("default")) args[it.key()] = it->at("default");
    }

    // Connect to daemon (safe mode: never kill/restart)
    chitta::SocketClient client(socket_path);
    if (!client.connect_only()) {
        const auto& err = client.last_error();
        std::cerr << "Error: " << err << "\n";
        if (err.find("still loading") != std::string::npos || err.find("warming up") != std::string::npos)
            std::cerr << "Hint: Daemon is starting up — retry in a few seconds\n";
        else if (err.find("incompatible") != std::string::npos)
            std::cerr << "Hint: Restart daemon with 'systemctl --user restart chittad'\n";
        else
            std::cerr << "Hint: Start daemon with 'chittad daemon' or let hooks start it\n";
        return 1;
    }

    // For session-aware tools, inject PPID for session lookup if session_id not provided
    static const std::set<std::string> SESSION_TOOLS = {
        "msg_inbox", "msg_send", "msg_ack", "msg_ack_all", "msg_history",
        "ledger_save", "narrative_log", "narrative_history",
        "anticipation_filter", "anticipation_gate_status",
        "recall", "smart_recall", "hybrid_recall", "recall_lanes"
    };
    if (SESSION_TOOLS.count(tool) && !args.contains("session_id")) {
        pid_t ppid = getppid();
        if (ppid > 1) {  // Sanity check: not init
            args["pid"] = static_cast<int64_t>(ppid);
        }
    }

    json tool_req = {
        {"jsonrpc", "2.0"},
        {"method", "tools/call"},
        {"params", {
            {"name", tool},
            {"arguments", args}
        }},
        {"id", 1}
    };

    auto resp = client.request(tool_req.dump());
    if (!resp) {
        std::cerr << "Error: Tool call failed: " << client.last_error() << "\n";
        return 1;
    }

    try {
        // DEBUG: Print raw response for troubleshooting
        if (std::getenv("CHITTA_DEBUG")) {
            std::cerr << "[DEBUG] Raw response (" << resp->size() << " bytes): " << resp->substr(0, 200) << "\n";
        }
        auto result = json::parse(*resp);
        if (result.contains("result") && result["result"].contains("structured") &&
            result["result"]["structured"].value("loading", false)) {
            std::cout << result["result"]["structured"].dump() << "\n";
            return tool == "health_check" || tool == "status" ? 0 : 75;
        }


        if (result.contains("error")) {
            std::cerr << "Error: " << result["error"]["message"].get<std::string>() << "\n";
            return 1;
        }

        switch (output_format) {
            case OutputFormat::Json:
                // Raw JSON output
                if (result.contains("result") && result["result"].contains("structured")) {
                    std::cout << result["result"]["structured"].dump(2, ' ', false, json::error_handler_t::replace) << "\n";
                } else {
                    std::cout << result.dump(2, ' ', false, json::error_handler_t::replace) << "\n";
                }
                break;

            case OutputFormat::Toon:
                // TOON format (compact, shell-friendly)
                if (result.contains("result") && result["result"].contains("structured")) {
                    std::cout << toon::encode(result["result"]["structured"]);
                } else if (result.contains("result")) {
                    std::cout << toon::encode(result["result"]);
                } else {
                    std::cout << toon::encode(result);
                }
                break;

            case OutputFormat::TextOnly:
                // Just the text content from results (for hooks/piping)
                if (result.contains("result") && result["result"].contains("structured")) {
                    auto& structured = result["result"]["structured"];
                    if (structured.contains("results") && structured["results"].is_array()) {
                        for (const auto& item : structured["results"]) {
                            if (item.contains("text")) {
                                std::cout << item["text"].get<std::string>() << "\n";
                            }
                        }
                    } else if (structured.contains("text")) {
                        std::cout << structured["text"].get<std::string>() << "\n";
                    }
                }
                break;

            case OutputFormat::Text:
            default:
                // Human-readable text output
                if (result.contains("result") && result["result"].contains("content")) {
                    auto& content = result["result"]["content"];
                    if (content.is_array() && !content.empty() && content[0].contains("text")) {
                        std::cout << content[0]["text"].get<std::string>() << "\n";
                    }
                }
                break;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error parsing response: " << e.what() << "\n";
        return 1;
    }

    return 0;
}

/// Thin client mode: forward stdin → daemon → stdout
int run_thin_client(const std::string& socket_path) {
    chitta::SocketClient client(socket_path);

    // Safe connect: never kill/restart daemon
    if (!client.connect_only()) {
        const auto& err = client.last_error();
        std::cerr << "[chitta] " << err << "\n";
        if (err.find("still loading") != std::string::npos || err.find("warming up") != std::string::npos)
            std::cerr << "[chitta] Hint: Daemon is starting up — retry in a few seconds\n";
        else if (err.find("incompatible") != std::string::npos)
            std::cerr << "[chitta] Hint: Restart daemon with 'systemctl --user restart chittad'\n";
        else
            std::cerr << "[chitta] Hint: Start daemon with 'chittad daemon'\n";
        return 1;
    }

    std::cerr << "[chitta] Connected to daemon at " << socket_path << "\n";
    std::cerr << "[chitta] Listening on stdin...\n";

    // Forward requests
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        auto response = client.request(line);
        if (response) {
            std::cout << *response << "\n";
            std::cout.flush();
            const auto reply = nlohmann::json::parse(*response, nullptr, false);
            if (reply.is_object() && reply.contains("result") &&
                reply["result"].contains("structured") &&
                reply["result"]["structured"].value("loading", false)) {
                const auto request = nlohmann::json::parse(line, nullptr, false);
                std::string method;
                if (request.is_object()) {
                    method = request.value("method", "");
                    if (method == "tools/call" && request.contains("params") && request["params"].is_object())
                        method = request["params"].value("name", "");
                }
                return method == "health_check" || method == "status" ? 0 : 75;
            }
        } else {
            std::cerr << "[chitta] Request failed: " << client.last_error() << "\n";

            // Try to reconnect (safe: don't restart daemon)
            client.disconnect();
            if (!client.connect_only()) {
                std::cerr << "[chitta] Reconnection failed: " << client.last_error() << "\n";
                return 1;
            }
            std::cerr << "[chitta] Reconnected to daemon\n";

            // Retry the request
            response = client.request(line);
            if (response) {
                std::cout << *response << "\n";
                std::cout.flush();
            } else {
                std::cout << R"({"jsonrpc":"2.0","error":{"code":-32603,"message":"Daemon connection lost"},"id":null})" << "\n";
                std::cout.flush();
            }
        }
    }

    std::cerr << "[chitta] Shutdown complete\n";
    return 0;
}

// Detect realm from environment/git/config
// Priority: CHITTA_REALM env > .cc-soul-realm file > git repo name > "brahman"
static std::string detect_realm() {
    // 1. Environment variable
    if (const char* env_realm = std::getenv("CHITTA_REALM")) {
        return env_realm;
    }

    // 2. Config file in current directory
    std::ifstream realm_file(".cc-soul-realm");
    if (realm_file.good()) {
        std::string realm;
        std::getline(realm_file, realm);
        // Trim whitespace
        realm.erase(0, realm.find_first_not_of(" \t\n\r"));
        realm.erase(realm.find_last_not_of(" \t\n\r") + 1);
        if (!realm.empty()) {
            return realm;
        }
    }

    // 3. Git repository name
    std::array<char, 256> buffer;
    std::string git_root;
    // --git-common-dir resolves a linked worktree to its main repository, so a
    // Codex stream worktree shares the project realm instead of its own.
    FILE* pipe = popen("d=$(git rev-parse --git-common-dir 2>/dev/null) && cd \"$(dirname \"$d\")\" 2>/dev/null && pwd -P", "r");
    if (pipe) {
        if (fgets(buffer.data(), buffer.size(), pipe)) {
            git_root = buffer.data();
            // Remove trailing newline
            if (!git_root.empty() && git_root.back() == '\n') {
                git_root.pop_back();
            }
        }
        pclose(pipe);
    }

    if (!git_root.empty()) {
        // Extract repo name from path
        size_t last_slash = git_root.rfind('/');
        std::string repo_name = (last_slash != std::string::npos)
            ? git_root.substr(last_slash + 1)
            : git_root;
        return "project:" + repo_name;
    }

    // 4. Default
    return "brahman";
}

int main(int argc, char* argv[]) {
    if (argc > 1 && std::string(argv[1]) == "endpoints") {
        bool probe = false, json_output = false, local = false;
        std::string teacher_model;
        for (int i = 2; i < argc; ++i) {
            std::string arg(argv[i]);
            if (arg == "--probe") probe = true;
            else if (arg == "--json") json_output = true;
            else if (arg == "--local") local = true;
            else if (arg == "--distill-model" && i + 1 < argc) teacher_model = argv[++i];
            else if (arg == "--help") {
                std::cout << "Usage: chitta endpoints [--probe] [--json] [--local] [--distill-model MODEL]\n";
                return 0;
            } else { std::cerr << "Unknown endpoints option: " << arg << "\n"; return 2; }
        }
        nlohmann::json report;
        // Explicit staging options inspect this process; otherwise show daemon admission state.
        if (!local && teacher_model.empty() && !std::getenv("CHITTA_ENDPOINT_DIR")) {
            const char* configured = std::getenv("CHITTA_SOCKET_PATH");
            chitta::SocketClient client(configured && *configured ? configured : chitta::SocketClient::default_socket_path());
            if (client.connect_only()) {
                nlohmann::json request = {{"jsonrpc", "2.0"}, {"method", "tools/call"}, {"id", 1},
                    {"params", {{"name", "endpoint_list"}, {"arguments", {{"probe", probe}}}}}};
                auto response = client.request(request.dump());
                if (response) {
                    auto result = nlohmann::json::parse(*response, nullptr, false);
                    if (result.contains("result") && result["result"].contains("structured")) report = result["result"]["structured"];
                }
            }
        }
        if (!report.is_object() || !report.contains("endpoints")) {
            if (!local && !std::getenv("CHITTA_ENDPOINT_DIR")) std::cerr << "Local inventory (daemon endpoint inventory unavailable or local model override).\n";
            report = chitta::endpoint_list(probe, teacher_model);
        }
        std::cout << (json_output ? report.dump(2) + "\n" : chitta::endpoint_table(report));
        return 0;
    }

    std::string socket_path = [] {
        if (const char* configured = std::getenv("CHITTA_SOCKET_PATH")) {
            if (*configured) return std::string(configured);
        }
        return chitta::SocketClient::default_socket_path();
    }();
    OutputFormat output_format = OutputFormat::Text;
    std::string tool;
    int tool_arg_index = 0;
    std::vector<std::string> tool_positional;  // positional args after the tool name, flags stripped

    // Pre-scan for --socket-path, --json, --toon flags, find tool name, and
    // collect any positional args that follow the tool (flags interleaved
    // after the tool name are honored as global flags, not tool args).
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0) {
            std::cout << "chitta " << CHITTA_VERSION << "\n";
            return 0;
        } else if (std::strcmp(argv[i], "--socket-path") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (std::strcmp(argv[i], "--json") == 0) {
            output_format = OutputFormat::Json;
        } else if (std::strcmp(argv[i], "--toon") == 0) {
            output_format = OutputFormat::Toon;
        } else if (std::strcmp(argv[i], "--text-only") == 0) {
            output_format = OutputFormat::TextOnly;
        } else if (tool.empty() && argv[i][0] != '-') {
            tool = argv[i];
            tool_arg_index = i;
        } else if (!tool.empty() && argv[i][0] != '-') {
            tool_positional.emplace_back(argv[i]);
        }
    }

    if (tool == "output_cap" || tool == "output_ref")
        return chitta::output_cache::run(tool, argc, argv, tool_arg_index);

    // Some daemon tools also have client-side shortcuts (realm_detect). Their
    // help still comes from the advertised schema, just like every other tool.
    if (tool == "realm_detect") {
        for (int i = tool_arg_index + 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
                if (auto spec = chitta::discover_cli_tool(socket_path, tool)) {
                    print_tool_help(*spec);
                    return 0;
                }
            }
        }
    }

    // Handle realm_detect command (client-side, no daemon needed)
    if (tool == "realm_detect") {
        std::string realm = detect_realm();
        switch (output_format) {
            case OutputFormat::Json:
                std::cout << "{\"realm\":\"" << realm << "\"}\n";
                break;
            case OutputFormat::Toon:
                std::cout << "realm: " << realm << "\n";
                break;
            default:
                std::cout << realm << "\n";
                break;
        }
        return 0;
    }

    // Offline fallback shares the daemon's pure policy and never opens a socket.
    if (tool == "prompt_context") {
        bool local = false;
        std::string state;
        for (int i = tool_arg_index + 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--local") == 0) local = true;
            else if (std::strcmp(argv[i], "--state") == 0 && i + 1 < argc) state = argv[++i];
        }
        if (local) {
            try {
                const auto result = chitta::prompt_policy::admit(nlohmann::json::parse(state));
                if (output_format == OutputFormat::Json) std::cout << result.dump() << "\n";
                else std::cout << result.at("fused_block").get<std::string>();
                return 0;
            } catch (const std::exception& error) {
                std::cerr << "prompt_context: " << error.what() << "\n";
                return 1;
            }
        }
    }

    // Handle status command (daemon health check)
    if (tool == "status") {
        using json = nlohmann::json;
        chitta::SocketClient client(socket_path);
        if (!client.connect()) {
            std::cout << "Daemon: not running\n";
            std::cout << "Socket: " << socket_path << " (not found)\n";
            return 1;
        }
        auto health = client.request(json{{"jsonrpc", "2.0"}, {"id", 1},
            {"method", "tools/call"}, {"params", {{"name", "health_check"},
            {"arguments", json::object()}}}}.dump());
        if (health) {
            auto response = json::parse(*health, nullptr, false);
            if (!response.is_discarded() && response.contains("result")) {
                auto state = response["result"].value("structured", json::object());
                if (state.value("loading", false)) {
                    std::cout << state.dump() << "\n";
                    return 0;
                }
            }
        }
        auto version = client.check_version();
        if (version) {
            std::cout << "Daemon: running\n";
            std::cout << "Socket: " << socket_path << "\n";
            std::cout << "Version: " << version->software << "\n";
            std::cout << "Protocol: " << version->protocol_major << "." << version->protocol_minor << "\n";
            return 0;
        }
        std::cout << "Daemon: running (version unknown)\n";
        return 0;
    }

    // queue_write: enqueue a JSONL request line to the mind-local queue.
    // Client-side only (no daemon required). Hooks use this instead of
    // `jq -c ... >> file` so args are parsed/compacted natively and the
    // append is a single atomic write() syscall (see sandbox::append_line_atomic).
    //
    // Usage: chitta queue_write <tool> <args-json>
    //   args-json may be multi-line / pretty-printed — it is normalized.
    //   Pass "-" to read args-json from stdin.
    if (tool == "queue_write") {
        if (tool_positional.size() < 2) {
            std::cerr << "usage: chitta queue_write <tool> <args-json|->\n";
            return 2;
        }
        std::string qtool = tool_positional[0];
        std::string raw_args = tool_positional[1];
        if (raw_args == "-") {
            std::string chunk((std::istreambuf_iterator<char>(std::cin)),
                              std::istreambuf_iterator<char>());
            raw_args = std::move(chunk);
        }

        nlohmann::json args_json = nlohmann::json::parse(raw_args, nullptr, false);
        if (args_json.is_discarded()) {
            std::cerr << "queue_write: invalid JSON for args\n";
            return 2;
        }

        char ack_buf[48];
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::snprintf(ack_buf, sizeof(ack_buf), "ack-%lld-%d",
                      static_cast<long long>(now_ms),
                      static_cast<int>(::getpid()));

        nlohmann::json entry = {
            {"ack_id", ack_buf},
            {"tool",   qtool},
            {"args",   args_json},
            {"ts",     now_ms / 1000}
        };

        const auto queue_path = chitta::queue_path_for_mind(chitta::SocketClient::default_mind_path());

        if (!chitta::sandbox::append_line_atomic(
                queue_path,
                entry.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace))) {
            std::cerr << "queue_write: append failed: " << queue_path << "\n";
            return 1;
        }
        return 0;
    }

    // Handle shutdown command specially (not a tool, direct daemon control)
    if (tool == "shutdown") {
        chitta::SocketClient client(socket_path);
        if (!client.connect()) {
            std::cerr << "No daemon running\n";
            return 1;
        }
        if (client.request_shutdown()) {
            std::cout << "Daemon shutdown requested\n";
            // 30s timeout: daemon may be finishing distillation or other long-running work
            if (client.wait_for_socket_gone(30000)) {
                std::cout << "Daemon stopped\n";
            }
            return 0;
        }
        std::cerr << "Failed to request shutdown\n";
        return 1;
    }

    // Check for --help without a tool
    if (tool.empty()) {
        for (int i = 1; i < argc; ++i) {
            if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
                print_usage(argv[0]);
                return 0;
            }
        }
    }

    // Handle MCP mode (JSON-RPC bridge to daemon via socket)
    if (tool == "mcp") {
        // MCP mode: connect to daemon via socket, forward JSON-RPC
        chitta::SocketClient client(socket_path);

        // Auto-starting a daemon from a client is how a SECOND chittad ended up on
        // the live mind dir on 2026-09-14 (the socket was slow, not absent) and
        // compacted away the running daemon's WAL segment. The systemd unit owns
        // the daemon; opt back in with CHITTA_CLI_AUTOSTART=1 for ad-hoc setups.
        const char* autostart_env = std::getenv("CHITTA_CLI_AUTOSTART");
        const bool autostart = autostart_env && std::string(autostart_env) == "1";
        if (!client.connect() && !autostart) {
            nlohmann::json error;
            error["jsonrpc"] = "2.0";
            error["error"]["code"] = -32603;
            error["error"]["message"] = "chittad is not reachable at " + socket_path + " (auto-start disabled; start the daemon or set CHITTA_CLI_AUTOSTART=1)";
            error["id"] = nullptr;
            std::cout << error.dump() << std::endl;
            return 1;
        }
        if (!client.connected()) {
            // Try to auto-start the daemon
            std::string chittad_path;
            if (const char* home = std::getenv("HOME")) {
                chittad_path = std::string(home) + "/.claude/bin/chittad";
            } else {
                chittad_path = "chittad";
            }

            // Start daemon in background
            std::string cmd = chittad_path + " daemon >/dev/null 2>&1 &";
            std::system(cmd.c_str());

            // Wait for daemon to start (up to 5 seconds)
            for (int i = 0; i < 50; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (client.connect()) break;
            }

            if (!client.connected()) {
                // Still can't connect - output MCP error
                nlohmann::json error;
                error["jsonrpc"] = "2.0";
                error["error"]["code"] = -32603;
                error["error"]["message"] = "Failed to start daemon";
                error["id"] = nullptr;
                std::cout << error.dump() << std::endl;
                return 1;
            }
        }

        // Read JSON-RPC from stdin, forward to daemon, write response to stdout
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.empty()) continue;

            try {
                auto mcp_request = nlohmann::json::parse(line);
                std::string method = mcp_request.value("method", "");
                auto request_id = mcp_request.value("id", nlohmann::json());

                // Handle MCP protocol methods
                if (method == "initialize") {
                    // MCP initialization response - match client's protocol version
                    auto params = mcp_request.value("params", nlohmann::json::object());
                    std::string client_version = params.value("protocolVersion", "2024-11-05");

                    nlohmann::json response;
                    response["jsonrpc"] = "2.0";
                    response["result"]["protocolVersion"] = client_version;  // Echo client's version
                    response["result"]["capabilities"]["tools"] = nlohmann::json::object();
                    response["result"]["serverInfo"]["name"] = "chitta";
                    response["result"]["serverInfo"]["version"] = CHITTA_VERSION;
                    response["id"] = request_id;
                    std::cout << response.dump() << std::endl;
                } else if (method == "notifications/initialized") {
                    // No response needed for notifications
                    continue;
                } else if (method == "tools/list") {
                    // Forward to daemon
                    nlohmann::json daemon_req;
                    daemon_req["jsonrpc"] = "2.0";
                    daemon_req["id"] = 1;
                    daemon_req["method"] = "tools/list";
                    daemon_req["params"] = nlohmann::json::object();

                    auto result_str = client.request(daemon_req.dump());
                    if (result_str) {
                        auto daemon_resp = nlohmann::json::parse(*result_str);
                        auto tools = daemon_resp.value("result", nlohmann::json::object()).value("tools", nlohmann::json::array());

                        nlohmann::json response;
                        response["jsonrpc"] = "2.0";
                        response["result"]["tools"] = tools;
                        response["id"] = request_id;
                        std::cout << response.dump() << std::endl;
                    }
                } else if (method == "tools/call") {
                    // Forward tool call to daemon
                    auto params = mcp_request.value("params", nlohmann::json::object());
                    std::string tool_name = params.value("name", "");
                    auto arguments = params.value("arguments", nlohmann::json::object());

                    // For session-aware tools, inject PPID for session lookup if session_id not provided
                    // In MCP bridge mode, getppid() returns Claude's PID
                    static const std::set<std::string> SESSION_TOOLS = {
                        "msg_inbox", "msg_send", "msg_ack", "msg_ack_all", "msg_history",
                        "ledger_save", "narrative_log", "narrative_history",
                        "anticipation_filter", "anticipation_gate_status",
                        "recall", "smart_recall", "hybrid_recall", "recall_lanes"
                    };
                    if (SESSION_TOOLS.count(tool_name) && !arguments.contains("session_id")) {
                        pid_t ppid = getppid();
                        if (ppid > 1) {  // Sanity check: not init
                            arguments["pid"] = static_cast<int64_t>(ppid);
                        }
                    }

                    nlohmann::json daemon_req;
                    daemon_req["jsonrpc"] = "2.0";
                    daemon_req["id"] = 1;
                    daemon_req["method"] = "tools/call";
                    daemon_req["params"]["name"] = tool_name;
                    daemon_req["params"]["arguments"] = arguments;

                    auto result_str = client.request(daemon_req.dump());
                    if (result_str) {
                        auto daemon_resp = nlohmann::json::parse(*result_str);
                        auto result = daemon_resp.value("result", nlohmann::json::object());

                        // Daemon already returns MCP-compatible content array
                        // Just pass it through
                        nlohmann::json response;
                        response["jsonrpc"] = "2.0";
                        response["result"]["content"] = result.value("content", nlohmann::json::array());
                        response["id"] = request_id;
                        std::cout << response.dump() << std::endl;
                    }
                } else {
                    // Unknown method
                    nlohmann::json response;
                    response["jsonrpc"] = "2.0";
                    response["error"]["code"] = -32601;
                    response["error"]["message"] = "Method not found: " + method;
                    response["id"] = request_id;
                    std::cout << response.dump() << std::endl;
                }
            } catch (const std::exception& e) {
                nlohmann::json error;
                error["jsonrpc"] = "2.0";
                error["error"]["code"] = -32700;
                error["error"]["message"] = e.what();
                error["id"] = nullptr;
                std::cout << error.dump() << std::endl;
            }
        }

        return 0;
    }

    // Discover schemas from this daemon; cached help remains available offline.
    if (!tool.empty()) {
        nlohmann::json loading;
        auto spec = chitta::discover_cli_tool(socket_path, tool, &loading);
        if (!loading.is_null()) {
            std::cout << loading["result"]["structured"].dump() << "\n";
            return tool == "health_check" || tool == "status" ? 0 : 75;
        }
        if (!spec && LEGACY_HANDLERS.count(tool)) {
            spec = nlohmann::json{{"name", tool}, {"description", "Legacy daemon handler"}};
        }
        if (spec) return run_cli(socket_path, tool, argc, argv, tool_arg_index + 1, output_format, *spec);
    }

    // No tool specified - run interactive mode or show usage
    if (tool.empty()) {
        return run_thin_client(socket_path);
    }

    // Unknown tool
    std::cerr << "Unknown option: " << tool << "\n";
    print_usage(argv[0]);
    return 1;
}
