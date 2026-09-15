# Startup phase optimization — 2026-09-15

## Result and scope

Clean-family startup fell from **31.384 / 27.193 s** to **13.300 / 13.138 s** (mean **29.289 → 13.219 s**, **54.9% faster**). Normalization and event-tape cache loads meet their respective 500 ms and 800 ms targets. The 12 s overall target is approached, not fully reached.

Optional startup caches remove repeated LSH projection and event-tape organ reconstruction, and skip TurboVec quantization. Snapshot format/version and embedding identity remain unchanged. All work and measurements are isolated from the live mind and services.

## Changes

1. **Normalization/LSH** — `chitta-field/src/hnsw.rs:1944`. `normalize_all` previously included rebuilding LSH projections, because `mem_lsh` is serde-skipped. Preserve the exact f32 normalization arithmetic, use the existing bounded Rayon pool, and skip division only when the computed norm is exactly 1.0. Persist LSH signatures in `.lsh`; rebuild inverted buckets on load. The input key hashes every post-WAL normalized vector's exact bits and ID, deletion state, model identity, dimension and algorithm revision. Same-count replacements invalidate the cache. The mmap path retains existing fallback behavior.
2. **Event-tape organs** — `chitta-field/src/startup_cache.rs:90`, `chitta-field/src/field.rs:1342`, and `chitta-field/src/organ/event_tape.rs:69`. `.organs` contains the CDAWG plus episode-HDC bit planes. Its key covers ordered events and the tool/entity name dictionaries, including same-length edits. Restore `CdawgOrgan::rebuilt`; validate HDC plane dimensions. Build from the tape rather than preserving transient runtime credit state, so cache presence retains previous startup semantics.
3. **Turbo** — `chitta-field/src/hnsw.rs:2060`. Use turbovec 0.9 native persistence for quantized rows, plus `.turbo.meta` containing memory IDs, the vector-input fingerprint, and the index-body checksum. Validate dimensions, bit width, row count and IDs. Run `prepare()` and per-worker search warmup before publication. Rotation, centroid and blocked-layout caches are private runtime-only TurboVec fields; the existing API regenerates those, so this removes quantization rather than all Turbo startup work.
4. **Snapshot profile** — `chitta-field/src/snapshot.rs:1209`. `CHITTA_PROFILE_SNAPSHOT=1` reports file-refill time and each V23 section's decode time. No decode algorithm/layout change. The existing snapshot phase also includes post-decode triplet-index rebuilding and state hydration; it is not solely bincode time.

`chitta-field/src/startup_cache.rs:11` validates cache magic, input fingerprint, and SHA-256 body checksum before bounded deserialization. Truncated, corrupt, absent or stale caches fall back to reconstruction. Writes use temporary files and atomic rename. Turbo metadata rejects a torn index/metadata pair. These optional files are not required manifest references.

`chitta-field/src/store.rs:8922` creates caches for the next load from a detached snapshot clone, after writing `.emb`. It applies the next loader's exact normalization and detaches shared Turbo Arcs before mutation. Unchanged index sidecars retain the existing dirty-skip behavior. Tape identity skips unchanged organ reconstruction. This shifts cold-cache work to checkpoint time; it does not change the live search index. Optional caches total approximately 89 MiB on the eval copy.

Replica selection/copying and active-save detection include `.lsh`, `.organs`, `.turbo`, and `.turbo.meta` (`scripts/eval-replica-select.py:233`, `scripts/eval-replica.sh:78`). Optional startup files follow snapshot-family pruning.

## Measurement protocol

- Frozen source: `/projects/caeg/scratch/kbd606/tmp/chitta-eval-mind/chitta-field`, selected with `scripts/eval-replica-select.py`: family `bbcaed33`, sequence `206208172`, manifest generation `38047`, 833,668,405-byte snapshot.
- Copied exactly the selected family, both manifests, canonical WAL segments, migration markers and lite encoder using the replica script's rules; verified selection before opening. No process opened the frozen source or live mind.
- Scratch daemon uses a private `--path`, worktree-root `XDG_RUNTIME_DIR`, port 17441, `CHITTA_NO_QUEUE=1`, `.quiesce`, disabled autonomous/distill/enrich/hygiene/embedding intervals, and the existing nomic GGUF model.
- Baseline daemon built from submodule `9a1052c373b1dac7e270938995dff87d7974c7e2`, before implementation; optimized daemon links the changed library. Same compiler, model and CMake settings. No builds run during the reported pairs.
- Each reported start is a fresh process over the same private files. No system page-cache flushing. File-cache/NFS effects are visible in the timings; fresh copies/checkpoints can be much slower than repeated process starts.
- Scratch-only preload pins realtime to 2026-09-15 19:00 UTC for scoring; monotonic startup timers remain real. Utility seed 42. Twenty fixed queries, `recall --json --limit 10 --no-learn`. Processes terminate by their own `Popen` handle; no broad process commands or service operations.
- Evidence and reproducible harnesses: untracked `startup-evidence/`; decisions: untracked `Plan.md`.

## Phase measurements

### Clean checkpointed family (db4b09ea)

| Phase (ms) | Before 1 | Before 2 | After 1 | After 2 |
|---|---:|---:|---:|---:|
| snapshot | 6235 | 4746 | 6131 | 5753 |
| normalize | 6428 | 6611 | 162 | 167 |
| event_tape_organs | 7078 | 5419 | 320 | 676 |
| turbo build / startup | 6734 | 6327 | 2612 | 2688 |
| keyword_reverse | 1272 | 1372 | 1294 | 1316 |
| emb | 332 | 265 | 322 | 314 |
| pld | 188 | 138 | 175 | 150 |
| hdc | 453 | 451 | 586 | 632 |
| span_store | 0 | 0 | 0 | 0 |
| symbols | 202 | 198 | 158 | 166 |
| lite_encoder | 28 | 29 | 151 | 165 |
| wal_replay | 346 | 226 | 331 | 309 |
| triplets | 0 | 0 | 0 | 0 |
| **field_store total** | 31384 | 27193 | 13300 | 13138 |

### Original frozen family copy (bbcaed33), including legacy cleanup

| Phase (ms) | Before 1 | Before 2 | After 1 | After 2 |
|---|---:|---:|---:|---:|
| snapshot | 6569 | 6290 | 5372 | 5235 |
| normalize | 6485 | 7122 | 166 | 171 |
| event_tape_organs | 6773 | 6062 | 301 | 375 |
| turbo build / startup | 6909 | 5481 | 2851 | 2491 |
| keyword_reverse | 1327 | 1560 | 1841 | 1624 |
| emb | 319 | 333 | 309 | 339 |
| pld | 129 | 186 | 130 | 182 |
| hdc | 335 | 323 | 1785 | 467 |
| span_store | 0 | 0 | 0 | 0 |
| symbols | 174 | 337 | 629 | 280 |
| lite_encoder | 58 | 57 | 1043 | 50 |
| wal_replay | 675 | 936 | 679 | 820 |
| triplets | 4266 | 4855 | 3390 | 4701 |
| **field_store total** | 36167 | 35583 | 17603 | 18067 |

Phases overlap: HDC and lite encoder run alongside Turbo. Do not sum individual rows. Baseline `turbo` measures build; optimized `turbo_startup` includes input validation, file loading, preparation and worker warmup, making that comparison conservative.

The original eval family has 1,401,549 triplets and lacks the existing clean marker. Normal startup deduplicates 21,823 entries; its 3.4–4.9 s `triplets` cost is separate from snapshot decode. A normal private checkpoint creates family `db4b09ea`, sequence `206243514`, generation `38048`, with the existing clean marker. No new migration or snapshot version is introduced. The baseline and optimized binaries are both measured against the identical checkpointed files.

## Snapshot profile

On the original frozen-family clean-cache pair:

| Snapshot breakdown (ms) | After 1 | After 2 |
|---|---:|---:|
| Snapshot phase total | 5372 | 5235 |
| File reads | 199 | 177 |
| Section decoding + allocation, excluding reads | 2752 | 2320 |
| Post-decode indexes / sanitization / hydration | 2421 | 2738 |

Largest decoded sections were triplet_store (684–730 ms), keyword_idx (441–451 ms), states (238–359 ms), and symbol_idx (321–336 ms), inclusive of their I/O/allocations. The file-read timer includes all snapshot refills. The post-decode residual is dominated by `TripletStore::rebuild_indexes()` and state-map hydration.

Perf captured **1,469 stacks containing the snapshot loader**: **238 (16.2%)** had allocator leaf frames, **97 (6.6%)** memory copy/clear, and **1,134 (77.2%)** decode/index/other work. The latter includes collection hashing/insertion and buffered-reader/serde work. Samples from unrelated embedding/worker threads are excluded. The main-thread-only profile was discarded because FieldStore loads on a dedicated loader thread.

Allocation percentages are sampled CPU attribution, not independent wall-clock timers. The post-decode residual also includes sanitization and state-map hydration. Improving these collection layouts or persisting their indexes would be a separate change; mmap/lazy payload changes are not included.

## Recall identity and rollback

- Default fused recall: unchanged baseline A/B already matches only **15/20** raw JSON responses. Baseline A vs optimized A/B matches **17/20** and **15/20**. All **20/20 result-ID orders** match. Differences are relevance scores, not normalization bits or returned IDs.
- Existing HDC search sorts solely by Hamming distance (`chitta-field/src/hdc.rs:323`), leaving ties in HashMap iteration order. Rank fusion assigns different scores to those tied positions. This is reproducible in unchanged baselines.
- With the existing evaluation flag `--disable-hdc true`, the same twenty queries are **20/20 byte-identical** before/after, with no JSON fields removed or rounded. This isolates normalization/Turbo identity. It is a qualified result, not a claim that default fused JSON is deterministic.
- Both clean-family baseline runs and both optimized runs also match **20/20 byte-for-byte** under that same control.
- After checkpointing, all three optional caches hit. The old binary loads the new snapshot and returns **20/20 byte-identical** controlled responses versus the optimized binary. Rollback requires no sidecar removal or migration.

## Validation

- `chitta-field/build.sh build --release`: passed.
- `chitta-field/build.sh test --release`: **269 passed, 2 ignored**, 271 library tests discovered; other binary/doc targets passed. Includes four new cache tests for corruption/truncation, same-count vector edits, tape edits, exact vector bits, Turbo row/score identity, and HDC accumulator round trips.
- Identity marker remains `768:nomic-embed-text-v1.5`; SHA-256 `85c1d228e600808cb82792374bf0da2bc2f99e8ec46d57c1222f30d6a0bc0e02` before/after.
- CMake uses main's CHITTA values (768, LLAMA_CPP/RPC/TESTS ON), Release, BLAS and compiler/Python paths, with CHITTA_FIELD_ROOT pointing inside this worktree. Build passed. **16/16 CTest cases passed**, including embed_pool_test with CHITTA_EMBED_MODEL supplied.
- All **18 hook suites**, SMRITI tests, changed-shell syntax, changed-Python ruff and diff whitespace passed. Replica test covers copying all four new optional files.
- Additional unchanged MCP suite: **126/127 passed**; SDK-session test errors because available MCP SDK lacks `_session_owners` expected by `server.py`. No MCP code changed. Earlier test-isolation path failures were fixed by short temporary paths and clearing inherited socket overrides; they are not remaining failures.

A first read of newly written NFS checkpoint files took 37.7 s (20.4 s snapshot, 8.3 s embeddings; organ read 957 ms). Subsequent matched clean-family starts are the table above. These are process-cold timings, not a guarantee for uncached NFS reads.

No install, service restart, push, live mind access, or deployment was performed.
