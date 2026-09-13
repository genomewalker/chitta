// Standalone test for the MDL consolidation gate (mdl_gate.hpp).
// Build: g++ -std=c++20 -lz src/mdl_gate_test.cpp -o t
#include "../include/chitta/mdl_evidence_pool.hpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <string>

using chitta::mdl::judge;
using chitta::mdl::kChunkBytes;

static int failures = 0;

static void expect(bool ok, const std::string& label) {
    std::cout << (ok ? "  PASS " : "  FAIL ") << label << "\n";
    if (!ok) ++failures;
}

int main() {
    // Deflate's ~32KB back-reference window already captures dense self-repetition
    // for free (see mdl_gate.hpp's module comment), so a compressive case needs the
    // fact to recur *sparsely*, once per otherwise-varied evidence chunk, spanning
    // multiple >32KB chunks — that's what actually gives a schema room to pay for
    // itself. build_sparse_evidence() below produces exactly that shape,
    // deterministically (no PRNG).
    static const std::string kFact =
        "the retry backoff for RPC calls is 3 attempts with 250ms base delay, doubling each time";
    static const char* kWords[] = {
        "turn", "session", "handled", "queued", "processed", "retried",
        "flushed", "acked", "committed", "dropped", "resumed", "paused"
    };
    auto noise_line = [](int i) {
        std::string s;
        for (int j = 0; j < 12; ++j) {
            s += kWords[(i * 31 + j * 7) % 12];
            s += ' ';
            s += std::to_string((i * 17 + j * 13) % 997);
            s += ' ';
        }
        return s;
    };
    // 8 sections of 300 noise lines each, with kFact inserted once per section.
    auto build_sparse_evidence = [&]() {
        std::string evidence;
        for (int c = 0; c < 8; ++c) {
            for (int i = 0; i < 300; ++i) {
                if (i == 150) { evidence += kFact; evidence += '\n'; }
                evidence += noise_line(c * 500 + i);
                evidence += '\n';
            }
        }
        return evidence;
    };

    std::cout << "== small evidence pool (three conversation chunks) ==\n";
    {
        chitta::mdl::EvidencePool pool;
        chitta::mdl::EvidencePool::Key key{"mind", "transcript", "session", "realm"};
        std::vector<std::string> evidence;
        for (int i = 0; i < 3; ++i) {
            evidence = pool.extend(key, "[user]\n" + noise_line(i) + "\n" + kFact,
                                   i * 10, (i + 1) * 10, 3);
        }
        auto pooled = chitta::mdl::judge_chunks(kFact, evidence);
        auto single = judge(kFact, evidence.front());
        auto unrelated = chitta::mdl::judge_chunks(
            "the user prefers dark roast coffee and bikes to work on Tuesdays", evidence);
        expect(evidence.size() == 3, "three actual small chunks, no padding");
        expect(!single.accept && pooled.accept, "recurring fact: single saving=" +
            std::to_string(single.saving) + " pooled saving=" + std::to_string(pooled.saving));
        expect(!unrelated.accept, "unrelated rejected: saving=" + std::to_string(unrelated.saving));
        expect(pooled.margin == 64, "margin unchanged");
        auto other = key; other[3] = "other-realm";
        expect(pool.extend(other, kFact, 30, 40, 3).size() == 1, "realm isolation");
        other = key; other[2] = "other-session";
        expect(pool.extend(other, kFact, 30, 40, 3).size() == 1, "session isolation");
        other = key; other[0] = "other-mind";
        expect(pool.extend(other, kFact, 30, 40, 3).size() == 1, "mind isolation");
        other = key; other[1] = "other-transcript";
        expect(pool.extend(other, kFact, 30, 40, 3).size() == 1, "transcript isolation");
        expect(pool.extend(key, kFact, 20, 30, 3).size() == 3, "retry not counted twice");
        expect(pool.extend(key, kFact, 30, 40, 1).size() == 2, "N selects recent history");
        expect(pool.extend(key, std::string(kChunkBytes, 'x'), 40, 50, 3).size() == 1,
               "large evidence is unchanged");
        expect(pool.extend(key, kFact, 50, 60, 0).size() == 1, "N=0 disables pooling");
        expect(pool.extend(key, kFact, 0, 10, 3).size() == 1, "rewind clears history");
        setenv("CHITTA_MDL_POOL_CHUNKS", "0", 1);
        expect(chitta::mdl::pool_chunk_limit() == 0, "environment disables pooling");
        setenv("CHITTA_MDL_POOL_CHUNKS", "-1", 1);
        expect(chitta::mdl::pool_chunk_limit() == 3, "invalid environment uses default");
        setenv("CHITTA_MDL_POOL_CHUNKS", "999", 1);
        expect(chitta::mdl::pool_chunk_limit() == 32, "environment is bounded");
        unsetenv("CHITTA_MDL_POOL_CHUNKS");
    }

    std::cout << "== (a) compressive rule, sparsely recurring across evidence => accept ==\n";
    {
        std::string evidence = build_sparse_evidence();
        auto v = judge(kFact, evidence);
        expect(v.accept, "accept (saving=" + std::to_string(v.saving) +
                          " margin=" + std::to_string(v.margin) + ")");
        expect(v.saving == v.c_e - v.c_we, "saving == c_e - c_we");
    }

    std::cout << "\n== (b) unrelated wisdom over the same evidence => reject ==\n";
    {
        std::string evidence = build_sparse_evidence();
        std::string wisdom = "the user prefers dark roast coffee and bikes to work on Tuesdays";

        auto v = judge(wisdom, evidence);
        expect(!v.accept, "reject (saving=" + std::to_string(v.saving) + ")");
    }

    std::cout << "\n== (c) empty inputs don't crash ==\n";
    {
        auto v1 = judge("", "");
        expect(!v1.accept, "empty/empty => not accepted, no crash");

        auto v2 = judge("some wisdom", "");
        expect(!v2.accept, "wisdom/empty-evidence => no crash");

        auto v3 = judge("", "some evidence with no wisdom to compare against");
        expect(!v3.accept, "empty-wisdom/evidence => no crash (no saving possible)");
    }

    std::cout << "\n== (d) evidence >32KB exercises the chunk path ==\n";
    {
        std::string wisdom = "config values live under <mind_path>/config.json, key 'realm'";
        std::string unit = "config values live under <mind_path>/config.json, key 'realm'\n";
        std::string evidence;
        while (evidence.size() < kChunkBytes * 3 + 500) evidence += unit;

        auto chunks = chitta::mdl::chunks(evidence);
        expect(chunks.size() >= 4, "evidence spans >= 4 chunks (" +
                                    std::to_string(chunks.size()) + ")");

        // Manual per-chunk sum, independent of judge()'s internals, to confirm
        // saving is the sum over chunks minus one C(w) charge.
        long c_w_manual = chitta::mdl::compress_len(wisdom, nullptr);
        long c_e_manual = 0, c_e_given_w_manual = 0;
        for (const auto& c : chunks) {
            c_e_manual += chitta::mdl::compress_len(c, nullptr);
            c_e_given_w_manual += chitta::mdl::compress_len(c, &wisdom);
        }
        long saving_manual = c_e_manual - (c_w_manual + c_e_given_w_manual);

        auto v = judge(wisdom, evidence);
        expect(v.c_e == c_e_manual, "c_e matches manual chunk sum");
        expect(v.saving == saving_manual, "saving matches manual chunk computation");
    }

    std::cout << "\n== latency: judge() over a ~100KB conversation ==\n";
    {
        std::string wisdom = "the retry backoff is 3 attempts with 250ms base delay, doubling";
        std::string evidence;
        while (evidence.size() < 100 * 1024)
            evidence += "turn: retried the RPC call after a timeout, backoff 250ms then 500ms\n";

        auto start = std::chrono::steady_clock::now();
        auto v = judge(wisdom, evidence);
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();
        std::cout << "  judge() on " << evidence.size() << " bytes: " << elapsed
                  << " us (accept=" << v.accept << ")\n";
    }

    std::cout << "\n" << (failures == 0 ? "ALL PASS" : std::to_string(failures) + " FAILURES") << "\n";
    return failures == 0 ? 0 : 1;
}
