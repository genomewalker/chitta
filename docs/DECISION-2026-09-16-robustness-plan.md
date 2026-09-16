# Decision 2026-09-16: robustness plan for chitta as agent infrastructure

> Status as of 2026-09-16: **draft by the orchestrator (Fable); Astra's
> adversarial review pending.** Each phase has an entry gate (what must be true
> to start), an exit gate (the measurement that proves it), and a rollback.
> Nothing in this plan changes the snapshot format or requires a migration.

## Findings this plan answers

| # | Finding (evidence) | Phase |
|---|---|---|
| F1 | Two locking authorities: the daemon's global `rpc_mutex_` over a store that synchronizes itself. Every stall this week was that mutex (`learn_codebase` 271 s, embedding under the lock, queued `observe` 6 s); fixes so far are allow-lists. | 1 |
| F2 | Recall returns what was distilled, not what is true now: 2 of 5 realistic probes missed (May pre-compaction summaries outranked current facts; a documented env var was unknown). Freshness is only decay. | 3 |
| F3 | No causal evidence that automatic learning improves outcomes; only planted memories are measured (SMRITI 41/45 vs 33/45). | 0 |
| F4 | Accretion without a retirement rule: ~60 organs on the store struct; measured contributions exist for the kind prior, RRF, keyed lanes and the reranker; VSA analogy scored 0/20 until narrowed, MDL accepted 0/461. | 2 |
| F5 | Surface width: 320 advertised tools, 76 unadvertised handlers, 10k lines of FFI marshalling; an agent uses ~15. | 4 |
| F6 | Policy in bash: ~8.5k lines implement admission, lanes and ledger; least testable layer. | 5 |
| F7 | One process for embedding, storage, distillation, maintenance and RPC; restart 9.5 s; BLAS contention. Runtime state on NFS caused two lock incidents in one day. | 6 |
| F8 | No chaos coverage: the incidents this week (stale NFS lock, second instance, kill mid-snapshot, WAL segment deleted) were all discovered live. | 7 |

## Phases

### Phase 0 — Evidence first (now; runs alongside everything else)
- Run the automatic-learning experiment as agreed (`DECISION-2026-09-15-learning-experiment.md`): 20 prospective tasks, 3 trials, Δ ≥ 3 retains automatic admission.
- Add a **current-truth panel**: 50 questions about this repository whose answers are checkable against code and docs at a pinned commit (env var names, file:line of a rule, which hook does X, a number in a dated doc). Score = fraction answered correctly from the top-3 recall results. Baseline today: 2/5 on the ad-hoc probe. Frozen with the same immutability discipline as the golden set.
- Exit gate: both panels run nightly on the frozen replica and appear in `benchmarks/noise.json` bands.

### Phase 1 — One locking authority
- Remove the exclusive `rpc_mutex_` path from the C++ dispatcher. Inventory every handler that mutates C++-side state (embedding caches, subconscious, sadhana manager, queue counters); move that state behind narrow mutexes or into the Rust store. The Rust per-component locks become the only consistency mechanism, as `is_lockfree_read`/`is_lockfree_write` already assume for the hot paths.
- Exit gate: a concurrency test (12 writers, 12 readers, 5 minutes on a replica) with zero `[lockprof]` holds over 50 ms; 20/20 recall identity; ctest; one day of live operation with no hold over 150 ms.
- Rollback: the allow-lists stay in the tree behind `CHITTA_GLOBAL_LOCK=1` until the gate has held for a week.

### Phase 2 — Retirement by measurement
- For each organ and lane, an ablation flag (`CHITTA_ABLATE_ORGANS`, like the existing lane ablation). Measure golden nDCG, SMRITI, the current-truth panel and hook latency with each organ off, on the replica, three runs.
- Rule: an organ with no effect beyond the noise band on any panel, and no keyed-lane or write-path dependency, is deleted with its sidecars, FFI entry points, tools and docs. A deleted organ is recorded in CHANGELOG with its ablation numbers; it can return only through an evolve card with a measured gain.
- Exit gate: lines in `chitta-field/src` and `chitta/src` down by at least a quarter; every remaining organ has a line in `docs/FIELD_PERF.md` stating the panel it moves.

### Phase 3 — Memory that knows what is current
- **Source-anchored memories**: a memory may carry an anchor (path, content sha, symbol, commit). The FileChanged hook marks memories whose anchor changed as `stale`; recall demotes stale-anchored memories below unanchored ones of equal score and shows the anchor state; the distiller writes anchors for facts it derives from files.
- **Docs and code as first-class memories**: the code-intel index already holds symbols; index the markdown docs the same way (heading-scoped chunks with anchors) so "which env var disables X" is answered from the doc, time-stamped, and superseded on edit. No LLM in the loop for this path.
- **Supersession over decay**: a newer anchored memory for the same anchor supersedes the older one (existing `supersedes` triplet) instead of both competing.
- Exit gate: current-truth panel ≥ 80% from the top-3; golden nDCG within its band; the five-probe set from 2026-09-16 answered 5/5.

### Phase 4 — Surface reduction
- Split the tool surface: `core` (what hooks, skills and agents call; target ≤ 80 tools) advertised by default; `advanced` behind an explicit MCP flag and the CLI's discovery; unadvertised handlers either promoted to core, moved to advanced, or deleted. The generated tables and the contract check make this mechanical.
- Exit gate: `tools/list` default ≤ 80; MCP tools/list payload under 8k tokens; contracts unchanged for every kept tool; hooks and skills pass.

### Phase 5 — Policy out of bash
- Move admission, lane fusion, budget accounting and ledger writes behind daemon RPCs (`recall_lanes` already exists; add `admit` and `ledger_event`). Hooks become transport: read payload, one RPC, print. One implementation serves Claude Code and Codex.
- Exit gate: hook output byte-identical on the existing fixtures; `hooks/*.sh` under 3k lines; prompt hook median ≤ 400 ms, session start ≤ 400 ms, Bash pre-hook ≤ 15 ms added; the parity tests remain.

### Phase 6 — Process and placement
- Embedding in its own process (`chitta_hintd` exists) or at minimum its own thread pool isolated from RPC and maintenance; the daemon's startup ≤ 5 s cached (snapshot decode is now the floor at ~3 s).
- Runtime state (queue, locks, sockets, ledger tail) on node-local storage with a periodic NFS checkpoint; the store stays on NFS. The instance lock records host and pid and refuses cross-host takeover, as today.
- Exit gate: restart ≤ 5 s; a fortnight with no NFS lock incident; embedding-heavy load (200 remembers) does not move recall p95 above 150 ms.

### Phase 7 — Chaos as tests
- Each incident of the last week becomes a ctest on a scratch store: SIGKILL mid-snapshot; second instance on the same directory; WAL segment deleted underneath; stale lock file with a dead pid; disk-full during save; daemon restart mid-hook; MCP restart mid-session.
- Exit gate: all pass in CI's build job; a nightly canary restarts the replica daemon and runs the panels, alerting on regression.

## Order and effort
0 starts now and never stops. Then 1 → 3 → 2 → 4 → 5 → 6 → 7, with 2 interleaved as measurements arrive. Rough effort: 1 (16 h), 3 (24 h), 2 (16 h + measurement time), 4 (8 h), 5 (24 h), 6 (16 h), 7 (12 h), all as Codex streams with the orchestrator reviewing and gating. Each phase is a branch that merges only when its exit gate is met on the replica and CI is green.

## What we do not do
- No new organs, lanes or tools until Phase 2 has removed what does not pay for itself.
- No LLM in any recall path added by this plan.
- No snapshot format bump; every new index is an optional sidecar.
