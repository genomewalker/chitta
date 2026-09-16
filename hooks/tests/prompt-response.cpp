// Synthetic transport for hook integration tests, using the production policy.
#include <chitta/hook_prompt_policy.hpp>
#include <chitta/prompt_fusion.hpp>
#include <fstream>
#include <iostream>

int main() {
  using namespace chitta::prompt_policy;
  json state;
  std::cin >> state;
  const auto envelope = state;
  const bool hook = state.contains("input");
  json plan;
  if (hook) {
    plan = chitta::hook_policy::prompt_prepare(state);
    if (plan.value("skip", false)) {
      std::cout << json{{"hook", plan}}.dump() << '\n';
      return 0;
    }
    state = plan.at("policy_state");
  }
  if (const auto *path = std::getenv("STUB_PREPARED_STATE")) {
    std::ofstream stream(path);
    stream << state.dump();
  }
  json lanes = json::object();
  const auto fixtures = envelope.at("fixture_lanes");
  for (const auto &name : state.at("retrieval").at("lanes")) {
    const auto text = fixtures.value(name.get<std::string>(), "");
    lanes[name.get<std::string>()] = {
        {"text", text}, {"ms", 0}, {"timed_out", text.empty()}};
  }
  auto fused = fuse(lanes, state.at("fusion_options"));
  if (fused.at("memories") == "" &&
      state.at("fusion_options").value("cross_realm", true))
    lanes["xr"] = {{"text", ""}, {"ms", 0}, {"timed_out", true}};
  for (const auto &key : {"memories", "c2_pct", "small_realm"})
    state[key] = fused[key];
  state["lane_ms"] = json::object();
  state["lane_timeout"] = json::object();
  for (auto it = lanes.begin(); it != lanes.end(); ++it) {
    state["lane_ms"][it.key()] = it.value()["ms"];
    state["lane_timeout"][it.key()] = it.value()["timed_out"];
  }
  auto reply = admit(state);
  fused["lanes"] = lanes;
  reply["retrieval"] = fused;
  if (hook) {
    reply["lane_ms"] = state["lane_ms"];
    reply["lane_timeout"] = state["lane_timeout"];
    auto invoke = [](const std::string &tool, const json &args) {
      std::string text;
      if (tool == "$hook_classifier") {
        if (const auto *path = std::getenv("STUB_PYTHON_CALLS")) {
          std::ofstream file(path, std::ios::app);
          file << "classifier\n";
        }
        return json{{"text", ""}, {"structured", {{"intent", "correction"}}}};
      }
      if (tool == "recall" && args.value("query", "") == "session_summary")
        if (const auto *path = std::getenv("STUB_SESSION_FILE")) {
          std::ifstream file(path);
          text.assign(std::istreambuf_iterator<char>(file), {});
        }
      return json{{"text", text}, {"structured", json::object()}};
    };
    reply["hook"] =
        chitta::hook_policy::prompt_finish(envelope, plan, reply, invoke);
  }
  std::cout << reply.dump() << '\n';
}
