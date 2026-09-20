# Mapped snapshot: serving without decoding

Status as of 2026-09-20: **revised after phase 0 (section 7a). The mapping gate
passed; the premise about which sections dominate was wrong and section 2 is
corrected. Implementation by Opus 5 on `feat/mapped-snapshot`.** Continues
[Runtime and storage core](DESIGN-2026-09-19-runtime-core.md) and
[the storage decision log](DECISION-2026-09-19-storage-core.md), whose phases
1, 2a and 2b are live in v5.73.0.

## 1. The floor this removes

Live restart on 2026-09-19 22:13Z (main `9b002e1f`, store `c5eec70`), 27 MB WAL:

| Step | Time |
|---|---|
| socket bound, answering `loading` | 0.014 s |
| snapshot decode | 6.05 s |
| WAL replay | 0.52 s |
| store open (all phases) | 11.49 s |
| listening | 1.2 s after store-ready |
| healthy | 19.7 s from the restart command |

On the frozen replica the same open is store-ready 5.5–6.0 s and first correct
recall 6.2–6.7 s. Everything deferrable has been deferred: keyword index,
Turbo, organs, hdc, symbols, spans and the code index all load after the socket
serves. What remains is decoding the sections that serving itself needs.

The live family is 2.4 GB:

| File | Size |
|---|---|
| `.snapshot` | 1,402 MB |
| `.emb` | 414 MB |
| `.cortex` | 212 MB |
| `.hdc` | 148 MB |
| `.sup.json` | 79 MB |
| `.pld` | 76 MB |
| `.turbo` | 52 MB |
| `.organs` | 36 MB |
| `.bin` | 14 MB |
| `.lsh` | 4 MB |

V23 is a sectioned container: `[magic][seqno]` then, per `FullSnapshot` field,
`[name_len][name][body_len][body]`, each body positional bincode of one whole
in-memory structure (`payloads: HashMap<MemoryId, MemoryPayload>`,
`states`, `semantic_idx`, `keyword_idx`, `triplet_store`, and twenty more).
Opening means allocating every record before the first answer. That is the
6 s, and no amount of deferral removes it, because `peek_memory` and
`recall_semantic` read those maps directly.

## 2. What the design does

Write the four structures that serving needs as fixed-layout, page-aligned
arrays, and map them instead of decoding them. Publish when the mapping is
validated. Everything else keeps its current path.

Serving needs exactly:

1. **state table** — one fixed-size row per memory: id, realm id, kind id,
   created/authored ms, flags (deleted, candidate), ack score, content offset
   and length, embedding row index.
2. **content arena** — raw memory bytes, concatenated, addressed by the state
   table's offset and length.
3. **embedding matrix** — row-major `f32` (or `f16` with a per-row scale, see
   §5), one row per memory, indexed by the state table's row index.
4. **LSH signatures** — the existing sidecar, already small (4 MB) and already
   cache-hit on load.

Everything else (Turbo, organs, hdc, symbols, spans, keyword index, HNSW
graph) is already deferred or rebuilt after publication. **The triplet store is
not** — see section 3a; taking it off the open path is step 1 of the work
order.

## 3. Measured constraint: NFS page behaviour

Measured on this cluster, 2026-09-20, on a 396 MB `.emb` sidecar that nothing
had read (`/projects/caeg/.../learning-cut-20260915-frozen`):

| Access | Cost |
|---|---|
| random first-touch page fault | 1,583 µs |
| repeat touch (page cached) | 0.7 µs |
| sequential read | 947 MB/s |

Reading the same file after it was warm: 5 µs first touch, 2,616 MB/s.

This decides two things. **Sequential is cheap**: streaming the whole 414 MB
embedding matrix costs about 0.44 s, and the 1.4 GB of state plus content about
1.5 s, both far below today's 6 s decode, because there is no allocation.
**Random is expensive when cold**: a recall touching 100 uncached rows would
pay 0.16 s. So the design maps for serving *and* immediately streams the
mapping on the maintenance thread (`madvise(MADV_WILLNEED)` plus a sequential
touch), which warms the whole family in about 2 s while requests are already
being answered.

## 3a. Phase 0 result, measured 2026-09-20

Frozen replica, third start (warm sidecars). Sections decode in a four-thread
pool, so the wall clock is the longest chain, not the sum.

| Section | decode_ms | Share of the 836 MB body |
|---|---|---|
| parallel_sections (wall clock) | 2,778 | |
| triplet_store + its index rebuild | 830 + 1,899 | 332 MB (39.7%) |
| keyword_idx | 344 | |
| symbol_idx | 289 | |
| payloads + states | 184 + 168 | 82 MB (9.8%) |
| everything else | ≤ 129 each | |

The §5 arrays, written from that family (134,804 rows, dim 768; 499 MB total:
state 8.6 MB, arena 77 MB, embeddings 414 MB):

| Metric | Median | Range |
|---|---|---|
| map + validate, cold | 9.8 ms | 7.6–13.4 |
| map + validate, warm | 0.30 ms | 0.24–0.79 |
| 1,000 random lookups, cold | 496 ms | 486–529 |
| 1,000 random lookups, warm | 0.77 ms | 0.71–1.20 |
| full CRC verify | 464 ms | 439–530 |
| embedding scan, 395 MB, cold | 376 ms | 1,050 MB/s |
| embedding scan, warm | 173 ms | 2,282 MB/s |

`MADV_WILLNEED` plus sequential touch over the 1.80 GB frozen family: 1.74 s
at 1,034 MB/s, so about 2.3 s for the live 2.4 GB family.

**Gate: passed.** Worst map+validate 13.4 ms against a 1 s bar; worst warm
1,000-lookup 1.20 ms against a 5 ms bar. Mapping is not the risk.

**The premise was wrong, and the design changes because of it.** Payloads and
states are 9.8% of the body and 352 ms of concurrent decode. The dominant term
is the triplet store: 332 MB decoded in 830 ms, chaining straight into
`rebuild_indexes()` at `chitta-field/src/snapshot.rs:1437`, together 2,729 ms of
the 2,778 ms critical path. Section 2 listed triplets as already deferred; they
are not. The `load phase=triplets ms=0` line belongs to a later step.

So mapping alone takes store-ready from about 6.0 s to about 5.2 s. **Triplet
decode and index rebuild must leave the open path**, and that is now the first
implementation step, ahead of the format work:

- The serving lanes that need the triplet store are the spreading-activation
  and triplet-query paths (`store/recall.rs:1604,1652,2506`), not semantic or
  keyword recall. They answer `loading` until the store is ready, exactly as
  `code_query` does today.
- Decode and rebuild move to the maintenance thread after publication, with a
  `deferred phase=triplets duration_ms=` log line like the other deferred
  phases.
- Keyword and symbol indexes (344 ms and 289 ms) follow the same rule if they
  are still on the critical path once triplets are off it.

Corrections to the rest of this document: the acceptance memory count is
134,804 for this family, not 134,805 (the `.emb` sidecar carries 134,807 rows,
which the parity check must tolerate); the embedding phase (723 ms) and `pld`
(104 ms) disappear when those arrays are mapped.

## 4. Startup sequence after this change

```
bind socket, answer loading                      0.01 s   (already live)
open family, read header + section table         ~0.01 s
validate: magic, version, section checksums sampled
map state table, content arena, embedding matrix, LSH
WAL tail replay (certified skip, already live)   0.5 s
publish store; health and recall answer          target < 1.5 s
  → maintenance thread: MADV_WILLNEED sweep      ~2 s, concurrent
  → maintenance thread: full checksum verify     concurrent
  → maintenance thread: HNSW load, Turbo, organs, hdc, symbols, spans,
    keyword index, triplets (already deferred today)
ready

Replay precedes publication: a reader must never see the mapped snapshot
without the acknowledged writes after it, and the family's coverage must be
consistent before anything is served (review finding, 2026-09-20).
```

Recall between publication and HNSW load uses the LSH path, which is how the
staged publication already behaves; the HNSW switch-in is unchanged.

## 5. Format, V24

A new family format written **alongside** V23, not replacing it (see §6).

```
[magic V24: u64][format_version: u32][flags: u32][seqno: u64]
[section count: u32]
per section: [name: 16 bytes, nul-padded][offset: u64][len: u64]
             [elem_size: u32][elem_count: u64][checksum: u64 xxh3][align: u32]
... padding to 4096 ...
sections, each starting on a 4096-byte boundary
```

Sections in the first V24:

- `state` — `elem_size` fixed (target 64 bytes), `elem_count` = memory count.
  Field order is explicit and documented in the struct; all integers
  little-endian; no padding holes (asserted in a test).
- `arena` — raw bytes; `elem_size` 1.
- `emb` — `elem_size` = `dim * 4` for f32; `flags` bit marks f16+scale.
- `strings` — interned realm and kind names, so the state table holds u32 ids
  rather than variable-length strings.

Everything else stays a V23-style bincode section in the same container, so the
migration is additive: the loader reads V24 sections by mapping and the rest by
decoding, and `FullSnapshot` keeps its remaining fields.

**Embedding precision.** f32 keeps recall bit-identical and costs 414 MB. f16
with a per-row scale halves it and changes scores in the last decimals. The
design starts with **f32** so parity is exact; f16 is a later, separately
measured change, not part of this one.

## 6. Compatibility and rollback

- Every commit writes a V23 family **and** a V24 family as twins with the
  same seqno and the same WAL coverage. Keeping only the *previous* V23 family
  is not lossless: `prune_certified_wal` certifies against the newest family
  (`chitta-field/src/store/maintenance.rs:1431-1463`), so segments between an
  older family's coverage and the newest would be deleted, and a rollback to
  the older family would lose acknowledged writes (review finding,
  2026-09-20). With twins, the previous binary opens the V23 twin of the same
  commit and needs no WAL that pruning could have removed. The V23 twin keeps
  `next_id` inside its bincode body, so the allocator high-water mark survives
  rollback. `prune_old_snapshots` keeps two commits, so two twin pairs. The
  rollback floor stays chitta-field v2.1.0; this adds a second floor line for
  V24, removed only when the twin writing is retired.
- A daemon that finds only V24 and cannot map it (older binary) fails with the
  existing "manifest family failed validation" path and falls back to the
  previous family, which is the current behaviour for a bad family.
- The first V24 commit migrates 140k memories into the arena. It runs on the
  maintenance thread like any other family commit (34 s measured for a full
  commit today) and must not block requests.

## 7. Phase 0, before any format code

Numbers to record in `docs/DECISION-2026-09-19-storage-core.md`:

1. Per-section decode times of the current V23 open on the replica
   (`snapshot section=<name> decode_ms=` lines already exist) — proves the
   1.4 GB `payloads`/`states` decode is the dominant term and not something else.
2. A standalone benchmark that writes the live family's memories as the §5
   arrays into a scratch file, then measures: map + validate time, time to
   answer `peek_memory` for 1,000 random ids, and time for one LSH recall,
   cold and warm. This is the design's proof and takes no store changes.
3. Cold-page sweep cost for the real family size (`MADV_WILLNEED` over 2 GB).

If (2) does not show map+validate under 1 s and random `peek_memory` under
5 ms warm, the design is wrong and stops there.

## 8. Acceptance

On the frozen replica, with content parity against the eager path:

| Metric | Today | Target |
|---|---|---|
| first health answer | 0.05 s | unchanged |
| store-ready, triplets deferred only | 5.5–6.0 s | < 3.0 s |
| store-ready, deferred + mapped | 5.5–6.0 s | < 1.0 s |
| first correct recall | 6.2–6.7 s | < 2.0 s |
| recall ids vs eager path | identical | identical |
| memory count | 134,804 | 134,804 |

Live target: healthy under 10 s from the restart command. Plus: Rust suite
three times, `ctest`, hook suites and contracts green on a detached checkout;
the p21 durability soak (36,000 writes, forced compactions) unchanged; a
V23-only family still opens; a V24 family opened by the previous release fails
safe.

## 9. Work order

1. Phase 0 measurements (§7): done, recorded in §3a.
1a. Triplet decode and `rebuild_indexes` move off the open path to the
   maintenance thread, with loading answers for the spreading-activation and
   triplet-query lanes. This is the largest single win and needs no format
   change; measure store-ready before and after.
2. V24 writer behind `CHITTA_SNAPSHOT_V24=1`, default off; V24 reader; V23 and
   V24 twins written on every commit with identical coverage (§6). Gates green
   with the flag off and on.
3. Mapped serving path: `peek_memory` and the recall scorers read through the
   mapping; publication happens after mapping; `MADV_WILLNEED` sweep on the
   maintenance thread.
4. Replica acceptance (§8), then default the flag on, then deploy.
5. Only after that: f16 embeddings as a separate measured change.

## Where to read the code

`chitta-field/src/snapshot.rs` holds the V23 container and `FullSnapshot`.
`chitta-field/src/store.rs` holds `peek_memory`, the read path this design
maps, and `chitta-field/src/store/recall.rs` holds `recall_semantic`, the scan
it maps. The two prior documents are the runtime core design and the storage
core decision log, both dated 2026-09-19 in this directory.
