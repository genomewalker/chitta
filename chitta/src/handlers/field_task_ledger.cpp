#include "../../include/chitta/rpc/field_handler.hpp"
#include <limits>
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
    try {
        const auto started = std::chrono::steady_clock::now();
        auto op   = params.at("op").get<std::string>();
        auto args = params.value("args", json::object());
        if (args.is_string()) args = json::parse(args.get<std::string>());
        if (!args.is_object()) return ToolResult::error("args must be an object");
        args        = rpc::clamp_read_arguments("ledger_op." + op, std::move(args));
        auto run = [&](const std::string& operation, const json& input) {
            std::lock_guard<std::recursive_mutex> lock(hook_ledger::capsule_mutex);
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
        // All handoff writers enter through this gateway. Hold the capsule lock
        // across lookup, revision validation and the ledger's durable transaction.
        const bool capsule_write = op == "capsule_save" || op == "hook_handoff_prepare" ||
            (op == "session_bind" && args.value("metadata", json::object()).contains("handoff"));
        std::unique_lock<std::recursive_mutex> capsule_lock(hook_ledger::capsule_mutex, std::defer_lock);
        if (capsule_write) capsule_lock.lock();
        auto latest_capsule = [&](const json& key) {
            json latest = json::object(), page_args = {{"limit", 100}};
            do {
                const auto page = run("session_list", page_args);
                for (const auto& row : page.at("rows")) {
                    const auto cap = hook_ledger::metadata(row).value("handoff", json::object());
                    if (cap.value("version", 0) == 2 && hook_ledger::capsule_key(cap) == hook_ledger::capsule_key(key)
                        && cap.value("revision", uint64_t(0)) > latest.value("revision", uint64_t(0))) latest = cap;
                }
                page_args["after"] = page.at("after");
            } while (!page_args["after"].is_null());
            return latest;
        };
        if (op == "capsule_save") {
            auto input = args.at("capsule");
            const auto facts = hook_ledger::git_identity(input.value("project_dir", ""));
            for (auto field : {"repository", "code_head", "stream_id"})
                if (!input.contains(field)) input[field] = facts.at(field);
            const auto bound = run("session_get", {{"session_id", input.value("session_id", "")}});
            const auto previous = hook_ledger::metadata(bound).value("handoff", json::object());
            if (previous.value("version", 0) == 2 &&
                hook_ledger::capsule_key(previous) != hook_ledger::capsule_key(input))
                return ToolResult::error("capsule session key is immutable");
            const auto old = latest_capsule(input);
            const auto revision = old.value("revision", uint64_t(0));
            if (!args.contains("expected_revision") || args.at("expected_revision") != revision)
                return ToolResult::error("capsule revision mismatch");
            const auto cap = hook_ledger::capsule_v2(input, revision + 1, TaskLedger::now());
            const auto sid = cap.at("session_id").get<std::string>();
            if (sid.empty()) return ToolResult::error("capsule session_id required");
            run("session_bind", {{"session_id", sid}, {"project_dir", cap.at("project_dir")},
                {"metadata", {{"handoff", cap}}}});
            return ok(cap);
        }
        if (op == "session_bind" && args.value("metadata", json::object()).contains("handoff")) {
            const auto cap = args.at("metadata").at("handoff");
            if (cap.value("version", 0) == 2) {
                if (cap.value("session_id", "") != args.value("session_id", ""))
                    return ToolResult::error("capsule session_id mismatch");
                const auto bound = run("session_get", {{"session_id", args.at("session_id")}});
                const auto previous = hook_ledger::metadata(bound).value("handoff", json::object());
                if (previous.value("version", 0) == 2 &&
                    hook_ledger::capsule_key(previous) != hook_ledger::capsule_key(cap))
                    return ToolResult::error("capsule session key is immutable");
                const auto old = latest_capsule(cap);
                const auto revision = old.value("revision", uint64_t(0));
                if (!args.contains("expected_revision") || args.at("expected_revision") != revision ||
                    cap.at("revision") != revision + 1) return ToolResult::error("capsule revision mismatch");
                args["metadata"]["handoff"] = hook_ledger::capsule_v2(cap, revision + 1, cap.at("saved_at"));
            } else {
                const auto old = hook_ledger::metadata(run("session_get", {{"session_id", args.at("session_id")}}));
                if (old.value("handoff", json::object()).value("version", 0) == 2)
                    return ToolResult::error("cannot overwrite v2 capsule with unversioned writer");
            }
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
            auto key = hook_ledger::git_identity(args.value("project_dir", ""));
            key["session_id"] = args.at("session_id");
            return ok(hook_ledger::prepare_capsule(args, session, thread,
                latest_capsule(key), TaskLedger::now()));
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
        auto result = run(op, args);
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
