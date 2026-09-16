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

### Phase 1 — One locking authority (highest risk)
- This is a concurrency redesign. Inventory (Astra): the global lock today protects the C++ task-ledger tables and revision transactions (`task_ledger.hpp`, `field_task_ledger.cpp`); the query LRU, health/soul caches, distill settings, subconscious queues, sadhana state, queue counters, the embed queue and budget counters already have local protection; registrations, pointers and callbacks need publication and lifetime discipline.
- Sequence: (a) a ledger transaction mutex; (b) audit maintenance, queue and subconscious callers for multi-FFI atomicity and durable sync; (c) instrument Rust lock waits and holds so `[lockprof]` silence is not vacuous once the dispatcher lock goes; (d) migrate one handler class at a time behind `CHITTA_GLOBAL_LOCK=1` (a real switch covering background callers too, tested, not the current allow-lists); (e) `scripts/stress-rpc.py` with 12 writers + 12 readers, invariant and deadlock detection, run per class migrated.
- Exit gate: stress run clean for 5 minutes with zero Rust-side holds over 50 ms; ordered recall identity checked on an unchanged corpus separately from the stress run (20 distinct queries, not one query 20 times); ctest; a live week with no hold over 150 ms.
- Rollback: the switch above, kept for two release cycles.

### Phase 3 — Memory that knows what is current
- Answer repository questions from the index first: extend code intel to Markdown headings (`code_intel.hpp` currently excludes Markdown), make the FileChanged hook handle deletions and lose its 300 s throttle for the index path, validate content hashes at query and startup because watchers miss edits. Identity is repository + path + heading (or symbol); the content hash is the version.
- Source-anchored memories only for hook-generated file facts at first (`artifact-trace.sh`, `[done]` provenance); explicit supersession through the queue's `observe` path and filtering in recall, not a blanket "unanchored is fresher" rule. Facts sharing a file must not supersede each other.
- Handoff capsule (F9): Stop writes, and SessionStart renders, a verified next action, branch, artifact paths and blocker from the ledger; measured by a cold-session continuation fixture (≥ 90% correct continuation on a 20-case set).
- Exit gate: current-truth ≥ 40/50 including holdout; the five 2026-09-16 probes kept as regression fixtures and answered 5/5; golden nDCG within its band; the continuation fixture met.

### Phase 2 — Retirement by measurement (with Phase 4)
- Inventory every organ by dependency class (Astra's table: recall/write, keyed, index maintenance, code intel, event/API, and snapshot-resident state marked). Snapshot-resident codecs and WAL replay stay until compatibility tests pass; V23 defaults do not preserve discarded data on rollback.
- `scripts/ablate-organs.py`: organ-wide ablation flags with write and keyed-lane invariants and pre-declared equivalence margins; recall-only panels cannot justify deleting infrastructure. An organ is retired only when its own API has no consumer and the ablation stays inside the margin on every panel; deletion is recorded in CHANGELOG with the numbers, and return requires an evolve card with a measured gain.
- No line-count quota; the measure is what is retired with evidence.
- Exit gate: every remaining organ has a `docs/FIELD_PERF.md` line naming the panel it moves or the API that consumes it.

### Phase 4 — Surface reduction (with Phase 2)
- Keep the existing tiering (`core` advertised, `advanced` and hidden callable). Add `scripts/check-mcp-surface.py` that measures the filtered `tools/list` and its payload in tokens for the model in use; target ≤ 80 advertised and payload ≤ 8k tokens. Unadvertised handlers are promoted, moved to advanced, or deleted; hidden direct calls stay compatible; contracts unchanged for kept tools.

### Phase 5 — Policy out of bash
- Move admission, budget accounting and lane fusion behind daemon RPCs incrementally (`recall_lanes` and the task-ledger RPC exist), keeping local safety checks and timeout fallbacks in the hooks. One implementation serves Claude Code and Codex.
- `scripts/bench-hook-parity.py` pins clock and session inputs, records per-process exit statuses (the current runner swallows them) and compares whole hook outputs before and after each move.
- Exit gate: byte-identical outputs on the parity fixtures; `hooks/*.sh` under 3k lines; targets with measured attribution from today's baselines (prompt 608 ms, SessionStart 599 ms, Bash added 24 ms): prompt ≤ 400 ms, SessionStart ≤ 400 ms, Bash added ≤ 15 ms.

## What we do not do
- No new organs, lanes or tools until Phase 2 has retired what does not pay for itself.
- No LLM in any recall path added by this plan.
- No snapshot format bump; every new index is an optional sidecar.

## Tooling this plan adds
`benchmarks/current_truth/run.py`, `scripts/chaos-replica.py`, `scripts/nightly-replica-canary.sh`, `scripts/stress-embed-recall.py`, `scripts/report-runtime-incidents.py`, `scripts/stress-rpc.py`, `scripts/ablate-organs.py`, `scripts/check-mcp-surface.py`, `scripts/bench-hook-parity.py`, Cargo/CTest in the build CI job.
