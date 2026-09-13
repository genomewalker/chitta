#pragma once
#include "mdl_gate.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>

namespace chitta::mdl {
inline size_t pool_chunk_limit() {
    const char* value = std::getenv("CHITTA_MDL_POOL_CHUNKS");
    if (!value || !*value) return 3;
    // Bound retained evidence even for an accidental huge environment value.
    std::string text(value);
    if (text.find_first_not_of("0123456789") != std::string::npos) return 3;
    try { return std::min<size_t>(std::stoul(text), 32); }
    catch (...) { return 3; }
}

// Shared across ephemeral NativeDistiller instances. Keys include the mind,
// transcript, session and realm; no transcript content crosses those boundaries.
// At most 64 keys, N <= 32 chunks/key and 32KB/chunk are retained in memory.
// Missing history (including after restart) leaves the original judge unchanged.
class EvidencePool {
public:
    using Key = std::array<std::string, 4>;
    std::vector<std::string> extend(const Key& key, const std::string& current,
                                    int64_t skip_lines, int64_t last_line,
                                    size_t prior_limit) {
        std::vector<std::string> evidence{current};
        std::lock_guard<std::mutex> lock(mutex_);
        prior_limit = std::min<size_t>(prior_limit, 32);
        if (!prior_limit || key[2].empty() || current.empty() || last_line <= skip_lines) {
            history_.erase(key);
            return evidence;
        }
        if (!history_.count(key) && history_.size() >= 64) {
            auto oldest = std::min_element(history_.begin(), history_.end(),
                [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
            history_.erase(oldest);
        }
        auto& history = history_[key];
        history.used = ++clock_;
        // Retries, rewinds and overlapping passes must not manufacture recurrence.
        while (!history.chunks.empty() && history.chunks.back().end > skip_lines)
            history.chunks.pop_back();
        while (history.chunks.size() > prior_limit) history.chunks.pop_front();
        if (current.size() < kChunkBytes) {
            for (const auto& chunk : history.chunks) evidence.push_back(chunk.text);
        }
        history.chunks.push_back({last_line,
            current.substr(current.size() > kChunkBytes ? current.size() - kChunkBytes : 0)});
        while (history.chunks.size() > prior_limit) history.chunks.pop_front();
        return evidence;
    }
private:
    struct Chunk { int64_t end; std::string text; };
    struct History { size_t used = 0; std::deque<Chunk> chunks; };
    std::map<Key, History> history_;
    size_t clock_ = 0;
    std::mutex mutex_;
};
} // namespace chitta::mdl
