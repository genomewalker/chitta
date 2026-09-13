#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace chitta::rpc {

struct ParameterLimit {
    std::string_view name;
    int64_t minimum;
    int64_t maximum;
};

// One source of truth for the numeric request fields which multiply read-side
// work. register_tools() copies these bounds into tools/list, and handle()
// applies the same table before dispatch. The graph aliases are retained for
// the current public spellings (max_hops/max_results/iterations).
inline constexpr std::array<ParameterLimit, 11> kReadParameterLimits{{
    {"limit",       1,  100},
    {"pool",        1,  160},
    {"max_nodes",   1,  512},
    {"hops",        0,    4},
    {"depth",       0,    4},
    {"k",           1,  100},
    {"top_k",       1,  100},
    {"window",      0, 1000},
    {"max_hops",    0,    4},
    {"max_results", 1,  100},
    {"iterations",  1,  100},
}};

inline const ParameterLimit* parameter_limit(std::string_view name) {
    for (const auto& limit : kReadParameterLimits) {
        if (limit.name == name) return &limit;
    }
    return nullptr;
}

inline nlohmann::json clamp_read_arguments(const std::string& tool,
                                           nlohmann::json args,
                                           std::ostream& log = std::cerr) {
    if (!args.is_object()) return args;
    for (const auto& limit : kReadParameterLimits) {
        auto it = args.find(std::string(limit.name));
        if (it == args.end() || (!it->is_number_integer() && !it->is_number_unsigned())) continue;

        int64_t before;
        if (it->is_number_unsigned()) {
            const uint64_t raw = it->get<uint64_t>();
            before = raw > static_cast<uint64_t>(INT64_MAX) ? INT64_MAX
                                                            : static_cast<int64_t>(raw);
        } else {
            before = it->get<int64_t>();
        }
        const int64_t after = std::clamp(before, limit.minimum, limit.maximum);
        if (after != before) {
            log << "[rpc] clamp tool=" << tool << " param=" << limit.name
                << " from=" << before << " to=" << after << "\n";
            *it = after;
        }
    }
    return args;
}

class BudgetTracker {
public:
    explicit BudgetTracker(long budget_ms = configured_budget_ms())
        : budget_ms_(budget_ms > 0 ? budget_ms : 5000) {}

    class Scope {
    public:
        Scope(BudgetTracker& tracker, std::string tool, long budget_ms)
            : tracker_(tracker), tool_(std::move(tool)), budget_ms_(budget_ms),
              started_(Clock::now()) {}
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        ~Scope() { tracker_.record(tool_, started_, budget_ms_); }

    private:
        using Clock = std::chrono::steady_clock;
        BudgetTracker& tracker_;
        std::string tool_;
        long budget_ms_;
        Clock::time_point started_;
    };

    Scope measure(std::string tool) { return Scope(*this, std::move(tool), budget_ms_); }
    Scope measure(std::string tool, long budget_ms) {
        return Scope(*this, std::move(tool), budget_ms > 0 ? budget_ms : budget_ms_);
    }
    uint64_t over_budget_count() const {
        return over_budget_count_.load(std::memory_order_relaxed);
    }
    long budget_ms() const { return budget_ms_; }

    static long configured_budget_ms() {
        const char* value = std::getenv("CHITTA_RPC_BUDGET_MS");
        if (!value || !*value) return 5000;
        char* end = nullptr;
        const long parsed = std::strtol(value, &end, 10);
        return end != value && parsed > 0 ? parsed : 5000;
    }

private:
    using Clock = std::chrono::steady_clock;

    void record(const std::string& tool, Clock::time_point started, long budget_ms) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - started).count();
        if (elapsed > budget_ms) {
            over_budget_count_.fetch_add(1, std::memory_order_relaxed);
            std::cerr << "[rpc] over-budget tool=" << tool << " ms=" << elapsed
                      << " budget_ms=" << budget_ms << "\n";
        }
    }

    long budget_ms_;
    std::atomic<uint64_t> over_budget_count_{0};
};

}  // namespace chitta::rpc
