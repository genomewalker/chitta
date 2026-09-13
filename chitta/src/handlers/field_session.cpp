// field_session RPC handlers — bodies for declarations in
// chitta/include/chitta/rpc/handlers/field_session.hpp.

#include "../../include/chitta/rpc/field_handler.hpp"

#include <cerrno>
#include <csignal>
#include <unistd.h>

namespace chitta {

namespace {
constexpr size_t kMaxPreviewChars = 500;
constexpr size_t kMaxTranscriptPageSize = 100;
constexpr size_t kMaxTranscriptCharsPerTurn = 2000;

int64_t session_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

json parse_object(const std::string& raw) {
    json value = json::parse(raw, nullptr, false);
    return value.is_object() ? value : json::object();
}

json object_param(const json& params, const char* key) {
    if (!params.contains(key)) return json::object();
    const auto& value = params[key];
    if (value.is_object()) return value;
    if (value.is_string()) return parse_object(value.get<std::string>());
    return json::object();
}

json transcript_registration(FieldStore* store, const std::string& session_id) {
    auto raw = store->get_latest_event("transcript", "register", session_id);
    return raw ? parse_object(*raw) : json::object();
}

std::string resolve_transcript_path(FieldStore* store, const std::string& session_id,
                                    const std::string& supplied = "") {
    if (!supplied.empty() && std::filesystem::is_regular_file(supplied)) return supplied;
    if (!session_id.empty()) {
        json reg = transcript_registration(store, session_id);
        std::string registered = reg.value("path", reg.value("transcript_path", ""));
        if (!registered.empty() && std::filesystem::is_regular_file(registered)) return registered;
    }

    const char* home_cstr = std::getenv("HOME");
    if (!home_cstr || session_id.empty()) return "";
    const std::vector<std::string> patterns = {
        std::string(home_cstr) + "/.claude/projects/*/" + session_id + ".jsonl",
        std::string(home_cstr) + "/.codex/sessions/*/*/*/*" + session_id + ".jsonl",
    };
    for (const auto& pattern : patterns) {
        glob_t g{};
        std::string found;
        if (glob(pattern.c_str(), GLOB_TILDE, nullptr, &g) == 0 && g.gl_pathc > 0) {
            found = g.gl_pathv[0];
        }
        globfree(&g);
        if (!found.empty()) return found;
    }
    return "";
}

void merge_session_metadata(json& entry, const json& payload) {
    for (const auto& key : {"pid", "transcript_path", "project_dir", "client",
                            "model", "thread_id", "host"}) {
        if (payload.contains(key)) entry[key] = payload[key];
    }
    json metadata = json::object();
    if (payload.contains("metadata")) {
        if (payload["metadata"].is_object()) metadata = payload["metadata"];
        else if (payload["metadata"].is_string()) metadata = parse_object(payload["metadata"]);
    }
    for (const auto& key : {"client", "model", "thread_id", "host"}) {
        if (metadata.contains(key)) entry[key] = metadata[key];
    }
    entry["metadata"] = metadata;
}

// One store round-trip for a whole (domain, kind) event slice, keyed by target.
// The alternative — get_latest_event() per session id — is O(n) FFI calls; the
// transcript list was doing up to 2x its limit of them.
std::unordered_map<std::string, json> latest_event_payloads(
    FieldStore* store, const std::string& domain, const std::string& kind, size_t cap) {
    std::unordered_map<std::string, json> result;
    json events = json::parse(
        store->get_events_by_domain_kind(domain, kind, cap), nullptr, false);
    if (!events.is_array()) return result;
    // Event queries are newest-first, so the first occurrence for each exact
    // target is the payload that belongs to the native registry row.
    for (const auto& event : events) {
        std::string sid = event.value("target", "");
        if (sid.empty() || result.count(sid)) continue;
        if (event.contains("payload") && event["payload"].is_object()) {
            result.emplace(sid, event["payload"]);
        } else if (event.contains("payload") && event["payload"].is_string()) {
            result.emplace(sid, parse_object(event["payload"].get<std::string>()));
        } else {
            result.emplace(sid, json::object());
        }
    }
    return result;
}
}

ToolResult FieldRpcHandler::tool_transcript_register(const json& params) {
    std::string session_id      = params.value("session_id", "");
    std::string transcript_path = params.value("transcript_path", "");
    std::string realm           = params.value("realm", "default");

    if (session_id.empty())      return ToolResult::error("session_id is required");
    if (transcript_path.empty()) return ToolResult::error("transcript_path is required");

    json payload = {
        {"transcript_id", session_id},
        {"path",      transcript_path},
        {"realm",     realm},
        {"last_line", 0}
    };
    uint64_t event_id = field_store_->emit_event(
        "transcript", "register", session_id, payload.dump());

    if (event_id == 0) return ToolResult::error("Failed to register transcript");

    return ToolResult::ok("Registered transcript", {
        {"session_id",      session_id},
        {"transcript_path", transcript_path},
        {"realm",           realm},
        {"event_id",        event_id}
    });
}

ToolResult FieldRpcHandler::tool_transcript_get(const json& params) {
    std::string session_id = params.value("session_id", "");
    if (session_id.empty()) return ToolResult::error("session_id is required");

    json reg = transcript_registration(field_store_, session_id);
    if (!reg.empty()) {
        reg["found"] = true;
        reg["session_id"] = session_id;
        return ToolResult::ok("Found transcript registration", reg);
    }

    return ToolResult::ok("Transcript not found", {
        {"found",      false},
        {"session_id", session_id}
    });
}

ToolResult FieldRpcHandler::tool_transcript_list(const json& params) {
    size_t limit = static_cast<size_t>(params.value("limit", 50));

    // Native registry is authoritative. Merge the durable register-event payload
    // to recover path, realm, and frontend metadata.
    std::string raw = field_store_->transcript_list(limit);
    json list_json;
    try {
        list_json = json::parse(raw, nullptr, false);
    } catch (...) {
        list_json = json::array();
    }
    if (list_json.is_discarded() || !list_json.is_array()) {
        list_json = json::array();
    }

    // One batched slice of transcript/register events instead of a per-session
    // get_latest_event() round-trip; the per-id lookup stays as the fallback for
    // a registration older than the newest `kRegisterSlice` events.
    constexpr size_t kRegisterSlice = 500;
    auto registrations = latest_event_payloads(field_store_, "transcript", "register", kRegisterSlice);
    auto registration_for = [&](const std::string& sid) -> json {
        auto it = registrations.find(sid);
        if (it != registrations.end()) return it->second;
        return transcript_registration(field_store_, sid);
    };

    std::unordered_set<std::string> seen;
    for (auto& item : list_json) {
        std::string sid = item.value("session_id", "");
        if (sid.empty()) continue;
        seen.insert(sid);
        json reg = registration_for(sid);
        for (const auto& [key, value] : reg.items()) item[key] = value;
    }

    // Compatibility recovery: old register events did not populate the native
    // TranscriptRegistry. Session IDs give us an exact, bounded lookup without
    // the previous unrelated keyword-recall fallback.
    json sessions = json::parse(field_store_->session_list(false), nullptr, false);
    if (sessions.is_array()) {
        for (const auto& session : sessions) {
            if (list_json.size() >= limit) break;
            std::string sid = session.value("session_id", "");
            if (sid.empty() || seen.count(sid)) continue;
            json reg = registration_for(sid);
            if (reg.empty()) continue;
            reg["session_id"] = sid;
            reg["transcript_id"] = sid;
            reg["turn_count"] = 0;
            reg["progress_pct"] = 0.0;
            list_json.push_back(reg);
            seen.insert(sid);
        }
    }

    std::ostringstream ss;
    ss << "Registered transcripts: " << list_json.size() << "\n";
    ss << "Note: session state tracked via events\n";

    return ToolResult::ok(ss.str(), {
        {"transcripts", list_json},
        {"count",       list_json.size()}
    });
}

ToolResult FieldRpcHandler::tool_transcript_update(const json& params) {
    std::string session_id = params.value("session_id", "");
    int64_t last_line      = params.value("last_line", int64_t(0));

    if (session_id.empty()) return ToolResult::error("session_id is required");

    json payload = {{"last_line", last_line}};
    uint64_t event_id = field_store_->emit_event(
        "transcript", "progress", session_id, payload.dump());

    if (event_id == 0) return ToolResult::error("Failed to update transcript progress");

    return ToolResult::ok("Updated transcript progress", {
        {"session_id", session_id},
        {"last_line",  last_line},
        {"event_id",   event_id}
    });
}

ToolResult FieldRpcHandler::tool_transcript_remove(const json& params) {
    std::string session_id = params.value("session_id", "");
    if (session_id.empty()) return ToolResult::error("session_id is required");

    uint64_t event_id = field_store_->emit_event(
        "transcript", "remove", session_id, "");

    if (event_id == 0) return ToolResult::error("Failed to remove transcript");

    return ToolResult::ok("Removed transcript", {
        {"session_id", session_id},
        {"event_id",   event_id}
    });
}

ToolResult FieldRpcHandler::tool_transcript_parse(const json& params) {
    std::string session_id = params.value("session_id", "");
    size_t min_turns       = static_cast<size_t>(params.value("min_turns", 4));

    if (session_id.empty()) return ToolResult::error("session_id is required");

    std::string path = resolve_transcript_path(field_store_, session_id);

    if (path.empty()) {
        return ToolResult::error("Could not find transcript for session: " + session_id);
    }

    std::ifstream file(path);
    if (!file) {
        return ToolResult::error("Cannot open transcript: " + path);
    }

    std::string line;
    size_t turn_count = 0;
    int64_t line_number = 0;

    while (std::getline(file, line)) {
        ++line_number;
        if (line.empty()) continue;
        auto obj = json::parse(line, nullptr, false);
        if (obj.is_discarded()) continue;
        std::string type = obj.value("type", "");
        if (type == "user" || type == "assistant") {
            ++turn_count;
        } else if (type == "response_item" && obj.contains("payload")) {
            const auto& payload = obj["payload"];
            std::string ptype = payload.value("type", "");
            std::string role = payload.value("role", "");
            if (ptype == "message" && (role == "user" || role == "assistant")) {
                ++turn_count;
            }
        }
    }

    bool ready = turn_count >= min_turns;

    std::ostringstream ss;
    ss << "Parsed " << turn_count << " turns from transcript\n";
    ss << "  Session: " << session_id << "\n";
    ss << "  Path: " << path << "\n";
    ss << "  Lines: " << line_number << "\n";
    ss << "  Ready: " << (ready ? "yes" : "no") << "\n";

    return ToolResult::ok(ss.str(), {
        {"session_id",  session_id},
        {"path",        path},
        {"turns_found", turn_count},
        {"min_turns",   min_turns},
        {"last_line",   line_number},
        {"ready",       ready}
    });
}

ToolResult FieldRpcHandler::tool_transcript_search(const json& params) {
    std::string query      = params.value("query", "");
    std::string session_id = params.value("session_id", "");
    size_t limit           = std::min(static_cast<size_t>(params.value("limit", 10)), kMaxTranscriptPageSize);

    if (query.empty()) return ToolResult::error("query is required");

    // Semantic search via embedding
    auto embedding = embed_query(query);
    json results_json = json::array();

    if (!embedding.empty()) {
        auto hits = field_store_->recall(embedding, limit, "");
        for (const auto& h : hits) {
            if (!session_id.empty() && h.content.find(session_id) == std::string::npos) continue;
            results_json.push_back({
                {"memory_id",  h.memory_id},
                {"score",      h.score},
                {"content",    h.content.size() > kMaxPreviewChars ? utf8_trunc(h.content, kMaxPreviewChars) + "..." : h.content},
                {"kind",       h.kind},
                {"realm",      h.realm}
            });
        }
    }

    // Keyword fallback / supplement
    if (results_json.size() < limit) {
        std::string kw_query = query;
        if (!session_id.empty()) kw_query = session_id + " " + query;
        auto kw_hits = field_store_->recall_keyword(kw_query, limit);
        std::unordered_set<uint64_t> seen;
        for (const auto& r : results_json) {
            seen.insert(r.value("memory_id", uint64_t(0)));
        }
        for (const auto& h : kw_hits) {
            if (seen.count(h.memory_id)) continue;
            results_json.push_back({
                {"memory_id",  h.memory_id},
                {"score",      h.score},
                {"content",    h.content.size() > kMaxPreviewChars ? utf8_trunc(h.content, kMaxPreviewChars) + "..." : h.content},
                {"kind",       h.kind},
                {"realm",      h.realm}
            });
            if (results_json.size() >= limit) break;
        }
    }

    std::ostringstream ss;
    ss << "Found " << results_json.size() << " matching passages for: " << query << "\n";

    return ToolResult::ok(ss.str(), {
        {"query",      query},
        {"results",    results_json},
        {"count",      results_json.size()}
    });
}

ToolResult FieldRpcHandler::tool_read_transcript(const json& params) {
    std::string path       = params.value("path", "");
    std::string session_id = params.value("session_id", "");
    int start_turn         = params.value("start_turn", 0);
    size_t limit           = std::min(static_cast<size_t>(params.value("limit", 20)), kMaxTranscriptPageSize);
    size_t max_chars       = std::min(static_cast<size_t>(params.value("max_chars_per_turn", 500)),
                                      kMaxTranscriptCharsPerTurn);
    std::string role_filter = params.value("role_filter", "");
    std::string keyword    = params.value("keyword", "");
    bool metadata_only     = params.value("metadata_only", false);

    path = resolve_transcript_path(field_store_, session_id, path);

    if (path.empty()) {
        return ToolResult::error("No transcript path provided and could not find session");
    }

    std::ifstream file(path);
    if (!file) {
        return ToolResult::error("Cannot open transcript: " + path);
    }

    // First pass: collect all turns
    struct Turn {
        std::string role;
        std::string content;
        int64_t line_number;
        int turn_index;
    };

    std::vector<Turn> all_turns;
    std::string line;
    int64_t line_number = 0;
    int turn_idx = 0;

    while (std::getline(file, line)) {
        ++line_number;
        if (line.empty()) continue;
        auto obj = json::parse(line, nullptr, false);
        if (obj.is_discarded()) continue;

        std::string type = obj.value("type", "");
        std::string role;
        std::string content;

        if (type == "user" || type == "assistant") {
            // Claude format
            role = type;
            if (obj.contains("message")) {
                const auto& msg = obj["message"];
                if (msg.contains("content")) {
                    const auto& mc = msg["content"];
                    if (mc.is_string()) {
                        content = mc.get<std::string>();
                    } else if (mc.is_array()) {
                        for (const auto& block : mc) {
                            if (block.contains("text") && block["text"].is_string()) {
                                if (!content.empty()) content += "\n";
                                content += block["text"].get<std::string>();
                            }
                        }
                    }
                }
            }
        } else if (type == "response_item" && obj.contains("payload")) {
            // Codex format
            const auto& p = obj["payload"];
            std::string ptype = p.value("type", "");
            std::string prole = p.value("role", "");

            if (ptype == "message" && (prole == "user" || prole == "assistant")
                && p.contains("content") && p["content"].is_array()) {
                role = prole;
                for (const auto& block : p["content"]) {
                    std::string btype = block.value("type", "");
                    if ((btype == "input_text" || btype == "output_text")
                        && block.contains("text") && block["text"].is_string()) {
                        if (!content.empty()) content += "\n";
                        content += block["text"].get<std::string>();
                    }
                }
            } else {
                continue;  // reasoning/function_call/developer/tool_outputs skipped here
            }
        } else {
            continue;  // session_meta, event_msg, unknown
        }

        if (content.empty()) continue;
        all_turns.push_back({role, content, line_number, turn_idx++});
    }

    size_t total_chars = 0;
    for (const auto& t : all_turns) total_chars += t.content.size();

    json result;
    result["path"]        = path;
    result["total_turns"] = all_turns.size();
    result["total_chars"] = total_chars;
    result["last_line"]   = line_number;

    if (metadata_only) {
        std::ostringstream ss;
        ss << "Transcript: " << path << "\n"
           << "Total turns: " << all_turns.size() << "\n"
           << "Total chars: " << total_chars << "\n"
           << "Lines: " << line_number;
        return ToolResult::ok(ss.str(), result);
    }

    // Apply filters
    std::vector<const Turn*> filtered;
    for (const auto& t : all_turns) {
        if (!role_filter.empty() && t.role != role_filter) continue;
        if (!keyword.empty() && t.content.find(keyword) == std::string::npos) continue;
        filtered.push_back(&t);
    }

    result["filtered_turns"] = filtered.size();

    // Resolve negative start_turn (Python-style: -1 = last, -30 = 30 from end)
    size_t effective_start = 0;
    if (start_turn < 0) {
        int from_end = static_cast<int>(filtered.size()) + start_turn;
        effective_start = from_end > 0 ? static_cast<size_t>(from_end) : 0;
    } else {
        effective_start = static_cast<size_t>(start_turn);
    }

    // Paginate
    json turns_arr = json::array();
    size_t output_chars = 0;
    const size_t max_output_chars = 30000;

    for (size_t i = effective_start;
         i < filtered.size() && turns_arr.size() < limit; ++i) {
        const Turn& t = *filtered[i];

        std::string content = t.content;
        if (max_chars > 0 && content.size() > max_chars) {
            content = utf8_trunc(content, max_chars) + "...";
        }

        if (output_chars + content.size() > max_output_chars) {
            turns_arr.push_back({
                {"role",       "system"},
                {"content",    "[output truncated — use start_turn=" + std::to_string(i) + " to continue]"},
                {"turn_index", -1}
            });
            break;
        }

        output_chars += content.size();
        turns_arr.push_back({
            {"role",        t.role},
            {"content",     content},
            {"turn_index",  t.turn_index},
            {"line_number", t.line_number}
        });
    }

    result["turns"]      = turns_arr;
    result["returned"]   = turns_arr.size();
    result["start_turn"] = static_cast<int>(effective_start);

    std::ostringstream ss;
    ss << "Transcript: " << path << "\n"
       << "Total: " << all_turns.size() << " turns";
    if (!role_filter.empty() || !keyword.empty()) {
        ss << " (filtered: " << filtered.size() << ")";
    }
    ss << "\nShowing turns " << effective_start
       << "-" << (effective_start + turns_arr.size() - 1) << ":\n\n";

    for (const auto& t : turns_arr) {
        ss << "[" << t["role"].get<std::string>() << "] ";
        std::string c = t["content"].get<std::string>();
        ss << (c.size() > 100 ? c.substr(0, 100) + "..." : c) << "\n";
    }

    return ToolResult::ok(ss.str(), result);
}

ToolResult FieldRpcHandler::tool_get_turns(const json& params) {
    std::string session_id = params.value("session_id", "");
    int start_index        = params.value("start_index", 0);
    size_t limit           = static_cast<size_t>(params.value("limit", 50));

    if (session_id.empty()) return ToolResult::error("session_id is required");

    std::string path = resolve_transcript_path(field_store_, session_id);

    if (path.empty()) {
        return ToolResult::ok("Transcript not found for session", {
            {"session_id", session_id},
            {"turns",      json::array()},
            {"count",      0}
        });
    }

    // Delegate to tool_read_transcript with adjusted params
    json rp = {
        {"path",        path},
        {"session_id",  session_id},
        {"start_turn",  start_index},
        {"limit",       static_cast<int>(limit)}
    };
    return tool_read_transcript(rp);
}

ToolResult FieldRpcHandler::tool_create_episode(const json& params) {
    std::string session_id   = params.value("session_id", "");
    std::string title        = params.value("title", "");
    int start_turn           = params.value("start_turn", 0);
    int end_turn             = params.value("end_turn", 0);
    std::string episode_type = params.value("episode_type", "distillation");
    std::string realm        = params.value("realm", "brahman");

    if (session_id.empty() || title.empty()) {
        return ToolResult::error("session_id and title are required");
    }

    std::string content = title + ": turns " + std::to_string(start_turn)
        + "-" + std::to_string(end_turn);

    std::vector<float> embedding;
    if (params.contains("_preembedding")) {
        embedding = params["_preembedding"].get<std::vector<float>>();
    } else {
        embedding = embed_text(content);
    }

    uint64_t episode_id = field_store_->remember(
        "episode", realm, content, embedding, 0.8f, 0.0f);

    // Link episode to session via triplet
    if (episode_id > 0) {
        field_store_->add_triplet(
            std::to_string(episode_id), "derived_from", session_id);
    }

    std::ostringstream msg;
    msg << "Created episode " << episode_id << ": " << title
        << " (turns " << start_turn << "-" << end_turn << ")";

    return ToolResult::ok(msg.str(), {
        {"episode_id",   episode_id},
        {"session_id",   session_id},
        {"title",        title},
        {"start_turn",   start_turn},
        {"end_turn",     end_turn},
        {"episode_type", episode_type},
        {"realm",        realm}
    });
}

ToolResult FieldRpcHandler::tool_msg_send(const json& params) {
    std::string target  = params.value("target", "");
    std::string content = params.value("content", "");

    if (target.empty())  return ToolResult::error("target is required");
    if (content.empty()) return ToolResult::error("content is required");

    std::string sender_session_id = params.value("sender_session_id", get_session_id());
    std::string sender_realm      = params.value("sender_realm", "");
    int priority                  = params.value("priority", 1);
    std::string content_type      = params.value("content_type", "text");

    // Auto-detect sender hostname for remote tracing
    std::string sender_host;
    {
        char buf[256];
        if (gethostname(buf, sizeof(buf)) == 0) sender_host = buf;
    }

    json payload = {
        {"content",           content},
        {"sender_session_id", sender_session_id},
        {"sender_realm",      sender_realm},
        {"sender_host",       sender_host},
        {"priority",          priority},
        {"content_type",      content_type}
    };
    std::string payload_str = payload.dump();

    // Resolve target sessions for broadcast targets
    std::vector<std::string> targets;
    if (target == "*" || target.rfind("realm:", 0) == 0) {
        std::string raw = field_store_->session_list(true);
        json sessions = json::parse(raw, nullptr, false);
        if (!sessions.is_discarded() && sessions.is_array()) {
            std::string realm_filter;
            if (target.rfind("realm:", 0) == 0) {
                realm_filter = target.substr(6);
            }
            for (const auto& s : sessions) {
                std::string sid = s.value("session_id", "");
                if (sid.empty() || sid == sender_session_id) continue;
                if (!realm_filter.empty() && s.value("realm", "") != realm_filter) continue;
                targets.push_back(sid);
            }
        }
    } else {
        targets.push_back(target);
    }

    if (targets.empty()) {
        return ToolResult::ok("No recipients found", {
            {"target",            target},
            {"recipients",        0},
            {"sender_session_id", sender_session_id}
        });
    }

    json delivered = json::array();
    for (const auto& t : targets) {
        uint64_t eid = field_store_->emit_event("msg", "send", t, payload_str);
        if (eid != 0) {
            delivered.push_back({{"session_id", t}, {"event_id", eid}});
        }
    }

    std::ostringstream ss;
    ss << "Message sent to " << delivered.size() << " recipient(s)";

    return ToolResult::ok(ss.str(), {
        {"target",            target},
        {"recipients",        delivered.size()},
        {"delivered_to",      delivered},
        {"priority",          priority},
        {"sender_session_id", sender_session_id}
    });
}

ToolResult FieldRpcHandler::tool_msg_inbox(const json& params) {
    std::string session_id = params.value("session_id", get_session_id());
    size_t limit           = static_cast<size_t>(params.value("limit", 20));

    auto events_json_str = field_store_->get_events_by_target("msg", "send", session_id, limit);

    json events = json::parse(events_json_str, nullptr, false);
    if (events.is_discarded() || !events.is_array()) {
        return ToolResult::ok("No messages found", {
            {"session_id", session_id},
            {"count",      0},
            {"messages",   json::array()}
        });
    }

    json msg_list = json::array();
    for (const auto& ev : events) {
        uint64_t event_id = ev.value("event_id", (uint64_t)0);
        // Skip acknowledged messages
        if (field_store_->has_event("msg", "ack", std::to_string(event_id))) continue;
        json payload = ev.value("payload", json::object());
        msg_list.push_back({
            {"memory_id",         event_id},
            {"content",           payload.value("content", "")},
            {"sender_session_id", payload.value("sender_session_id", "")},
            {"sender_realm",      payload.value("sender_realm", "")},
            {"sender_host",       payload.value("sender_host", "")},
            {"content_type",      payload.value("content_type", "text")},
            {"score",             payload.value("priority", 1)},
            {"ts_ms",             ev.value("ts_ms", 0)}
        });
    }

    if (msg_list.empty()) {
        return ToolResult::ok("No messages found", {
            {"session_id", session_id},
            {"count",      0},
            {"messages",   json::array()}
        });
    }

    std::ostringstream ss;
    ss << msg_list.size() << " message(s) found for " << session_id << "\n";

    return ToolResult::ok(ss.str(), {
        {"session_id", session_id},
        {"count",      msg_list.size()},
        {"messages",   msg_list}
    });
}

ToolResult FieldRpcHandler::tool_msg_respond(const json& params) {
    auto [msg_id, msg_id_str] = parse_id(params, "message_id");
    if (msg_id <= 0) return ToolResult::error("message_id is required");

    std::string content = params.value("content", "");
    if (content.empty()) return ToolResult::error("content is required");

    // Look up original event to find sender and original target
    auto ev_str = field_store_->get_event_by_id(static_cast<uint64_t>(msg_id));
    json ev = json::parse(ev_str, nullptr, false);
    if (ev.is_discarded() || !ev.contains("target")) {
        return ToolResult::error("message_id not found");
    }

    std::string original_target = ev.value("target", "");  // who received it (= us)
    json payload = ev.value("payload", json::object());
    std::string reply_to = payload.value("sender_session_id", ""); // who sent it (= reply target)

    if (reply_to.empty()) return ToolResult::error("original message has no sender_session_id");

    // Use caller-supplied session_id if given, otherwise use original_target (who the message was for)
    std::string my_session_id = params.value("session_id", original_target);
    if (my_session_id.empty()) my_session_id = get_session_id();

    int priority         = params.value("priority", 1);
    std::string ctype    = params.value("content_type", "text");

    json reply_payload = {
        {"content",           content},
        {"sender_session_id", my_session_id},
        {"priority",          priority},
        {"content_type",      ctype},
        {"reply_to_id",       msg_id}
    };
    uint64_t event_id = field_store_->emit_event("msg", "send", reply_to, reply_payload.dump());

    return ToolResult::ok("Reply sent to " + reply_to, {
        {"event_id",   event_id},
        {"target",     reply_to},
        {"sender",     my_session_id},
        {"reply_to_id", msg_id}
    });
}

ToolResult FieldRpcHandler::tool_msg_ack(const json& params) {
    std::string message_id = params.value("message_id", "");
    if (message_id.empty()) {
        // Try numeric
        auto [id, id_str] = parse_id(params, "message_id");
        if (id <= 0) return ToolResult::error("message_id is required");
        message_id = id_str;
    }

    uint64_t event_id = field_store_->emit_event(
        "msg", "ack", message_id, "");

    return ToolResult::ok("Message acknowledged", {
        {"message_id", message_id},
        {"event_id",   event_id}
    });
}

ToolResult FieldRpcHandler::tool_msg_ack_all(const json& params) {
    std::string session_id = params.value("session_id", get_session_id());

    uint64_t event_id = field_store_->emit_event(
        "msg", "ack_all", session_id, "");

    return ToolResult::ok("All messages acknowledged", {
        {"session_id", session_id},
        {"event_id",   event_id}
    });
}

ToolResult FieldRpcHandler::tool_msg_history(const json& params) {
    std::string session_id = params.value("session_id", get_session_id());
    size_t limit           = static_cast<size_t>(params.value("limit", 30));

    auto events_json_str = field_store_->get_events_by_target("msg", "send", session_id, limit);

    json events = json::parse(events_json_str, nullptr, false);
    if (events.is_discarded() || !events.is_array()) events = json::array();

    json msg_list = json::array();
    for (const auto& ev : events) {
        json payload = ev.value("payload", json::object());
        msg_list.push_back({
            {"memory_id",         ev.value("event_id", 0)},
            {"content",           payload.value("content", "")},
            {"sender_session_id", payload.value("sender_session_id", "")},
            {"content_type",      payload.value("content_type", "text")},
            {"score",             payload.value("priority", 1)},
            {"ts_ms",             ev.value("ts_ms", 0)},
            {"kind",              "msg_send"}
        });
    }

    if (msg_list.empty()) {
        return ToolResult::ok("No message history", {
            {"session_id", session_id},
            {"count",      0},
            {"messages",   json::array()}
        });
    }

    std::ostringstream ss;
    ss << "Message history (" << msg_list.size() << " messages):\n";

    return ToolResult::ok(ss.str(), {
        {"session_id", session_id},
        {"count",      msg_list.size()},
        {"messages",   msg_list}
    });
}

ToolResult FieldRpcHandler::tool_session_register(const json& params) {
    std::string session_id      = params.value("session_id", get_session_id());
    std::string realm           = params.value("realm", "brahman");
    std::string transcript_path = params.value("transcript_path", "");
    std::string project_dir     = params.value("project_dir", "");
    json metadata_obj           = object_param(params, "metadata");
    std::string client          = params.value("client", metadata_obj.value("client", "unknown"));
    int32_t pid = params.contains("pid")
        ? params["pid"].get<int32_t>()
        : static_cast<int32_t>(getpid());

    json payload = {
        {"kind",            client},
        {"client",          client},
        {"realm",           realm},
        {"pid",             pid},
        {"transcript_path", transcript_path},
        {"project_dir",     project_dir},
        {"metadata",        metadata_obj}
    };

    uint64_t event_id = field_store_->emit_event(
        "session", "register", session_id, payload.dump(), 0, realm);

    if (event_id == 0) return ToolResult::error("Failed to register session");
    json binding = {{"session_id",session_id},{"client",client},{"project_dir",project_dir},
        {"transcript_path",transcript_path},{"metadata",metadata_obj}};
    auto tid = metadata_obj.value("thread_id", "");
    if (!tid.empty()) binding["thread_id"] = tid;
    auto bound = tool_ledger_op({{"op","session_bind"},{"args",binding}});
    if (bound.is_error) return bound;

    return ToolResult::ok("Session registered", {
        {"session_id",      session_id},
        {"realm",           realm},
        {"pid",             pid},
        {"transcript_path", transcript_path},
        {"event_id",        event_id}
    });
}

ToolResult FieldRpcHandler::tool_session_heartbeat(const json& params) {
    std::string session_id = params.value("session_id", get_session_id());
    json payload = {{"metadata", object_param(params, "metadata")}};

    uint64_t event_id = field_store_->emit_event(
        "session", "heartbeat", session_id, payload.dump());

    if (event_id == 0) return ToolResult::error("Failed to send heartbeat");
    auto touched = tool_ledger_op({{"op","session_touch"},{"args",{{"session_id",session_id}}}});
    if (touched.is_error) return touched;

    return ToolResult::ok("Heartbeat sent", {
        {"session_id", session_id},
        {"event_id",   event_id}
    });
}

ToolResult FieldRpcHandler::tool_session_list(const json& params) {
    std::string status_filter = params.value("status", "active");
    bool active_only = params.value("active_only", status_filter == "active");
    std::string realm_filter = params.value("realm", "");
    int64_t ttl_seconds = params.value("ttl_seconds", int64_t(900));
    if (const char* env_ttl = std::getenv("CHITTA_SESSION_TTL_SECONDS")) {
        try { ttl_seconds = std::stoll(env_ttl); } catch (...) {}
    }
    ttl_seconds = std::max<int64_t>(30, ttl_seconds);
    int64_t now_ms = session_now_ms();

    json native = json::parse(field_store_->session_list(false), nullptr, false);
    if (!native.is_array() || native.empty()) {
        return ToolResult::ok("No sessions found", {
            {"count",    0},
            {"sessions", json::array()}
        });
    }

    json sessions_out = json::array();
    auto registrations = latest_event_payloads(field_store_, "session", "register", 500);
    auto heartbeats = latest_event_payloads(field_store_, "session", "heartbeat", 500);
    char host_buf[256]{};
    std::string local_host;
    if (gethostname(host_buf, sizeof(host_buf)) == 0) local_host = host_buf;
    for (auto entry : native) {
        std::string sid = entry.value("session_id", "");
        if (sid.empty()) continue;
        auto reg = registrations.find(sid);
        if (reg != registrations.end()) merge_session_metadata(entry, reg->second);
        auto beat = heartbeats.find(sid);
        if (beat != heartbeats.end()) merge_session_metadata(entry, beat->second);
        if (!realm_filter.empty() && entry.value("realm", "") != realm_filter) continue;

        int64_t last_heartbeat = entry.value("last_heartbeat_ms", int64_t(0));
        bool fresh = last_heartbeat > 0 && now_ms - last_heartbeat <= ttl_seconds * 1000;
        bool registry_active = entry.value("status", "") == "active";
        int32_t pid = entry.value("pid", int32_t(0));
        std::string host = entry.value("host", "");
        bool local_pid = host.empty() || local_host.empty() || host == local_host;
        bool pid_alive = pid <= 0 || !local_pid || kill(pid, 0) == 0 || errno == EPERM;
        bool live = registry_active && fresh && pid_alive;
        entry["is_live"] = live;
        entry["heartbeat_age_ms"] = last_heartbeat > 0 ? now_ms - last_heartbeat : -1;
        entry["status"] = live ? "active" : (registry_active ? "stale" : "closed");
        if (active_only && !live) continue;
        if (!status_filter.empty() && status_filter != "all"
            && entry.value("status", "") != status_filter) continue;
        sessions_out.push_back(entry);
    }

    // Sort by actual heartbeat rather than register-event retrieval order.
    std::sort(sessions_out.begin(), sessions_out.end(), [](const json& a, const json& b) {
        return a.value("last_heartbeat_ms", int64_t(0))
             > b.value("last_heartbeat_ms", int64_t(0));
    });

    std::ostringstream ss;
    ss << sessions_out.size() << " session(s) found";
    if (active_only) ss << " (active only)";

    return ToolResult::ok(ss.str(), {
        {"count",    sessions_out.size()},
        {"sessions", sessions_out}
    });
}

ToolResult FieldRpcHandler::tool_session_deregister(const json& params) {
    std::string session_id = params.value("session_id", get_session_id());
    if (session_id.empty()) return ToolResult::error("session_id is required");

    uint64_t event_id = field_store_->emit_event(
        "session", "deregister", session_id, "");

    if (event_id == 0) return ToolResult::error("Failed to deregister session");
    auto closed = tool_ledger_op({{"op","session_close"},{"args",{{"session_id",session_id},{"status",params.value("status","ended")}}}});
    if (closed.is_error) return closed;

    return ToolResult::ok("Session deregistered", {
        {"session_id", session_id},
        {"event_id",   event_id}
    });
}

ToolResult FieldRpcHandler::tool_session_sync(const json& params) {
    std::string projects_dir = params.value("projects_dir", "");

    if (projects_dir.empty()) {
        const char* env = std::getenv("CLAUDE_PROJECTS_DIR");
        if (env) {
            projects_dir = env;
        } else {
            const char* home_cstr = std::getenv("HOME");
            if (home_cstr) projects_dir = std::string(home_cstr) + "/.claude/projects";
        }
    }

    size_t discovered = 0;
    if (!projects_dir.empty() && std::filesystem::exists(projects_dir)) {
        for (const auto& entry : std::filesystem::directory_iterator(projects_dir)) {
            if (!entry.is_directory()) continue;
            for (const auto& file : std::filesystem::directory_iterator(entry.path())) {
                if (file.path().extension() == ".jsonl") ++discovered;
            }
        }
    }

    json payload = {
        {"projects_dir", projects_dir},
        {"discovered",   discovered}
    };
    field_store_->emit_event("session", "sync", "all", payload.dump());

    std::ostringstream ss;
    ss << "Session sync: discovered " << discovered << " transcript(s) in " << projects_dir;

    return ToolResult::ok(ss.str(), {
        {"projects_dir", projects_dir},
        {"discovered",   discovered}
    });
}

} // namespace chitta
