// Native-policy transport for hook envelope integration fixtures.
#include <chitta/hook_ancillary_policy.hpp>
#include <chitta/hook_pretool_policy.hpp>
#include <chitta/hook_prompt_policy.hpp>
#include <chitta/hook_saddle_policy.hpp>
#include <chitta/hook_session_policy.hpp>
#include <chitta/prompt_fusion.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
int main(int argc, char **argv) {
  using namespace chitta::hook_policy;
  if (argc > 1 && std::string(argv[1]) == "realm_detect") {
    std::cout << "project:test\n";
    return 0;
  }
  if (argc > 1) {
    if (std::string(argv[1]) == "queue_write")
      return 1;
    std::cout << "{}\n";
    return 0;
  }
  json request;
  std::cin >> request;
  const auto params = request.at("params").at("arguments");
  json fixtures = json::object();
  if (const auto *path = std::getenv("HOOK_POLICY_FIXTURE")) {
    std::ifstream file(path);
    file >> fixtures;
  }
  auto invoke = [&](const std::string &tool, const json &args) {
    if (const auto *delay = std::getenv("HOOK_FIXTURE_DELAY_MS"))
      std::this_thread::sleep_for(std::chrono::milliseconds(std::stoi(delay)));
    auto key = tool;
    if (tool == "ledger_op")
      key += "." + str(args, "op");
    if (tool == "recall")
      key += "." + (args.contains("realm") ? "scoped" : str(args, "query"));
    if (tool == "triplet_history")
      key += "." + str(args, "subject");
    if (tool == "sql_query") {
      auto query = str(args, "query");
      key += query.find("FROM theme") != std::string::npos          ? ".themes"
             : query.find("GROUP BY kind") != std::string::npos     ? ".kinds"
             : query.find("COUNT(*) as total") != std::string::npos ? ".counts"
                                                                    : ".recent";
    }
    if (fixtures.contains(key))
      return fixtures[key];
    if (fixtures.contains(tool))
      return fixtures[tool];
    if (tool == "ledger_op") {
      const auto op = str(args, "op");
      auto value = json::object();
      if (op == "hook_session_context")
        value = chitta::hook_ledger::ledger_card(
            fixtures.value("_ledger", json::object()),
            str(args.at("args"), "source"), args.at("args").value("now", 0.));
      if (op == "hook_task_context")
        value = {{"text", chitta::hook_ledger::task_card(
                              fixtures.value("_inbox", json::array()),
                              fixtures.value("_threads", json::array()),
                              str(args.at("args"), "realm"))}};
      if (op == "hook_handoff_context")
        value = {{"text", chitta::hook_ledger::handoff_card(
                              fixtures.value("_sessions", json::array()),
                              str(args.at("args"), "project_dir"),
                              str(args.at("args"), "branch"))}};
      return json{{"text", ""},
                  {"structured", {{"value", value}, {"assembly_ms", 0}}}};
    }
    return json{{"text", ""}, {"structured", json::object()}};
  };
  if (request.at("params").at("name") == "prompt_context") {
    const auto envelope = params.at("state");
    auto plan = prompt_prepare(envelope);
    if (plan.value("skip", false)) {
      std::cout << json{{"hook", plan}}.dump() << '\n';
      return 0;
    }
    auto state = plan.at("policy_state");
    if (const auto *path = std::getenv("STUB_PREPARED_STATE")) {
      std::ofstream file(path);
      file << state.dump();
    }
    auto fused =
        chitta::prompt_policy::fuse(json::object(), state.at("fusion_options"));
    for (const auto *key : {"memories", "c2_pct", "small_realm"})
      state[key] = fused[key];
    auto reply = chitta::prompt_policy::admit(state);
    reply["retrieval"] = fused;
    reply["lane_ms"] = json::object();
    reply["lane_timeout"] = json::object();
    reply["hook"] = prompt_finish(envelope, plan, reply, invoke);
    std::cout << reply.dump() << '\n';
    return 0;
  }
  const auto op = params.at("op").get<std::string>();
  json result;
  if (op == "hook_session_start")
    result = session_start(params.at("args"), invoke);
  else if (op == "hook_ancillary")
    result = ancillary(params.at("args"), invoke);
  else if (op == "hook_pre_tool")
    result = pretool(params.at("args"), invoke);
  else if (op == "hook_post_tool")
    result = bash(params.at("args"), invoke);
  else if (op == "hook_pre_compact")
    result = precompact(params.at("args"), invoke);
  else if (op == "hook_compact_restore")
    result = compact_restore(params.at("args"), invoke);
  else if (op == "hook_saddle")
    result = chitta::hook_saddle::detect(params.at("args"));
  else
    return 1;
  std::cout << json({{"result", {{"value", result}}}}).dump() << '\n';
}
