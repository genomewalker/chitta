#include <chitta/rpc/work_policy.hpp>

#include <cassert>
#include <chrono>
#include <sstream>
#include <thread>

int main() {
    using nlohmann::json;

    std::ostringstream log;
    json args = {{"unrelated", 999999}};
    for (const auto& limit : chitta::rpc::kReadParameterLimits) {
        args[std::string(limit.name)] = limit.maximum + 1000;
    }
    auto clamped = chitta::rpc::clamp_read_arguments("synthetic_read", args, log);
    for (const auto& limit : chitta::rpc::kReadParameterLimits) {
        assert(clamped[std::string(limit.name)] == limit.maximum);
    }
    assert(clamped["unrelated"] == 999999);
    assert(log.str().find("tool=synthetic_read param=limit") != std::string::npos);

    json below_minimum;
    for (const auto& limit : chitta::rpc::kReadParameterLimits) {
        below_minimum[std::string(limit.name)] = limit.minimum - 1000;
    }
    clamped = chitta::rpc::clamp_read_arguments("synthetic_read", below_minimum, log);
    for (const auto& limit : chitta::rpc::kReadParameterLimits) {
        assert(clamped[std::string(limit.name)] == limit.minimum);
    }

    chitta::rpc::BudgetTracker tracker(1);
    {
        auto scope = tracker.measure("synthetic_handler");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(tracker.over_budget_count() == 1);
    return 0;
}
