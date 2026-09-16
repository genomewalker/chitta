# chitta-field performance

Status as of 2026-09-16. The dated results below preserve the `fix/field-perf` measurements on private eval copies, including unmet targets and the ancillary MCP SDK failure. Two original JSON artifacts are absent; their committed tables are linked instead. Current live startup is about 9.5 s with sidecar hits, about 20 s on the first start after deployment or a format change; see [startup and recovery](CLI.md#startup-sidecars-and-instance-lock). These current operational figures do not replace the historical control/experiment measurements below.

## Measurement boundary

The supplied live baseline (134,129 memories, 7.95 GB RSS, 62 s first recall, hybrid p95 not supplied) is context, not the control arm. The scratch family is `bbcaed33`, generation 38047, snapshot sequence 206208172, with its selected WAL. It reports 133,724 records (133,712 live in detailed health), approximately 1.37M triplets, and 768-dimensional `nomic-embed-text-v1.5` embeddings. Both primary arms load the same validated family. Autonomous work is quiesced; this does not reproduce live writer contention.

`benchmarks/field-perf/run.sh` copies the manifests, selected snapshot family, WAL, migration markers and lite encoder according to `scripts/eval-replica.sh`, validating selection before and after copying. Each child owns a private store, runtime directory, socket and RPC port with `CHITTA_NO_QUEUE=1`. The runner only terminates its own child. It leaves the copy available for audit and never opens a daemon on the source.

Measurements use socket JSON-RPC, excluding CLI process-launch overhead. Readiness is the first non-warming health response; the daemon also logs process-start-to-dispatch time. First recall is issued immediately, before a common 30-second settling interval. Hybrid runs before other timed workloads so they cannot consume its refresh backlog. Each workload has 20 repetitions; p95 is nearest rank (19th sample). Query: `chitta recall performance lock contention`; realm: `project:cc-soul`; limit: 5; `no_learn=true`. Lanes: sem, ctx, hyb, kw, corr. Both primary arms enable profiling. Results retain IDs and schema shapes, never memory text.

The control is `e109286` chitta-field / `f8686fed` superproject plus load/recall timers and allocation diagnostics, measured before optimizations. Wall-clock performance varies with this shared node's load. A single 20-sample workload is evidence for this replica/query, not a general latency guarantee.

## Before and after

Primary control: [BEFORE JSON](../benchmarks/field-perf/results-before.json). Primary final implementation: [AFTER table, label `primed`](../benchmarks/field-perf/table-primed.md). The original primed JSON is not present in this checkout; the committed table preserves its reported measurements. Earlier `after` / `final` artifacts are intermediate steps, identified below.

| Metric | before |
|---|---:|
| Ready (ms) | 35928.8 |
| First recall (ms) | 6440.6 |
| RSS (MiB) | 4322.4 |
| hybrid p50 / p95 (ms) | 33.7 / 245.1 |
| keyword p50 / p95 (ms) | 6.3 / 8.5 |
| fused p50 / p95 (ms) | 33.6 / 42.4 |
| smart p50 / p95 (ms) | 11.9 / 17.8 |
| recall_lanes p50 / p95 (ms) | 33.6 / 35.6 |
| Load snapshot (ms) | 3657 |
| Load pld (ms) | 120 |
| Load emb (ms) | 222 |
| Load lite_encoder (ms) | 15 |
| Load hdc [10](#ref-10) (ms) | 247 |
| Load triplets (ms) | 3665 |
| Load turbo (ms) | 5926 |

| Metric | primed |
|---|---:|
| Ready (ms) | 28712.3 |
| First recall (ms) | 92.4 |
| RSS (MiB) | 3220.2 |
| hybrid p50 / p95 (ms) | 34.7 / 64.1 |
| keyword p50 / p95 (ms) | 6.3 / 6.8 |
| fused p50 / p95 (ms) | 34.1 / 35.8 |
| smart p50 / p95 (ms) | 12.2 / 18.0 |
| recall_lanes p50 / p95 (ms) | 35.0 / 41.6 |
| Load snapshot (ms) | 3982 |
| Load pld (ms) | 131 |
| Load emb (ms) | 251 |
| Load lite_encoder (ms) | 22 |
| Load hdc (ms) | 276 |
| Load turbo (ms) | 6456 |
| Load triplets (ms) | 3311 |

The process-ready log reports 35,912 → 28,696 ms; the tables use the external readiness probe. One/five/fifteen-minute load averages were [67.63, 69.67, 70.09] BEFORE and [56.68, 55.73, 57.18] AFTER.

| Requested target | Final observation | Verdict |
|---|---|---|
| First recall < 2× warm p50 | 92.4 / 34.1 = 2.71× | Not met |
| Hybrid p95 < 400 ms | 64.1 ms | Met |
| Hybrid p50 unchanged or better | 33.7 → 34.7 ms | Not demonstrated; final median is slightly higher |
| RSS reduced by ≥25% | 4322.4 → 3220.2 MiB (25.50% reduction) | Met |
| Same hybrid IDs/order | 20/20 identical | Met for this query |

First recall improved 98.57%, but remaining cold-query/competitive-refresh work still exceeds the requested ratio against repeated warm queries. Host variation does not establish median parity.

Clean checkpoint restart with the final binary ([committed table](../benchmarks/field-perf/table-clean-final.md); the original JSON is not present in this checkout):

| Load measurement | BEFORE legacy family | AFTER same legacy family | AFTER clean checkpoint |
|---|---:|---:|---:|
| Triplet migration (ms) | 3665 | 3311 | 0 |
| Ready (ms) | 35928.8 | 28712.3 | 28533.2 |


## Design and evidence by item

1. **Startup phases and readiness.** `chitta-field/src/field.rs:654` times snapshot decoding separately from `.pld` and `.emb`; `field.rs:1270` times lite/HDC loads; `field.rs:1473` times triplet migration; `src/hnsw.rs:3112` times Turbo construction. Lines have the requested `[chitta-field] load phase=<name> ms=<n>` form. `chitta/src/simple_cli.cpp:892` logs process-start-to-ready immediately before normal request dispatch. These phases overlap and do not add up to total startup: WAL replay, derived-map reconstruction, event tape/CDAWG [6](#ref-6), model setup and other initialization remain in the wall time.

2. **Warm before serving.** `chitta-field/src/field.rs:1265` starts Turbo and lite work beside HDC loading after replay/normalization, before store locks exist, and joins both before open returns. `hnsw.rs` publishes a completed immutable index, with no age-only maintenance rebuild on an unchanged corpus. The C++ loader also primes the encoder queue while the store loads and joins before leaving warming (`simple_cli.cpp:1674`). Before publishing Turbo, every bounded refresh worker executes a read-only search with a corpus vector, initializing the pool and search scratch without touching learned state. This last step reduced first recall from 118.5 to 92.4 ms in sequential measurements. No `.turbo` format is introduced. The startup regression test asserts that opening a persisted fixture returns with Turbo already available.

3. **Triplet migration.** `src/organ/triplet.rs:140` tracks whether the live SPO set is clean. Duplicate insertion/replay and invalidation clear that flag. Snapshot writes clean the cloned triplet organ and persist the optional V23 `triplets_clean` section (`src/snapshot.rs:1267`); an old V23 without the section still migrates once. Dirty WAL replay forces migration again. Borrowed dedup keys preserve the highest-weight winner without allocating three strings per row. The clean-checkpoint restart measures the migration portion as 0 ms; decoding and rebuilding derived triplet maps remain part of loading. This avoids recurring dedup, not every triplet-related allocation.

4. **Hybrid tail.** The control's first recall spent several seconds in competitive-weight refresh, with the first control diagnostic showing approximately 3.07 + 3.15 seconds in its two semantic lanes. The primary BEFORE log has a maximum single refresh stage of 4.20 seconds. Early warm hybrid samples remained around 245 ms while later samples fell near 33 ms: they were paying the outstanding refresh work. `src/store.rs:3008` captures an immutable fresh-Turbo view under a short semantic guard, runs independent neighbor searches in a bounded pool off all store guards, then applies weights using the existing two-phase logic. The original single-query arithmetic, deletion filtering and candidate order are retained. Dirty, scalar and HNSW [4](#ref-4) routes fall back to the existing search. A bitwise score/ID test covers serial versus parallel behavior. A four-query batching experiment was discarded because it changed float rounding without a measured latency gain.

   `CHITTA_RECALL_PROFILE=1` enables Rust semantic search / CW refresh / scoring timers and C++ setup, semantic lanes, keyword, HDC, bridge/fusion, rerank, prefilter, metadata, format and span timers (`src/profile.rs:1`, `chitta/src/handlers/field_memory_recall.cpp:17`). Output stays on stderr. In the verified run, CW refresh peaked at 12.63 ms; rerank/prefilter/format were not dominant. The supplied live 500–1100 ms tail was not reproduced in the quiesced scratch daemon, so this result does not establish that all live contention is resolved.

5. **Memory.** `src/profile.rs:44` emits capacity estimates for payloads, states, embeddings, HNSW, HDC, triplets, spans, CDAWG, semantic caches, lite encoder, keyword and episode structures. Each guard is held alone. `chitta/src/handlers/field_system.cpp:149` adds a `memory_breakdown` line to detailed health; request `health_check` with `details=true` to bypass the atomic fast path. The shared FFI additions are `cf_memory_breakdown` and `cf_recall_profile_enabled`; allocated C strings retain the existing `cf_free_string` ownership rule.

   The largest avoidable allocation found was the keyword reverse index: 13,426,880 per-document term entries replicated strings for just 108,299 distinct terms. `src/organ/keyword.rs:27` uses runtime `u32` term IDs, reclaiming IDs when the last posting disappears; serialized postings and BM25 [24](#ref-24) scoring stay unchanged. Episode HDC uses exact bit-plane counters instead of a `u16` per dimension (`src/hdc.rs:567`), including saturation behavior. Triplet source paths share `Arc<str>` (`src/organ/triplet.rs:16`): 804,781 paths referenced only 6,197 distinct strings, approximately 79.35 MB repeated bytes versus 0.61 MB unique. Their custom serde encoding is exactly the former String representation. Subject/predicate/object strings were left alone because the reverse keyword map offered the larger measured saving.

| Component allocation estimate (MiB) | BEFORE | AFTER |
|---|---:|---:|
| payloads | 168.31 | 168.31 |
| states | 62.25 | 62.25 |
| embeddings | 399.99 | 399.99 |
| hnsw | 0.00 | 0.00 |
| hdc | 269.50 | 269.50 |
| triplets | 712.42 | 627.04 |
| spans | 0.00 | 0.00 |
| cdawg | 85.89 | 85.89 |
| semantic_caches | 50.74 | 50.74 |
| lite_encoder | 11.09 | 11.09 |
| keyword_postings | not captured | 223.34 |
| keyword_reverse | 672.09 (legacy estimate) | 66.99 |
| episode_hdc | 51.70 (legacy estimate) | 7.30 |

   Estimates exclude allocator headers/fragmentation, Turbo internals, several other organs, the embedding model and thread stacks. They are allocation attribution, not additive RSS measurements. The reported RSS comes separately from `/proc/<pid>/status`. HNSW is zero for this replica's flat-scan route; zero spans means this copied family has no loaded span store. Legacy keyword/episode figures are explicit counterfactual capacity estimates on the final data, not component counters captured in BEFORE.

6. **Reproduction.** See [benchmark instructions](../benchmarks/field-perf/README.md). `run.sh <label> --profile` writes `results-<label>.json`, a standalone Markdown table and an ignored daemon log. `--checkpoint` saves only the scratch child after all measurements. A subsequent `--source <scratch>/chitta-field` makes another private copy and measures the clean marker. `compare.py` checks the same family/query/realm, response shapes and ordered IDs; its nonzero exit on adaptive differences is intentional and must not be treated as success.

Rust implementation commit: `bc709a3` (committed before the superproject).

## Compatibility and validation

- Release Rust build and `./build.sh test --release` passed: **277 passed, 2 existing ignored** (279 total; 44.84 s). The exact bitwise search comparison is included.
- CMake configured inside this worktree from the production `CHITTA_*`, compiler, Python and BLAS cache values, overriding only `CHITTA_FIELD_ROOT` to this checkout. Dependency sources are read from the production build; generated files stay here. Final C++ build passed and **all 18 CTests passed**, using the existing local GGUF for `embed_pool_test` (18.56 s total).
- The supplied worktree had no `target/release/.chitta-embed-identity` stamp and the wrapper did not create one. Its absence remains unchanged. Generated `embed_config.rs` verifies dimension 768 and model `nomic-embed-text-v1.5`, matching the production identity read before building. No embedding-identity migration or model change was performed; see [identity evidence](../benchmarks/field-perf/embed-identity.json).
- Fixed-input Rust regressions cover serial/parallel bitwise scores and IDs; stale-view safety and dirty-route fallback; warm-on-open; absent/present V23 marker; duplicate replay and invalidation; keyword legacy encoding plus removal/reindex/rebuild; shared source-path String encoding; and episode-counter growth/saturation. Original V23 loads and the new checkpoint reloads. Existing snapshot sections and WAL records retain their wire representation; `triplets_clean` is an optional named section old V23 readers skip.
- [Contract comparison](../benchmarks/field-perf/comparison.json): top-level structured key sets are unchanged for `recall`, `smart_recall`, `hybrid_recall`, `recall_keyword`, and `recall_lanes`. Keyword/fused/hybrid ordered IDs match 20/20. Smart matches 8/20 and combined lanes 3/20, with 4 variants already present within each BEFORE workload. The pre-existing route learner seeds from time and episode ID (`src/learner/route.rs:75`). Optional `affect` appears on different first selected sem/ctx rows in the final combined-lane contract probe; strict sampled-shape equality therefore fails there. No recall response construction or schema keys were changed by this patch, but deterministic parity for adaptive routes is **not established**. `compare.py` exits 1, as recorded.
- Changed shell syntax, touched-Python ruff and both `git diff --check` checks passed. All 15 hook scripts passed after rerunning the self-isolating session-cards test without a conflicting outer socket override; all 6 of its fixtures passed. SMRITI: 46 passed.
- Additional MCP suite: 130 tests, one error in unmodified HTTP session integration. Available SDK 1.27.2 lacks `_session_owners`, which existing `create_http_session_manager` expects. This is an outstanding ancillary gate failure; no out-of-scope SDK installation or MCP code change was made. [Ancillary exit codes](../benchmarks/field-perf/ancillary-checks.json) preserve this failure.


## Intermediate measurements

| Artifact label / step | Ready (ms) | First recall (ms) | Hybrid p50 / p95 (ms) | RSS (MiB) |
|---|---:|---:|---:|---:|
| before / instrumentation control | 35928.8 | 6440.6 | 33.7 / 245.1 | 4322.4 |
| warm / eager Turbo + borrowed dedup | 50357.5 | 345.0 | 49.5 / 343.3 | 4266.8 |
| memory / episode counters + clean marker | 38625.4 | 343.9 | 58.8 / 336.7 | 4233.4 |
| after / parallel refresh | 34057.2 | 102.4 | 43.5 / 64.4 | 4217.5 |
| final / compact keyword reverse map | 26572.4 | 111.6 | 37.3 / 60.1 | 3312.4 |
| optimized / shared paths + encoder prime + rejected batching | 30951.8 | 110.0 | 49.0 / 70.4 | 3223.5 |
| verified / exact arithmetic restored | 28758.1 | 118.5 | 40.8 / 65.0 | 3219.7 |
| primed / warm workers (primary AFTER) | 28712.3 | 92.4 | 34.7 / 64.1 | 3220.2 |

These are sequential implementation measurements on a shared node, not an additive attribution experiment. `optimized` contains the discarded four-query batching experiment; `verified` restored exact single-query arithmetic. The primary final comparison uses the final implementation only. The clean restart uses a newly checkpointed family and is a load-path check, not the same-family retrieval control.

2026-09-16 — Snapshot decode (V23 unchanged): four bounded Rayon workers decode immutable mmap section ranges, pre-size root maps, and overlap triplet-index reconstruction; keyword reverse reconstruction overlaps Turbo/HDC/lite startup. Two cold-process starts on the same scratch checkpoint plus one-row replay delta (Turbo cache hit both arms): snapshot **9195/7819 → 4263/3567 ms**, field_store **14999/14575 → 9516/9263 ms**. Targets <2500/<9000 ms were **not met**; triplet reconstruction remains 2944/2325 ms. The repeated-query restart gate is **20/20 byte-identical CLI JSON pairs**; the broader 20-query diagnostic is 19/20 after versus 18/20 in the warmed control (not a claim of universal determinism). Release build, 279 Rust tests (2 ignored), and all 16 CTests pass; current-main binary opens the new writer's checkpoint. Embedding constants remain 768/nomic-embed-text-v1.5/format-1; the worktree identity stamp was absent before/after and the main stamp hash is unchanged. OS caches and shared-host load were uncontrolled; overlapping phases must not be summed.

<!-- BEGIN CITATIONS -->
## References

- <a id="ref-4"></a>**[4]** Yu. A. Malkov and D. A. Yashunin. Efficient and robust approximate nearest neighbor search using Hierarchical Navigable Small World graphs. IEEE TPAMI 42(4), 824–836 (2020); arXiv:1603.09320 (2016). [source](<https://arxiv.org/abs/1603.09320>) [source](<https://doi.org/10.1109/TPAMI.2018.2889473>)
- <a id="ref-6"></a>**[6]** A. Blumer, J. Blumer, D. Haussler, R. McConnell, and A. Ehrenfeucht. Complete inverted files for efficient text retrieval and analysis. Journal of the ACM 34(3), 578–595 (1987). [source](<https://doi.org/10.1145/28869.28873>)
- <a id="ref-10"></a>**[10]** Pentti Kanerva. Hyperdimensional Computing: An Introduction to Computing in Distributed Representation with High-Dimensional Random Vectors. Cognitive Computation 1, 139–159 (2009). [source](<https://doi.org/10.1007/s12559-009-9009-8>)
- <a id="ref-24"></a>**[24]** Stephen Robertson and Hugo Zaragoza. The Probabilistic Relevance Framework: BM25 and Beyond. Foundations and Trends in Information Retrieval 3(4), 333–389 (2009). [source](<https://doi.org/10.1561/1500000019>)
<!-- END CITATIONS -->

2026-09-16 (live, after the split-store deploy): ready 13.0 s with all three sidecars hitting; the first recall after `ready` still took 10.5 s, then 0.27 s and 0.04 s. On the replica the first recall during the turbo rebuild returned in 395 ms, so the live cost is not the rebuild alone; Phase 6 of `DECISION-2026-09-16-robustness-plan.md` measures it with `scripts/stress-embed-recall.py` (suspects: query-embedding kernel warm-up contending with the rebuild for BLAS threads).
