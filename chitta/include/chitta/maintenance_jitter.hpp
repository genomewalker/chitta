#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <string_view>
#include <unistd.h>

namespace chitta {

// Stateless deterministic jitter. A label and sequence/anchor select a stable
// point in [80%, 120%], so independent maintenance jobs do not synchronize and
// tests/reproduction can pin the schedule with CHITTA_JITTER_SEED.
class MaintenanceJitter {
public:
    explicit MaintenanceJitter(uint64_t seed = environment_seed()) : seed_(seed) {}

    template <class Rep, class Period>
    std::chrono::milliseconds delay(std::chrono::duration<Rep, Period> base,
                                    std::string_view label,
                                    uint64_t sequence) const {
        const auto base_ms = std::chrono::duration_cast<std::chrono::milliseconds>(base).count();
        if (base_ms <= 0) return std::chrono::milliseconds(0);
        const uint64_t h = mix(seed_ ^ fnv1a(label) ^ mix(sequence));
        const int64_t basis_points = 8000 + static_cast<int64_t>(h % 4001); // inclusive ±20%
        return std::chrono::milliseconds((base_ms * basis_points) / 10000);
    }

    uint64_t seed() const { return seed_; }

    static uint64_t environment_seed() {
        if (const char* value = std::getenv("CHITTA_JITTER_SEED")) {
            if (*value) {
                char* end = nullptr;
                const auto parsed = std::strtoull(value, &end, 10);
                if (end != value) return parsed;
            }
        }
        std::random_device rd;
        const uint64_t entropy = (static_cast<uint64_t>(rd()) << 32) ^ rd();
        const uint64_t clock = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        return mix(entropy ^ clock ^ static_cast<uint64_t>(getpid()));
    }

private:
    static uint64_t fnv1a(std::string_view text) {
        uint64_t h = 1469598103934665603ULL;
        for (unsigned char c : text) {
            h ^= c;
            h *= 1099511628211ULL;
        }
        return h;
    }

    static uint64_t mix(uint64_t x) {
        x += 0x9e3779b97f4a7c15ULL;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        return x ^ (x >> 31);
    }

    uint64_t seed_;
};

}  // namespace chitta
