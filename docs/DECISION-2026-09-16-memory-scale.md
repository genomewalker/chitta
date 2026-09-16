# Decision: memory footprint and the road to one million memories

> Status as of 2026-09-16: **agreed between Fable (orchestrator) and Codex
> gpt-6-astra (read-only consultation, memo in the appendix).** This adds
> Phase 8 to `DECISION-2026-09-16-robustness-plan.md`. Nothing here changes the
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

## Appendix: Astra's consultation memo (verbatim)

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
