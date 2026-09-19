# Storage core baseline and runtime design review

Status as of 2026-09-19: **phase 0 measurements pending; v2 implementation authorised.**
The governing design is [Runtime and storage core](DESIGN-2026-09-19-runtime-core.md)
v2, merged from origin/main (`e7ae2aaa`) in `2683c9e0`. The WAL-vanish
baseline is chitta-field `ec22685`. Complete the baseline before phase 1;
retain shared_mutex and do not start epoch/RCU work.

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
`/projects/caeg/scratch/kbd606/tmp/p22-phase0-v2-Bjwbtq`.

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

## Review of runtime design v2

1. **Continuous readiness.** One responder must own the socket from bind through
   steady state, including set_mind_path. Only health/status are admitted while
   loading; writes return loading plus retry_after_s. No incoming-write spool
   is required. Probe throughout startup, including the final readiness gap.
   Risk: a successful early probe conceals the later code-navigation stall.
2. **Capsule lookup.** Upgrade thread_sessions repository/stream metadata and
   filter session_list before paging. capsule_save looks up once. The read-only
   live counts query returned 364 sessions on 2026-09-19. The replica curve
   includes that count when the frozen table is smaller, plus 100, 1,000 and
   5,000 rows. Each point records the actual row count and 30 missing-key
   capsule_get timings including CLI overhead. Target p95 is <=50 ms at the
   live count. Risk: an empty/frozen table hides quadratic scans.
3. **Replay profile.** CHITTA_PROFILE_REPLAY=1 counts and times each applied
   operation kind, including separately named deferred state applications.
   Covered records are excluded; decode and final index-build costs remain
   in the existing load-phase timers. Report counts and total apply_ns, not
   just a dominant percentage. Risk: instrumentation overhead and synthetic
   observations do not represent all production operation kinds.
4. **Writer and durability.** Keep shared_mutex. Acknowledgements must wait
   for successful fsync; tune adaptive group commit from measured NFS p99,
   rather than promising a fixed 50 ms interval. Snapshot publication and
   directory sync precede pruning. Recovery must retain WAL for both valid
   families. Risk: an acknowledged suffix is absent after family fallback.
5. **Background work.** compact_wal completion cannot join on the responder.
   Code navigation loads per root lazily; rebuilds run as jobs. Prune absent
   roots, store spans rather than bodies, and enforce a logged size budget.
   The parked 1.5 GB index is separate from the frozen-store measurement.
   Risk: background workers still starve short RPCs through shared locks.
6. **Local mirror.** A snapshot-only mirror does not preserve the latest
   acknowledged local WAL after node loss. Document the recovery boundary
   and measured mirror lag before offering this mode. Default NFS remains
   the durability baseline.

## Phase 0 controlled chaos comparison (2026-09-19)

On compute with TMPDIR=/tmp set inside the allocation, the requested
`chaos_partial_snapshot_preserves_acknowledged_prefix_and_replay` passed
three of three runs with replay instrumentation and three of three controls
at chitta-field ec22685 with both modified Rust files stashed. Each run
executed the named test (one passed, 304 filtered out). The stash was restored.
The earlier full-suite failure (302 passed, one failed, two ignored) is not
reproducible in isolation. These six runs do not establish its cause or prove
it pre-existing; they show no deterministic failure caused by instrumentation.
It remains recorded for the full gate; no unrelated chaos fix is included.
Logs: `/projects/caeg/scratch/kbd606/tmp/p22-phase0-relaunch-EfNgwJ/`.

The harness records continuous health gaps, per-kind apply profiles and the
capsule row-count curve. Each restart reads only newly appended daemon logs.
Replica measurements and the current full gate remain pending.
