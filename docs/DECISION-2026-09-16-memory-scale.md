# Decision: memory footprint and the road to one million memories

> Status as of 2026-09-16 (round 2, laptop): **agreed between Fable (orchestrator)
> and Codex gpt-6-astra (two read-only consultations, memos in the appendices).**
> Round 2 adds the laptop constraint, which makes RAM binding and replaces the
> round-1 8–12 GB target with the profile below. This adds Phase 8 to
> `DECISION-2026-09-16-robustness-plan.md`. Nothing here changes the
> snapshot magic; every step keeps the two-family rollback floor.

## Measurements the discussion started from (dandycomp07fl, 2026-09-16)

| Quantity | Value |
|---|---|
| chittad RSS | 6.45 GB at 16:20 CEST, 7.15 GB at 18:10 after a restart (drift to watch) |
| Memories | 135,678 (about 47 KB resident per memory) |
| Counted allocations (`health_check --details true --memory_breakdown true`) | 2.84 GB |
| Largest counted structures | triplets 1.07 GB, embeddings 0.43 GB, keyword 0.34 GB, spans 0.32 GB, HDC 0.28 GB, payload text 0.18 GB, CDAWG 0.09 GB |
| Unattributed remainder | about 2.3 GB (model and contexts, snapshot clones, allocator slack, organs without counters) |
| Dense index in use | Turbo four-bit scan; HNSW is built only above `FLAT_SCAN_MAX` = 2,000,000 (`chitta-field/src/hnsw.rs:57`) |
| Store on disk | 4.7 GB: two 1 GB families, two 214 MB cortex snapshots, 0.8 GB stale `.tmp` |
| Startup | 10 to 18 s cached; 105 s after a SIGTERM mid-snapshot |
| Recall, CLI, live | 0.20 to 0.30 s; prompt-hook fan-out 7.6 s at load average 143 |

## What we agree on

1. **RAM is not the first constraint on this node.** Straight-line growth to 1M
   is about 48 GB on a 1 TB machine. The constraints already failing are
   latency under load and recovery time. Footprint work is justified by copies,
   cache locality, checkpoint peaks and startup, not by capacity.
2. **The accounting exists and is incomplete.** `memory_breakdown` counts
   2.84 GB and calls itself a capacity estimate. Cortex, Turbo internals,
   co-activation and association maps, state children, LSH metadata, code
   intelligence, registries, and the model contexts have no counters. The
   first deliverable is a reconciled census with an explicit residual, not an
   optimization.
3. **Triplets are the largest structure, 2.5 times the embeddings.** Subject,
   predicate and object are owned `String`s duplicated into string-keyed
   indexes. Interning plus compact id arrays is lossless and saves 0.3 to
   0.5 GB now, 2.4 to 3.9 GB at 1M.
4. **Snapshot capture clones the derived indexes** (triplets, keyword, symbols,
   semantic) and the Turbo build materializes all f32 rows; those copies can
   coincide. At 135k this is a 1.5 to 2.5 GB peak; at 1M it would double the
   process at every save. Immutable generation views replace the clones.
5. **Quantizing the resident vectors is not the lever.** f16 saves 0.2 GB now,
   int8 0.3 GB, both with rank-change risk. They come after the lossless
   ownership and layout changes, and only if the census shows a need.
6. **One process, tiered residency.** Turbo stays the only continuously
   resident dense index, exact f32 vectors move to immutable local read caches,
   lexical and keyed indexes stay resident in compact form, and cold sections
   (historical triplets, spans, symbols) decode lazily. V23 already has named
   length-prefixed sections, so section-level deferred decode needs no new
   magic. A DiskANN-style disk graph is a benchmark branch only if the 384 MB
   full scan at 1M cannot meet the latency gate at the measured query rate.
7. **Latency needs one request deadline and visible degradation.** Lanes
   currently mark themselves over budget only after completing; the 50 ms
   embedding deadline silently turns a semantic lane into keyword-only. Every
   lane result carries a status and reason; semantic and hybrid share one dense
   pass; the prompt hook targets 400 ms p95 with a 600 ms ceiling.

## Where we differed, and how it was settled

- Astra ranks snapshot-capture views first by bytes saved per hour because the
  peak is the largest number. Fable orders the census, then the lane budgets,
  then triplets, then capture views: the peak only exists during a save, the
  lane budgets fix a failure users feel on every prompt, and triplets are the
  largest steady-state structure. Astra's own "first execution order" agrees.
- Fable had put retention limits on derived data (spans, CDAWG, event tape)
  near the top. Astra's point stands: those cuts are semantic, not layout, and
  belong to Phase 2's ablation evidence. They are gated on that stream's
  numbers and archived reversibly before any prune.
- Thread-pool sizing is a latency experiment, not a footprint change: 235
  threads touch tens of megabytes of stack, not gigabytes.

## Phase 8, memory and latency budget (after Phase 2 lands)

Each step is one Codex stream with the plan's standard gates (golden band with
the matching configuration, current-truth, SMRITI, restart identity 20/20,
chaos 9/9, contracts unchanged, prompt p95) plus the step's own number.

0. **Census.** Extend `memory_breakdown` to every structure above, report
   logical, capacity, mapped and resident-mapped bytes, dedupe shared
   allocations, and print the residual against RSS. Sample at quiescence,
   after warm recall, during a checkpoint. Gate: residual under 15% of RSS.
1. **Lane budgets and visible degradation.** One cooperative deadline per
   request, shared dense candidates for semantic and hybrid, per-lane status
   and reason in the result, named and capped pools. Gate: prompt-hook p95
   ≤ 400 ms on the replica at the 200-writer stress, zero silent lane loss.
2. **Triplet compaction.** Intern entities and predicates, id arrays and CSR
   postings, exact strings preserved, legacy section read through a
   compatibility path. Gate: ≥ 0.3 GB saved at 135k, identical query results.
3. **Snapshot capture without clones, and shutdown that finishes an in-flight
   save.** Generation-pinned immutable views for capture; SIGTERM waits for the
   running save within the unit's 300 s; committed-checkpoint age capped by
   measured replay cost. Gate: save peak under 1.2 × steady RSS, restart after
   SIGTERM during a save ≤ 20 s.
4. **Keyword and HDC layout.** u32 row ordinals, tf arrays, immutable segments;
   flat HDC rows. Gate: ≥ 0.25 GB saved, byte-identical scores.
5. **Tiered startup.** Section-level deferred decode for cold organs behind a
   dependency audit, mapped immutable generation files, incremental
   verification. Gate: cached start ≤ 5 s on the replica (Phase 6's unmet
   gate), chaos 9/9 including truncated-family cases.
6. **1M qualification.** A frozen synthetic corpus of one million memories with
   representative rare facts, corrections, large realms and code-heavy data;
   the full gate set plus the 8 to 12 GB steady residency envelope and ≤ 14 GB
   checkpoint peak.

Not planned: product quantization as the primary index, deleting derived data
because a panel did not move, and trading the two-family rollback floor for
the 0.8 GB of stale temp files (those are pruned only after manifest
validation proves them unreferenced).


## Round 2: chitta on a laptop

Constraint from the owner: chitta must also run on a laptop. Working envelope:
16 GB RAM shared with an IDE, a browser and the coding agent, 8 cores, NVMe,
no GPU, battery. Targets: chittad resident ≤ 1.5 GB at 135k and ≤ 3 GB at 1M,
cold start to usable recall ≤ 2 s, prompt-hook p95 ≤ 300 ms with the agent
running, background ≤ 1 core average and pausable on battery, disk ≤ 3 ×
resident.

**What changes.** 1.5 GB is a 77% cut from today's 6.45 GB. Interning and layout
fixes cannot get there; the round-1 order ("compact before lazy") is reversed.
The architecture becomes immutable mapped segments plus a bounded heap write
delta, so resident memory is the working set and cold start is a map plus a
header check. Astra's round-1 8–12 GB residency target is withdrawn for laptops.

**Verdicts** (arithmetic and citations in the appendix):

| Option | Verdict | Why |
|---|---|---|
| Zero-decode mapped store (row-major codes, CSR postings, sorted id tables, offset-indexed strings, small heap delta) | adopt | V23 bodies are bincode, so keeping the magic does not keep the bodies: start with optional mapped sidecars keyed by generation and codec, then versioned compact body codecs with dual read and an export path. Resident is the working set, not the file size. |
| Matryoshka truncation of nomic-embed-text-v1.5 to 256-d (four-bit codes 128 B/memory, 17 MB at 135k, 128 MB at 1M) | measure first | Published MTEB loss 1.24 points at 256, 2.94 at 128. Keep 768-d canonical on disk, add a 256-d derived candidate sidecar with its own transform identity plus exact rerank; 128-d rejected as a default. Model dimension, canonical space and search-code dimension become three separate identities. |
| Binary codes with asymmetric scoring and on-disk exact rerank | measure first | The code records that a 400-candidate sign-code prefilter lost golden nDCG 0.712 → 0.331. Saves only 13 MB / 96 MB over four-bit 256-d; not the laptop default. |
| Realm-hot-set residency (rows and postings clustered by realm, global range separate) | adopt locality, measure the saving | Today Turbo scans globally and filters afterwards, so passing realm saves no bytes. Realm-only loading without cold fallback is rejected. |
| Payload text out of the heap (content-addressed zstd blocks, 16–48 MB LRU) | adopt | Transcript files as the only copy rejected: they move and rotate. Hydration today copies content for 60–160 candidates, not just the returned hits. |
| Triplets, spans, event tape, co-activation, code intelligence as one immutable segment layer (LSM-style, dictionary for entities and predicates) | adopt one layer, not six databases | Triplets 90 MB / 400 MB resident windows instead of 1.07 GB / 7.9 GB heap. |
| Crates | stdlib + memmap2 first; turbovec mapped; zstd after a microbenchmark; tantivy, redb/LMDB, usearch measure first; generic HNSW crate rejected | The chitta WAL stays the single authority; a second transactional store would be derived, never co-authoritative. |
| Organ retirement (HDC 283 MB, cortex, CDAWG 90 MB, spans 317 MB, episode HDC) | measure first (Phase 2) | "Not loaded until a consumer asks" is the laptop mode; unmeasured contribution is not zero contribution. |
| Model residency | adopt: one query context, two threads, ~260 MB cap; no resident hintd (1.1 GB); prompt reranker off | A smaller model (MiniLM 384-d) only if nomic misses the 90 ms embedding budget. |
| Background work | adopt: one shared CPU token bucket (1 core on AC, 0 discretionary on battery), heavy passes only on AC and idle, ≤ 20 ms chunks with pause points | The current quiesce flag and load probe know nothing about power or system pressure. |

**Two findings that matter on the node today, not only on a laptop.**
Competitive-weight refresh runs up to 16 extra corpus searches per ordinary
query (`scoring/config.rs:272`, `store/recall.rs:288`): about 886 MB of code
traffic per recall at 135k, 6.5 GB at 1M. That is a strong candidate for the
multi-second prompt lanes seen under load and moves into Phase 8 step 1
(background-only refresh behind a flag). And `realm_list` returns 344 names of
which 200 differ only by outer whitespace: realm scoping is diluted by a data
hygiene bug; `trim_realm_names` exists and runs first.

**Fable's additions, accepted by Astra's framing.** A laptop corpus needs an
admission and archive policy, not deletion: episodes and spans with no recall
in 90 days move to a cold archive segment that stays queryable on demand, so
the working corpus a laptop maps is the tens of thousands that are alive.
Byte quotas with backpressure replace silent growth. Neither the archive nor
the quotas exist yet; both are Stream 5 work.

**Laptop profile** (`CHITTA_PROFILE=laptop`, env-selected, one ProfileConfig
shared by C++ and Rust, node defaults untouched): mapped segments with a
64 MB / 128 MB heap delta, one blocked Turbo image, exact vectors on disk with
a bounded block cache, 2 RPC workers, 2 foreground compute threads, 1 embed
context with 2 threads, 0 document workers on battery, 1 background worker,
hintd and the MCP reranker on demand only, organs lazy. Envelope: 1.21 GB at
135k, 2.75 GB at 1M (per-structure table in the appendix). Disk 3.4 GB / 8.1 GB
only with 256-d canonical vectors, segment sharing between checkpoints and
measured compression; keeping 768-d exact vectors for memories and symbols
adds 3.85 GB at 1M and breaks the 9 GB line. Cold start budget 2.0 s (map and
validate 50 ms, fault hot pages 300 ms, model 900 ms, delta replay 150 ms,
first recall 300 ms, margin 300 ms). Prompt p95 critical path 300 ms
(embedding 90, dense 45, lexical 35 in parallel, fusion 35, hydration 35,
render 40, admission 20, margin 35).

**Order of streams** (replaces the round-1 Phase 8 list; 300–600 engineering
hours plus soak, so a qualified 135k read-serving prototype comes first):

0. Census and contracts: complete `memory_breakdown` (mapped and resident
   pages, model buffers, per-realm counts, refresh count, hydration bytes,
   disk by owner, WAL replay work, background CPU seconds); freeze model and
   transform identity.
1. Power-aware bounded execution: one query context, named bounded pools,
   shared recall preparation, cooperative deadlines with per-lane status,
   foreground refresh off behind a flag, AC/battery/pressure policy.
2. Mapped candidate, payload and BM25 path with lossless parity, optional
   sidecars from an immutable legacy family; legacy opener unchanged.
3. 256-d candidates and binary experiment after the prototypes; separate
   canonical and search identities.
4. Compact canonical tables and incremental checkpoints (triplets, spans,
   code intelligence), versioned body codecs, dual read, export.
5. Organ lazy modes from Phase 2's ablations, the archive tier and quotas,
   disk qualification.

**Three prototypes first, each with the number that decides it:**

1. Query model on a real laptop, one context and two threads: query-embedding
   p95 ≤ 90 ms, or Matryoshka cannot rescue the prompt budget and a smaller
   model enters.
2. 256-d four-bit candidate sweep on a frozen copy (768 vs 256 vs 128 vs
   binary, exact rerank at fixed caps, corrections and rare identifiers
   included): paired lower confidence bound on golden nDCG@20 delta ≥ −0.01.
3. Mapped serving image at 1M under a 3 GB resident cap with two recoverable
   checkpoints and the legacy import bytes counted: disk ≤ min(9 GB, 3 ×
   measured steady RSS) with no authoritative record dropped.

## Appendix A: Astra's round-1 memo (node, verbatim)

## chitta memory and million-memory scale memo — 2026-09-16
To Fable; source checkout cf5800f5; read-only consultation.
[M] Measured = supplied measurements or explicitly identified live diagnostics.
[C] Read-from-code = implementation evidence, cited as repository-relative file:line.
[E] Estimated = arithmetic, assumptions, proposed budgets, or engineering judgment; not a benchmark.
[E] Units below are decimal GB/MB and bytes unless stated otherwise; 135k means N=135,678.
[E] Recommendation: one store process, compact ownership, bounded CPU work, and cold immutable data.
[M] This node has 1 TB RAM; supplied chittad RSS is 6.45 GB and Private_Dirty is 6.29 GB.
[E] Even straight-line 1M growth is 6.45×1,000,000/135,678=47.54 GB, about 4.75% of node RAM.
[M] The already failing constraints are latency (7.6 s prompt hook) and recovery (10–18 s cached, 105 s after interruption).
[E] RAM efficiency matters for copies, cache locality and recovery; raw capacity is not the first failure on this node.
[C] HNSW is skipped below the default 2M flat-scan threshold (chitta-field/src/hnsw.rs:57,730).
[C] Payload embeddings already have a single persistent home in the semantic index (chitta-field/src/store.rs:1264).
[M] A newly collected detailed health diagnostic reports HNSW=0 and 2.839 GB of counted allocation estimates.
[M] Its largest entry is triplets=1.067 GB; embeddings=0.426 GB; spans=0.317 GB; keyword structures=0.342 GB.
[M] That later observation reports RSS=8.054 GB at approximately 135,696 memories, not the earlier 6.45 GB.
[E] These are separate observations; neither timing nor allocation data justifies claiming that the difference is a leak.
[E] Do not fabricate a complete attribution of 6.29 GB: report counted allocations, omitted structures, and an explicit residual.
[C] The measurement surface already partly exists; expose and complete it before optimizing (chitta-field/src/profile.rs:44).
[E] First execution order: bound and expose latency; compact triplets; eliminate snapshot-copy/recovery amplification.

### 1. Where the 6.45 GB goes

[M] Supplied baseline: 135,678 memories; RSS 6.45 GB; Private_Dirty 6.29 GB; 257 threads, including 235 unnamed pool threads; raw 768-dimensional f32 embeddings approximately 0.42 GB. Separate processes consume 1.1 GB hintd, 0.2 GB bridge, 0.1 GB MCP HTTP. Those processes are not components of chittad RSS.

[M] Additional read-only observations during this consultation: health_check reports version 5.72.0, PID 1780992, 135,696 memories, no pending embeddings, eight RPC workers, no queued RPCs. The command health_check --details true --memory_breakdown true --json returned the counters below and RSS 8,054,276,096 bytes. A subsequent detailed health call reported 135,697 memories and 119,387 symbols. This was a moving live store, not a frozen measurement replica.

[C] health_check --memory_breakdown true alone still takes the cheap response: the outer dispatcher tests only details (chitta/src/simple_cli.cpp:1143). With details=true, the dedicated branch returns memory_breakdown before the expensive ordinary detailed health path (chitta/src/handlers/field_system.cpp:101). Thus “nothing reports bytes” describes the original measurement, but is no longer an accurate description of this checkout and live binary.

[E] The following is the strongest available accounting, not a heap profile. “MB at 135k” scales the live capacity counters by 135,678/135,696; rounding makes them practically identical. Bytes/memory uses the observed 135,696 denominator. Projecting by memory count assumes an unchanged mix of text length, triplets, symbols, spans and events, which is especially questionable for code intelligence.

| Structure | Evidence | Estimated B/memory | Estimated MB at 135k | Code ownership / counter basis |
|---|---|---:|---:|---|
| Semantic f32 and map | [M] 425,539,584 B | [E] 3,136 | [E] 425.5 | [C] hnsw.rs:565,659 |
| Payload metadata and text | [M] 176,438,916 B | [E] 1,300 | [E] 176.4 | [C] payload.rs:7; profile.rs:45 |
| State map, shallow only | [M] 65,273,856 B | [E] 481 | [E] 65.3 | [C] profile.rs:55 |
| Keyword postings and lengths | [M] 271,591,172 B | [E] 2,001 | [E] 271.6 | [C] organ/keyword.rs:194 |
| Keyword reverse map and dictionary | [M] 70,422,496 B | [E] 519 | [E] 70.4 | [C] organ/keyword.rs:199 |
| HDC vectors/codebook/realm maps | [M] 282,794,741 B | [E] 2,084 | [E] 282.8 | [C] hdc.rs:210,226 |
| Episode HDC | [M] 7,745,279 B | [E] 57 | [E] 7.7 | [C] hdc.rs:567,640 |
| Triplet entries and derived indexes | [M] 1,067,407,558 B | [E] 7,866 | [E] 1,067.3 | [C] organ/triplet.rs:69,108 |
| Spans and counted indexes | [M] 316,561,273 B | [E] 2,333 | [E] 316.5 | [C] organ/span_store.rs:130,181 |
| CDAWG | [M] 90,391,693 B | [E] 666 | [E] 90.4 | [C] profile.rs:62; organ/cdawg.rs:29 |
| Binary semantic caches, partial | [M] 53,400,128 B | [E] 394 | [E] 53.4 | [C] hnsw.rs:669 |
| Lite encoder, partial | [M] 11,625,943 B | [E] 86 | [E] 11.6 | [C] profile.rs:68 |
| HNSW base/delta/realm graphs | [M] 0 B | [E] 0 | [E] 0 | [C] hnsw.rs:664,730 |
| Counted subtotal | [M] 2,839,192,639 B live | [E] 20,923 | [E] 2,838.8 | [C] profile.rs:75 |

[C] Paths abbreviated in the table are under chitta-field/src. These counters explicitly call themselves capacity_estimate_not_rss (profile.rs:78). HashMap bucket allocation is approximated from capacity (profile.rs:37); mapped file length is charged as allocated embeddings even when pages are not resident (hnsw.rs:663); serialized bitmap size is substituted for heap capacity (profile.rs:66; organ/span_store.rs:190). Therefore subtraction from RSS is a reconciliation aid, not a physically exact accounting identity.

[E] The omitted structures need their own counters. Here is an explicit *working hypothesis*, not additional measurements; it provides allocation-size arithmetic and avoids hiding missing ownership inside a fabricated “model” number:

| Omitted structure | Estimated B/memory; MB at 135k | Arithmetic and code |
|---|---|---|
| Turbo packed + blocked representations, row IDs, scales, rotation | [E] about 796; 108 MB | 2×(768/2)+8+4=780 B/row, plus 768²×4=2.36 MB rotation. [C] turbo.rs:13,41; hnsw.rs:513 |
| LSH/coarse memberships and realm mapping | [E] about 200; 27 MB | Four bucket IDs×8 + four u16 signatures + two coarse memberships, per-row Vec/map/string overhead; 48×768×4=147 KB planes. [C] hnsw.rs:14,595,635 |
| Cortex sparse codes, postings, prototypes and optional PQ | [E] about 2,064; 280 MB | Fully encoded row: 64×(4+4)=512 B code + 64×24≈1,536 B padded postings + map overhead; occupancy/slack unknown. 135,678×2,064≈280 MB. [C] organ/cortex.rs:19,34,286,312 |
| Symbols, code files and call graph | [E] about 3,317; 450 MB scenario | If all 119,387 symbols have 768 floats: 366.76 MB; allow 83 MB strings/maps/edges. Embedding coverage unknown, so 450 MB is a scenario, not a lower bound. [C] organ/symbol.rs:8,33; organ/codefile.rs:23; organ/callgraph.rs:6 |
| Coactivation stats | [E] about 361; 49 MB scenario | Supplied 347k pairs × about 140 B (map entry plus eight allocated context hashes). At 64 hashes, roughly 200 MB instead. [C] field.rs:65,76 |
| Assoc edges | [E] 192; 26 MB scenario | Assume 12 directed edges/memory×16 B, excluding owner-map slack. No count was supplied. [C] field.rs:56,289 |
| State nested retrieval histories | [E] about 405; 55 MB scenario | About half the memories carrying ~800 B of eight context sketches and signature. [C] state.rs:8,18,24 |
| Retrieval surfaces | [E] 200; 27 MB scenario | Assume mean 200 resident bytes per memory; actual coverage and text length unmeasured. [C] field.rs:284 |
| Event tape | [E] about 59; 8 MB scenario | Assume 250k events×32 B; dictionaries extra. [C] organ/event_tape.rs:40,58 |
| Other organs, learners, temporal/artifact/keyed maps | [E] about 590; 80 MB provisional allowance | E.g. plasticity map ~48 B per populated memory≈6.5 MB; temporal entries repeat kind/realm; many registries have independent cardinality. 80 MB is a budget hypothesis, not a fitted estimate of every organ. [C] learner/plasticity.rs:5; learner/mod.rs:15; organ/temporal.rs:7; field.rs:290,307,338,403 |

[E] These hypotheses add approximately 1.11 GB, yielding 2.839+1.11=3.949 GB. Relative to the supplied 6.29 GB Private_Dirty target, the **unattributed remainder is 2.341 GB**, or 17,254 B/memory; 3.949+2.341=6.290 GB. The RSS difference above Private_Dirty is another supplied 0.160 GB. This reconciles the target without claiming to have found the missing allocations. Relative to the later RSS, the corresponding remainder is 8.054−3.949=4.105 GB, including non-private pages. Because the observations are not contemporaneous, neither remainder proves allocator waste.

[C] Plausible owners of the residual include model/context buffers, snapshot clones and serialization buffers, turbo construction and retired Arc generations, queue/request data, and allocator-retained pages. One llama_model is shared among up to 16 contexts, default four (chitta/include/chitta/vak_llama.hpp:85,95,143,175); model/context byte sizes are not reported. The embed queue holds eight reads, default 64 writes and 512 cached vectors (chitta/include/chitta/embed_queue.hpp:36). Queue cap and worker count are independently configurable.

[E] A 512×768×4 embedding cache is only 1.57 MB; two such cache layers would be ~3.15 MB plus keys. It cannot explain gigabytes. Likewise 235 threads×8 MiB would be 1.97 GB of *virtual stack reservation*, not demonstrated RSS. If each touched 256 KiB, stack RSS would be only 61.6 MB. Do not present thread count times default stack reservation as private resident memory.

[C] Definite duplication:
- f32 semantic vectors coexist with four-bit Turbo; binary codes coexist in a HashMap and a flat vector (hnsw.rs:567,611,615,646). Turbo itself retains packed and blocked forms (turbo.rs:17,23).
- Triplet subject/predicate/object Strings coexist with String-keyed indexes; source_file already uses Arc<str> (organ/triplet.rs:16,39,89). Symbols copy kind/name/path into dedup keys and names into the name index (organ/symbol.rs:52,79).
- Sparse feature IDs/activations coexist with their inverted postings; memory kind is duplicated again (organ/cortex.rs:314,320,324). Keyword terms have a postings String and a separate interned name, but per-document reverse terms are already u32 IDs, not strings (organ/keyword.rs:22,27,161).
- Payload content is copied during snapshot capture (store/maintenance.rs:1064). Startup reads the entire .pld into a byte buffer and then copies every payload slice (snapshot.rs:1514,1527). RecallHit owns another content String for returned hits (recall.rs:4), a bounded query copy rather than an entire permanent corpus copy.
- Snapshot capture clones triplets, keyword, symbols and semantic index (store/maintenance.rs:1106); turbo build materializes N×768 floats (hnsw.rs:1311). These can coincide.
- The persistent payload f32 copy was already removed, including hydration on startup (store.rs:1264; field/opening.rs:682). Do not count it twice or claim its removal as new savings.

[M/E] Already-realized optimizations must not be sold again: the diagnostic's modeled legacy keyword reverse size is 706.711 MB versus current 70.422 MB, a modeled difference of 636.289 MB; episode HDC is 54.902 MB legacy estimate versus 7.745 MB current, a modeled difference of 47.157 MB. These are counterfactual estimates returned by the live code, not measured before/after RSS. [C] organ/keyword.rs:204; hdc.rs:568,640.

### 2. What breaks first at one million, in order

[E] Use R=1,000,000/135,678=7.3704. There is no supplied per-lane CPU profile, disk bandwidth measurement, HNSW benchmark, or count of triplets/events. Wall-time extrapolations below are scenarios, not performance promises.

1. **The prompt deadline and CPU contention are already broken.** [M] CLI recall is 0.20–0.30 s under load, but prompt semantic/hybrid/keyword lanes are 3.3/6.1/5.1 s, total 7.6 s versus ~0.9 s idle replica. Load average is 143 on 96 cores; bounded-write stress p95 is 114–163 ms against 150 ms. [E] Load average does not distinguish runnable CPU from uninterruptible I/O, and different queries/realms/cache states prevent attributing the whole CLI/hook gap to one mutex. Still, 6.1−0.3=5.8 s of excess wall time is a much larger immediate problem than saving 0.21 GB with f16. Multiplying full measured latency by R gives a warning scenario of 1.47–2.21 s CLI and 56 s hook, **not** a prediction; only the corpus-dependent CPU component should scale that way.

2. **Full scans and duplicated lane work scale before RAM capacity does.** [C] Default semantic retrieval uses Turbo below 2M; the returned stable-row scores are quantized scores, not a subsequent full f32 rerank (hnsw.rs:1643,1665,1676,1693). [E] One four-bit pass touches at least N×384 bytes: 52.10 MB now, 384 MB at 1M. Two redundant dense lanes move at least 104.2/768 MB; a distinct context lane raises this to 156.3 MB/1.152 GB. At assumed effective bandwidth 5–20 GB/s, one 1M pass has a 19–77 ms data-movement floor before scoring, scheduling and filtering. Three passes alone consume 58–230 ms of shared bandwidth budget.

3. **Startup/recovery misses its gate by a widening margin.** [M] Cached startup 10–18 s; snapshot decode alone 4–5 s. [E] Linear unchanged-format decode at 1M is 29.5–36.9 s; whole cached startup 73.7–132.7 s if all phases scaled, an intentionally pessimistic scenario. The 5 s gate requires at least 6–7× less eager decode work, not just more decode threads. [C] Current V23 loader maps input but eagerly deserializes all section bodies into owned allocations (snapshot.rs:1658,1675).

4. **Snapshot copies, save duration, and crash replay amplify each other.** [M] Two ~1 GB families plus two 214 MB cortex snapshots; 17 stale .tmp files total ~0.8 GB; all disk files 4.7 GB. [E] Unchanged content mix gives a 7.37 GB family plus 1.577 GB cortex, two committed generations≈17.90 GB, one building generation another ~8.95 GB. Entire 4.7 GB×R=34.64 GB includes unrelated/stale components and is not a justified required steady-state disk size. Saving ~8.95 GB at an assumed 100–500 MB/s effective serialization/write/fsync throughput takes 18–90 s, versus 2.4–12.1 s for 1.214 GB now; measure actual NFS throughput. [C] Snapshot cloning currently happens before sidecar dirty-skip decisions (store/maintenance.rs:1106,1187).
   
   [M] Interrupted-save startup is 105 s with 32 s WAL replay plus 41 s normalize. [E] WAL recovery scales with operations since the last *committed* checkpoint, not total corpus N. At unchanged write rate/age, 32 s need not become 236 s. If backlog grows by R, replay≈236 s and full normalize≈302 s; with 29–37 s decode, recovery exceeds 9 minutes. Snapshot cancellation plus two-hour checkpoint age is the defect, not “WAL is large” after the supplied fresh snapshot where WAL≈0. Budget WAL by measured replay work/bytes, retain durable coverage, and make SIGTERM finish or safely abandon only the uncommitted generation.

5. **Derived rebuilds become a CPU storm.** [C] LSH has 4×12 projections (hnsw.rs:20,2731), and optional full coarse reconstruction tests 256 centroids per vector (hnsw.rs:14,2653,2738). [E] LSH requires 135,678×48×768≈5.00 billion coordinate multiply-adds; at 1M, 36.864 billion. At assumed aggregate 10–50 billion coordinate MAC/s, pure projection cost is 0.10–0.50 s now, 0.74–3.69 s at 1M, excluding allocation, normalization, hashing and contention. Coarse reassignment costs another 26.68/196.61 billion coordinate MACs. The measured 41 s normalize cannot be assigned entirely to LSH.
   
   [C] Turbo loads packed bytes and repacks into blocked rows; build copies all f32 input and calls dependency quantization (turbo.rs:27,41; hnsw.rs:1311,3284). [E] Merely copying a 3.072 GB input is 0.15–0.61 s at 5–20 GB/s; building involves more work. If the dependency applies a dense 768×768 rotation per vector, arithmetic is N×768²≈80.0 billion/589.8 billion MACs, or 1.6–8 s / 11.8–59 s at the same assumed MAC rate. That rotation complexity is a hypothesis requiring dependency profiling, not proven by the wrapper. Use T_build(1M)≈7.37×T_build(135k) only after measuring the isolated phase. Event-organ rebuild follows event/entity cardinality, not memory count (field/opening.rs:953).

6. **Keyword and triplet indexes dominate expanding non-vector memory and random accesses.** [M/E] Current keyword allocations 342.014 MB project to 2.52 GB; triplets 1.067 GB to 7.87 GB; spans 316.561 MB to 2.33 GB. [C] BM25 scores posting lists into a per-query HashMap, retaining at most four query terms and normally excluding common terms above 20% corpus frequency (organ/keyword.rs:138,211). [E] Four retained 20%-frequency terms imply up to 0.8N postings: 108,542 now and 800,000 at 1M. The exemption for the rarest selected term means this is not a hard bound; all-document terms can still traverse N or more postings. Keyword reindex removes by scanning each relevant posting vector (organ/keyword.rs:95). Sorted compact postings, deletion bitmaps and exact top-k pruning are more useful than another embedding compression scheme for this path.

7. **Coactivation's apparent cap is not a strict live limit.** [C] It is pruned on load (field.rs:827) and on the snapshot clone (store/maintenance.rs:1113), not by an always-on admission bound; 64 context hashes are retained per pair (field.rs:76). [M/E] 347k pairs means 2.558 pairs/memory, projecting to 2.558M pairs. At 140 B/pair, ~49 MB now/~358 MB then; with ~584 B/pair at full context capacity, ~203 MB/~1.49 GB. A strict undirected degree-20 cap would bound pairs to 20N/2=10N: 1.357M/10M pairs, still 0.19–0.79 GB /1.4–5.84 GB depending on context capacity. Pruning itself builds endpoint lists and a removal set (field.rs:104), so it also needs a peak-memory budget. Association-edge growth is separate from this cap.

8. **HNSW is a conditional risk, not the current 1M bottleneck.** [M] Live HNSW=0. [C] Build gate is N>flat_scan_max, default 2M; sidecar skip defaults on (hnsw.rs:57,82,730). [E] At 1M unchanged configuration, HNSW build/insert cost remains zero. If forced on, average graph edge storage alone≈(32+16/15)×8=264.5 B/node; maps/layers/slack make roughly 350–600 B/node: 47–81 MB now, 0.35–0.60 GB at 1M, potentially doubled by realm graphs. This excludes embeddings.
   
   [C] Forced bulk construction allocates 17 SmallVec<[u32;32]> mutex cells per row, 24 workers, 3,000-row epochs, an f32 arena then int8 arena, and repeatedly rebuilds CSR snapshots (hnsw.rs:809,840,852,864,979). [E] At an assumed 152–160 B per mutex/SmallVec cell, adjacency scratch alone is 2,584–2,720 B/node, ~351–369 MB /2.58–2.72 GB. f32→int8 overlap adds 3,840 B/node, ~521 MB/3.84 GB at its peak; these phases need not all peak simultaneously. Measure size_of rather than trust the ABI assumption. Per-worker visited arrays add O(workers×N).
   
   [E] Conventional N log N build work grows R×ln(1M)/ln(135,678)=8.62×; per-insert search grows ~1.17× at equal graph quality. If an insert actually evaluates 200×32=6,400 candidate distances, that is ~4.92M coordinate MACs/insert; at 0.5–2G MAC/s/core, pure compute is 2.5–9.8 ms before heap/lock costs. ef_construction=200 is not a bound on distance evaluations, so benchmark this hypothetical. Worse, rebuilding O(N) CSR after each 3k epoch can contribute O(N×delta/3000); a full rebuild's such work grows R²≈54.3×. Do not turn HNSW on as an unmeasured “scale fix.”

9. **Capacity is last on this machine, though waste is substantial.** [E] 47,539 resident B/memory×1M≈47.54 GB. Counted allocations alone project to 20.92 GB; treating all residual/model cost as per-memory overstates growth if fixed, and understates event growth if it outpaces memories. Later 8.054 GB would project to ~59.36 GB. Neither reaches 1 TB, but both create unnecessary serialization, cache and peak-copy costs. [M] The other processes add 1.4 GB today, not R×1.4 GB.

### 3. Footprint plan, ranked by bytes saved per engineering hour

[E] Engineering hours are focused implementation/review estimates, excluding required soaks and evaluation elapsed time. Savings are alternatives and overlap; do not add all rows. “Peak” savings cannot be booked as steady-state RSS savings. The ranking uses the central expected *allocation* saving per hour at 135k; risk and latency priority can override it.

[E] Gates used below: G = golden nDCG@20 within the separately calibrated configuration band plus a predeclared equivalence margin; T = current-truth ≥40/50 including holdout and 5/5 incident probes; S = no SMRITI regression on the pinned panel; I = ordered restart and within-process identity, 20/20 across three restarts with pinned clock and complete embeddings; C = existing chaos suite 9/9, including corrupt/missing sidecars, crash/disk-full and durable-prefix assertions; P = recall p95≤150 ms during 200 concurrent writes and the hook budgets in §5; U = cached usable-recall startup≤5 s. [C] The plan explicitly calls the 2-SD band descriptive, not an equivalence test, and records the temporary “no worse than baseline” identity exception (docs/DECISION-2026-09-16-robustness-plan.md:35,73). An unmet baseline is not a passing gate; do not label a change scale-qualified while identity/degradation remains unresolved.

| Rank / change | Estimated savings now / 1M | Estimated cost and allocation MB/hour now | Risk, gate and rollback |
|---|---|---|---|
| 1. Capture immutable snapshot views; avoid cloning derived indexes; stream sections | [E] ~1.5–2.5 GB peak /11–18 GB peak. Existing triplet+semantic+keyword+payload clones alone sum ~2.01 GB before symbols. | [E] 24–48 h; central 2,000/36≈56 MB/h peak | [E] No intended rank change; consistency risk high. Gates I,C,U,P plus acknowledged-write coverage. Roll back capture implementation while retaining V23 writer/reader. |
| 2. Intern triplet entities/predicates and use compact row IDs/CSR postings | [E] 30–50%×1.067=0.32–0.53 GB /2.36–3.93 GB under proportional mix | [E] 16–32 h; 427/24≈18 MB/h | [E] Exact IDs/strings and historical queries preserved: G,T,S,I,C,P. Keep legacy section encoding through a compatibility DTO; feature switch reconstructs old maps. |
| 3. Flatten HDC IDs/vector rows and eliminate HashMap bucket amplification | [E] From 282.8 MB to ~142–180 MB: 103–141 MB /0.76–1.04 GB if occupancy/codebook mix scales | [E] 8–16 h; 120/12≈10 MB/h | [E] Bit-identical Hamming, stable tie-breaks; G,S,I and HDC/API fixtures. Sidecar version or derived-only layout; rebuild old representation on rollback. |
| 4. f16 authoritative in-memory vector rows; drop f32 heap copy | [E] 768×2×N=208.4 MB /1.536 GB, plus optional map-allocation reduction | [E] 16–32 h; 208/24≈8.7 MB/h | [E] Quantized ranks can change. G,T,S,I,P; identity is against this configuration across restart, not claimed identical to f32. Keep exact f32 durable backing for rollback/rebuild; codec not silently overwritten. |
| 5. int8 authoritative rows with per-vector or per-block scale | [E] (3072−772)×N=312.1 MB /2.300 GB | [E] 24–48 h; 312/36≈8.7 MB/h | [E] More quality risk than f16. Same gates, rare-entity/near-tie strata, training/calibration determinism. Rebuild from retained exact vectors. Alternative to rank 4, not additive. |
| 6. Compact BM25 postings and reverse lists using u32 row ordinals, tf arrays, immutable segments | [E] About 35–55%×342=120–188 MB /0.88–1.39 GB | [E] 24–40 h; 154/32≈4.8 MB/h | [E] Preserve exact tf, df, document length and tie order. G,T,S,I,P; keyword fixtures. Dual-read old index or rebuild from payloads; no truncation of “common” factual terms. |
| 7. One resident blocked Turbo representation | [E] N×384=52.1 MB /384 MB; keep row scales/IDs | [E] 8–16 h; 52/12≈4.3 MB/h | [E] Exact kernel outputs if packing identical; G,I,P and snapshot-cache parity. Keep packed canonical sidecar, reconstruct only for write on cold path or version a blocked sidecar. |
| 8. Payload compression/dictionary coding and compact metadata interning | [E] Hypothesis: 0.7 KB text/memory×50%=47.5 MB; +16 MB repeated metadata≈64 MB /0.47 GB | [E] 16–32 h; 64/24≈2.7 MB/h | [E] Exact decompression, no lossy rewriting. G,T,S,I,C,P, byte-for-byte content/API tests. .pld stays canonical during transition; optional compressed offset-index sidecar. Current entire payload allocation is only 176 MB, so no GB-scale saving is credible here. |
| 9. Retention-bounded derived data | [E] Example 25%×(316.6 spans+90.4 CDAWG+7.7 episode HDC+8 tape)=105.7 MB /0.78 GB if mix scales; coactivation separate | [E] 24–48 h; 106/36≈2.9 MB/h, uncertain eligibility | [E] Highest semantic/history risk; G,T,S plus each organ's own consumer/API fixtures and I,C. Archive reversibly before pruning; never delete merely because recall panel is unchanged. |
| 10. Product quantization as primary candidate codes | [E] f32→32 B codes saves (3072−32)×N=412.5 MB /3.040 GB, less ~0.79 MB codebook; extra ~0.104/0.768 GB only if both current Turbo copies also replaced | [E] 48–96 h; 412/72≈5.7 MB/h before evaluation/training | [E] Lower execution priority despite rate: larger recall risk/new search implementation. G,T,S,I,P, candidate recall and out-of-distribution strata. Shadow codes, retain exact sidecar, rollback index selector. |
| 11. Lazy cold structures and bounded caches | [E] Example cold 70% of 1.067 GB triplets+0.450 GB symbols+0.317 GB spans≈1.284 GB /9.46 GB; clean mapped pages may still remain RSS | [E] 48–96 h; 1,284/72≈17.8 MB/h conditional | [E] High dependency/startup complexity puts it after compact representations despite favorable theoretical ratio. G,T,S,I,C,U,P including cold-cache recalls. Roll back to eager V23 load; retain untouched sections. |

[E] Ranks 9–11 are risk-adjusted rather than a dishonest exact bytes/hour ordering: pure arithmetic would move lazy loading near rank 2 and PQ above BM25, but their unknown working set and quality work invalidate that precision. Measure eligible cold bytes and accepted PQ quality before promoting them.

[C] Existing PQ is 32 sub-vectors with 256 centroids, used for cortical *residuals*, not a ready replacement global semantic index (organ/pq.rs:4,12; organ/cortex.rs:327). [E] An ADC query would perform about N×32 code lookups: 4.34M/32M, but its candidate quality must be measured. Reusing its type does not supply global-index training, centering, filtering, delta handling or exact rescoring.

[C] The raw Turbo path currently returns quantized stable-row scores; dirty rows use f32 dot products (hnsw.rs:1676,1681). Other routes use get_embedding, centered cosine, HNSW distances, centroid refresh and reconstruction (hnsw.rs:118,1245,1712). [E] Dropping f32 loses exact rescoring for those consumers, a stable source for re-quantization, and the ability to distinguish close vectors after rounding. f16/int8/PQ are not “free 2×/4×/96×” changes. Preserve canonical exact f32 on disk, use a bounded exact-row cache for final candidate rerank, and budget its I/O. At 1M, 200 candidates×3072≈0.614 MB/query, but random latency matters more than bytes. If exact f32 is permanently discarded, say explicitly that rollback to old exact behavior is impossible without re-embedding; I do not recommend that first.

[E] Thread/context sizing is a separate low-cost latency experiment: 4–8 h, try one query worker plus one or two document workers, 2–4 model contexts, 2–4 threads/context, one bounded 4–8-thread background budget and a fixed RPC limit. These are trial settings, not optimal settings inferred from 96 cores. Reducing 235 to 32 auxiliary threads saves only (235−32)×64–256 KiB≈13–53 MB of touched stacks, plus an unknown allocator/context effect; fixed with N. Four-to-two contexts saves 2×measured_context_bytes, presently unmeasured. Gates P, query admission/embedding coverage, write throughput, and U; rollback env settings. [C] RPC starts at 8/max16, OpenBLAS is explicitly set to four, refresh pool caps at 16, HNSW build pool at 24 (chitta/src/simple_cli.cpp:264,1529; chitta-field/src/hnsw.rs:531,814). Eight live RPC workers do not explain 235 unnamed workers. Name/profile Rayon, llama/ggml and BLAS pools before attribution.

[E] Retention rules: cap coactivation on *live admission/drain*, with deterministic tie-breaks and a persisted retirement record, preserving useful context-diversity behavior; consider compact unique hash sets only after measuring the 64-hash occupancy distribution. Keep spans referenced by live memories, extant transcript locators, corrections or current task artifacts; evict decoded cold text before deleting evidence. Event retention should preserve failure/correction/decision/provenance anchors and archive full sequences before retaining summaries or recent windows. [C] Span liveness depends on locators or memory references (organ/span_store.rs:79); event compaction rebuilds the vector (organ/event_tape.rs:229), so a memory-saving pass can itself peak. [E] Age alone is not authority to erase a rare correction or falsify historical/current-truth queries.

[E] Stale .tmp cleanup saves the supplied 0.8 GB **disk**, zero established RSS. Only remove provably unreferenced abandoned-generation temporaries, under the existing store ownership protocol, after manifest validation. Never trade away the two-family rollback floor for this small saving.

### 4. Scale design: one process with tiered residency

[E] Recommend **one process, smarter memory**, with Turbo as the sole continuously resident *dense* search index, exact f32 in immutable local-read-cache files backed by the durable store, and compact all-corpus lexical/keyed indexes. “Only Turbo resident” must not mean deleting BM25, provenance/current-truth maps, state, or historical evidence. A disk graph is a later measured option if scan traffic exceeds the aggregate CPU/bandwidth budget; it is not justified by 1M×384 B=384 MB of dense codes on a 1 TB node.

[E] Keep all-corpus candidate coverage: Turbo codes for every live embedded memory; BM25 postings/dictionary/doc lengths; compact state/status/supersession/source-version maps; sparse cortical postings if their ablation justifies them; keyed corrections/provenance/task state. Do not equate cold with excluded. Keep payload text, raw exact vectors, inactive graph neighborhoods, historical event data and most code-intelligence details cold behind bounded caches. A cold candidate still participates in global ranking and can be hydrated; a timeout must report incomplete retrieval.

[E] An explicit 1M target envelope, not an achieved result:
- Dense blocked Turbo+IDs/scales/rotation ~0.40 GB: 384+8+4+2.36 MB.
- Compressed lexical index ~1.1 GB: reduce projected 2.52 GB by ~56%.
- Compact cortical postings/codes ~1.2 GB: reduce modeled ~2.06 GB by ~42%.
- State, row-ID maps, current-source/supersession/keyed maps ~1.0 GB.
- Active triplet adjacency/dictionary cache ~1.5 GB; preserve cold historical access.
- Full HDC vectors, if its consumers require them, ~1.1 GB: 1,000,000×1024 plus IDs/codebook; do not silently substitute a hot-only HDC corpus.
- Payload/span cache ~0.5 GB; exact-vector cache ~0.5 GB; symbol/code cache ~0.5 GB.
- Model, bounded contexts/threads, small organs and allocator reserve ~2.0 GB, to be replaced by measurement.
Sum ~9.8 GB. A reasonable planning band is 8–12 GB steady resident with ≤14 GB checkpoint peak, subject to measured model size, graph working set and quality gates. This is a residency policy target, not a guarantee of 10 GB saved from an unknown residual.

[C] V23 already contains named length-prefixed sections, allows unknown sections, and has an indexable section table (snapshot.rs:73,1377). It currently decodes every section, so mmap input is not lazy owned state (snapshot.rs:1663,1675). [E] Section-level deferred decode is compatible with **unchanged V23 magic and unchanged bodies**. Retain the immutable file handle/mapping and a validated section directory, decode selected sections on demand, and serialize unmodified unloaded bodies by copying original bytes. Otherwise a save risks replacing an unloaded organ with its default and destroying it.

[E] Dependency audit comes first: WAL may mutate an unloaded section; eager query state can depend on historical organs; current-truth and deletion filtering cannot wait for asynchronous repair. Provide typed lazy access that loads on first mutation, or a bounded section delta journal with a tested merge. A whole triplet section is too coarse for per-entity low-latency access; add an optional versioned dictionary/CSR sidecar for random access while preserving the canonical legacy triplet section. Likewise add payload offsets and compressed blocks in a new sidecar. Do not silently reinterpret positional bincode fields. Later canonical-layout changes need dual readers, a versioned section name/body, a migration command, export to the rollback format and a retained pre-migration generation.

[C] The current embedding mmap path activates only above 500k, after loading embeddings into heap (hnsw.rs:95; field/opening.rs:677,722). Some comments still say 200k or associate mmap with binary-only retrieval; actual flat threshold is 2M. [E] Do not mistake activation for zero-copy startup. Map verified immutable generation files directly, preserve concurrent WAL updates in a delta, and pin the generation until all readers drop it. Unlinking an open mapping on ordinary local POSIX filesystems is not itself invalidation; in-place truncation/rewrite, remote/NFS lifetime behavior and stale-file handling are the issues to test. The current snapshot loader itself relies on immutable replacement (snapshot.rs:1658). Use node-local verified read caches where appropriate without moving the authoritative NFS writer-fencing lock.

[E] Cached usable-recall startup budget: ≤0.3 s manifest/directory/header validation; ≤1.2 s map and fault hot candidate indexes; ≤1.0 s eager critical state/IDs; ≤0.8 s bounded WAL delta apply; ≤1.2 s model/query readiness; ≤0.5 s margin =5.0 s. These are allocations of a target, not measured phase times. If model initialization cannot fit 1.2 s, use the measured value and overlap safe phases or retain a separately managed embedder; merely opening a socket is not readiness. Background organs must expose readiness state. Missing hot sidecars must produce a visible degraded mode or a longer recovery SLO; do not claim 5 s fully qualified startup while silently losing semantic lanes.

[E] Avoid reading and hashing the entire ~9 GB generation on every cached boot: at 200 MB/s that alone takes 45 s. Use immutable generation identity, per-section/block checksums, incremental verification, and independently tested validation of critical sections; cold sections must be verified before use. Integrity and durable-prefix semantics must survive corrupt-cache and truncated-family chaos tests.

[E] A DiskANN-style disk graph introduces candidate-path random I/O, cache/graph construction, update/compaction and durability work. Under this node's shared/NFS setting it would require a qualified local SSD placement and p95 I/O evidence. Keep it as a branch to benchmark only if the 384 MB full scan cannot meet P at actual simultaneous query rate. At assumed 10 scans/s, codes alone demand 3.84 GB/s; at 100 scans/s, 38.4 GB/s. Admission control and shared candidate computation could avoid that threshold without changing the retriever. Quantized Turbo alone plus cold exact candidates is the smaller first architectural step and retains the existing well-tested global candidate coverage.

### 5. Latency under load: budgets, CPU work and visible degradation

[C] The current 50 ms query deadline returns an empty vector for full queue or wait timeout (chitta/include/chitta/embed_queue.hpp:63,91,100; chitta/include/chitta/rpc/field_handler.hpp:770). The lane aggregator launches std::async per lane and waits future.get() for every lane; it marks over-budget only after completion (chitta/src/handlers/field_memory_recall.cpp:1786,1800,1807). [E] A nominal lane budget is not a cancellation deadline. A shell timeout also cannot be presumed to cancel already-running daemon work.

[E] Use one request deadline and shared query preparation/candidate work. Preserve each lane's scoring and fusion semantics, deduplicate only demonstrably identical query/model/realm/filter/clock/no_learn work. A semantic lane and a hybrid lane can share dense candidates while hybrid adds BM25/graph evidence; a context query with different text cannot reuse that embedding. Route background rebuilds through separate bounded CPU admission, and make cancellation cooperative inside scan/posting loops. Do not detach unbounded futures that keep burning CPU after the hook returns.

[E] Proposed 400 ms end-to-end p95 allocation, with a 600 ms absolute interactive ceiling until measured qualification:
- transport/admission ≤20 ms;
- shared embedding ≤100 ms including queue wait, query priority and coalescing;
- keyword lane ≤60 ms from admission, independent of embedding;
- dense semantic compute ≤100 ms after embedding; semantic lane ready by ~220 ms;
- hybrid incremental fusion/graph/rerank ≤60 ms after shared candidates; ready by ~280 ms;
- distinct context lane ≤180 ms total when capacity permits; keyed correction/provenance/task lookup ≤20 ms;
- payload hydration and final assembly ≤50 ms; hook render/ledger overhead ≤50 ms; ~20 ms scheduling margin.
Parallel stages overlap: do not add every lane's budget as a serial sum. At 1M, the ≤100 ms dense budget leaves only 23 ms after a 77 ms bandwidth floor in the pessimistic 5 GB/s case; if measured contention exceeds it, reduce repeated scans and admit fewer concurrent requests, rather than declaring parity after lane loss.

[E] Every result should carry per-lane {status: complete|empty|degraded|timeout|not_ready|skipped, reason, elapsed_ms, queue_ms, cpu_ms, candidates, embedding_status, index_generation}. Reasons distinguish embed_queue_full, embed_deadline, index_not_ready, cold_io_deadline and explicit budget admission. Keyword-only fallback from an intended semantic lane must say degraded with embedding_status=missing; an empty complete search is different. Preserve text-contract compatibility by carrying status in structured metadata and a controlled hook marker. Do not let skipped/failed lanes update confidence as though they supplied negative evidence. Expose counts and quality conditional on embedding completeness; canary success requires complete semantic coverage for the quality panel.

[E] CPU/service cost model (numbers are estimates; supplied 0.20–0.30 s recall is end-to-end wall time, not CPU):
| Lane/work | Estimated work at 135k | Estimated work at 1M | Budget implication / code |
|---|---|---|---|
| Dense Turbo pass | 104.2M coordinates, ≥52.1 MB codes | 768M coordinates, ≥384 MB | At assumed measured-equivalent kernel rate 10–30G coordinates/s/core: 3.5–10.4 /25.6–76.8 core-ms, plus memory and top-k. Do not add overlapping memory/compute lower bounds. [C] turbo.rs:64; hnsw.rs:1665 |
| Exact full f32 scan fallback | 104.2M MACs, 416.8 MB | 768M MACs, 3.072 GB | At 5–20 GB/s: 20.8–83.4 /153.6–614.4 ms data floor. Cold Turbo fallback cannot be treated as equivalent latency. [C] hnsw.rs:1703 |
| BM25, illustrative four 20%-DF lists | 108.5k posting visits | 800k visits | At assumed 10–30M visits/s/core: 3.6–10.9 /26.7–80 core-ms. Common-term exception can be worse; cache misses/hash allocations dominate. [C] organ/keyword.rs:138,231 |
| Hybrid | One dense + BM25 + graph/rerank | Same components with scaled corpus work | Without sharing roughly 7–22 /52–157 core-ms before graph and payload costs; shared dense candidates remove its duplicate dense pass. [C] chitta/src/handlers/field_memory_recall.cpp:1498,1508 |
| Cortex if used | Uniform-code assumption: 64×N×64/16384≈33.9k posting visits | ~250k visits | ~1.1–3.4 /8.3–25 core-ms at 10–30M visits/s, plus prototype lookup/encoding; skew can be much worse. [C] organ/cortex.rs:34,464 |
| HDC if requested | N×128=17.37M word comparisons | 128M comparisons | At assumed 0.5–2G word comparisons/s: 8.7–34.7 /64–256 core-ms, plus top-k/sort. Not a free fallback lane. [C] hdc.rs:25,309 |
| Keyed lanes | O(query tokens)+matched postings | Same unless a trigger has many matches | Target <20 ms; correction postings are explicitly unbounded. [C] field.rs:342,362 |
| Span lane | Trigram union plus candidate text scoring | Depends on span and trigram cardinality | Record candidates and postings touched; not justified to claim O(log N). [C] organ/span_store.rs:723 |

[E] Query embedding cost primarily follows input tokens/model/context contention, not N. Graph expansion follows visited edges and branching, not N alone; impose work limits with visible truncation. Symbol semantic search separately scans symbol vectors and sorts scored results (organ/symbol.rs:214,241); at the measured 119,387 symbols it can read another 366.8 MB if all are embedded, and at proportional 1M-memory code mix ~2.70 GB. Profile these advanced lanes separately rather than charging them to ordinary prompt recall.

[E] To qualify latency, record CPU time and queue/lock/embedding/search/hydration/render wall time for identical queries and realms at fixed load; report p50/p95/p99, throughput, cancellation lag, completed-lane coverage, timeout rate, cold-cache behavior and maintenance overlap. A faster p95 caused by dropping the hard lanes is a failed quality test. Reproduce the 200-writer stress, then prompt fan-out under the supplied shared-node load condition; an idle replica median does not certify production p95.

### 6. The measurement first, then three changes

[E] Add one measurement product before any optimization: **a reconciled per-structure memory census**, sampled at quiescence, after warm recall, during checkpoint/rebuild, and after completion. Extend the existing implementation rather than create a competing one. It must report cardinality, logical bytes, capacity bytes, mapped bytes, optional resident mapped pages, shared-allocation ownership, generation, and high-water transient allocation separately. Also include model/context bytes from native APIs or instrumentation, worker counts by pool, touched stack/allocator statistics where available, and an explicit unattributed RSS residual.

[C] Existing useful base: Rust memory_breakdown (profile.rs:44), FFI cf_memory_breakdown (ffi.rs:116), C++ field_store wrapper (chitta/include/chitta/field_store.hpp:2211), and dedicated health branch (chitta/src/handlers/field_system.cpp:104). Missing: full Cortex, Turbo internals, coactivation/assoc, state children, LSH/coarse metadata, code intelligence, many registries, allocator/native model accounting. Current map estimator and mmap/bitmap approximations must be labelled, not silently promoted to exact resident bytes.

[E] Proposed Rust surface, preserving the cheap health fast path:

    struct StructureBytes {
        name: &'static str,
        items: u64,
        logical_bytes: u64,
        owned_capacity_bytes: u64,
        shared_capacity_bytes: u64, // attributed once by allocation identity
        mapped_bytes: u64,          // not automatically resident
        resident_mapped_bytes: Option<u64>,
        transient_live_bytes: u64,
        transient_peak_bytes: u64,
        estimate_kind: EstimateKind,
    }
    trait AccountBytes {
        fn account_bytes(&self, sink: &mut ByteSink);
    }
    impl ChittaField {
        fn sample_memory(&self, mode: SampleMode) -> MemoryCensus;
        fn cached_memory_census(&self) -> Arc<MemoryCensus>;
    }

[E] ByteSink deduplicates Arc-owned buffers, recursively visits Vec/String capacities, and records allocation assumptions for HashMap/BTreeMap/bitmap internals. Capture one organ lock at a time; mark a non-atomic census with start/end mutation epochs. Cache the result asynchronously with sampled_at and duration; fast health returns the cached summary and staleness, explicit detailed health triggers a bounded sample. A 0.5 s diagnostic on the live store must not become a synchronous health poll every few seconds. Track temporary owners with RAII counters during clone, decode, hydration, Turbo build and Arc retirement. Compare process RSS/PSS/private/file-backed categories separately from allocator allocated/active/resident statistics; do not force them to equal.

[E] **First change: bound CPU admission and make lane failure visible (8–24 h).** Replace completion-only lane budgets with propagated cooperative deadlines; share equivalent dense/BM25 work; instrument queues/locks; name and cap pools through the measured sweep. Expected work reduction for semantic+hybrid sharing: one dense pass saved, 52 MB/query now and 384 MB at 1M, plus duplicate BM25 where equivalent. Touched-stack saving scenario 13–53 MB, not gigabytes. Target prompt p95≤400 ms with a 600 ms ceiling and complete quality-panel embeddings; maintain recall p95≤150 ms during 200 writes. Current 7.6 s versus 0.9 s idle does not justify promising those targets without the queue profile. Gates G,T,S,I,P; rollback scheduler/consolidation flags, retain observability and explicit degradation. This ranks first because latency already fails, even though it is not the largest bytes/hour patch.

[E] **Second change: compact triplet ownership (16–32 h).** It is the largest measured counted structure, 1.067 GB, about 2.5× semantic vectors. Intern subjects/predicates/objects with exact UTF-8 preservation, compact ID/posting arrays, preserve source_file Arc sharing, and eliminate avoidable derived-index clone copies. Expected steady allocation reduction 0.32–0.53 GB now, 2.36–3.93 GB at proportional 1M; RSS saving may lag until freed pages return to the OS. Preserve historical/supersession APIs and existing snapshot section bytes via DTO encoding. Gates G,T,S,I,C,P and query_subject_as_of/believed_at equivalence; rollback rebuilds old in-memory maps from the same canonical section.

[E] **Third change: generation-pinned snapshot capture and tiered startup (48–96 h as a staged stream).** First remove derived clone-before-clear waste and .pld whole-buffer hydration, then retain immutable section mappings and delay only dependency-audited cold organs. Make snapshot shutdown safe and cap committed-checkpoint replay exposure by measured replay cost; do not weaken fsync acknowledgements. Expected capture peak reduction ~1.5–2.5 GB now/~11–18 GB at 1M, cold private-allocation reduction scenario ~1.28 GB/~9.46 GB before cache residency, and removal of the measured 32+41 s interrupted-save penalty when the latest family is durably committed. Meeting 5 s at 1M requires the explicit §4 eager-work budget; retaining the same eager decoder cannot achieve it. Gates C 9/9, I, G,T,S,U,P, first-cold-query quality/latency, and WAL coverage checks. Rollback restores eager loading/capture code while retaining two readable V23 families and all unchanged cold section bodies.

[E] These three changes do not certify one-million scale by themselves. Finish the compact all-corpus index/residency budget, measure the remaining residual, and run the full 1M frozen-corpus qualification with representative rare facts, current corrections, large realms, code-heavy data and real event/triplet cardinalities. Prefer lossless ownership/layout changes before f16; only promote quantization/PQ or retirement after their own gates pass. The practical success criteria are complete recall within the deadline, bounded recovery, and a reconciled resident/peak budget—not a smaller RSS produced by silently discarding useful lanes.

## Appendix B: Astra's round-2 memo (laptop, verbatim)

## chitta laptop profile — round 2, 2026-09-16
To Fable; read-only consultation against cf5800f5; no laptop benchmark was run.
[M] Measured: owner constraints, supplied observations, or identified live read-only results.
[C] Read-from-code: repository-relative file:line; Rust abbreviations mean chitta-field/src.
[P] Published: linked primary documentation/model results, not measurements of chitta.
[E] Estimated: arithmetic, design targets, effort, or judgment; decimal MB/GB throughout.
[M] Required: chittad ≤1.5 GB at 135,678 memories; ≤3 GB at 1M; usable cold start ≤2 s.
[M] Required: hook p95≤300 ms on eight shared CPU cores; background≤1 core and pausable on battery.
[M] Required: disk≤3×resident; at the resident ceilings this allows at most 4.5/9 GB.
[E] Round 1's 8–12 GB residency target is rejected for laptops; small heap optimizations cannot rescue it.
[E] Recommend immutable mapped segments, bounded write deltas/caches, one small-context embedder, and one foreground retrieval pass.
[E] Proposed working-resident envelopes are 1.210/2.750 GB; disk targets are 3.4/8.1 GB, including rollback/migration reserves.
[E] These targets are conditional designs, not certified results; 8.1 GB requires compact canonical storage and measured compression.
[C] Existing V23 bodies are bincode, not native mapped arrays (snapshot.rs:73); preserving magic does not preserve body compatibility.
[P/E] Nomic's published 256-d MTEB loss is 1.24 points; 128-d loses 2.94; neither proves chitta quality parity.
[E] Matryoshka reduces search bytes, not transformer inference or model weights; use 256-d as a gated candidate, not an assumption.
[C] The MiniLM cross-encoder is in MCP, not the CLI prompt hook (chitta-mcp/recall_gateway.py:190; hooks/prompt-core.sh:572).
[C] Foreground competitive-weight refresh can add 16 corpus searches (scoring/config.rs:272; store/recall.rs:288).
[E] Keep the current node profile and legacy V23 readers; isolate laptop canonical migrations and retain an explicit export/rollback path.
[E] First prototypes: model latency, candidate-quality loss, then bounded mapped-store startup/disk feasibility.

### What changes from round 1

[E] RAM is now a binding constraint. From the original measured 6.45 GB, reaching 1.5 GB requires a 76.7% reduction: 1−1.5/6.45. Proportional round-1 growth of 47.54 GB at 1M must shrink 93.7% to reach 3 GB. Interning triplets saves a useful estimated 0.32–0.53 GB at 135k but cannot deliver this architecture alone. My previous “compact before lazy load” ranking is wrong for this target: **stop materializing cold data first**, while retaining exact logical access.

[M/E] The round-1 live capacity census counted 2.839 GB before most native allocations: triplets 1.067 GB, embeddings 425.5 MB, spans 316.6 MB, HDC 282.8 MB, keyword 342.0 MB, CDAWG 90.4 MB. The later RSS was 8.054 GB. These are node measurements, not laptop forecasts. Removing all five speculative organs cannot by itself explain or remove the residual.

[E] Three qualifications govern every “adopt” verdict:
- This is a target for a representative bounded corpus and workload, not a guarantee from memory count alone. One memory can contain arbitrarily large text; events, triplets, symbols and write rates have separate cardinalities. Enforce byte quotas and admission/backpressure, not silent deletion.
- Count clean file-backed resident pages as RSS. Also measure chitta's page-cache/dirty-page pressure at the service or system level. Moving an allocation into another process or the page cache does not make it free.
- Distinguish backward reading of existing V23 families from old binaries reading new compact checkpoints. New readers must retain the former. The latter needs dual encoding or explicit export and cannot be achieved merely by retaining V23 magic.

[E] Common gates: G = golden nDCG@20 within its per-configuration noise band AND a predeclared equivalence margin (propose absolute 0.01, with paired confidence intervals); T = current-truth ≥40/50 including holdout and incident 5/5; S = no SMRITI panel regression; I = ordered 20/20 within-process and across three restarts, pinned clock, complete query embeddings; C = existing chaos 9/9 plus new segment/corruption/migration cases; L = actual eight-core laptop hook p95≤300 ms while IDE/browser/agent run; U = truly cold process and storage-cache start to complete usable recall≤2 s; B = measured RSS, disk peak and CPU budgets. An unresolved baseline identity failure does not become a pass by weakening the gate. [C] The accepted robustness plan requires configuration-specific noise bands and preserves snapshot-resident state until compatibility tests pass (docs/DECISION-2026-09-16-robustness-plan.md, Phase 0 and Phase 2).

### 1. Zero-decode store: adopt the representation, reject a reinterpret-cast migration

[C] V23 is a named, length-prefixed container; existing body bytes are positional bincode (snapshot.rs:73). Its section_table indexes body slices without decoding (snapshot.rs:1381), but load then eagerly deserializes sections into owned objects (snapshot.rs:1663,1675). Turbo cache load also reads owned packed rows and repacks another owned blocked copy (turbo.rs:41). Neither is a zero-decode serving format.

[E] **Adopt**, 80–160 engineering hours for the first useful mapped serving path; another 120–240 h for all major accessors/deltas, staged rather than a rewrite of every organ. Keep immutable segments with:
- explicitly encoded little-endian scalars, checked counts/offsets, alignment and format version;
- dense row ordinals distinct from durable MemoryId; sorted MemoryId→row tables, deletion/version bitmaps;
- row-major blocked vector codes, document-length arrays, CSR term/graph postings, sorted dictionary IDs;
- offset-indexed byte strings or compressed blocks, generation checksums, vector-transform identity and WAL coverage;
- a small heap write overlay, frozen immutable delta segments, deterministic base+delta merge, generation-pinned readers.

[E] First move .emb access and already-blocked Turbo, payload content+offsets, then BM25 forward/reverse postings and ID/state columns. Next triplets and symbol tables. Move serialized historical state last after its mutation and dependency audit. “First” here means read-path migration priority; the model/candidate prototypes below must establish that the laptop can meet the latency target at all.

[E] No *entire HashMap* fundamentally requires heap because it mutates: a sorted immutable base plus latest-wins delta replaces it. Heap is appropriate for active transactions, mutable recent states, coactivation/touch accumulators, pending embeddings, write dictionaries, locks, bounded query scratch and LRU metadata. Persistent mature entries belong in segments. Route reads through accessors; current direct payload.content and map accesses cannot simply point at empty lazy placeholders.

[E] Working-set arithmetic, not mapping size: a 2 GB mapped triplet file whose queries touch 20k distinct 4 KiB pages contributes about 81.9 MB resident, not 2 GB. Conversely a global 768-d four-bit scan touches all 384 MB of codes at 1M; “cold mapping” saves no steady scan working set. A 256-d code scan touches 17.37/128 MB at 135k/1M. Sorted IDs cost 16N≈2.17/16 MB if storing u64 ID+u64 row/offset; a delete bitmap costs N/8≈0.017/0.125 MB. Explicit 64/128 MB overlays replace unbounded mutation residency.

[E] A 16 MB state-column footprint at 1M is feasible only for a 16-byte *query-hot projection*, not the whole MemoryState. A 96-byte hot state record is 13.0/96 MB; nested histories and text remain cold. Read-before-write and current-truth filters still inspect authoritative latest state.

[E] Cap mapped working sets deliberately: segmented/windowed mappings with pinned active readers, bounded prefetch, and eviction/unmapping of inactive windows. Advisory page-discard hints alone are not a hard cap. Bound the number of simultaneously pinned generations and maintenance pages. Track system file-cache pressure separately; a single process is not allowed to scan tens of GB and advertise a small heap as the laptop footprint.

[P/E] rkyv supplies an archived directly accessible representation; zerocopy supplies checked layout/conversion building blocks. Neither makes arbitrary existing bincode HashMaps map-compatible. V23 body starts are not guaranteed suitable alignment; use body-internal relative offsets/padding or dedicated page-aligned files, version schemas explicitly, and validate offsets without blindly casting Rust structs. Prefer installed memmap2 plus small explicit array codecs first; consider zerocopy for checked scalar structs; rkyv only where archived complex types demonstrably reduce implementation risk. [rkyv architecture](https://rkyv.org/zero-copy-deserialization.html), [zerocopy API](https://docs.rs/zerocopy/latest/zerocopy/).

[E] Compatibility: initially add optional mmap sidecars keyed by canonical generation+codec+model/transform hash. Existing sections remain unchanged, old readers ignore sidecars, and new readers rebuild them during explicit conversion, not the first 2-second boot. Later compact canonical sections need distinct names/versioned bodies, fail-closed admission by a compatible reader, dual-read support and an export migration. Unknown-section skipping is not a safe required-codec negotiation mechanism. G,T,S,I,C,U,B; lossless layouts should yield identical output on a frozen profile.

### 2. Matryoshka truncation: measure 256-d first; reject an unqualified 128-d default

[P] Nomic publishes MTEB 62.28/61.04/59.34 for 768/256/128 dimensions: losses 1.24/2.94 points, approximately 2.0%/4.7% relative. Its example applies full-vector layer normalization, then truncates and L2-normalizes, with search_query/search_document task prefixes. This is benchmark evidence for that model, not chitta's path-heavy memories. [Nomic model card](https://huggingface.co/nomic-ai/nomic-embed-text-v1.5).

[E] Vector-only arithmetic, excluding IDs, scales, rotations, temporary copies and caches:

| Representation | B/memory | MB at 135,678 | MB at 1M |
|---|---:|---:|---:|
| 768-d f32 | 3,072 | 416.80 | 3,072 |
| 256-d f32 | 1,024 | 138.93 | 1,024 |
| 128-d f32 | 512 | 69.47 | 512 |
| 768-d four-bit | 384 | 52.10 | 384 |
| 256-d four-bit | 128 | 17.37 | 128 |
| 128-d four-bit | 64 | 8.68 | 64 |
| 768-d binary | 96 | 13.03 | 96 |
| 256-d binary | 32 | 4.34 | 32 |

[E] At 1M, 256 versus 768 four-bit saves 256 MB per resident code representation; 128 versus 256 saves another 64 MB. That last 64 MB is not worth presuming away a larger quality loss. With one resident code copy plus 12 B/row IDs+scales, 256-d costs about 19/140 MB. Drop both resident f32 and redundant packed Turbo arrays; otherwise the advertised 128 B/memory is false.

[C] Native model output is rejected unless n_embd==EMBED_DIM (chitta/include/chitta/vak_llama.hpp:146); the returned vector copies EMBED_DIM coordinates and only L2-normalizes (same file:273). Both Rust and C++ define embedding dimension at build time and require a multiple of 64 (chitta-field/build.rs:14; chitta/CMakeLists.txt:11). StoreHeader matches model ID, dimension and vector-space hash (snapshot.rs:120,156); Turbo requires its dimension to equal compiled EMBED_DIM (turbo.rs:43).

[E] Do not set CHITTA_EMBED_DIM=256 and feed the existing 768 model: that rejects the model today. Do not merely weaken the check and copy the first 256 either: the native-output, normalization and identity contracts remain wrong. Separate **native model dimension**, **canonical stored vector space**, and **derived search-code dimension**. Use a runtime StoreDescriptor/TransformDescriptor through query preparation, FFI length validation and index constructors; retain 64-multiple checks. The same executable can keep node defaults and select a laptop derived index. Existing fixed binary-word arrays and cortex/PQ dimension-dependent code must be audited; a profile env variable alone is not implementation.

[E] Low-risk first experiment: keep 768-d canonical vectors and current node store identity; add a 256-d derived candidate sidecar with its own transform identity, then exact 768-d rerank of a bounded candidate set. It tests truncation without destroying information. If native vectors are only L2-scaled versions of the required pooled output, the transform may be derivable offline; prove llama output parity against the model's reference pipeline. Wrong model/version/pooling/normalization means re-embedding, not slicing arbitrary vectors.

[E] For the strict disk profile, promoting 256-d f32 to the laptop's canonical exact space may be necessary. Then .shdr/WAL identity, snapshot vectors, delta vectors, query cache keys, codebook/centroid fingerprints, HDC codes derived from embeddings, cortex/PQ training and startup caches all need a transactional migration or rebuild. Record model artifact hash, prefix policy, pooling, full-vector transform, output dimension and text format, not just “nomic”. A return from canonical256 to canonical768 for new memories requires re-embedding from retained text; there is no inverse truncation. Preserve current 768 V23 families as the rollback/import source. Node profile remains 768 or its configured model.

[C/E] The code's default model discovery is bge-large, so 768 alone is not proof of Nomic identity (chitta/include/chitta/vak_llama.hpp:117). The user's Nomic premise must be checked against the actual .shdr and loaded model digest in the conversion tool. Truncating non-MRL vectors is a separate, unsupported quality experiment.

[C] MiniLM cross-encoding is lazily loaded in chitta-mcp/recall_gateway.py:190. Prompt lanes call the CLI directly (hooks/prompt-core.sh:572), and daemon “rerank” includes scalar filtering (chitta/src/handlers/field_memory_recall.cpp:1100). [E] A text cross-encoder can reorder retained candidates; it cannot recover a correct answer absent from the candidate pool. Do not use reranker-on evaluation to certify a reranker-off hook. Run 768/256/128 × f32/four-bit × reranker off/on, freeze candidate depths, include rare corrections and cross-realm questions.

[E] Verdict: **measure first** for 256-d; **reject as default** for 128-d until separately qualified. Prototype 16–32 h; runtime dimensional separation/identity migration 48–96 h. Gates G,T,S,I plus candidate gold coverage@K (propose ≥99% of the 768 baseline union at K≤256), L and B. Published MTEB loss does not waive any gate.

### 3. Binary candidates plus on-disk exact rerank: measure, do not repeat the known failure

[C] Binary codes and full f32 rescoring infrastructure exist; code records that a 400-candidate sign-code prefilter missed relevant answers and that switching to the flat path improved historical golden nDCG from 0.331 to 0.712 (hnsw.rs:44). This is evidence recorded in source, not a fresh measurement. It strongly argues against declaring one-bit recall safe.

[E] Symmetric Hamming first stage stores 13.03/96 MB at 768-d, or 4.34/32 MB at 256-d. Use an unquantized query to score sign-coded documents via asymmetric weighted bit scoring as a separate candidate experiment; it is not the same kernel as Hamming. Then fetch top K exact vectors from a stable on-disk file. At K=256, exact768 reads 256×3072=0.786 MB logical bytes; exact256 reads 0.262 MB. Random 4 KiB pages make physical I/O at least approximately 1 MB and potentially ~2 MB for unaligned 768 rows. A cold serial 256×50 microsecond latency is 12.8 ms; batch/coalesce reads and measure p95 under agent I/O. K=2,048 raises logical exact768 bytes to 6.29 MB and serial latency to 102 ms under that assumption.

[E] Compared with four-bit256, binary256 saves only 13.03 MB now/96 MB at 1M. If candidate coverage needs thousands of exact reads, energy and latency can outweigh those bytes. Keep a 32/96 MB exact-row/block cache; do not accidentally fault the whole exact file during scoring or normalization. Use deterministic query/table transforms, immutable IDs and explicit stale-vector replacement.

[E] Verdict: **measure first**, 16–32 h prototype, 24–48 h integration if accepted. Reject the current 400-candidate Hamming route as the laptop default. G,T,S,I and gold coverage@K with K≤256 initially; expand K only inside measured 300 ms. Retain four-bit fallback and exact canonical vectors; no snapshot magic change.

### 4. Realm residency: adopt locality, reject “1/number-of-realms” arithmetic

[M] Live read-only realm_list returned 344 names, 200 with outer whitespace (including newline variants). The list provides names only. health_check --details true reported 135,710 memories but no spectral_by_realm entries. No usable owner-specific population histogram was obtained. I did not invoke realm cleanup or another mutating tool.

[C] realm_list returns names (chitta/src/handlers/field_system.cpp:829); health emits spectral counts only when those derived statistics are populated (same file:189). Geometry also omits groups below two samples (store.rs:220), so even a populated spectral histogram is not an authoritative total-membership census. [E] Add read-only counts and logical/resident bytes from actual realm membership, preserving exact identifiers; do not merge whitespace variants during this consultation.

[E] Let f be the union fraction of active realm(s), required global/brahman memories and explicit cross-realm obligations. At f=10%, four-bit256 hot code bytes are 1.74/12.8 MB; f=50% gives 8.68/64 MB; f=100% gives 17.37/128 MB. Exact768 hot vectors would be 41.68/307.2 MB at 10%, but do not retain them just because a realm is active. A 50k-memory project+global union costs 6.4 MB four-bit256 regardless of total corpus. These are scenarios, not this owner's distribution.

[C] The hook supplies realm and has explicit include-global corrections and cross-realm fallback (hooks/prompt-core.sh:592,731). SemanticIndex presently searches global Turbo and filters returned rows afterward (hnsw.rs:1663,1674); merely passing realm does not reduce bytes scanned and limited overfetch can miss in-realm candidates.

[E] Physically cluster rows and term/graph postings by primary realm, with a separate global range and exact filter-aware candidate enumeration. Do not retrain Turbo calibration per realm in a layout-only patch: one global codebook/rotation maintains comparability; merge exact reranked candidates across eligible ranges. Cross-realm operations must still read cold ranges, not silently disappear. Warm up likely realms on session activation, retain at most two recent realm page windows, and cap by bytes rather than realm count. Global queries must qualify too; the profile budget below does not depend on a favorable f.

[E] Verdict: **adopt locality**, **measure first** for owner-specific savings; 24–48 h. Gates G,T,S,I,L for tiny and dominant realms, realm switches, global and global-correction access. Roll back row ordering/index selector, not logical membership. Realm-only loading without cold fallback is rejected.

### 5. Payloads out of heap: adopt content-addressed blocks; reject transcript-only truth

[M] Payloads including metadata were 176.4 MB at 135k in round 1. [E] At 1M the same mix is ~1.30 GB. Example mean text 700 B gives 95.0/700 MB raw; 2:1 compression gives 47.5/350 MB. SHA-256 content IDs plus offset/length entries at 48 B/memory cost another 6.51/48 MB before dictionary dedup. Content addressing deduplicates identical bytes, not semantically similar memories; inspect actual distributions.

[E] Store exact UTF-8 payload/retrieval-surface bytes in immutable 16–64 KiB independently compressed blocks. Keep ID→content digest/version/offset in mapped metadata, a bounded 16/48 MB decompressed LRU and a bounded compressed-block cache. Batch retrieval by block offset. Avoid one file per memory and whole-corpus decompression. Index all authoritative text at write/segment-build time; query hydration should not rebuild BM25.

[C] The current .pld loader reads the entire file, then copies each content slice into a Vec (snapshot.rs:1514,1527). Recall copies content while building candidates, before final output truncation (store/recall.rs:377). CLI internal pool defaults to 60, with limits up to 160 (chitta/src/handlers/field_memory_recall.cpp:549); semantic candidate depth is at least 128 for a scoped search (store/recall.rs:234). Returned hook lane limits are 6 semantic+4 context+5 hybrid+3 keyword+3 correction=21 rows before merging, plus keyed lanes/enrichment (hooks/prompt-core.sh:532).

[E] So “hydrate 21 hits” describes only a proposed late-hydration path, not today's work. Preserve content-dependent lexical/prefilter features by either exact compact token/feature indexes or batch-hydrating the selected prefilter pool. If 60–160 candidates each need 1 KiB text, logical text is 60–160 KiB; worst-case one 64 KiB block each decompresses 3.75–10 MiB. A 21-hit final union reads ~21 KiB logical, up to 1.31 MiB blocks. Measure distinct blocks, not just output rows. Reuse hydrated hits across lanes and only then serialize CLI/JSON text.

[C] TranscriptRegistry itself stores turn content Strings (organ/transcript.rs:4,12) and is another cold-data candidate. Span transcript read_from reads from an offset to EOF (organ/span_store.rs:1072), unsuitable as the generic bounded payload reader. Missing .pld currently rejects content-stripped snapshots (field/opening.rs:252), reflecting its role as authoritative content.

[E] Registered transcripts are not equivalent to distilled/corrected/manual memories: files may move, be rotated, disappear, or contain different source text. **Reject transcript-only payload storage.** It is acceptable as optional provenance verification or a content-hash-verified reconstruction source with a stored fallback, never as the sole copy. Content-addressed compressed blocks: **adopt**, 24–48 h including accessor/late-hydration changes; G,T,S,I,C,L plus exact byte roundtrip, missing-block errors, long-text/Unicode fixtures. Existing V23 .pld remains readable; optional compressed sidecar first.

### 6. Triplets and other organs on disk: adopt one segment system, not six new databases

[E] Use a common immutable table/block layer and WAL coverage. Base+small sorted delta lookup must respect deletions, supersession and bi-temporal history. Stable entity/predicate IDs, with dictionary strings stored once, serve both triplet records and postings. Use subject/object/predicate sorted projections or CSR row ordinals; recent corrections/task facts get a reserved hot cache. Do not change normalization or discard historical versions to save memory.

| Structure | Existing evidence / projected heap | Proposed working residency at 135k / 1M | Verdict, effort, risk |
|---|---|---|---|
| Triplets | [M/E] 1.067/7.87 GB at unchanged mix | [E] 90/400 MB windows+dictionary/delta budget, not full-file size | [E] Adopt; 48–80 h. Cold random graph traversal may miss L; G,T,S,I,C and historical-query equivalence. |
| Spans and adjacency/trigrams | [M/E] 316.6 MB/2.33 GB | [E] 40/160 MB pages; ~4 span links/output hit need direct ID adjacency | [E] Adopt disk layout; 32–56 h. Preserve exact atoms/locators and rare-identifier fixtures; no age-only pruning. |
| Event tape | [C] Vec<TurnEvent> plus dictionaries (organ/event_tape.rs:40,58) | [E] 32-byte row; 250k-event scenario=8 MB now, proportional 1.84M=58.9 MB disk; resident recent ring 2/4 MB | [E] Adopt append-only blocks; 16–32 h. Event counts unmeasured and independent of N. |
| CDAWG | [M/E] 90.4/666 MB naive scaling | [E] Shared optional-organ cache; zero if gated off, otherwise budget 16/32 MB incremental/windowed access | [E] Measure first; 40–80 h if kept. Not a simple sorted table: transitions, suffix links and end-position bitmaps are graph state (organ/cdawg.rs:29). |
| Episode HDC | [M/E] 7.7/57 MB proportional mix | [E] ≤4/8 MB optional hot cache or zero after ablation | [E] Measure first; 8–16 h. Already bit-sliced; little priority compared with triplets. [C] hdc.rs:567. |
| Coactivation+assoc | [M] 347k pairs; [E] eight-hash scenario ~49 MB, ~358 MB at proportional 1M | [E] 25/70 MB combined hot-page/accumulator budget; cold edges sorted by source and pair | [E] Adopt; 24–48 h. Persist exact retained stats, bounded live delta and deterministic pruning; graph API/scoring gates. |
| Code intelligence | [M] round-1 119,387 symbols; [E] if all embedded, 366.8 MB raw768, ~2.70 GB at proportional 1M | [E] 45/120 MB pages including symbol-vector candidates; source descriptors cold | [E] Adopt; 32–64 h. Revalidate source versions; symbol search is a separate quality panel. [C] organ/symbol.rs:8,214. |
| Transcript/message/analytics history | [C] resident collections: organ/transcript.rs:20; organ/msg.rs:18; organ/analytics.rs:11 | [E] Fit recent state in 10/20 MB of the registry budget; older exact records disk-backed | [E] Adopt; 24–48 h. Do not break current session/handoff or ledger lookup. |

[C] Coactivation cap20 is enforced on load and on the snapshot clone, not every live insertion (field.rs:827; store/maintenance.rs:1113). [E] Fix live byte/cardinality bounds before promising a 70 MB cache. Required evidence remains available in cold storage; bounded recent evidence must not claim more confidence than the persisted state supports.

[E] Avoid full-database compaction every checkpoint. Flush 8–32 MB immutable segments and atomically publish covered WAL positions; merge bounded ranges later. Use at most four query-visible delta runs per shard plus a bounded active table, then apply write backpressure if compaction cannot keep up. Tombstones and historical readers pin necessary versions; disk quotas must include pinned generations. With AC unavailable indefinitely and unlimited writes, bounded delta+disk+no maintenance is impossible: reject/defer new bulk work visibly before quota exhaustion.

### 7. Existing crates: use dependencies to reduce risk, not to add another unbounded cache

[C] Declared Rust dependencies already include memmap2, byteorder, crc32fast, sha2, roaring, smallvec, serde/bincode, rayon and turbovec (chitta-field/Cargo.toml:14). No tantivy/usearch/redb/LMDB/zstd/rkyv/zerocopy is declared there. There is no Cargo.lock in this supplied chitta-field checkout, so transitive/pinned versions were not established.

| Rung / option | What it replaces; arithmetic | Verdict and compatibility |
|---|---|---|
| Stdlib sorted arrays/BufReader + installed memmap2, byteorder, checksum/hash crates | [E] Replace eager maps for mature rows; IDs 2.17/16 MB at 16 B each, bounded 64/128 MB heap delta. Existing roaring helps sparse row sets. | [E] Adopt first. Explicit versioned array sidecars; 32–64 h shared infrastructure, included in zero-decode stream. Avoid implementing a full transactional database casually. |
| Installed turbovec | [E] Preserve tested four-bit kernel; one mapped blocked copy costs 17.4/128 MB at256, versus current packed+blocked 34.7/256 MB. | [E] Adopt with mapped wrapper/kernel access, 16–32 h. Loader change required; current wrapper retains Vecs. Freeze calibration/bit packing and parity tests. [C] turbo.rs:13,64. |
| Tantivy | [P] MmapDirectory, segmented index, explicit writer thread/memory budget. [E] Replace KeywordIndex including reverse deletions/dictionary; target resident postings/dictionary 80/280 MB instead of 342/2,520 MB, plus up to32 MB writer charged to delta budget. | [E] Measure first, 40–80 h. Tokenizer, BM25 norm/IDF, global DF, deletion and tie-order differences can change scores. First derived shadow index with original authoritative payloads/WAL; publish segment coverage in chitta checkpoint. |
| USearch mmap view | [P] Supports file views, Rust bindings and compact scalar representations. [E] Candidate graph at 16 neighbors×4-byte rows adds ≥64N=8.7/64 MB just base links; practical layers/headers/deletion/filter state could be 0.1–0.3 GB at1M before vectors. | [E] Measure first only if flat codes fail L/energy; 24–48 h experiment, 40–80 h integration. Mmap is not DiskANN's I/O algorithm, and no crate guarantees random-page latency. Rebuildable versioned sidecar; keep exact candidate-quality gate. |
| Generic HNSW crate | [E] Same graph+vector and build scratch costs; 128 MB flat256 codes are already small. | [E] Reject dependency addition without verified mmap/filter/delta requirements. Replacing a custom graph that is currently off at1M is no saving. |
| redb | [P] ACID copy-on-write B-trees, explicit cache sizing; documented default cache is1 GiB. [E] Replace selected high-churn keyed maps if bespoke WAL+overlay complexity dominates; set 16/32 MB cache, never default. | [E] Measure first, 32–64 h plus cross-store recovery tests. Do not create two independent authoritative transactions: chitta WAL remains authority and redb is derived/applied-through-seq, or perform an explicit transaction redesign. |
| LMDB | [P] Mapped transactional KV pages; readers and writer have specific lifetime/concurrency rules. [E] Similar16/32 MB intended hot-page budget, but mapping itself is not a cap and long readers retain old pages. | [E] Measure first as an alternative to redb, not in addition. 32–64 h integration. Prefer NVMe-local laptop placement; preserve existing NFS store fencing for node mode. |
| zstd dictionaries | [P] Dictionary compression targets small repeated records. [E] 700 B mean payload×N×0.5≈47.5/350 MB compressed; ratio unmeasured. Dictionary IDs and block checksums are part of the codec. | [E] Adopt after a compression microbenchmark, 8–16 h codec work plus payload stream. Add one focused dependency; no trained dictionary replacement without retaining old decoder/dictionary. |
| rkyv / zerocopy | [E] Reduce decode/copy overhead, not entropy or graph cardinality; no automatic corpus-byte saving beyond the chosen layout. | [E] Measure first for rkyv, consider zerocopy for explicit arrays. 8–16 h format spike. Version archived schema/endianness/alignment; never serialize platform Rust pointers or overwrite existing bincode bodies. |

[P] Primary sources for the above API properties: [Tantivy mmap](https://docs.rs/tantivy/latest/tantivy/directory/struct.MmapDirectory.html), [Tantivy writer budget](https://docs.rs/tantivy/latest/tantivy/index/struct.Index.html), [USearch](https://github.com/unum-cloud/usearch), [redb](https://docs.rs/redb/latest/redb/), [redb cache configuration](https://docs.rs/redb/latest/redb/struct.Builder.html), [LMDB API](https://github.com/LMDB/lmdb/blob/mdb.master/libraries/liblmdb/lmdb.h), [zstd](https://facebook.github.io/zstd/). [E] Pin versions at implementation and qualify on the laptop; upstream benchmark hardware is not this target.

### 8. Organ retirement: no contribution measurement is not a zero contribution

[C] The plan records measured contributions for kind prior, RRF, keyed lanes and reranker, but does not provide a current organ-by-organ equivalent-quality verdict (docs/DECISION-2026-09-16-robustness-plan.md, findings F4/Phase2). scripts/ablate-organs.py is absent in this checkout. HDC actively enters hybrid fusion unless disable_hdc is supplied (chitta/src/handlers/field_memory_recall.cpp:615). Span links attach verbatim atoms to returned beliefs (same file:219). Cortex has its own recall and maintenance consumers; CDAWG/episode HDC power specialized event/counterfactual APIs (store/recall.rs:1281,1541).

[E] Pending actual ablations, label HDC, cortex, CDAWG, spans and episode HDC **contribution not quantified here**, not unused. A lean profile that turns them all off may remove exact paths, weak-signal candidates, temporal/event answers or write-side learning. No numerical quality loss can honestly be assigned from their byte sizes.

[M/E] If every relevant ablation passes, removing eager residency of HDC282.8+CDAWG90.4+spans316.6+episode7.7≈697.5 MB saves approximately5.14 GB at proportional1M; add the round-1 *estimated* cortex280 MB/~2.06 GB for a total~0.98/~7.20 GB. These alternatives overlap the mapped-cache savings; do not count twice.

[E] Adopt a “not loaded until consumer requests it” mode for specialized organs; turn off their production/update tasks only if the data can be deterministically rebuilt or an exact deferred update log is retained. If an organ's recall/API consumer is part of the required prompt contract, its lazy path must meet L or the profile is unqualified. A genuinely equivalent ablation means quality delta within the declared margin with adequate power, not “no statistically significant difference on a small set.” Keep payloads/serialized state and expose disabled/not_ready status. Do not let a save serialize empty defaults over preserved organs.

[E] Verdict: **measure first**, 16–32 h shared ablation/profile harness plus4–8 h per organ; retirement only after G,T,S,I and its own API/write/restart fixtures. HDC mapping or ablation beats spending time shrinking its HashMap to half size, reversing my round-1 rank3. Retaining the 7.7 MB episode organ may be cheaper than engineering it away.

### 9. Model residency: reserve the query model; remove always-on generation

[C] The embedder uses one model and default four contexts, up to16 (chitta/include/chitta/vak_llama.hpp:85,95). N_CTX=8192, MAX_TOKENS=8000 (same file:27), and n_ctx, n_batch and n_ubatch all become the effective model context (same file:162). Nomic can therefore reserve substantially larger inference workspace than a short hook query needs. Matryoshka changes the returned vector; it does not remove transformer layers or weights.

[E] **Adopt** one resident query context, two inference threads initially, measured 256–512 token query allocation; query-length policy must be explicit and tested. Preserve the accepted document tokenization contract using a separate longer context created only for admitted AC/idle ingestion, evicted after use. Do not silently clip long stored documents merely to make the memory graph look good. If query text can exceed the short context, route an explicit bounded fallback or use a separately qualified query compression policy; mark degradation, do not claim complete recall.

[E] Reserve 260 MB for model weights+tokenizer+one short context/runtime at both corpus sizes; this is an acceptance budget, not a measured Nomic footprint. If roughly100–150M parameters are stored at one byte each, weights alone are100–150 MB, leaving110–160 MB for context/runtime. At f16 they consume200–300 MB before workspace. Use the actual GGUF artifact size, llama buffer counters and RSS deltas, not this parameter approximation. Weight quantization needs G,T,S and embedding parity/quality checks; any re-embedded corpus must match the query artifact.

[E] **Reject always-on hintd** in the laptop default: the supplied1.1 GB alone is73% of the135k chittad budget, even though it is another process. Disable autonomous hint generation/distillation/dreaming on battery; allow explicit or AC/idle use under a total-service transient budget. Unloading/terminating an idle on-demand model worker can return arenas more reliably than assuming free() returns RSS. This is an operational recommendation only; no service was touched.

[C] MCP reranker imports ONNX or Torch lazily; native tokenizer avoids a documented ~10 s cold Transformers/Torch path (chitta-mcp/recall_gateway.py:119,190). The ONNX path defaults to eight threads and records developer e2e~500 ms median with max_len128 (same file:106,148). [E] That is not compatible evidence for a300 ms eight-shared-core hook. Keep prompt reranker off as in its existing direct CLI path. For explicit MCP reranking, use ONNX int8, a small candidate cap,1–2 threads and an on-demand worker, show cold/warming/omitted status. Its warmed model belongs in a separate measured service-wide memory allowance, not hidden outside chittad's headline.

[P/E] Smaller embedding model alternative: all-MiniLM-L6-v2 publishes384 dimensions,22.7M parameters and default256-wordpiece truncation. Weight-only int8≈22.7 MB or f32≈90.8 MB; an estimated80–160 MB total query-runtime budget is plausible but unmeasured. It requires a different embedding space and full corpus conversion; it is not the ms-marco cross-encoder. [MiniLM embedding model card](https://huggingface.co/sentence-transformers/all-MiniLM-L6-v2).

[E] Verdict: **adopt** context/thread limits and no resident generation; **measure first** Nomic quantization/short-context latency; **measure first** smaller model only if Nomic misses the90 ms embedding sub-budget. Truncation is the right storage/search trade to test first; a smaller network is the inference/battery trade. Estimate8–16 h controls,16–32 h model bakeoff tooling, and re-embedding time N/r: at20 memories/s,135k takes1.88 h and1M13.9 h; at100/s,22.6 min/2.78 h, excluding checkpoint overhead. Run conversion on AC or export a verified laptop image from the node. G,T,S,I,L,U and a per-model joules/query measurement.

### 10. Background work: pause expensive work, preserve durability and bounded growth

[C] Quiesce is a30-minute mtime-based evaluation flag (chitta/include/chitta/mind/subconscious.hpp:96). Periodic subconscious passes check it, after processing incoming events (chitta/src/subconscious.cpp:122,142). The “load probe” checks RPC pending/active counts and recent lock waits, not total machine CPU or battery (chitta/include/chitta/rpc/field_handler.hpp:253). Bounded embedding backfill can bypass that maintenance load gate (chitta/src/simple_cli.cpp:669). These are useful hooks, not a laptop power policy.

[E] **Adopt**,24–48 h shared cooperative scheduler plus8–16 h per unchunked heavy task. Profile-selected PowerPolicy should combine explicit pause, AC/battery source, recent foreground requests, system CPU pressure/load, memory pressure and thermal state. Linux adapter reads power-supply state and pressure counters; macOS/Windows need their native power/pressure APIs. Unknown power state conservatively pauses optional work. A persistent user pause must not expire like the eval flag.

[E] One shared background CPU token bucket, not one per subsystem: accrue at most1 CPU-second/second on AC, default0 on battery for discretionary tasks; measure thread/process CPU time including nested BLAS/Rayon workers. Run one single-thread worker for compaction/quantization at a time, avoid eight-core bursts that happen to average one core later. Divide jobs into≤10–20 ms compute chunks with cancellation/pause checkpoints, checkpoint progress and yield immediately on query admission.

[E] Run only on AC+idle: bulk re-embedding, full Turbo rebuild, cortical training, HDC/event-organ reconstruction, full graph maintenance, distillation, dream/think, broad source reindexing, and major compaction. Lightweight admitted write work persists WAL and updates a bounded delta; it must never require a full corpus snapshot. Essential fsync, journal rotation and small immutable-run flushes belong to the acknowledged foreground write contract and remain necessary on battery. If the owner interprets “pausable maintenance” as pausing even those, stop accepting new writes at the delta limit rather than violating memory/disk/durability.

[E] Budget example: flush64 MB deltas; at50 MB/s serialized output throughput,1.28 CPU-seconds per run is a measurement hypothesis. At64 MB every10 minutes the average is0.0021 core; a full5 GB rewrite at the same rate costs100 CPU-seconds, cannot be the latency-sensitive shutdown path, and consumes power even if capped. Incremental immutable runs bound restart replay without mandatory frequent full compaction.

[C/E] Additional code-discovered priority: competitive-weight refresh defaults to16 candidates and60 s interval (scoring/config.rs:261,272); each candidate can issue a full index search (store/recall.rs:288,315). One ordinary semantic query may therefore read17×52.1 MB≈886 MB at135k, or17×384 MB=6.53 GB at1M, before extra forms/lanes. Even with256-d codes that is295 MB/2.18 GB. At5 GB/s shared effective bandwidth,1M costs436 ms of code traffic alone. Move refresh to persisted background epochs or a bounded local-neighborhood update, retain the last committed scoring value in foreground reads, and qualify changed freshness with G,T,S,I. This is more urgent than replacing HNSW, which is off at this scale.

[E] Read-path touching/coactivation can also grow write deltas and make a supposedly read-only benchmark unrealistically cheap. Include production strengthen behavior in L/CPU tests; use no_learn only for frozen identity/quality comparisons. Persist enough scoring state that pausing learning does not create restart-dependent ranking.

### Laptop profile and budgets

[E] One env-selected profile, no code fork. Proposed CHITTA_PROFILE=laptop applies defaults before pool/model/store construction; explicit compatible overrides remain visible in health. Refuse contradictory settings or mark the profile unqualified. Node defaults, formats and scheduling remain unchanged unless separately enabled. These are proposed settings, not all existing env variables:

    storage = mapped_segments_with_bounded_delta
    native_model = verified current model
    search_dimension = 256 only after quality qualification; otherwise 768
    canonical_dimension = existing identity until explicit conversion
    dense_index = one mapped blocked Turbo representation
    exact_vectors = disk, bounded row/block cache
    rpc_workers = 2; foreground_compute_threads = 2
    CHITTA_EMBED_CONTEXTS = 1
    CHITTA_EMBED_THREADS = 2
    document_embedding_workers = 0 on battery; 1 admitted on AC/idle
    background_workers = 1; background_cpu = 1 core total on AC, 0 optional on battery
    generator/hintd = on demand, AC/idle default
    prompt_cross_encoder = off; MCP reranker = explicit/on demand
    query_state = committed scoring epoch plus durable bounded deltas
    organ_mode = qualified lazy/disabled choices, never guessed empty state

[E] Resident-bytes envelope below includes mapped pages, heap and conservative capacity allowances. It is an allocation of the limit, not a measurement. Global-query qualification must fit without assuming realm balance. Shared rows must be charged once, reserve fields are not permission to allocate memory just to satisfy disk/RSS ratio.

| Resident component |135k MB|1M MB|Basis|
|---|---:|---:|---|
| Embedding model/tokenizer/query context |260|260|Fixed acceptance cap from §9|
| One blocked Turbo+IDs/scales/query scratch |32|160|256-d raw codes17.37/128 MB plus metadata/kernel margin|
| Hot mutable/read state columns |40|150|~96N B plus flags/hot overlay|
| Sorted IDs, payload offsets, realm/current-truth keyed metadata |30|90|Mapped small columns+dictionary/key caches|
| BM25 dictionary/posting windows |80|280|Bounded mapped working set, compressed segments|
| Triplet dictionary/adjacency/history windows |90|400|Cold-aware graph cache|
| Span indexes/text/links |40|160|Keep exact identifier consumer available|
| Payload decoded+compressed-block caches |35|100|16/48 MB decoded plus compressed/index overhead|
| Heap active/frozen write delta |64|128|Hard admission budget; flush small runs|
| Symbol/code intelligence windows |45|120|Separate vector/source pages|
| Coactivation/assoc hot state |25|70|Bounded live delta+edge pages|
| Optional HDC/cortex/event-organ working pages |45|100|Only if qualified consumers fit; no hidden eager rebuild|
| Runtime/threads/query temporary scratch |100|160|Named pools, bounded concurrent requests|
| Allocator/page-pin/registry safety allowance |164|322|Unattributed bytes must be reconciled, not perpetually excused|
| Additional measured peak/working-set margin |160|250|Stop admitting work before crossing ceiling|
| **Total envelope** |**1,210**|**2,750**|Below1,500/3,000 by290/250 MB|

[E] If256 fails, one768 Turbo copy adds34.73/256 MB.135k remains within1.5 GB;1M would become3.006 GB, just over3 GB before extra transform scratch. Recovering~50 MB elsewhere is plausible, but keeping exact768 canonical disk is the larger difficulty. Thus 256 is a valuable experiment, not logically required for RSS alone.

[E] Optional-organ row is the most contingent resident budget. A consumer requiring a global full HDC scan touches~1.024 GB at1M, not100 MB. Either its ablation passes, its required queries fit a segmented bounded-I/O implementation, or the proposed profile fails. Likewise high-degree cold graph queries or long query contexts can violate L. Do not solve this by silently returning incomplete “complete” results.

#### Disk is a separate release gate

[M/E] Round1 measured two~1 GB families+two214 MB cortex snapshots≈2.428 GB;17 stale temporary files≈0.8 GB; total4.7 GB. Removing only abandoned temps yields3.9 GB, before adding laptop views/models. The unitemized4.7−2.428−0.8=1.472 GB must be inventoried; it is not automatically disposable. At1M, unchanged two-family/cortex projection is2.428×7.3704≈17.90 GB, already above9 GB.

[E] Therefore **reject** “keep writing two full old-style1M families, add mapped sidecars, meet9 GB.” Also reject hiding old snapshots on another partition or counting a3.072 GB exact-vector file as free. Retained user transcripts needed for reconstruction must count as dependencies; text is preserved in chitta's own compact content plane.

[E] A narrowly feasible *conditional* physical-disk target assumes canonical256 for memories and qualified256 for code-symbol vectors, segment sharing between checkpoints, and real measured compression. Example at1M:
- exact256 memory vectors1.024 GB; proportional119,387/135,678 code-symbol population at256 f32≈0.901 GB;
- one Turbo image with IDs/scales0.140 GB;
- compressed payloads/surfaces0.400 GB; BM25 segments0.650 GB;
- dictionary-coded triplets0.800 GB; state/IDs/keyed/provenance0.200 GB;
- spans0.250 GB; preserved other history/organ state0.200 GB.
Corpus subtotal4.565 GB. These non-vector figures are byte targets, not proven compression ratios; triplet and history cardinality could invalidate them.

[E] Add0.4 GB retained changed segments for rollback,0.4 GB bounded merge scratch,0.2 GB model/runtime assets,0.08 GB WAL reserve, and the original2.428 GB V23 rollback/import families:8.073 GB, rounded8.1 GB. At135k, proportional new corpus0.619 GB+original2.428+model0.2+scratch/WAL~0.15≈3.397 GB, rounded3.4 GB. These fit3×the proposed resident envelopes:3.630/8.250 GB. At lower actual RSS, re-evaluate the literal ratio; do not inflate RSS. Disk≤3×resident is a peculiar floating quota, so report actual disk/RSS as well as the absolute4.5/9 GB ceilings.

[E] The above deliberately exposes how tight1M is: only~0.18 GB spare below3×2.75 GB. Keeping768 exact vectors for both memories and proportional symbols adds~3.85 GB, breaking the target. More cold raw history or the unexplained1.472 GB could also break it. Prototype real physical packed sizes before committing to this design. If lossless preservation/compression and qualified256 cannot fit, the owner must relax disk, accept an explicit smaller corpus/retention policy, or reject the laptop1M target. I cannot certify all constraints from today's measurements.

[E] Segment sharing reduces duplicated unchanged content across retained *new* checkpoints; it does not make a second full rewrite free. Bound delta versions and merge scratch before beginning compaction. A mass rewrite may require an offline streaming conversion with external temporary space; that migration is outside the steady-profile disk qualification and must be declared in advance, not sprung on a full laptop. Old families remain readable and intact through conversion.

#### Cold start and foreground budget

[E] Proposed cold usable-recall timeline, measured at both sizes and after cache eviction:
- validate bounded manifest/section directory and open mappings≤50 ms;
- fault critical state/keys and candidate code pages≤300 ms at1M, less at135k;
- model mapping, initialization and short-context warmup≤900 ms;
- bounded delta replay≤150 ms; no all-corpus normalization, reverse-map rebuild or code repack;
- complete first representative recall≤300 ms;
- scheduling/verification margin300 ms.
Total2.0 s. Safe model/index work may overlap, but this budget does not depend on more than two foreground compute threads. A header-only socket-ready event is not usable recall.

[E] These times require prebuilt validated images and bounded WAL/run counts. Cold-file corruption triggers fail-visible recovery, not a silently degraded pass. A full multi-GB integrity walk cannot fit2 s on arbitrary NVMe; verify immutable blocks on use with bounded header/directory validation and persist build-time whole-generation hashes. Check every touched block before returning its facts. Qualification includes all required first-query lanes, not just cached keyword results.

[E] Warm prompt p95 budget: transport/admission20 ms; one shared embedding90 ms; shared dense search≤45 ms; BM25/keyed work≤35 ms in parallel; graph/filter/fusion35 ms; exact-vector/text hydration35 ms; hook rendering/remaining overhead40 ms; scheduling margin35 ms. Critical path20+90+45+35+35+40+35=300 ms. A second distinct context query can consume another model inference; retain it only if shared preparation/parallel capacity and its measured contribution fit the same deadline. No reranker rescue is budgeted.

[E] Four-bit256 traffic floor at1M is128 MB; at assumed5 GB/s effective shared bandwidth,25.6 ms.768 costs76.8 ms. Binary256 costs6.4 ms of sequential traffic plus extra exact-read latency. These are bandwidth floors, not kernel benchmarks. Foreground refresh×16 and repeated semantic/hybrid forms must not multiply them unnoticed. Current code launches each recall_lanes future and waits all results before marking elapsed-budget violations (chitta/src/handlers/field_memory_recall.cpp:1786,1807); replace this with cooperative request deadlines and bounded shared work.

[E] Keep fail-visible per-lane status, embedding completeness, model/transform/index generation, candidate count, queue/CPU/I/O times and cancellation lag. A timeout that causes keyword-only fallback is not a quality pass. Report p95 complete-lane latency and completion fraction, including thermally throttled battery operation and adversarial realm switches. No CPU architecture or input-length distribution was supplied, so8 cores alone cannot guarantee90 ms embedding.

### Migration and stream order

[E] Stream0 — measurement and contracts,16–32 h. Extend existing memory_breakdown (profile.rs:44) with mapped resident/page-cache categories, all omitted structures, model/context buffers, transient peaks, exact per-realm counts, foreground refresh count, query hydration/block counts, disk-by-owner, WAL replay work, and background CPU seconds. Freeze model/transform identity and workload: representative prompt lengths, concurrency, agent load, corpus text/triplet/event/symbol distributions. No optimization claim without G/T/S baselines and accurate actual-RSS accounting.

[E] Stream1 — power-aware bounded execution and model controls,24–48 h. One query context, named bounded pools, shared recall preparation, cooperative deadlines, foreground neighborhood-refresh removal behind a flag, AC/battery/pressure policy. Integrate with all maintenance threads, not only subconscious quiesce. Gates L,≤1 core average, pause response≤100 ms for optional chunks, durable-write/queue semantics, G,T,S,I. Rollback profile scheduler flags; keep observability.

[E] Stream2 — profile accessors and lossless mapped candidate/payload/BM25 path,80–160 h. Produce optional view sidecars from an immutable legacy family; new reader bypasses eager canonical decode when view generation/WAL coverage matches. Keep legacy opener unchanged for node mode and cache-miss recovery. Build converted views explicitly before qualification. Gates bitwise output parity for lossless changes, C,I,U,B. Stop if the extra physical disk cannot fit the migration budget.

[E] Stream3 — model/truncation and binary candidates,48–96 h integration after the prototypes. Maintain separate canonical-space and derived-search identity, qualify256 four-bit against768, and compare binary only if it meets coverage cheaply. Legacy families remain untouched. Canonical256 conversion requires a new laptop store identity and full per-ID validation; old model vectors cannot mix into new deltas. Gates G,T,S,I,L plus golden candidate coverage. Do not reclassify a quality regression as a necessary laptop optimization.

[E] Stream4 — compact canonical tables, incremental checkpoint manifests, triplets/spans/code-intelligence mapping,120–240 h. Retain V23 envelope magic but introduce explicitly versioned compact body codecs and dependency-pinned immutable segments. New loader supports both old bodies and compact ones. During transition retain legacy families and provide streamed export to old representation for the appropriate vector-space identity. Old binaries must not open compact-only stores: use a distinct store directory and a loader/store-format admission fence; preserving the old magic alone cannot enforce this. Existing old binaries that ignore new metadata cannot be made safe retroactively—keep them pointed only at their retained legacy directory.

[E] The existing .shdr identity mismatch can fence canonical256 from old768 binaries (snapshot.rs:156), but that is a vector-space fence, not sufficient generic codec negotiation. Add explicit required-codec support before a newer binary writes compact canonical data; prove accidental legacy-open behavior. Never omit an existing critical section and trust “unknown skipped/missing defaults.” For rollback of new canonical256 writes, export256 data to a compatible reader; returning to original768 semantics needs re-embedding. State this limitation in the release contract.

[E] Stream5 — per-organ ablation/lazy consumers and disk qualification,40–80 h harness/integration plus accepted organ work. Snapshot codecs, cold bytes and deferred updates survive disabled residency. Gates every consumer/write invariant, C9/9 plus new fault cases, G,T,S,I, actual disk≤3×actual resident, and peak disk during compaction. No automatic episode pruning: the code explicitly records prior data-loss incidents and keeps it manual (chitta/src/subconscious.cpp:190).

[E] Estimates overlap shared infrastructure; do not sum every option table as separate mandatory work. A complete qualified laptop profile is plausibly300–600 engineering hours plus evaluations/soak, not a weekend env patch. A useful135k constrained read-serving prototype can arrive earlier. All settings resolve through one ProfileConfig shared by C++ and Rust/FFI, with defaults preserving the node path. A profile is declared qualified only after the whole8-core end-to-end workload passes; individual byte wins do not suffice.

### Three prototypes to run first

1. **Query model on the actual laptop.** [E] One context, two threads, verified Nomic artifact; compare present weights versus a qualified compact artifact and a smaller-model control. Fix realistic query-length distribution and run alongside the agent. Enforce≤260 MB model/runtime allocation while measuring cold warmup separately. **One deciding number: query-embedding p95≤90 ms.** If it misses, Matryoshka cannot fix that stage; change inference/model strategy or the300 ms target.8–16 h harness. G/T/S remain prerequisites for selecting a different artifact, not secondary latency metrics.

2. **256-d four-bit candidate-quality sweep from a frozen vector/payload copy.** [E] Full prescribed transform; compare768,256,128, binary asymmetry and exact rerank at fixed candidate caps, with no MCP-only reranker hiding loss. Include current corrections, rare identifiers, longest queries and dominant/tiny realms. **One deciding number: paired lower confidence bound on golden nDCG@20 delta must be≥−0.01** (and remain in the calibrated band). T,S and candidate coverage prevent a misleading aggregate pass. If256 fails, keep768 code path and re-evaluate disk feasibility; do not proceed on the published MTEB claim.16–32 h.

3. **Mapped serving-image feasibility at1M under a3 GB resident cap.** [E] Offline create a representative immutable image with exact compressed payloads, actual triplet/symbol distributions, bounded64/128 MB delta, native-model reservation and two recoverable checkpoint generations. Include the retained legacy-import bytes in disk accounting; require disk≤min(9 GB,3×measured steady RSS), no dropped authoritative records, and complete first-query results. **One deciding number: cold boot-to-first-complete-recall≤2.0 s.** If it fails under those preconditions, header-only mmap claims have not solved startup. Record page faults, decoded bytes and packed disk size to identify whether the blocker is model setup, insufficient locality, or preservation overhead.32–64 h prototype; no all-store rewrite required to establish this failure early.

[E] Laptop verdict: adopt a bounded, disk-backed serving architecture and power-aware execution; measure256-d candidate quality and the model before betting on either; reject transcript-only storage, unchanged two-full-family1M snapshots, silent organ/lane loss, and “mmap means no RAM.” The requested limits are a coherent engineering target for a qualified workload, but their simultaneous feasibility—especially disk preservation and complete300 ms CPU recall—remains unproven.
