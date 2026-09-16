// Fixture transport invokes the daemon's policy, never a shell implementation.
#include <chitta/hook_ledger_policy.hpp>
#include <iostream>

int main() {
    using namespace chitta::hook_ledger;
    json request;
    std::cin >> request;
    const auto op = request.at("op").get<std::string>();
    const auto args = request.value("args", json::object());
    json value;
    if (op == "hook_handoff_prepare") {
        auto cap = capsule(args, request.value("session", json::object()),
            request.value("thread", json::object()), args.value("saved_at", 100.0));
        value = {{"op", "session_bind"}, {"args", {{"session_id", args.at("session_id")},
            {"project_dir", args.at("project_dir")}, {"metadata", {{"handoff", cap}}}}}};
    } else if (op == "hook_handoff_context") {
        value = {{"text", handoff_card(request.value("rows", json::array()),
            args.value("project_dir", ""), args.value("branch", ""))}};
    } else if (op == "hook_session_context") {
        value = ledger_card(request.value("ledger", json::object()), args.value("source", ""),
            args.value("now", 100.0));
    } else if (op == "hook_task_context") {
        value = {{"text", task_card(request.value("inbox", json::array()),
            request.value("threads", json::array()), args.value("realm", ""))}};
    } else return 1;
    std::cout << json({{"value", value}, {"assembly_ms", 0}}).dump() << '\n';
}
