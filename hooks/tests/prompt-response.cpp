// Synthetic transport for hook integration tests, using the production policy.
#include <chitta/prompt_fusion.hpp>
#include <iostream>

int main() {
    using namespace chitta::prompt_policy;
    json state;
    std::cin >> state;
    json lanes = json::object();
    const auto fixtures = state.at("fixture_lanes");
    for (const auto& name : state.at("retrieval").at("lanes")) {
        const auto text = fixtures.value(name.get<std::string>(), "");
        lanes[name.get<std::string>()] = {{"text", text}, {"ms", 0},
            {"timed_out", text.empty()}};
    }
    auto fused = fuse(lanes, state.at("fusion_options"));
    if (fused.at("memories") == "" && state.at("fusion_options").value("cross_realm", true))
        lanes["xr"] = {{"text", ""}, {"ms", 0}, {"timed_out", true}};
    for (const auto& key : {"memories", "c2_pct", "small_realm"}) state[key] = fused[key];
    state["lane_ms"] = json::object();
    state["lane_timeout"] = json::object();
    for (auto it = lanes.begin(); it != lanes.end(); ++it) {
        state["lane_ms"][it.key()] = it.value()["ms"];
        state["lane_timeout"][it.key()] = it.value()["timed_out"];
    }
    auto reply = admit(state);
    fused["lanes"] = lanes;
    reply["retrieval"] = fused;
    std::cout << reply.dump() << '\n';
}
