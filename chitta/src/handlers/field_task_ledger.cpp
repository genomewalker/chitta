#include "../../include/chitta/rpc/field_handler.hpp"
#include <limits>

namespace chitta {
void FieldRpcHandler::load_task_ledger() {
    auto events = json::parse(field_store_->task_ledger_events());
    std::sort(events.begin(), events.end(), [](const json& x, const json& y) {
        return x.at("payload").at("revision") < y.at("payload").at("revision");
    });
    for (const auto& event : events)
        task_ledger_.replay(event.at("payload"));
}

ToolResult FieldRpcHandler::tool_ledger_op(const json& params) {
    try {
        auto op   = params.at("op").get<std::string>();
        auto args = params.value("args", json::object());
        if (args.is_string()) args = json::parse(args.get<std::string>());
        if (!args.is_object()) return ToolResult::error("args must be an object");
        args        = rpc::clamp_read_arguments("ledger_op." + op, std::move(args));
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
    // QueueProcessor already owns rpc_mutex_; never call handle() recursively.
    if (tool == "session_register") return tool_session_register(args);
    if (tool == "session_heartbeat") return tool_session_heartbeat(args);
    if (tool == "session_deregister") return tool_session_deregister(args);
    return ToolResult::error("unknown session operation");
}
} // namespace chitta
