# Runtime and storage core: design

Status as of 2026-09-19: **design under review, not implemented.** Owner: lead
session; implementation stream `p22-storage-core` follows this document after
an architecture review. Supersedes the ad-hoc mitigations of 2026-09-18/19
(parked endpoint files, parked `code-navigation.json`, guarded GPU scripts,
manual restarts), which stay in place only until the phases below ship.

## Why

The daemon is the core of chitta and it is neither fast nor solid under load.
Measured on the live store (140k memories, Isilon NFS home) on 2026-09-18/19:

| symptom | measurement | cause |
|---|---|---|
| startup | 17 s after a clean stop; 295–350 s otherwise | WAL replayed record by record through the hot insert path (34 MB in 255–315 s); no shutdown snapshot when the stop timed out |
| startup, second cause | 14+ min in `CodeNavigation::open → Impl::rebuild()` on the main thread | `code-navigation.json`, one 1.5 GB JSON with 20 worktree roots and inline symbol bodies, parsed and rebuilt before the socket opens |
| request latency | `ledger_op` waits of 2–5 min, `session_heartbeat` 780 s, `prompt_context` 4.8 h accumulated | one serialized queue; `learn_codebase` full rebuilds, endpoint probes and capsule paging run inline under the store lock |
| write failures | `Stale file handle (os error 116)` until restart, three times in two days | the writer's open WAL segment was unlinked on NFS (fixed in `feat/wal-vanish`, merged 2026-09-19) |
| shutdown | `TimeoutStopSec=300` hit, snapshot abandoned | shutdown waits for the jammed queue before it can snapshot |
| hooks | `[chitta] daemon unavailable` for the whole load window | health answers only after all post-load work |

Every one of these is architectural. Fixing them one symptom at a time produced
the last two days.

## Principles

1. **The socket opens first.** A client never sees "unavailable" because the
   daemon is loading; it sees a loading status with an ETA, reads served from
   the last snapshot, and writes accepted durably.
2. **Nothing unbounded runs on the request path.** Every request has a class
   and a budget; work that cannot finish inside the budget becomes a job.
3. **Derived state is disposable and lazy.** Indexes are rebuilt from the
   log or from the source repository on demand, in the background, per unit
   (per repository root, per snapshot family), never as one blob at startup.
4. **Durability is a policy, not a side effect.** One writer, append-only
   segments, group commit, explicit fsync points, size-bounded WAL, and a
   documented loss bound (nothing acknowledged is lost after a crash).
5. **Measured, not asserted.** Targets are numbers, benchmarks run on the
   frozen replica in the gate, and regressions fail the merge.

## Targets

| metric | target | today |
|---|---|---|
| time to socket open | ≤ 2 s after exec | 17 s to 15 min |
| time to full readiness, ≤ 1 h of WAL | ≤ 20 s | 17 s to 350 s |
| time to full readiness, 24 h of WAL | ≤ 60 s | unbounded |
| WAL replay throughput | ≥ 10 MB/s | ~0.1 MB/s |
| shutdown, queue jammed | ≤ 10 s, no data loss | 300 s timeout, snapshot lost |
| request p95 under 16 concurrent clients (mixed) | ≤ 500 ms; no request > 5 s | minutes |
| write acknowledgement | durable (fsync group ≤ 50 ms) | durable |
| loss on `kill -9` | nothing acknowledged | nothing acknowledged (after wal-vanish) |
| code index open | O(1) at startup; per-root load ≤ 200 ms | 14 min, all roots |

## Architecture

### 1. Process model

```
socket (async, non-blocking)        opens at t≈0
   │
   ├─ admission: classify request → {read, mutate, ledger, slow, job}
   │     per-class concurrency limits and budgets; overload → 429 + retry-after
   │
   ├─ read pool (N threads): recall, search, status, code queries
   │     reads the immutable snapshot view + the in-memory delta; never blocks on writers
   ├─ mutate lane (1 thread): store mutations in commit order; WAL append + group fsync
   ├─ ledger lane (1 thread): task ledger, sessions, leases, capsules
   ├─ slow pool (M threads, bounded): anything with budget > 2 s and no lock
   └─ job manager: background jobs with progress and cancellation
         learn_codebase, index rebuilds, snapshots, compaction, endpoint probes,
         maintenance; visible in `status`; never inline in a request
```

- The current single `rpc_mutex_` becomes a reader–writer split: reads take a
  snapshot view (epoch-based, lock-free readers), the mutate lane is the only
  writer. Long-held locks are a bug and the lock profiler already in place
  (`[pool]` lines) becomes a gate: any wait above 5 s in the benchmark fails.
- Capsule and session operations page by cursor with an index on
  `(repository, stream_id)`; no operation walks every session.
- Endpoint probes are jobs with a hard timeout (3 s) writing into the router's
  state; the router never blocks a request on a probe (this is `feat/router-fixes`,
  folded in unchanged).

### 2. Startup

```
t=0    open socket; status = {loading: true, phase: "snapshot"}
t≈1s   map the latest snapshot family (mmap, sidecars .lsh/.turbo/.organs)
       reads served from it; writes appended to a fresh WAL segment (durable),
       and applied after replay in order
t≈?    bulk replay of WAL since the family:
         decode segments in parallel → batches → apply to the delta with
         derived indexes deferred → one index build at the end
       status = {loading: true, phase: "replay", replayed, total, eta_s}
done   status = {loading: false}; queued writes applied; maintenance thread starts
```

- Derived indexes for the snapshot family are immutable sidecars produced when
  the family is written; startup never rebuilds them unless missing.
- The code index is not opened at startup at all (see 4).

### 3. Storage

- **WAL**: append-only segments, single writer, `O_APPEND`, group commit with
  an fsync every ≤ 50 ms or N records; directory fsync after create, rename
  and unlink; the writer's open segment is never a deletion candidate
  (`feat/wal-vanish`). Every unlink is logged with its reason.
- **Snapshot trigger**: WAL bytes since the last family ≥ 16 MB, or records
  ≥ 20k, or the timer, whichever first. Snapshots run on the job manager with
  a short cut-over; two families kept.
- **Shutdown**: stop admission, drain the mutate lane (bounded), fsync and
  cut the WAL, write a checkpoint marker `{family, wal_offset}`. Full family
  only if the last one is older than 15 min and the machine is idle;
  otherwise the next start's bulk replay handles it. `TimeoutStopSec` becomes
  60 s.
- **Replay**: columnar decode of segments in parallel; batched application;
  indexes (LSH, Turbo, organs, graph adjacency, quantized index) built once
  at the end from the applied rows; ≥ 10 MB/s on the replica.
- **Placement**: `CHITTA_STORE_LOCAL_DIR` runs the live segments and sidecars
  on node-local or scratch storage with the NFS mind as the durable mirror
  (families copied after commit, verified by hash). Default stays NFS with the
  discipline above; the option exists because NFS metadata latency dominates
  snapshot and replay time today.

### 4. Code navigation index

- One compact binary file per repository root:
  `<mind>/code-index/<sha256(root)[:16]>.idx`, containing file hashes, symbol
  spans (file, byte range, kind, name), and edges. **No symbol bodies**; bodies
  are read from the file on demand.
- Loaded lazily on the first query for that root; evicted when idle; roots
  whose directory no longer exists are pruned at open. A size budget per root
  with a log line when exceeded.
- Rebuilds and refreshes are jobs with progress; a query on a stale root
  returns the stale index and schedules the refresh. Git hooks in linked
  worktrees do nothing (already shipped in 763edbd6); the main checkout's
  hook enqueues a job.

### 5. Observability

- `status` returns: loading state and phase, queue depth per class, active
  jobs with progress, last snapshot age, WAL bytes since snapshot, lock wait
  p95 per class, endpoint router state.
- Phase timers stay in the log; the lock profiler stays on; a `[slow]` line
  for any request over its budget with the class and the handler.

### 6. Verification

- Benchmark (`benchmarks/storage/`) on a replica of the frozen cut: startup
  with 0/10/60/240 min of synthetic WAL; shutdown under a jammed queue;
  mixed load of 16 clients with p50/p95; replay MB/s; snapshot time; code
  index open per root. Numbers in the decision doc before and after; the
  full gate fails if startup or p95 regress by more than 20 %.
- Chaos: `kill -9` during write, snapshot and replay; ESTALE injection;
  disk-full during snapshot; a second daemon attempting to open the store.
  Acceptance: nothing acknowledged is lost, the next start meets the targets.

## Phases and gates

| phase | delivers | exit gate |
|---|---|---|
| 0 | baseline benchmark + this design reviewed | review sign-off; numbers recorded |
| 1 | socket-first startup, loading status, reads from the snapshot, durable write queueing | socket ≤ 2 s; hooks never see "unavailable" |
| 2 | bulk replay with deferred index build; size-triggered snapshots; fast shutdown; unit timeout 60 s | readiness ≤ 20 s with 1 h WAL; shutdown ≤ 10 s |
| 3 | request classes, read pool, mutate and ledger lanes, job manager, capsule/session cursors | p95 ≤ 500 ms, no wait > 5 s under 16 clients |
| 4 | per-root binary code index, lazy, no bodies | index open O(1); per-root ≤ 200 ms |
| 5 | NFS discipline, local store dir with mirror, chaos suite in the gate | chaos green; loss bound documented |

Each phase is one verified merge with its benchmark numbers in
`docs/DECISION-2026-09-19-storage-core.md`; nothing merges on a symptom fix
alone.

## Out of scope here

Billion-scale tiering, dataset registry, graph export (deferred by decision on
2026-09-18); the student distiller backend (`feat/student-productise`); the
decision layer (`feat/decision-layer`). They build on this core once it holds.
