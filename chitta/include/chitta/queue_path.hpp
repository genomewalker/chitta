#pragma once
#include <cstdlib>
#include <filesystem>
#include <string>

namespace chitta {
// Keep persistent queues with the mind: /tmp is node-local and cannot carry
// pending hook writes across nodes. CHITTA_QUEUE_PATH is a legacy explicit alias.
inline std::string queue_path_for_mind(const std::string& mind) {
    for (const char* name : {"CHITTA_QUEUE", "CHITTA_QUEUE_PATH"}) {
        if (const char* value = std::getenv(name); value && *value) return value;
    }
    return (std::filesystem::path(mind) / "queue.jsonl").string();
}
} // namespace chitta
