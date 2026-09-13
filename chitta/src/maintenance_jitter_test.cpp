#include <chitta/maintenance_jitter.hpp>

#include <cassert>
#include <chrono>

int main() {
    using namespace std::chrono_literals;
    chitta::MaintenanceJitter a(1234567);
    chitta::MaintenanceJitter b(1234567);
    chitta::MaintenanceJitter c(7654321);
    bool different_seed_changed_a_sample = false;
    for (uint64_t i = 0; i < 10000; ++i) {
        auto x = a.delay(1000ms, "distill", i);
        auto y = b.delay(1000ms, "distill", i);
        auto z = c.delay(1000ms, "distill", i);
        assert(x >= 800ms);
        assert(x <= 1200ms);
        assert(x == y);
        if (x != z) different_seed_changed_a_sample = true;
    }
    assert(different_seed_changed_a_sample);
    assert(a.delay(0ms, "zero", 0) == 0ms);
    return 0;
}
