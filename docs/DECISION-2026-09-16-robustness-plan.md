# Decision 2026-09-16: robustness plan for chitta as agent infrastructure

> Status as of 2026-09-16: **agreed between Fable (orchestrator) and Codex
> gpt-6-astra (adversarial review).** Astra's amendments are folded in below;
> the original draft is in git history (`git show 710d3450:docs/DECISION-2026-09-16-robustness-plan.md`).
> Each phase has an entry gate, an exit gate (the measurement that proves it),
> and a rollback. Nothing here changes the snapshot format or needs a migration.

## Findings this plan answers

| # | Finding (evidence) | Phase |
|---|---|---|
| F1 | Two locking authorities: the daemon's global `rpc_mutex_` over a store that synchronizes itself. The week's lock stalls were that mutex (`learn_codebase` 271 s, embedding under the lock, queued `observe` 6 s); one stall was a different cause (OpenBLAS's atfork barrier under `popen`, fixed with `posix_spawn`). Fixes so far are allow-lists. | 1 |
| F2 | Recall returns what was distilled, not what is true now: on a five-question probe 2 were right, 1 partial, 2 wrong (May pre-compaction summaries outranked current facts; a documented env var was unknown); Astra's independent probe scored useful hits 6/15 with obsolete `pkill` advice ranked above the `install` correction. Freshness is only decay. | 3 |
| F3 | No causal evidence that automatic learning improves outcomes; only planted memories are measured (SMRITI 23/27 → 27/27, 41/45 vs 33/45). Only 1,236 of 133,712 memories carry an identifiable session. | 0 |
| F4 | Accretion without a retirement rule: ~60 organs on the store struct; measured contributions exist for the kind prior, RRF, keyed lanes and the reranker; VSA analogy scored 0/20 until narrowed (now 14/14 on the narrowed contract), MDL accepted 0/461 and is retired. | 2 |
| F5 | Surface width: 319 advertised tools, 76 unadvertised handlers, 359 MCP entries before filtering, 10k lines of FFI marshalling; an agent uses ~15. MCP already tiers tools (`server.py` list_tools filters hidden/advanced). | 4 |
| F6 | Policy in bash: ~8.5k lines implement admission, lanes and ledger; the least testable layer. | 5 |
| F7 | One process for embedding, storage, distillation, maintenance and RPC; restart 9.5 s cached with snapshot decode at 3.6–4.3 s as the floor. Runtime state on NFS caused two lock incidents in one day (daemon instance lock; Codex thread-store lock). `chitta_hintd` has no embedding model, so a process split is not a rename. | 6 |
| F8 | No chaos coverage: the stale NFS lock (49 failed restarts), second instance, kill mid-snapshot and WAL segment deletion were all discovered live. Cargo/CTest are not wired into the build CI job. | 7 |
| F9 | Session-start handoff injects thread titles, a ledger id and corrections but no verified next action, branch, artifact path or blocker (Astra's fixture: 1,216 bytes, zero actionable items). | 3, 5 |

## Order
**0 + 7 first** (evidence and chaos coverage never stop), **then 6** (placement of runtime state, bounded embedding workers), **then 1** (single locking authority, the riskiest phase, only with the stress and rollback tooling from 0/7/6 in place), **then 3** (freshness), **then 2 + 4 together** (retire by measurement, shrink the surface), **then 5** (policy out of bash). Effort is re-estimated after Phase 2's inventory; the draft's 116 h excluded tooling and the week/fortnight soaks.

## Phases

### Phase 0 — Evidence first
- Run the prospective automatic-learning experiment exactly as frozen (`DECISION-2026-09-15-learning-experiment.md`, `benchmarks/learning/protocol.md`): 20 tasks × 2 arms × 3 trials, Δ ≥ 3 retains automatic admission. It is a one-time causal test with model runs, not a nightly replica panel.
- Add `benchmarks/current_truth/run.py`: 50 questions about this repository with pinned answers and citations (file:line or doc heading at a pinned commit), abstention scored (a wrong confident answer costs more than "not in memory"), an independent holdout split, frozen under `EVAL_IMMUTABLE.txt`. Baseline from the two 2026-09-16 probes: 2–3/5 and 6/15. Target after Phase 3: ≥ 40/50 on the top-3, holdout included.
- Recalibrate noise bands per configuration (hybrid/depth/reranker) before any phase claims a delta; the 2-SD band is descriptive, not an equivalence test.
- Provenance coverage: every writer records a session or source triplet; measure the share of new memories with identifiable provenance weekly (today 1,236/133,712 overall; post-cut writers are labelled).
- Exit gate: both panels run on the frozen replica nightly and appear in `benchmarks/noise.json`; the learning experiment has a verdict or a dated reason it cannot run yet.

### Phase 7 — Chaos as tests (starts with Phase 0)
- `scripts/chaos-replica.py`: each incident of the week as a test on a scratch store: SIGKILL mid-snapshot; second instance on the same directory; WAL segment deleted underneath; stale lock with a dead pid, on NFS (a local scratch cannot reproduce server-side lock failures); disk-full during save; daemon restart mid-hook; MCP restart mid-session. Assertions are durable-prefix recovery, queue idempotence and bounded hook/MCP recovery, not "it restarted".
- Cargo and CTest wired into the build CI job; `scripts/nightly-replica-canary.sh` restarts the replica daemon and runs both panels, alerting on regression.
- Subprocess-under-load regression (the `popen`/atfork stall) as a test.
- Exit gate: all chaos tests pass in CI; canary has run for a week.

### Phase 6 — Placement and bounded workers
- Keep the store and its instance lock on NFS (cross-host fencing is the point of that lock; record host and pid, refuse cross-host takeover, as today). Move the queue, hook markers, sockets and the ledger tail to node-local storage with a specified checkpoint-and-replay contract (what is lost on node loss, how it is replayed, idempotence keys).
- Bounded embedding workers isolated from RPC and maintenance threads first; a separate embedding process only if the bounded pool cannot hold recall p95 ≤ 150 ms under 200 concurrent remembers (`scripts/stress-embed-recall.py`).
- Acknowledged-write durability defined and tested: which RPCs return after fsync, which after WAL append.
- Exit gate: restart ≤ 5 s cached (`benchmarks/field-perf/run.sh` extended); a fortnight without a runtime lock incident, measured by `scripts/report-runtime-incidents.py` over `chittad.log`; the stress target above.

> Phase 6 status 2026-09-16 (evening): merged and deployed — bounded embedding
> workers (recall p95 during 200 concurrent writes 625 → 114–163 ms; the 150 ms
> gate is borderline and re-measured by the canary), runtime-dir placement of
> hook markers and the queue behind `CHITTA_RUNTIME_LOCAL=1` (default off),
> acknowledged-write durability test, timestamped daemon log, incident report
> flags other-host lock holders, store-lock wait for a live holder
> (`CHITTA_STORE_LOCK_WAIT_S`), and the primary-node gate now enforced by the
> daemon itself (exit 75) with the installer writing the marker and drop-in.
> Not met: cached restart 16 s on the replica against the 5 s gate (snapshot
> decode is the floor); deferred: cross-node RPC fallback for hooks and MCP on
> non-primary nodes (zero transport changes landed), atomic ack replay as one
> store transaction. The fortnight soak starts today.

### Phase 1 — One locking authority (highest risk)
- This is a concurrency redesign. Inventory (Astra): the global lock today protects the C++ task-ledger tables and revision transactions (`task_ledger.hpp`, `field_task_ledger.cpp`); the query LRU, health/soul caches, distill settings, subconscious queues, sadhana state, queue counters, the embed queue and budget counters already have local protection; registrations, pointers and callbacks need publication and lifetime discipline.
- Sequence: (a) a ledger transaction mutex; (b) audit maintenance, queue and subconscious callers for multi-FFI atomicity and durable sync; (c) instrument Rust lock waits and holds so `[lockprof]` silence is not vacuous once the dispatcher lock goes; (d) migrate one handler class at a time behind `CHITTA_GLOBAL_LOCK=1` (a real switch covering background callers too, tested, not the current allow-lists); (e) `scripts/stress-rpc.py` with 12 writers + 12 readers, invariant and deadlock detection, run per class migrated.
- Exit gate: stress run clean for 5 minutes with zero Rust-side holds over 50 ms; ordered recall identity checked on an unchanged frozen replica separately from stress using `scripts/restart-identity.py --restarts 3` (20 distinct committed golden queries, 20/20 on each restart) and `--within-process` (20/20), with a pinned evaluation clock, complete query embeddings, and no warm-up panel calls; invocation and private-copy requirements in `docs/FIELD_PERF.md`; ctest; a live week with no hold over 150 ms.
- Rollback: the switch above, kept for two release cycles.

> Gate note 2026-09-16 (afternoon): the "20/20 ordered recall identity across
> a restart" exit gate fails on unchanged main (15, 18, 11, 19 and 16 of 20 in
> three independent streams; a fixed-query control stays 20/20). Until
> `feat/restart-identity` root-causes it, running phases are held to "no worse
> than before on the same replica copy, same script"; the 20/20 gate returns
> once the shared `scripts/restart-identity.py` passes on main.

> Phase 1 status 2026-09-16 (evening): merged and deployed (ac09d5b7, store
> 6792a05). Task-ledger transactions own their mutex; handler queue metadata,
> write-notify and subconscious callbacks are published under narrow locks;
> the sadhana manager is published before serving and the compact_wal thread
> is joined at shutdown; every Rust store component RwLock is named and
> profiled (`[lockprof] RUST component=…`). `CHITTA_GLOBAL_LOCK` exists and
> stays **on** by default: `scripts/stress-rpc.py` (12 writers + 12 readers,
> 300 s) preserved 40,279 writes through WAL replay, but Rust holds over 50 ms
> occurred in every tested class, so the switch does not flip. Restart
> identity on the same replica copy 18/20 → 20/20 after the store fixes.

### Phase 3 — Memory that knows what is current
- Answer repository questions from the index first: extend code intel to Markdown headings (`code_intel.hpp` currently excludes Markdown), make the FileChanged hook handle deletions and lose its 300 s throttle for the index path, validate content hashes at query and startup because watchers miss edits. Identity is repository + path + heading (or symbol); the content hash is the version.
- Source-anchored memories only for hook-generated file facts at first (`artifact-trace.sh`, `[done]` provenance); explicit supersession through the queue's `observe` path and filtering in recall, not a blanket "unanchored is fresher" rule. Facts sharing a file must not supersede each other.
- Handoff capsule (F9): Stop writes, and SessionStart renders, a verified next action, branch, artifact paths and blocker from the ledger; measured by a cold-session continuation fixture (≥ 90% correct continuation on a 20-case set).
- Exit gate: current-truth ≥ 40/50 including holdout; the five 2026-09-16 probes kept as regression fixtures and answered 5/5; golden nDCG within its band; the continuation fixture met.

> Phase 3 status 2026-09-16 (evening): merged and deployed (45de7d13, store
> 36039ba). Markdown headings and code chunks are indexed as first-class
> sources with anchors; obsolete file facts are demoted only within exact
> score ties and superseded ones dropped; recall merges up to three source
> chunks unless `sources=false`; Stop writes a handoff capsule to the task
> ledger and SessionStart renders it. On the frozen replica: current-truth
> 20 → 36/50 with no losses, golden with `sources=false` unchanged (0.4804),
> restart identity 20/20 on three restarts, chaos 9/9, prompt median within
> the noise band. Not met: the 40/50 target, probes 3/5, and the continuation
> gate (0/20: none of the 20 past sessions wrote an explicit plan line, which
> the capsule now adds going forward; `benchmarks/continuation` re-scores as
> sessions accumulate).

### Phase 2 — Retirement by measurement (with Phase 4)
- Inventory every organ by dependency class (Astra's table: recall/write, keyed, index maintenance, code intel, event/API, and snapshot-resident state marked). Snapshot-resident codecs and WAL replay stay until compatibility tests pass; V23 defaults do not preserve discarded data on rollback.
- `scripts/ablate-organs.py`: organ-wide ablation flags with write and keyed-lane invariants and pre-declared equivalence margins; recall-only panels cannot justify deleting infrastructure. An organ is retired only when its own API has no consumer and the ablation stays inside the margin on every panel; deletion is recorded in CHANGELOG with the numbers, and return requires an evolve card with a measured gain.
- No line-count quota; the measure is what is retired with evidence.
- Exit gate: every remaining organ has a `docs/FIELD_PERF.md` line naming the panel it moves or the API that consumes it.

> Phase 2 status 2026-09-17 (matrix complete; retirement unqualified): 43 organ flags preserve
> rollback state and core writes. The 105 completed pre-fix trials are quarantined;
> after merging main, three controls score 0.5000783626223507 with spread 0
> against the frozen 0.0017901251267712533 margin. Four groups have three runs
> each: recall-adjacent and all-organ ablation are not equivalent (golden delta
> +0.004632109418440722); event/API and event prediction remain unqualified.
> The eight largest organs also have three runs each: HDC is not equivalent
> (golden delta +0.005005509837521904); cortex, CDAWG, spans, episode HDC,
> event tape, lite encoder and sparse encoder remain unqualified.
> All 35 remaining individuals were measured three times without load deferral
> (limit 96). Learners are also not equivalent (golden delta
> −0.0021989989178248792). Final individual verdicts: 2 not equivalent,
> 41 unqualified, 0 equivalent. Of 144 total trial records, 142 recover
> 200 writes, preserve three keyed lanes and pass canonical identity 20/20.
> Agent-registry repetition 2 and interaction-ledger repetition 3 retain
> process-disappearance errors with incomplete invariants; neither was replaced.
> Current-truth margins, a real isolated SMRITI panel and per-organ consumer-test
> evidence are still absent. No organs/tools/tests removed (retirement LOC 0).
> See `docs/FIELD_PERF.md`
> for the table and every retained organ's consumer/dependency justification.

### Phase 4 — Surface reduction (with Phase 2)
- Keep the existing tiering (`core` advertised, `advanced` and hidden callable). Add `scripts/check-mcp-surface.py` that measures the filtered `tools/list` and its payload in tokens for the model in use; target ≤ 80 advertised and payload ≤ 8k tokens. Unadvertised handlers are promoted, moved to advanced, or deleted; hidden direct calls stay compatible; contracts unchanged for kept tools.

> Phase 4 status 2026-09-16 (afternoon): merged and deployed (757aa78c).
> Advertised `tools/list` 89 → 54 tools, ~11.0k → ~6.3k estimated tokens;
> `scripts/check-mcp-surface.py` gates core ≤ 80 and ≤ 8000 tokens in CI,
> new tools default to advanced, hidden tools stay callable directly and via
> `advanced(tool=...)`. Docs regenerate with
> `python3 scripts/gen-tools-static.py --docs`. Every tier change and its
> evidence is in CHANGELOG.md.

### Phase 5 — Policy out of bash
- Move admission, budget accounting and lane fusion behind daemon RPCs incrementally (`recall_lanes` and the task-ledger RPC exist), keeping local safety checks and timeout fallbacks in the hooks. One implementation serves Claude Code and Codex.
- `scripts/bench-hook-parity.py` pins clock and session inputs, records per-process exit statuses (the current runner swallows them) and compares whole hook outputs before and after each move.
- Exit gate: byte-identical outputs on the parity fixtures; `hooks/*.sh` under 3k lines; targets with measured attribution from today's baselines (prompt 608 ms, SessionStart 599 ms, Bash added 24 ms): prompt ≤ 400 ms, SessionStart ≤ 400 ms, Bash added ≤ 15 ms.

### Phase 8 — Memory and latency budget (after Phase 2)
- Agreed with Astra on 2026-09-16 in two rounds; the laptop constraint (round 2) governs: steps, verdicts, the `CHITTA_PROFILE=laptop` envelope and the three deciding prototypes are in `DECISION-2026-09-16-memory-scale.md`. Order: census and identity freeze → power-aware bounded execution (foreground competitive-weight refresh off, per-lane status) → mapped candidate/payload/BM25 path → 256-d candidates → compact canonical tables and incremental checkpoints → organ lazy modes, archive tier, disk qualification.
- Exit gate (laptop profile): resident ≤ 1.5 GB at 135k and ≤ 3 GB at 1M, cold start ≤ 2 s, prompt-hook p95 ≤ 300 ms on 8 shared cores with zero silent lane loss, background ≤ 1 core and paused on battery, disk ≤ 3 × resident. Node profile keeps its formats and defaults.

## Backlog (owner-listed, not yet scheduled)

- Shutdown must finish an in-flight snapshot before exiting (the unit already allows 300 s): a SIGTERM mid-save abandons the family and the next start replays hours of WAL (105 s to ready on 2026-09-16 versus 10–18 s). Until then the operator checks the log before restarting (CLAUDE.md).
- Production recall silently degrades under load: the 50 ms query-embedding deadline (`CHITTA_RECALL_EMBED_WAIT_MS`) drops the semantic lanes and falls back to keyword-only without any marker in the result. Found by the restart-identity stream on 2026-09-16 (13/20 identical restarts were exactly the ones with missing embeddings). Decide a budget that fits the prompt-hook p95 and mark degraded results (`status`) so the hook and the canary can count them.
- **Independent qualification stage for evolve verdicts** (card
  `rekursiv-qualification-stage`, from rekursiv.ai's auto-autoresearch run):
  a second agent re-measures a candidate on a fresh replica before a verdict
  is recorded, with completion and qualification stamped separately; missing
  or disagreeing qualification means "unqualified", never "accepted".
- **One daemon per store across login nodes** (found 2026-09-16 15:0x): the
  chittad user unit lives in the shared home, so every login node starts it;
  units on two other nodes had retried the store lock every 10 s for ten days
  (85,272 restarts on one) and took the lock the instant a restart here
  released it. Installed today on this node: `<mind>/.daemon-node` names the
  primary host and a `primary-node.conf` drop-in makes other nodes' units exit
  75 without restarting; it takes effect on a node only after its systemd
  reloads the unit. Phase 6 must add: `smart-install.sh` writes the marker and
  drop-in; hooks and MCP on non-primary nodes reach the primary daemon over
  the RPC port (7432) instead of a node-local socket; `report-runtime-incidents`
  flags a lock holder on another host.
- **Timestamps in `chittad.log`**: lines carry none, so incident reports and
  the canary cannot date events; prefix each stderr line in the daemon.

## What we do not do
- No new organs, lanes or tools until Phase 2 has retired what does not pay for itself.
- No LLM in any recall path added by this plan.
- No snapshot format bump; every new index is an optional sidecar.

## Tooling this plan adds
`benchmarks/current_truth/run.py`, `scripts/chaos-replica.py`, `scripts/nightly-replica-canary.sh`, `scripts/stress-embed-recall.py`, `scripts/report-runtime-incidents.py`, `scripts/stress-rpc.py`, `scripts/ablate-organs.py`, `scripts/check-mcp-surface.py`, `scripts/bench-hook-parity.py`, Cargo/CTest in the build CI job.
