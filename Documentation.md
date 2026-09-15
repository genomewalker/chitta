# Startup caches with replay delta and deterministic recall — 2026-09-15

## Result

All requested native, latency, identity, and rollback gates pass. The final fixture contains 133,975 snapshot vectors and exactly 200 replayed vector writes, plus 20 temporary triplet additions/invalidation pairs. All work and daemon runs used this worktree and private copies of the eval replica family.

- Cached-with-delta `turbo_startup`: **1213 ms** (<1,500 ms).
- Recall issued after the maintenance rebuild started returned in **395.1 ms**, before rebuilding finished. `CHITTA_TURBO_REBUILD_MIN=1000000000` verifies startup bypasses the ordinary mutation threshold.
- **20/20 byte-identical CLI JSON responses across three cold restarts at each stage:** before the delta, after the 200-write delta, and after checkpointing that delta. HDC uses its default enabled behavior. This covers 180 complete response files, compared without normalization or field removal.
- Release build passed; **276 Rust tests passed, 2 ignored**; CMake build and **16/16 CTests passed**; embedding identity unchanged; preserved pre-change binary opens the new sidecars and completes all 20 queries.

## Implementation

### Snapshot reuse and maintenance

`chitta-field/src/hnsw.rs` captures the raw snapshot `.emb` digest before replay. The LSH cache stores each snapshot row's exact f32 normalization divisor and signatures. Replay additions/replacements recompute their data; deletions disappear. Regeneration handles missing and corrupt caches before replay, including initialization of serde-skipped bucket arrays. The latter is covered by a regression that starts from a deserialized index.

Turbo loads and prepares the snapshot's quantized rows at startup. Its mutation watermark excludes replay replacements/deletions from cached results, while replay rows receive exact cosine scores through the existing overlay. The first maintenance tick plans a full rebuild regardless of the usual mutation threshold. Quantization runs outside store locks and publishes an immutable replacement. No calibrated append is used.

`chitta-field/src/turbo.rs` prepares loaded codes in independent, block-aligned chunks through TurboVec's public packer, then uses its original search kernel. Indexed concatenation preserves every byte, including the padded final block. Native quantization and native persistence remain in use. Real-cache validation compared 20 top-90 searches against native TurboVec: all indices and score bits matched. The delta regression compares packed row codes, scales, and complete calibration data after maintenance against a cold full rebuild, including 200 additions, replacements, and a deletion.

`chitta-field/src/organ/triplet.rs` limits dedup selection to dirty subjects, retains stable survivors and first-entry ties, and updates existing indexes after stable compaction. Historical duplicate IDs force the legacy scan only when an affected subject involves an ambiguous ID. Tests compare with the old full cleaner, including invalidations and historical ID collisions.

### Recall determinism

The audit found three sources of instability:

1. HDC and several semantic/sparse/fusion rankings omitted ID tie-breaks. Rankings now use total float ordering and IDs; bounded fallback buckets and graph reductions have deterministic traversal order.
2. Whole-corpus Turbo builds consumed HashMap iteration order. Build rows now follow sorted memory IDs; parallel refresh collection preserves candidate order.
3. Result hydration called content/kind/realm getters that each queued an access update despite `--no-learn`. The maintenance timer changed access counts and ACT-R scores between queries. The new `cf_peek_content` and metadata-only getters make hydration read-only; C++ recall uses that path. Existing explicit content reads still record accesses. A regression checks no-learn recall, all hydration getters, buffer-size retry, and timer drain without changing access counts or WAL position.

The C++ bridge-anchor selection and final reranks also break score ties by ID.

## Final delta phase table

Both columns use the final binary and the same snapshot/WAL content. Cold removes the optional `.lsh`, `.turbo`, `.turbo.meta`, and `.organs` caches on its private copy; cached restores snapshot-only caches before replay. Starts are process-cold, with no competing builds and no system page-cache flush. Filesystem/cache residency can affect I/O phases. Concurrent startup phases overlap, so columns must not be summed.

| Phase (ms) | Cold caches | Cached with delta |
|---|---:|---:|
| snapshot | 5594 | 4724 |
| pld | 96 | 128 |
| emb | 1047 | 709 |
| wal_replay | 676 | 604 |
| normalize | 176 | 122 |
| keyword_reverse | 1131 | 1199 |
| symbols | 191 | 168 |
| hdc | 1488 | 317 |
| lite_encoder | 161 | 54 |
| turbo_startup | 2680 | 1213 |
| span_store | 0 | 0 |
| event_tape_organs | 6105 | 700 |
| triplets | 247 | 479 |
| field_store | 19120 | 11669 |


The cold embedding phase includes one-time LSH cache regeneration. Snapshot decoding, keyword reverse-index construction, and stable triplet compaction still scale with store size. The full Turbo rebuild is off the ready path in the cached arm.

## Protocol and evidence

The snapshot source is family `db4b064f`, seqno 206244559, generation 38052. The pre-delta copy uses its recorded file list before the CLI writer segment. The exact delta retains all 200 verified 768-coordinate CLI writes and 20 matched temporary triplet additions/invalidation records. The checkpointed copy is family `db4b095a`, seqno 206245400, generation 38053. A standalone helper checkpoints only the stopped private copy because the CLI has no full-snapshot RPC.

Each identity restart restores the selected WAL and snapshot caches. The harness uses the same 20 `chitta recall --json --no-learn` queries, warms embeddings through an empty realm, fixes realtime for age-dependent scores, keeps monotonic timers real, and pins utility seed 42. Autonomous work and queue consumption are disabled. The overlap probe runs separately from identity comparisons. The harness waits for the actual daemon-ready log, since its socket begins listening during warmup.

Embedding identity SHA-256: `85c1d228e600808cb82792374bf0da2bc2f99e8ec46d57c1222f30d6a0bc0e02`.

Reproduction and raw evidence remain untracked under `startup-evidence/`: `f3-measure.py`, `f3-run-gates.py`, `f3-report.py`, `f3-results.json`, `f3-lsh-init-*` build/test logs, the final pre/delta/snapshot response directories, cold/cached logs, and `f3-rollback/`. Earlier failed exploratory runs are retained separately. No services were restarted, binaries installed, live mind/configuration modified, or commits pushed.
