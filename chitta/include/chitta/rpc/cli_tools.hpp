#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace chitta {

// Resolve tools/list schemas, refreshing unknown names. Previously fetched
// schemas remain available when the daemon is offline (notably for --help).
std::optional<nlohmann::json> discover_cli_tool(const std::string& socket_path,
                                               const std::string& name,
                                               nlohmann::json* loading = nullptr);

} // namespace chitta
