#include "../../include/chitta/rpc/field_handler.hpp"
#include <limits>
#include <chitta/hook_ledger_policy.hpp>

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
