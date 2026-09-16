// The generated include is the exact production function, extracted at configure
// time so this regression cannot drift into testing a second implementation.
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <pthread.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <memory>
#ifdef CHITTA_WITH_LLAMA_CPP
#include "chitta/vak_llama.hpp"
#endif
// Standard BLAS ABI; no optional vendor CBLAS headers required.
extern "C" void sgemm_(const char*, const char*, const int*, const int*, const int*,
                       const float*, const float*, const int*, const float*,
                       const int*, const float*, float*, const int*);
extern char** environ;
#include "format_id_probe.inc"

static void slow_prefork() { usleep(1500000); }
int main(int argc, char** argv) {
    assert(argc == 2);
    // Deterministically exposes a regression to fork/popen even on a machine
    // whose BLAS atfork implementation does not exhibit the production deadlock.
    assert(pthread_atfork(slow_prefork, nullptr, nullptr) == 0);
#ifdef CHITTA_WITH_LLAMA_CPP
    std::unique_ptr<chitta::LlamaYantra> yantra;
    if (const char* model = std::getenv("CHITTA_EMBED_MODEL")) {
        yantra = std::make_unique<chitta::LlamaYantra>(model);
        assert(yantra->ready());
    }
#endif
    std::atomic<bool> stop{false};
    std::atomic<unsigned> working{0};
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i) workers.emplace_back([&] {
        constexpr int n = 192;
        std::vector<float> a(n*n, .1f), b(n*n, .2f), c(n*n);
        const char trans = 'N';
        const float one = 1, zero = 0;
        bool first = true;
        while (!stop.load()) {
#ifdef CHITTA_WITH_LLAMA_CPP
            if (yantra) {
                auto result = yantra->transform("chitta durable prefix recovery under embedding load", chitta::EmbedMode::Document);
                assert(result.certainty == 1.0f);
            } else
#endif
                sgemm_(&trans, &trans, &n, &n, &n, &one, a.data(), &n,
                       b.data(), &n, &zero, c.data(), &n);
            if (first) { working.fetch_add(1); first = false; }
        }
    });
    while (working.load() != 4) std::this_thread::yield();
    for (int i = 0; i < 3; ++i) {
        auto begin = std::chrono::steady_clock::now();
        const auto id = spawn_format_id_probe(argv[1]);
        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
        assert(id != 0);
        std::cout << "format-id under embedding matrix load: " << seconds << " s\n";
        assert(seconds < 1.0);
    }
    stop = true;
    for (auto& worker : workers) worker.join();
}
