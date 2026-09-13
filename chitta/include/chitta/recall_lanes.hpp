#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace chitta {

struct RecallLaneOutput {
    std::string name;
    std::string text;
    nlohmann::json standalone_structured;
    long ms = 0;
    bool timed_out = false;
};

// Preserve the standalone handler's text byte-for-byte while presenting the
// common lane envelope consumed by prompt-core.sh.
inline nlohmann::json assemble_recall_lanes(
    const std::vector<RecallLaneOutput>& outputs, long total_ms) {
    nlohmann::json lanes = nlohmann::json::object();
    for (const auto& output : outputs) {
        nlohmann::json results = nlohmann::json::array();
        if (output.standalone_structured.is_object()) {
            auto it = output.standalone_structured.find("results");
            if (it != output.standalone_structured.end()) results = *it;
        }
        lanes[output.name] = {
            {"text", output.text},
            {"results", std::move(results)},
            {"ms", output.ms},
            {"timed_out", output.timed_out},
        };
    }
    return {{"lanes", std::move(lanes)}, {"total_ms", total_ms}};
}

}  // namespace chitta
