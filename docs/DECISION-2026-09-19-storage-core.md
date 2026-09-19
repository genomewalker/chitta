# Storage core baseline and runtime design review

Status as of 2026-09-19: **phase 0 measured (below); phase 1 authorised.**
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

Phase 0 baseline, 2026-09-19, job 22915371 on dandycmpn21fl, frozen cut
`learning-cut-20260915-frozen` (134,805 memories), worktree `f6f395ff`
(chitta-field `c5e0ff0`). Report: `/projects/caeg/scratch/kbd606/tmp/p22b0/report.json`.

Initial start (snapshot family da86decb, empty WAL):

| Metric | Value |
|---|---|
| socket accepting | 0.54 s |
| first successful health_check | 18.1 s |
| field_store open (all phases) | 15.4 s |
| of which snapshot decode | 2.8 s |
| of which turbo (+startup) | 8.4 s (8.7 s) |
| of which emb / keyword_reverse / embed_kernel | 1.3 s / 0.9 s / 0.6 s |
| of which wal_replay | 0.03 s |

The 17.6 s between socket and health is the blackout the design's phase 1
removes: the socket is bound before `set_mind_path()` and not polled until it
returns.

Restart after synthetic writes (1 write/s of small records, 20 writes/s
sustained during the fill; same snapshot family throughout):

| WAL age | records | socket | first health = max gap |
|---|---|---|---|
| 0 min | 0 | 0.34 s | 8.0 s |
| 10 min | 600 | 0.35 s | 7.9 s |
| 60 min | 3,600 | 0.38 s | 10.1 s |
| 240 min | 14,400 | 0.30 s | 14.6 s |

Live daemon the same morning, for scale (chittad.log, restarts at 03:32Z and
04:19Z): snapshot 8.9 s / 7.5 s, WAL replay 224 s / 95 s, socket-to-serving
5.9 min / 2.3 min. The synthetic records are one to two orders smaller than
live WAL records (embeddings, transcripts), so this baseline does not
reproduce the live replay cost. Phase 2b's step 0 must replay a copy of the
live WAL family, not synthetic writes.

capsule_get p95 (30 samples per point, CLI overhead included; live ledger
count today is 364 sessions):

| rows | p95 | max |
|---|---|---|
| 123 | 14.8 ms | 15.0 ms |
| 364 | 24.0 ms | 24.5 ms |
| 1,000 | 49.5 ms | 49.8 ms |
| 5,000 | 894.6 ms | 1,287 ms |

Superlinear from 1,000 rows, consistent with the O(N²) `capsule_rows()` scan
named in the design; phase 1's server-side filtering is gated on ≤ 50 ms at
the live count and must hold the 5,000-row point under 100 ms.

Mixed traffic, 16 clients, 320 requests: p50 0.15 s, p95 2.91 s, max 5.72 s,
80 requests errored (the harness does not record the error kind; fix before
phase 4 uses this number). Clean shutdown 15.1 s with compaction pending;
forced snapshot 35.0 s.

Harness gaps found in this run, to fix in phase 2b step 0 (they do not block
phase 1): the restart trials captured empty daemon logs because the harness
reads `replica.log` from the pre-restart offset and eval-replica truncates it
on restart, so per-phase timings and the replay apply profile of the restarts
are lost; the apply profile printed nothing on the initial start (replay of
an empty WAL); mixed-traffic errors need a kind breakdown.

The reported live incidents motivating this work are separate evidence:
17 s clean load; 295–350 s field-store open after unclean stops; approximately
34 MB WAL taking 255–315 s; queue waits of minutes. The lead's 05:40 finding
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

## Phase 1 implementation (2026-09-19)

The socket responder owns bind, loading replies, steady dispatch and close on
one thread. Initialization, including set_mind_path, stays on the caller. Health
and status report loading with field_store/initializing phases; replay counts
and ETA are null when unavailable. Other requests return an error=loading with
retry_after_s=1 and isError=true. No incoming writes are acknowledged or queued.

Session rows gain repository and stream_id columns, derived from capsule
metadata during legacy journal replay and each mutation. This is an intentional
additive row-contract change; the shared row-shape fixture is regenerated.
Repository aliases normalize identically to capsule keys. Filtered indexes
limit capsule lookups to their repository and stream; manifests retain the
repository-wide view. A dedicated bounded worker handles compaction, avoiding
a join on the responder. Rust replay and the WAL format are unchanged.

The benchmark reads each truncated restart log from offset zero and records
loading phases alongside health samples. Phase 1 validation and replica
measurements are pending; this section must be completed before merge.

### Phase 1 checkpoint — not merge-ready

The first rebuilt-daemon run (`p22p1b01`) still measures a health gap of
18.585 s on initial load, 8.872 s at zero WAL age, and 8.832 s at ten minutes.
No loading health samples were observed through the CLI probe. Socket connection
logs show clients arriving during load; investigate the CLI preflight/schema
fetch and response framing before assuming the responder is working. The
sub-second availability gate fails. Remaining ages and capsule timings are
still running; no successful phase 1 performance claim is made.

The first full gate passed 36/37 C++ tests; its sole failure was the old
thread_sessions fixture. That fixture is now updated, and the full gate rerun
is in flight. Public contract checking was blocked by an unreachable live
daemon on the invocation node. Logs: `p22p1-bik5XT` under project scratch.

The checkpoint quick gate passes its other checks but fails the contracts
check. The additive `thread_sessions` row contract (`repository`, `stream_id`)
is intentional; the public contract snapshot still needs verification from a
node that can reach the daemon. This is an explicitly failing checkpoint.

### Phase 1 CLI/probe continuation checkpoint (2026-09-19)

The previous CLI-only health blackout is not a valid measurement of raw socket
availability: CLI connection preflight waited for full readiness. Startup now
records independent raw AF_UNIX and CLI series. Raw probes send one health_check
JSON-RPC line with a one-second timeout; the raw series determines the gate.
Both series include first-response delay and the final gap to readiness. Restart
logs are read from offset zero because eval-replica truncates them.

CLI preflight now accepts the warming responder immediately. Loading replies
print structured JSON and exit zero for health_check/status or 75 for other
commands, including unknown tools discovered through tools/list. The dedicated
status path also preserves loading JSON. A private fake-socket regression covers
eight direct/thin-client combinations and passed 8/8 on compute. The incremental
C++ build and focused Python lint passed. No WAL or replay changes were made.

Validation remains in flight at the mandatory thread checkpoint. Compute job
22916095 runs the complete replica benchmark, then regenerates the intentionally
changed ledger session_list filter contracts against that private replica and
runs the full gate. Artifacts: `/projects/caeg/scratch/kbd606/tmp/p22p1c-P02Ikf`;
benchmark: `/projects/caeg/scratch/kbd606/tmp/p22p1c01`. A separate checkpoint
quick gate was submitted; collect its quick-checkpoint.log. Do not merge until
raw max_health_gap_s is below one second at every WAL age, capsule_get p95 is
<=50 ms at 364 rows and <=100 ms at 5,000, and Rust/ctest/contracts gates pass.
The contract regeneration and final measured values still require a follow-up
commit; this checkpoint does not claim phase 1 completion.
