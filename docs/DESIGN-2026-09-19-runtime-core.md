# Runtime and storage core: design

Status as of 2026-09-19 (v2): **reviewed; phase 0 running, phases 1+ authorised
on this version.** v1 was reviewed by an Opus architecture pass on 2026-09-19;
its corrections are folded in below and marked (rev). Owner: lead session;
implementation stream `p22-storage-core`. Supersedes the ad-hoc mitigations of
2026-09-18/19 (parked endpoint files, parked `code-navigation.json`, guarded GPU
scripts, manual restarts), which stay only until the phases below ship.

## Why

The daemon is the core of chitta and it is neither fast nor solid under load.
Measured on the live store (140k memories, Isilon NFS home) on 2026-09-18/19,
with the causes as verified in code by the review:

| symptom | measurement | cause (verified) |
|---|---|---|
| startup | 17 s after a stop that happened to follow a consolidation snapshot; 295–350 s otherwise | WAL replay decodes **every segment from 0** into one vector, sorts globally, then applies (`chitta-field/src/field/opening.rs:585-600`, `log.rs:441,490-493`); coverage filtering happens after decode. (rev) There is **no shutdown snapshot at all**: `Drop for ChittaField` only flushes and syncs the WAL (`field.rs:477-483`); the only snapshot site is the sleep-consolidation timer (`chitta/src/subconscious.cpp:692-693`). |
| startup, second cause | 14+ min before any request is answered | (rev) `cmd_daemon` runs `handler.set_mind_path()` **between** the warming responder loop (`simple_cli.cpp:1939-1961`) and the request loop (`:1066`); it synchronously opens the repository index and `CodeNavigation::open → Impl::rebuild()` over every root (`field_handler.hpp:180-181`, `code_navigation.cpp:326-340`). The socket is bound but **unpolled**: clients wait in the listen backlog until their own timeout. The 1.5 GB JSON with 20 worktree roots made this window 14 min; the hole exists at any size. |
| request latency | `ledger_op` waits of 2–5 min; `session_heartbeat` 780 s; `prompt_context` 4.8 h accumulated | (rev) **Not** lock contention behind `learn_codebase` (it is lock-free by design, `field_handler.hpp:553-564`). It is an O(N²) scan: `capsule_rows()` pages `session_list` 100 rows at a time over all sessions (`field_task_ledger.cpp:141-149`), each page doing an unfiltered `select` plus a `partial_sort` over every row (`task_ledger.hpp:485`), all under the global `hook_ledger::capsule_mutex` (`field_task_ledger.cpp:139`), and `capsule_save` does it twice (`:159`, `:186`). |
| I/O thread stalls | every RPC blocked during a second compaction | `compact_wal_thread.join()` runs inline in the request loop (`simple_cli.cpp:1233`). |
| router stalls | `endpoint_list` 12+ min, RPC queue behind it | `EndpointRouter::choose → ready()` waits on `changed_` with no deadline (`llm_http.hpp:450-455`); a probe against a saturated server never returns. |
| write failures | `Stale file handle (os error 116)` until restart, three times in two days | the writer's open WAL segment was unlinked on NFS; **fixed** in `feat/wal-vanish` (merged 2026-09-19). |
| shutdown | `TimeoutStopSec=300` hit; long tail | shutdown arms `alarm(15)` + `_Exit(0)` (`simple_cli.cpp:1318-1324`), cancels it at `:1339`, then `~FieldStore` runs unbounded; `cf_close` joins `chitta-maint`, which may be inside an uncancellable Turbo rebuild (`ffi.rs:150-170`). |
| hooks | `[chitta] daemon unavailable` for the whole load window | consequence of the startup hole above; `fast_health_check_json` exists lock-free (`simple_cli.cpp:1131-1144`) but nothing serves it during `set_mind_path`. |

What already exists and is kept (rev): `rpc_mutex_` is a `std::shared_mutex`
with a shared/exclusive split (`field_handler.hpp:272,285`) and the outermost-lock
invariant across the FFI (`:255-260`); the lock profiler (`:596`); `ThreadPool(8,16,256)`,
`write_pool(2,2)`, `waiting_writes` admission, `inflight_learn_codebase`, the
`compact_wal_thread` (`simple_cli.cpp:253-260,1051-1063,1221-1239`) and the queue
processor's fast/slow lanes (`queue_processor.hpp:51-52,90`); a WAL-only sync timer
(`ffi.rs:141-157`) with `sync_wal` off the C++ lock (`field.rs:494-498`); HNSW
insertion already inhibited during replay (`field.rs:627`, cleared at
`field/opening.rs:751`); `replay_from_offset` (`log.rs:698`); `daemon_client.py:303`
already retries on `warming_up`. The design changes holders and mechanisms, not
these primitives.

## Principles

1. **The socket is owned by one responder from bind to steady state.** Health and
   status are answered continuously while loading; a client never lands in an
   unpolled backlog.
2. **Nothing unbounded runs on the request path or on the socket-owning thread.**
   Every request has a class and a budget; longer work is a job; no `join()` or
   blocking call on the I/O thread.
3. **Derived state is disposable and lazy.** Indexes are rebuilt per unit (per
   repository root, per snapshot family) in background jobs, never at startup.
4. **Durability is a policy with a stated bound.** Single writer, append-only
   segments, acknowledgement only after the op's fsync completes, size-bounded
   WAL, a shutdown checkpoint that is bounded and covered by the watchdog.
5. **Measured, not asserted.** Targets are numbers, benchmarks run on the frozen
   replica in the gate, regressions fail the merge, and any exclusive lock hold
   over 5 s in the benchmark fails it.

## Decisions (rev, taken on the review's recommendation)

- **(a) Writes during load are refused, not queued**: `{"error":"loading","retry_after_s":<eta>}`;
  the MCP client retries. Queueing would need a second durable log merged into
  `log.replay`'s timestamp+instance+seqno order (`log.rs:490`); not worth it once
  the load window is ~20 s.
- **(b) During replay the daemon serves `health_check` and `status` only.** Reads
  from a snapshot view would need a second immutable read path in Rust and an
  epoch handoff (replay mutates the same structures through one `ApplyCtx`,
  `field.rs:628-671`); that is a later phase if ever, and not what buys the win.
- **(c) Keep `shared_mutex`; fix the holders.** No epoch/RCU across the FFI. The
  gate is a hard bound on exclusive holds (≤ 5 s) from the existing profiler.
- **(d) NFS stays the default.** `CHITTA_STORE_LOCAL_DIR` ships as opt-in for the
  benchmark replica; the loss-bound wording is decided before it is offered for
  live use.
- **(e) fsync policy**: acknowledgement returns after the op's fsync; the existing
  WAL sync timer stays with an adaptive interval; p99 measured on the replica.
  No fixed "50 ms" assertion.
- **(f) Router**: the invariant is `choose` returns within 3 s (stale or empty
  allowed); the branch name is not the contract.

## Targets

| metric | target | today |
|---|---|---|
| health/status answered from t=0, continuously | ≤ 2 s after exec, never gaps | up to 15 min unanswered |
| full readiness, ≤ 1 h of WAL | ≤ 20 s | 17 s to 350 s |
| full readiness, 24 h of WAL | ≤ 60 s | unbounded |
| WAL replay | skip segments the manifest already covers; ≥ 10 MB/s on the rest (target held until the phase-0 apply profile confirms it) | decode everything, ~0.1 MB/s |
| shutdown, queue jammed | ≤ 10 s, checkpoint written, nothing acknowledged lost | 300 s timeout |
| `capsule_get` p95 at current session count | ≤ 50 ms | seconds to minutes |
| request p95, 16 mixed clients | ≤ 500 ms; no exclusive hold > 5 s | minutes |
| code index open at startup | O(1); per-root load ≤ 200 ms | 14 min, all roots |

## Architecture

### 1. Startup (phase 1)

```
t=0     bind; ONE responder thread owns the fd until steady state:
        health_check/status → {loading:true, phase, replayed, total, eta_s}; writes → loading + retry_after_s
t≈1s    open the snapshot family (mmap, sidecars) on a loader thread
        set_mind_path work (repository index, code navigation) moves OFF this path (phase 3)
t≈?     replay (phase 2b); status phase="replay"
done    hand the fd to the request loop atomically; status loading:false; maintenance starts
```

Acceptance: no gap in health responses across the whole load in the benchmark;
no `open`/`load`/`join` on the socket-owning thread (review-enforced).

### 2. Ledger (phase 1)

- Server-side filtering: `session_list` accepts `repository`/`stream_id` filters and
  a cursor; capsule operations never enumerate all sessions. `capsule_save` looks up
  once. Migration: `thread_sessions` is indexed on `{thread_id, project_dir, status}`
  only (`task_ledger.hpp:81`) and the capsule key lives in `metadata_json`; adding
  `repository`/`stream_id` columns needs a row upgrade on load, since `validate_row`
  rejects rows with a different key count (`:103`).
- Gate: `capsule_get` p95 ≤ 50 ms at the live session count; a row-count curve from
  phase 0 decides the index shape.

### 3. Shutdown and snapshots (phase 2a)

- A **bounded shutdown checkpoint**: stop admission, drain the mutate path (bounded),
  fsync and cut the WAL, write `{family, wal_offset}`; run it **before** the watchdog
  is armed, cover it with the watchdog plus margin, and add a cancellation flag
  checked inside the maintenance loop so `cf_close` never waits on an uncancellable
  rebuild. `TimeoutStopSec` drops to 60 s only after a measured checkpoint p99.
- Size-triggered snapshots on the maintenance thread: WAL bytes ≥ 16 MB or records
  ≥ 20k or the existing timer; two families kept.

### 4. Replay (phase 2b)

- Do not decode what the family already covers: use the manifest's per-writer
  `covered` vector plus segment seqno ranges to skip whole segments; use
  `replay_from_offset` for the boundary segment.
- A k-way merge over per-segment iterators replaces the global sort; derived
  indexes are built once at the end (HNSW inhibition already exists).
- Phase 0 delivers a per-op-kind apply profile first; the 10 MB/s target stands
  or is revised on that evidence.

### 5. Code navigation index (phase 3)

- One compact binary file per repository root under `<mind>/code-index/`,
  containing file hashes, symbol spans, edges, **and the BM25 postings and
  lengths** (bodies are dropped, so `terms` must be stored; IDF becomes per-root
  and the ranking change is accepted). Edge resolution already refuses to cross
  roots (`code_navigation.cpp:192,202,210,220`), so partitioning loses nothing.
- Lazy load on first query, eviction when idle, pruning of roots whose directory
  is gone, a size budget with a log line. `update()` rebuilds and rewrites only
  the touched root (today every call rebuilds and saves everything,
  `code_navigation.cpp:101-109,422,431,439`). Refreshes are jobs with progress.

### 6. Request classes and the job manager (phase 4)

- Consolidate, do not add: this phase names which of the existing mechanisms
  (`ThreadPool`, `write_pool`, `waiting_writes`, `inflight_learn_codebase`,
  `compact_wal_thread`, the queue processor lanes) each class replaces.
  Classes: read, mutate, ledger, slow, job. Budgets per class; overload returns a
  typed retry. `compact_wal` becomes a job; nothing joins on the I/O thread.
- Router: `choose` bounded to 3 s; probes are jobs with a hard timeout.

### 7. Durability discipline and proof (phase 5)

- Directory fsync after create/rename/unlink; single writer `O_APPEND`; unlink
  logging (already in `feat/wal-vanish`). Loss bound documented: nothing
  acknowledged is lost on `kill -9`.
- `CHITTA_STORE_LOCAL_DIR` for the benchmark replica only.
- Chaos in the gate: `kill -9` during write, checkpoint and replay; ESTALE
  injection; disk-full during snapshot; a second daemon opening the store.

### 8. Observability (every phase)

- `status`: loading state and phase, per-class queue depth, jobs with progress,
  last family age, WAL bytes since family, exclusive-hold p95, router state.
- `[slow]` log line for any request over its budget, with class and handler.

## Phases and gates (rev)

| phase | delivers | exit gate |
|---|---|---|
| 0 | baseline benchmark on the replica; per-op-kind apply profile; `capsule_get` row-count curve; review notes | numbers recorded in the decision doc |
| 1 | continuous warming responder; `loading` + `retry_after_s`; ledger server-side filtering with the row upgrade; no join on the I/O thread | no health gap in the benchmark; `capsule_get` p95 ≤ 50 ms |
| 2a | bounded shutdown checkpoint under the watchdog; maintenance cancellation; size-triggered snapshots | shutdown ≤ 10 s with a jammed queue; checkpoint p99 measured; then `TimeoutStopSec=60` |
| 2b | replay skip-by-coverage; k-way merge; deferred index build | readiness ≤ 20 s with 1 h WAL, ≤ 60 s with 24 h |
| 3 | per-root binary code index with stored postings, lazy, pruned, per-root updates | index open O(1); per-root ≤ 200 ms; `learn_codebase` touches one root |
| 4 | request classes and job manager consolidating the existing lanes; router bound | p95 ≤ 500 ms, no exclusive hold > 5 s under 16 clients |
| 5 | durability discipline, local dir opt-in for the replica, chaos in the gate | chaos green; loss bound documented |

Each phase is one verified merge with its benchmark numbers in
`docs/DECISION-2026-09-19-storage-core.md`; nothing merges on a symptom fix alone.

## Out of scope here

Billion-scale tiering, dataset registry, graph export (deferred 2026-09-18); the
student distiller backend (`feat/student-productise`); the decision layer
(`feat/decision-layer`). They build on this core once it holds.
