# Storage core baseline and runtime design review

Status as of 2026-09-19: **phase 0 in progress; no implementation approval.**
The governing design is [Runtime and storage core](DESIGN-2026-09-19-runtime-core.md).
The stream merged origin/main at `f45e5958`; chitta-field is `ec22685`, including
the WAL-vanish fix. Phases 1–5 wait for the lead’s review sign-off in that design.

## Baseline protocol

The driver is `benchmarks/storage/baseline.py`. Run with the conda CPython via
`scripts/on-compute.sh -c 16 -m 64G -- <python> benchmarks/storage/baseline.py
--output <new-project-scratch-directory>`. It invokes `scripts/eval-replica.sh`
with the frozen cut as CHITTA_LIVE_MIND and a private mind and port. It never
opens the live mind. CPU embeddings, single-threaded BLAS and pinned recall
clock/embedding wait apply. Results and daemon logs stay outside Git.

The initial frozen-family load is measured separately. An explicit compact
then establishes a zero-added-WAL baseline. Synthetic 10/60/240 minutes mean
600/3,600/14,400 approximately 256-byte observations at a declared reference
rate of one observation per second, generated as fast as possible through the
RPC writer. Observations carry deterministic 768-dimensional unit vectors;
this isolates persistence from embedding inference. They may produce several
WAL records each. SIGKILL followed by harness restart prevents shutdown
snapshots from masking replay. Manifest fingerprints detect an intervening
snapshot: such a trial cannot be interpreted as the declared WAL backlog.
A heartbeat refreshes the replica quiesce flag throughout the run so periodic
maintenance cannot expire the freeze after 30 minutes. Actual segment sizes
and load phase timers are retained, not inferred from
synthetic time. One trial per backlog is exploratory, not a regression gate.

The socket and first successful health timings start at the kernel process
start time; copy time is excluded. Health probes use a 50 ms sampling interval
and a one-second timeout. Mixed traffic uses 16 clients, each issuing 20 RPCs,
with equal shares of health_check, status, recall and observe. Timings include
CLI launch/transport overhead. Errors are counted independently of latency.
The shutdown probe sends SIGTERM with compact_wal outstanding. A pending
future alone does not prove a saturated queue; daemon logs must establish
whether it overlapped snapshot work. This is not yet the full heavy-handler
queue-jam workload required by phase 3.

## Measurements

Pending the compute baseline. Do not substitute live incident numbers for
replica measurements. Artifact parent:
`/projects/caeg/scratch/kbd606/tmp/p22-storage-phase0-5emZ3u`.

The reported live incidents motivating this work are separate evidence:
17 s clean load; 295–350 s field-store open after unclean stops; approximately
34 MB WAL taking 255–315 s; queue waits of minutes. The lead’s 05:40 finding
identified a second independent startup blocker: a 1.5 GB code-navigation JSON
covering 20 roots, with inline symbol bodies, and over 14 minutes in
CodeNavigation::open → Impl::rebuild before the socket opened. That parked
file is not part of this frozen-family benchmark; do not claim its cost was
reproduced here.

## Targets retained from the design

| Metric | Required result |
|---|---|
| Socket and loading health | within 2 s of exec |
| Full readiness, ≤1 h / 24 h WAL | ≤20 s / ≤60 s |
| Replay | ≥10 MB/s of actually applied WAL |
| Shutdown under a jammed queue | ≤10 s |
| 16-client mixed traffic | p95 ≤500 ms; no request >5 s |
| Acknowledgement | durable; fsync group delay ≤50 ms |
| Crash loss | zero acknowledged writes lost |
| Code navigation | no startup dependency; root load ≤200 ms |

The prescribed 240-minute maximum baseline does not qualify the 24-hour
readiness target. Add a 24-hour-equivalent backlog before phase 2 acceptance.

## Review of the runtime design

The separation of admission, mutation, reads and background jobs addresses
both observed startup blockers and queue stalls. The following contracts need
to be explicit before implementation sign-off; these are review findings,
not changes to the approved design.

1. **Loading writes and replay order.** Bind the socket before snapshot load,
   with a minimal health/status handler independent of store locks. Before a
   valid snapshot is available, reads need a typed loading/retry response.
   During replay, a durable incoming-write spool must use a separate segment
   and sequence boundary so replay cannot ingest its own concurrent tail.
   Persist IDs, deduplication keys and ordering before acknowledgement; apply
   the historical prefix before queued writes. Bound the spool and reject
   overload explicitly. Do not report full readiness until queued writes and
   required indexes are visible. Define read-your-writes during loading.
   Risk: acknowledged writes disappear or are applied twice after restart.

2. **Acknowledgement and mirroring.** “fsync every ≤50 ms” alone does not make
   responses durable: release acknowledgements only after their group’s sync
   succeeds. Propagate failed fsync/directory sync to admission and callers.
   Copying only snapshot families from a node-local store cannot preserve
   acknowledged writes if that node is lost. Either mirror the WAL durably
   before acknowledgement, or explicitly narrow the guarantee to same-node
   process crashes and quantify the mirror recovery-point lag. Keep default
   NFS mode until this is signed off. Risk: silent loss between mirror copies.

3. **Snapshot cut and checkpoint identity.** Capture an immutable generation
   and its durable WAL high-water mark under a short writer cut-over; encode
   and sync outside the writer lane. Publish manifests only after every
   required family member and directory is durable. The shutdown marker needs
   format version, family generation/hash, segment identity plus byte offset,
   sequence number and checksum; an offset alone is ambiguous after rotation.
   Marker corruption must fall back to validated family plus WAL, not truncate
   recovery. Retain two valid families and all WAL needed by the fallback.
   Risk: a newer marker or prune hides acknowledged records from recovery.

4. **Bulk replay semantics.** Parallelize decoding but preserve commit order
   at application, including deletes, corrections, graph/event mutations and
   cross-segment dependencies. Bound decoded bytes in flight. Sidecar cache
   validity must include the family identity, codec/configuration and replay
   coverage; replaying any index-affecting operation invalidates that view.
   Benchmark deferred rebuild separately from decode/apply and end-to-end
   readiness. V23 bincode bodies are not zero-decode mmap views: the laptop
   decision requires versioned codecs/sidecars for that later capability.
   Risk: throughput improves while semantic state or the memory budget drifts.

5. **Shutdown and job ownership.** SIGTERM must close admission independently
   of the request mutex. Signal handlers only set a flag/wake the controller.
   Cap the admitted mutation backlog, bound fsync/drain errors, and make slow
   jobs cancellable; never join an uninterruptible job before the durable cut.
   An optional full snapshot needs a time budget, not just an age/idle check.
   Do not lower TimeoutStopSec until the congested-queue proof passes. NFS I/O
   stalls make an unconditional ten-second durability guarantee impossible;
   specify the tested fault envelope and surface a failed checkpoint.
   Risk: the new shutdown path waits on the same work it was meant to bypass.

6. **Request classes and reader lifetime.** Classify before dispatch; measuring
   a handler after it exceeds two seconds does not move its already-running
   work off the queue. Give every class bounded concurrency and queue capacity,
   deadlines, saturation counters and a clear retry response. Ledger mutations
   and store mutations still share durable ordering when they affect the same
   state. Audit all reachable mutable caches before lock-free reads; publish
   coherent snapshot/delta generations and bound epoch reclamation memory.
   Risk: a worker pool moves contention without removing it, or introduces
   races and unbounded retained generations under slow readers.

7. **Code navigation.** Keep all index open/migration/rebuild work off startup.
   A per-root binary index should store source fingerprints with spans and
   validate them before reading symbol text; stale spans must not silently
   return unrelated bytes. Canonicalize root identity, handle hash collisions,
   and distinguish missing roots from temporarily unavailable mounts before
   destructive pruning. Root existence checks and directory scans can stall
   on NFS, so they also belong in background jobs. Enforce RAM/disk size budgets
   and coalesce repeated rebuild requests. Risk: a lazy first query becomes a
   new queue blocker or migrations briefly recreate the 1.5 GB peak.

8. **Proof and attribution.** Retain acknowledgement IDs and verify exact
   recovered state, including updates/deletes, across crash points before and
   after fsync and manifest publication. Exercise ESTALE, disk-full and second
   writer exclusion with the existing chaos harness. Record bytes replayed,
   derived-index rebuild time, snapshot capture versus publication time,
   manifest identity, memory peak and per-class queue wait separately from
   end-to-end RPC latency. Include learn_codebase/rebuild and ledger/capsule
   traffic in the eventual jam test. Repeat matched trials on the same storage
   tier before enforcing a 20% regression threshold.

## Handoff boundary

No daemon, Rust storage, hook or unit changes in phase 0. Full implementation
and documentation of new persistence behavior follow review sign-off, not this
memo. Pending measurements and gates are explicitly incomplete.

## Phase 0 preparation checkpoint (2026-09-19)

The benchmark driver passes Ruff lint and formatting; the repository quick
gate passes, including unchanged public contracts. The full gate remains in
its native C++ build; the benchmark is scheduled after it with these worktree
binaries. No measured baseline result or performance qualification is claimed
at this checkpoint. Phase 0 remains incomplete until the replica results and
full-gate outcome are incorporated here. Runtime implementation is still
blocked on the lead's design sign-off.
