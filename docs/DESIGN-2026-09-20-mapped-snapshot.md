# Mapped snapshot: serving without decoding

Status as of 2026-09-20: **design, authored by the lead (Fable); implementation
by Opus 5 on `feat/mapped-snapshot`. No code until the phase 0 numbers in
section 7 are recorded.** Continues
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

Everything else (Turbo, organs, hdc, triplets, symbols, spans, keyword index,
HNSW graph) is already deferred or rebuilt after publication.

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

## 4. Startup sequence after this change

```
bind socket, answer loading                      0.01 s   (already live)
open family, read header + section table         ~0.01 s
validate: magic, version, section checksums sampled
map state table, content arena, embedding matrix, LSH
publish store; health and recall answer          target < 1 s
  → maintenance thread: MADV_WILLNEED sweep      ~2 s, concurrent
  → maintenance thread: full checksum verify     concurrent
  → maintenance thread: HNSW load, Turbo, organs, hdc, symbols, spans,
    keyword index, triplets (already deferred today)
WAL tail replay (certified skip, already live)   0.5 s
ready
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

- A V24 commit writes V24 **and** keeps the previous V23 family on disk;
  `prune_old_snapshots` keeps two families, so an older daemon always has a
  readable family for two save cycles. The rollback floor stays chitta-field
  v2.1.0 as documented in CLAUDE.md; this adds a second floor line for V24.
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
| store-ready | 5.5–6.0 s | < 1.0 s |
| first correct recall | 6.2–6.7 s | < 2.0 s |
| recall ids vs eager path | identical | identical |
| memory count | 134,805 | 134,805 |

Live target: healthy under 10 s from the restart command. Plus: Rust suite
three times, `ctest`, hook suites and contracts green on a detached checkout;
the p21 durability soak (36,000 writes, forced compactions) unchanged; a
V23-only family still opens; a V24 family opened by the previous release fails
safe.

## 9. Work order

1. Phase 0 measurements (§7), recorded, no store changes.
2. V24 writer behind `CHITTA_SNAPSHOT_V24=1`, default off; V24 reader; both
   families written on commit. Gates green with the flag off and on.
3. Mapped serving path: `peek_memory` and the recall scorers read through the
   mapping; publication happens after mapping; `MADV_WILLNEED` sweep on the
   maintenance thread.
4. Replica acceptance (§8), then default the flag on, then deploy.
5. Only after that: f16 embeddings as a separate measured change.

## References

- [Runtime and storage core](DESIGN-2026-09-19-runtime-core.md)
- [Storage core decisions and numbers](DECISION-2026-09-19-storage-core.md)
- `chitta-field/src/snapshot.rs` — V23 container and `FullSnapshot`
- `chitta-field/src/store.rs` — `peek_memory`, the read path this maps
- `chitta-field/src/store/recall.rs` — `recall_semantic`, the scan this maps
