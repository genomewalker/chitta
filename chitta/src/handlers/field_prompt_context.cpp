#include "chitta/rpc/field_handler.hpp"
#include "chitta/prompt_policy.hpp"

namespace chitta {
ToolResult FieldRpcHandler::tool_prompt_context(const json& params) {
    try {
        const auto& state = params.at("state");
        if (!state.is_object()) return ToolResult::error("state must be an object");
        auto result = prompt_policy::admit(state);
        return ToolResult::ok(result.at("fused_block").get<std::string>(), result);
    } catch (const std::exception& error) {
        return ToolResult::error(error.what());
    }
}
} // namespace chitta
