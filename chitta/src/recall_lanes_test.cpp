#include <cassert>
#include <cstdio>

#include <chitta/hit_line.hpp>
#include <chitta/recall_lanes.hpp>

int main() {
    const std::string standalone_text = "Smart recall (semantic, ep=7): 1 results\n"
        + chitta::hit_line("42", 87, "wisdom", 0, "synthetic memory", true, false);
    const nlohmann::json standalone = {
        {"results", nlohmann::json::array({{{"id", "42"}, {"text", "synthetic memory"}}})},
        {"intent", "semantic"},
    };
    const auto response = chitta::assemble_recall_lanes(
        {{"sem", standalone_text, standalone, 17, false},
         {"corrk", "NO CORRECTION", {{"found", false}}, 2, false}},
        19);

    assert(response.contains("lanes") && response["lanes"].is_object());
    assert(response["total_ms"] == 19);
    assert(response["lanes"]["sem"].size() == 4);
    assert(response["lanes"]["sem"]["text"] == standalone_text);
    assert(response["lanes"]["sem"]["results"] == standalone["results"]);
    assert(response["lanes"]["sem"]["ms"] == 17);
    assert(response["lanes"]["sem"]["timed_out"] == false);
    assert(response["lanes"]["corrk"]["results"].is_array());
    assert(response["lanes"]["corrk"]["results"].empty());
    std::printf("recall_lanes_test: all assertions passed\n");
    return 0;
}
