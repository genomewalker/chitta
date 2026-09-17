#include <chitta/hook_ledger_policy.hpp>
#include <chitta/hook_session_policy.hpp>
#include <cassert>
#include <iostream>
int main() {
    using namespace chitta::hook_ledger;
    json input = {{"repository", "/repo/.git"}, {"stream_id", "a"}, {"session_id", "old"},
                  {"next_action", "run gates"}, {"code_head", std::string(40, 'a')}};
    const auto old = capsule_v2(input, 1, 200);
    input["session_id"] = "new";
    auto latest = capsule_v2(input, 2, 100); // Revision wins over clock skew.
    json rows = json::array();
    auto add = [&](const json& cap) { rows.push_back({{"metadata_json", json({{"handoff", cap}}).dump()}}); };
    add(old); add(latest);
    assert(latest_by_key(rows, input) == latest);
    auto other = input; other["stream_id"] = "other";
    assert(latest_by_key(rows, other).empty());
    latest["state"] = "invalidated"; latest["revision"] = 3; add(latest);
    assert(latest_by_key(rows, input).at("state") == "invalidated");
    for (int i = 0; i < 200; ++i) {
        auto cap = old; cap["stream_id"] = "stream-" + std::to_string(i); add(cap);
    }
    auto manifest = capsule_manifest(rows, "/repo/.git");
    assert(manifest.dump().size() <= 450);
    assert(manifest.at("omitted").get<size_t>() + manifest.at("streams").size() == 201);
    auto large = old; large["next_action"] = std::string(400, 'x');
    add(large); assert(capsule_manifest(rows, "/repo/.git").dump().size() <= 450);
    chitta::hook_policy::SessionContext context;
    context += capsule_card(latest) + "\n";
    context += std::string(1300, 'x'); // Whole oversized section omitted.
    context += "[corrections] preserved\n";
    assert(context.finish().size() <= 1500);
    assert(context.finish().find("invalidated") != std::string::npos);
    assert(context.finish().find("preserved") != std::string::npos);
    assert(context.omitted == 1);
    for (int i = 0; i < 10000; ++i) context += "é😀\n";
    assert(context.finish().size() <= 1500);
    assert(context.finish().find("retrieve") != std::string::npos);
    std::cout << "exact key/revision/invalidation + 201-stream manifest <=450 + SessionStart <=1500: PASS\n";
}
