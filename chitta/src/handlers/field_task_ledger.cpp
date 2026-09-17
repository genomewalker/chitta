#include "../../include/chitta/rpc/field_handler.hpp"
#include <limits>
#include <mutex>
#include <chitta/hook_ledger_policy.hpp>
#include <chitta/hook_pretool_policy.hpp>
#include <chitta/hook_saddle_policy.hpp>
#include <chitta/hook_ancillary_policy.hpp>
#include <chitta/hook_session_policy.hpp>

namespace chitta {
void FieldRpcHandler::load_task_ledger() {
    // Constructor-only replay completes before the handler is published to workers.
    auto events = json::parse(field_store_->task_ledger_events());
    std::sort(events.begin(), events.end(), [](const json& x, const json& y) {
        return x.at("payload").at("revision") < y.at("payload").at("revision");
    });
    for (const auto& event : events)
        task_ledger_.replay(event.at("payload"));
}

ToolResult FieldRpcHandler::tool_ledger_op(const json& params) {
    // Keep multi-operation stream lifecycle updates ordered, including recursive
    // session registration/heartbeat calls. The underlying ledger also locks WAL writes.
    static std::recursive_mutex stream_mutex;
    std::lock_guard<std::recursive_mutex> guard(stream_mutex);
    try {
        const auto started = std::chrono::steady_clock::now();
        auto op   = params.at("op").get<std::string>();
        auto args = params.value("args", json::object());
        if (args.is_string()) args = json::parse(args.get<std::string>());
        if (!args.is_object()) return ToolResult::error("args must be an object");
        args        = rpc::clamp_read_arguments("ledger_op." + op, std::move(args));
        auto run = [&](const std::string& operation, const json& input) {
            return task_ledger_.run(operation, input, [&](const json& batch) {
                if (!field_store_->emit_event("ledger", "task_records", "task-ledger", batch.dump()))
                    throw std::runtime_error("ledger WAL append failed");
            });
        };
        auto ok = [&](const json& value) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            return ToolResult::ok("Ledger operation complete", {{"value", value}, {"assembly_ms", ms}});
        };
        auto invoke = [&](const std::string& tool, const json& input) {
            const auto handler = handlers_.find(tool);
            if (handler == handlers_.end()) throw std::runtime_error("missing hook dependency: " + tool);
            const auto result = handler->second(input);
            if (result.is_error) throw std::runtime_error(result.text);
            return json{{"text", result.text}, {"structured", result.structured}};
        };
        auto owned_lease = [&](const std::string& tid, const std::string& sid) -> json {
            const auto page = run("lease_list", {{"session_id", sid}, {"active_only", false}, {"limit", 1000}});
            for (const auto& lease : page.at("rows"))
                if (lease.value("thread_id", "") == tid) return lease;
            return nullptr;
        };
        if (op == "session_touch") {
            const auto sid = args.at("session_id").get<std::string>();
            const auto session = run("session_get", {{"session_id", sid}});
            if (session.is_null()) return ok(run(op, args));
            const auto metadata = json::parse(session.value("metadata_json", "{}"));
            const auto stream = metadata.value("stream", "");
            const auto tid = "stream:" + stream;
            const auto lease = stream.empty() ? json(nullptr) : owned_lease(tid, sid);
            const auto touched = run(op, args);
            if (!lease.is_null() && lease.value("session_id", "") == sid)
                run("lease_claim", {{"thread_id", tid}, {"session_id", sid},
                    {"ttl", metadata.value("ttl", 21600)}});
            return ok(touched);
        }
        if (op == "stream_handoff") {
            const auto sid = args.at("session_id").get<std::string>();
            const auto stream = args.at("stream").get<std::string>();
            const auto lease = owned_lease("stream:" + stream, sid);
            if (lease.is_null() || lease.value("session_id", "") != sid ||
                lease.value("expires_at", 0.0) <= TaskLedger::now())
                return ToolResult::error("stream handoff requires a live owned claim");
            const auto content = args.at("content").get<std::string>();
            if (content.rfind("[handoff] stream=" + stream + " ", 0) != 0)
                return ToolResult::error("handoff must name the claimed stream");
            const auto saved = invoke("remember", {{"type", "signal"}, {"realm", "chitta"},
                {"visibility", 1}, {"tags", json::array({"handoff"})}, {"content", content}});
            invoke("ledger_op", {{"op", "session_touch"}, {"args", {{"session_id", sid}}}});
            return ok({{"saved", saved}, {"lease", owned_lease("stream:" + stream, sid)}});
        }
        // Stream ownership uses the ledger's atomic, WAL-backed lease transaction.
        // Names are prefixed so ordinary conversational threads cannot collide.
        if (op == "stream_claim" || op == "stream_release" || op == "stream_list") {
            const auto name = args.value("stream", "");
            if (op != "stream_list" && name.empty())
                return ToolResult::error("stream is required");
            const auto tid = "stream:" + name;
            if (op == "stream_list") {
                json claims = json::array();
                const auto page = run("lease_list", {{"limit", 1000}});
                for (auto lease : page.at("rows")) {
                    const auto id = lease.value("thread_id", "");
                    if (id.rfind("stream:", 0) != 0 ||
                        lease.value("expires_at", 0.0) <= TaskLedger::now() ||
                        (!name.empty() && id != tid)) continue;
                    const auto session = run("session_get", {{"session_id", lease.at("session_id")}});
                    lease["stream"] = id.substr(7);
                    lease["metadata"] = json::parse(session.value("metadata_json", "{}"));
                    claims.push_back(lease);
                }
                return ok({{"claims", claims}});
            }
            const auto sid = args.at("session_id").get<std::string>();
            if (sid.empty()) return ToolResult::error("session_id is required");
            if (op == "stream_release") {
                const auto released = run("lease_release", {{"thread_id", tid}, {"session_id", sid}});
                if (released == true)
                    invoke("session_heartbeat", {{"session_id", sid}, {"metadata", {{"stream_claim", nullptr}}}});
                return ok({{"released", released}});
            }
            const auto ttl = args.value("ttl", 21600);
            if (ttl < 30 || ttl > 604800) return ToolResult::error("ttl must be 30..604800 seconds");
            json metadata = {{"stream", name}, {"worktree", args.at("worktree")},
                             {"branch", args.at("branch")}, {"task_title", args.at("title")},
                             {"ttl", ttl}};
            run("thread_create", {{"id", tid}, {"title", args.at("title")}, {"realm", "chitta"}});
            run("session_bind", {{"session_id", sid}, {"project_dir", args.at("worktree")}});
            auto claim = run("lease_claim", {{"thread_id", tid}, {"session_id", sid}, {"ttl", ttl}});
            if (!claim.value("claimed", false)) return ok(claim);
            metadata["expires_at"] = claim.at("expires_at");
            run("session_bind", {{"session_id", sid}, {"metadata", metadata}});
            invoke("session_register", {{"session_id", sid}, {"name", name},
                   {"metadata", {{"stream_claim", metadata}}}});
            invoke("session_heartbeat", {{"session_id", sid}, {"metadata", {{"stream_claim", metadata}}}});
            claim["stream"] = name;
            claim["metadata"] = metadata;
            return ok(claim);
        }
        if (op == "hook_session_start") return ok(hook_policy::session_start(args, invoke));
        if (op == "hook_ancillary") {
            if (args.value("family", "") == "shepherd" && args.value("list_tasks", false)) {
                auto plan = hook_policy::plan();
                plan["tasks"] = json::array();
                for (const auto& task : json::parse(field_store_->task_list("long_task", true))) {
                    const auto id = task.value("task_id", "");
                    if (id.rfind("shepherd-", 0) != 0) continue;
                    const auto current = tool_long_task_get({{"task_id", id}});
                    if (current.is_error) throw std::runtime_error(current.text);
                    plan["tasks"].push_back(current.structured);
                }
                return ok(plan);
            }
            return ok(hook_policy::ancillary(args, invoke));
        }
        if (op == "hook_apply") {
            static const std::set<std::string> allowed = {
                "remember", "forget", "import_soul", "learn_codebase", "long_task_event", "long_task_update",
                "msg_ack", "log_event"
            };
            const auto tool = args.at("tool").get<std::string>();
            if (!allowed.count(tool)) return ToolResult::error("unsupported deferred hook write");
            return ok(invoke(tool, args.at("args")).at("structured"));
        }
        if (op == "hook_saddle") return ok(hook_saddle::detect(args));
        if (op == "hook_pre_tool") return ok(hook_policy::pretool(args, invoke));
        if (op == "hook_post_tool") return ok(hook_policy::bash(args, invoke));
        if (op == "hook_pre_compact") return ok(hook_policy::precompact(args, invoke));
        if (op == "hook_compact_restore") return ok(hook_policy::compact_restore(args, invoke));
        if (op == "hook_handoff_context") {
            auto rows = run("session_list", args);
            return ok({{"text", hook_ledger::handoff_card(rows.at("rows"),
                args.value("project_dir", ""), args.value("branch", ""))}});
        }
        if (op == "hook_task_context") {
            const auto realm = args.value("realm", "");
            auto inbox = run("inbox_list", {{"target_realm", realm}, {"state", "pending"}, {"limit", 5}});
            auto threads = run("thread_list", {{"realm", realm}, {"status", "active"}, {"limit", 3}});
            return ok({{"text", hook_ledger::task_card(inbox.at("rows"), threads.at("rows"), realm)}});
        }
        if (op == "hook_session_context") {
            auto loaded = tool_ledger_load({{"project", args.value("project", "")}});
            if (loaded.is_error) return loaded;
            return ok(hook_ledger::ledger_card(loaded.structured, args.value("source", ""),
                args.value("now", TaskLedger::now())));
        }
        if (op == "hook_handoff_prepare") {
            json session = json::object(), thread = json::object();
            if (args.value("next_action", "").empty()) {
                session = run("session_get", {{"session_id", args.at("session_id")}});
                const auto tid = hook_ledger::str(session, "thread_id");
                if (!tid.empty()) thread = run("thread_get", {{"thread_id", tid}});
            }
            const auto capsule = hook_ledger::capsule(args, session, thread,
                args.value("saved_at", TaskLedger::now()));
            return ok({{"op", "session_bind"}, {"args", {{"session_id", args.at("session_id")},
                {"project_dir", args.at("project_dir")}, {"metadata", {{"handoff", capsule}}}}}});
        }
        if (op == "hook_stop_progress") return ok(hook_ledger::stop_progress(args));
        if (op == "hook_stop_checkpoint") {
            return ok(hook_ledger::stop_checkpoint(args));
        }
        if (op == "hook_turn") {
            // Preserve the existing queue event payload exactly. Durable queue
            // acknowledgement still follows the store sync in QueueProcessor.
            const auto sid = args.value("session_id", "");
            if (sid.empty()) return ToolResult::error("session_id is required");
            if (args.value("content", "").empty())
                return ToolResult::error("turn content is required");
            const auto event = field_store_->emit_event("transcript", "turn", sid, args.dump());
            if (!event) return ToolResult::error("hook ledger event append failed");
            return ok({{"event_id", event}});
        }
        // The ledger holds its own transaction lock through WAL append and publish.
        auto result = task_ledger_.run(op, args, [&](const json& batch) {
            if (!field_store_->emit_event("ledger", "task_records", "task-ledger", batch.dump()))
                throw std::runtime_error("ledger WAL append failed");
        });
        // Envelope also represents null/bool/string results without losing them
        // in ToolResult::ok's optional structured payload handling.
        return ToolResult::ok("Ledger operation complete", {{"value", result}});
    } catch (const std::exception& e) {
        return ToolResult::error(e.what());
    }
}

ToolResult FieldRpcHandler::dispatch_session(const std::string& tool, const json& args) {
    // QueueProcessor has already applied the global-lock policy. The ledger
    // owns its transaction mutex; never recurse through the RPC dispatcher.
    if (tool == "ledger_op") return tool_ledger_op(args);
    if (tool == "session_register") return tool_session_register(args);
    if (tool == "session_heartbeat") return tool_session_heartbeat(args);
    if (tool == "session_deregister") return tool_session_deregister(args);
    return ToolResult::error("unknown session operation");
}
} // namespace chitta
