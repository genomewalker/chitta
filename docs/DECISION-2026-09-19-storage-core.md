# Storage core baseline and runtime design review

Status as of 2026-09-20: **live since 2026-09-19 22:13Z (main 9b002e1f, store c5eec70): socket answering 14 ms after start, ready 12.7 s from process start, healthy 19.7 s from the restart command. Replica floor with the current snapshot format: store-ready 5.5-6.0 s, first recall 6.2-6.7 s; a mapped startup cache (job 22916949) passed its gates without improving it and was not committed. Next phase: zero-copy mapped memory and embedding sections (format change), designed before coded.**
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


### Phase 1 measured result (2026-09-19)

Compute job 22916095, report `/projects/caeg/scratch/kbd606/tmp/p22p1c01/report.json`,
measured the same frozen replica after the responder and session-index changes.
Raw AF_UNIX probes are the availability gate; the CLI preflight previously hid
loading replies. Socket ownership now continues from bind through steady state.

| Trial | Maximum raw health gap |
|---|---:|
| Initial clean start | 0.490 s |
| WAL 0 minutes | 0.379 s |
| WAL 10 minutes | 0.396 s |
| WAL 60 minutes | 0.383 s |
| WAL 240 minutes | 0.354 s |

All gaps are below one second, versus the phase 0 clean-start 17.6 s blackout.

| Session rows | capsule_get p95 (30 samples) |
|---|---:|
| 123 | 11.55 ms |
| 364 | 11.02 ms |
| 1,000 | 11.19 ms |
| 5,000 | 11.01 ms |

The 364-row target (50 ms) and 5,000-row target (100 ms) pass; phase 0 measured
24 ms at 364 rows, 49 ms at 1,000, and 895 ms at 5,000.

Mixed load: 16 clients, 320 requests; p50 0.131 s, p95 1.666 s, maximum 4.154 s.
There were 80 errors, all `status` requests with JSON-RPC code -32601,
`Unknown tool: status`. The harness incorrectly called a CLI-only command via
`tools/call`. These are not loading refusals or storage errors. The harness now
invokes CLI `status` and records error-kind counts. The saved latency numbers
include those errors and do not establish an error-free mixed-load gate.

The validation job failed six of 37 ctests: daemon isolation and five chaos
fixtures sent commands while loading and received immediate CLI exit 75. The
client now retries loading replies with a minimum 250 ms delay until its
`--timeout`/`CHITTA_CLI_TIMEOUT` budget (seconds; default 300 s). Health/status
still return loading immediately with exit zero; other commands print the last
loading reply and exit 75 only when their budget expires. A fake-socket test
covers both direct and thin clients receiving loading twice then becoming ready.
Final-fix checkpoint: build, CLI loading 11/11, Ruff, quick gate and contract
check pass (`contracts unchanged` after regeneration). All 37 CTests now pass,
including daemon isolation, chaos lock and chaos disk. Isolation also passes
with an invalid inherited CHITTA_SOCKET_PATH. The compute full gate has passed
its Rust and CTest stages; its hook suites are still running at this checkpoint.
The three failure causes and fixes are documented below. Final validation logs:
`/projects/caeg/scratch/kbd606/tmp/p22fix-dBboCj`; the runner writes its terminal
result to `validation.log`, and the full gate writes its result to `full.log`.

The expected public contract change is additive ledger `session_list` filtering
by `repository` and `stream_id`, with upgraded thread_sessions rows. The generic
`ledger_op` args schema may leave the captured tool schemas byte-identical;
regeneration and verification are still required. WAL format and replay are unchanged.

### Contract changes and final failure diagnosis (2026-09-19)

Loading health is availability, not readiness: callers requiring an open store
must wait until `loading` is false. The chaos harness now observes that distinction
before checking the recovered lock inode. It does not weaken the lock invariant.
Writes during load remain refused with `error: loading` and `retry_after_s`;
the CLI retries within its deadline. No test was changed to accept a lost write.

The three remaining failures had distinct causes:

- `chaos_lock_test` checked the inode as soon as loading health answered, before
  the daemon opened the store. The harness now waits for ready health.
- `chaos_disk_test` exposed a CLI bug: the loading retry helper called JSON
  `value()` on the null `structured` member of a compaction error. Object guards
  preserve the error reply; the CLI fixture covers this null error payload.
- `daemon_isolation_test` inherited the validation replica's `CHITTA_SOCKET_PATH`,
  sending RPCs to that replica while writing the queue in its own temporary mind.
  It passes in isolation; the fixture now clears the inherited socket override.
  Validation explicitly supplies an invalid inherited socket to prove isolation.

These fixes leave WAL format, replay and the additive `session_list` repository /
stream filters unchanged. The public schema snapshot is regenerated for those
filters. Validation artifacts are under
`/projects/caeg/scratch/kbd606/tmp/p22fix-dBboCj`.

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

## Phase 1 live (2026-09-19 09:18Z)

Measured from `systemctl --user restart chittad` on the live mind after the
afda805e deploy: first `loading` answer with `retry_after_s: 1` at 17.8 s
(about 15 s of it is the previous daemon's clean shutdown, phase 2a's
target), first `Status: ok` at 127.8 s (snapshot 10.4 s, WAL replay 59.4 s,
field_store 97.3 s). Before phase 1 the socket was bound and silent for that
whole window. The CLI retries loading answers until its own deadline, so
hooks and scripts see the old blocking behaviour with an honest reason.

## Phase 2b step 0: replay profile on a real family (2026-09-19)

July NFS snapshot `Index-Snapshot-Current-1783764427` (family 558a2f48,
4,995 segment files, 5.3 GB copied), replayed twice on a compute node with
`CHITTA_PROFILE_REPLAY=1` (jobs 22916102 and 22916107, store c5e0ff0):

| Item | Value |
|---|---|
| wal_replay phase | 102.8 s / 104.8 s |
| records applied | 12,234 |
| apply time, all kinds summed | 38 ms |
| of which UpdateState (11,727 records) | 7.5 ms |
| of which PutPayload / UpdateMemoryContent / UpdateSparseCode (12 each) | 11.3 / 8.9 / 9.4 ms |
| segments rejected by the lineage fence | 117, spanning 46 s of log time |

Applying records is 0.04% of replay. The other 99.96% is opening, validating
and decoding segment files that the snapshot already covers, at roughly
20 ms per file on NFS before any record is looked at. Replay cost therefore
scales with the number of files in `segments/`, not with the records to
apply. Phase 2b's target is set by this: skip every segment the manifest's
coverage already commits without opening it, decode only the tail after
coverage, and keep `segments/` small by pruning covered files after each
committed family. Acceptance: replay of this family under 2 s with the same
12,234 records applied and the same post-replay memory count.

A profiled replay of the live mind is scheduled (job 22916105) to confirm the
same shape on today's records.

## Phase 2b implementation checkpoint (2026-09-19)

Inspection of the July replica changes the proposed coverage-skip assumption:
all 4,995 segment files have different writer IDs, and both retained manifests
have empty `segments` arrays. The filename supplies a first sequence number,
not a last sequence number. Coverage of a segment's first record does not prove
coverage of its tail. The existing
`prune_covered_segments_respects_coverage_vector` regression deliberately
protects a foreign segment with sequence numbers 1 and 2 when coverage is 1.
Neither a filename-only skip nor filename-only pruning is safe on this corpus.
The manifest's coverage vector is also not a substitute for the cortical
snapshot's potentially older replay boundary.

This checkpoint instead bounds WAL decoder reads with a 64 KiB `BufReader`
in both chained replay and offset replay. It preserves decoding, CRC checks,
lineage fencing, hash-chain checks, replay ordering, and exact torn-tail repair
positions. Segment bytes, manifests, and public contracts are unchanged.
Existing writer-safe pruning is retained. This is a safe incremental change;
it does **not** implement coverage skipping or automatic post-family pruning.

A future skip path needs trustworthy per-segment end ranges and lineage
metadata committed with a family, with a validated fallback for legacy
manifests. In particular, an older full snapshot and the cortical snapshot
must both cover every skipped operation. Mutable writer tails cannot be
certified solely from their filenames. The July corpus needs a one-time
validated metadata upgrade before such certificates can accelerate a later
open; silently treating its missing ranges as covered would risk data loss.

Validation uses isolated copies made by `scripts/eval-replica.sh` from the
specified July snapshot. Each trial starts from a fresh copy, so a preceding
trial's shutdown snapshot cannot change its replay input. “Cold” below means
the first changed-binary trial and “warm” the second; neither evicts NFS or OS
caches. Logs and per-kind reports are under
`/projects/caeg/scratch/kbd606/tmp/p22b-kzF9gn/`.

Checkpoint validation: Rust release tests passed 303/303 (two ignored), the
quick gate passed, and the contract check reported `contracts unchanged`.
The first Rust launch failed before executing tests because `libopenblas.so.0`
was absent from the job's runtime search path; the successful rerun supplied
the conda library path. The full compute gate and replica timings remain
pending in compute job 22916116 (`validate.sh` in that log directory): control
replay, full gate, first buffered replay, second buffered replay, in that
order. The last observed control phases were snapshot 2,491 ms, payload
123 ms, embedding 2,775 ms, and LSH cache 521 ms; replay had not finished.
The 36,000-write durability soak has not been rerun. No under-two-second
acceptance claim, per-kind equality claim, or phase 2b completion is made.

### Phase 2b certificate foundation checkpoint (2026-09-19)

The inherited control run has completed: WAL replay 269,424 ms, field-store
open 293,325 ms, snapshot load 2,491 ms and embedding load 2,775 ms. The health
response after normalization reported 120,590 memories. It applied
12,234 records, including 11,727 UpdateState operations. The other kinds were
AddAssocEdge 296, AddTriplet 35, AnalyticsEvent 13, MsgEvent 11, PutPayload 12,
RecordRecallBatch 56, SessionEvent 19, TranscriptEvent 41,
UpdateMemoryContent 12 and UpdateSparseCode 12. This is the measured control
for the buffered comparison; the earlier 104.8-second observation is a
different run, not a matched speedup denominator.

New crate-local certificate helpers conservatively scan sealed segments and
check a recorded range against both per-writer coverage vectors and exact
file size. Scanning rejects unknown headers, foreign V3 lineage, incomplete
records, nonmonotone sequences and files changed during the scan. Inventory
paths must have the canonical segment name, and the pinned writer is excluded.
These helpers do not yet change replay, manifest commits or pruning. Their
caller must establish sealing and bind the inventory and both coverage vectors
to the validated loaded family; legacy scalar cortical coverage is insufficient.

Integration must also preserve replay ordering: the current merge carries
each writer's effective timestamp through clockless operations across segment
boundaries. Skipping a covered prefix before an uncovered tail must restore
that timestamp or conservatively decode that writer. The full-family commit
currently records only full coverage, while cortical loading independently
selects a snapshot by scalar sequence. Both loaded snapshots must be explicitly
bound before any certificate is used. Missing bindings must fall back to decode.

The helper checkpoint passed Rust release tests (306 passed, two ignored),
the quick gate and the explicit contract check (`contracts unchanged`). Tests
cover both coverage vectors, pinned writers, malformed inventory paths, foreign
lineage, torn segments and exact-size invalidation. The initial compile failed
on the private header-reader visibility; making it crate-local fixed the build.
Logs: `p22cert2-9nLC3k/rust.log` and `p22quick-e92F6n/gate.log` under scratch.
The inherited full gate has passed 37/37 C++ tests;
its hook suites and then buffered cold/warm trials remain in job 22916116.
The unchanged 36,000-write, 30-minute proof with forced compactions is scheduled
as job 22916122, artifacts `p22soak-SFHtQq` under project scratch. No certified
skip speedup, normalized-count equality or phase 2b completion is claimed.

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
capsule row-count curve. Restart trials now read their truncated daemon log from offset zero.
The completed replica measurements appear above.

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
loading phases alongside health samples. Phase 1 measurements and validation
are recorded above.

### Historical phase 1 checkpoint — before raw socket probing

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


### Phase 2b integration checkpoint (2026-09-19)

The July control took 269,424 ms in WAL replay and 293,325 ms in field-store
open. The first 64 KiB buffered trial took 99,493 ms and 123,288 ms respectively.
Both applied 12,234 operations, including 11,727 UpdateState operations; every
reported per-kind count matched. These sequential trials are not a controlled
cache-state comparison. Artifacts are in `p22b-kzF9gn` under project scratch.
The inherited full gate passed before this certificate integration.

Each new full family now binds its segment inventory, vector-space lineage,
full coverage and cortical coverage to an accompanying cortical snapshot.
Legacy manifests deserialize with empty certificates and retain full scanning.
Replay enables certificates only for the full family actually loaded and its
successfully decoded cortical snapshot. It skips a writer only if every segment
of that writer is certified and size-matched: an uncertified tail retains prefix
decoding to preserve timestamp carry-forward and hash-chain context. This is a
conservative limitation of this checkpoint, not a claim of tail-decode speedup.

Every successful full-family commit now invokes certified pruning, with both
coverage vectors, size and lineage validation, the existing sealed-file guard,
and audited directory-synced deletion. The pinned writer remains excluded.
The segment format is unchanged; the manifest gains optional family metadata.
The July forced-commit/reopen acceptance and a soak against this integration
remain required before phase 2b can be declared complete.

Checkpoint gates: quick PASS; contracts unchanged; Rust validation pending.
The initial attempts failed on Python configuration and an older conda Cargo;
the pinned run uses modern Cargo, conda Python/BLAS, and TMPDIR=/tmp on compute.
Its log is `p22integrate-3ZxynF/rust-pinned.log`. The final cortical fsync edit
may postdate compilation: rerun Rust tests on the committed tree before merge.
The inherited p21 soak reached 22,800 acknowledgements at 1,140 seconds; it
validates the pre-integration binary and does not replace a fresh soak.

### Phase 2b acceptance preparation (2026-09-19)

The second buffered July trial completed: WAL replay 101,174 ms, field-store
open 125,081 ms, 12,234 applied records including 11,727 UpdateState, and
120,590 memories. Together with the preceding 99,493 ms buffered trial and
269,424 ms control, this demonstrates the buffered-reader improvement on
these sequential runs; filesystem caches were not forcibly evicted.

Job 22916126 failed Rust validation for two unrelated fixture assumptions:
`sync_foreign_split_tests` hard-coded 768-dimensional vectors while the
default Rust build expects 1,024. The fixture now uses `crate::ops::EMBED_DIM`.
Its pinned run passed 306 tests and failed those two; two tests were ignored.
Earlier attempts also failed due to PyPy configuration and an old Cargo that
cannot read lockfile version 4. Final validation pins modern Cargo, conda
CPython/BLAS, and the daemon's 768-dimensional build identity.

`benchmarks/storage/certified_reopen.py` makes the July acceptance sequence
repeatable: copy through eval-replica, assert the legacy per-kind replay counts
and normalized memory count, force `compact_wal`, preserve the pruning audit,
then SIGKILL only its owned replica and reopen twice from the committed family.
The first legacy open must still apply 12,234 records; certified reopens must
apply zero operations already captured by that family, retain 120,590 memories,
and spend less than 2,000 ms in WAL replay. Cold/warm labels identify successive
process opens, not controlled eviction of client or NFS server caches.

Final-code full gate, July acceptance and a fresh p21 soak are submitted as
job 22916128, artifacts `p22final-8_i5xzr6` under project scratch. Results are
pending; the inherited soak validates the pre-integration binary only.

The inherited pre-integration soak (job 22916122) completed successfully:
36,000/36,000 acknowledged unique writes at 20.0/s, 30 forced compactions,
memory count 134,805 → 170,805, zero count regressions, vanished-segment lines,
or ESTALE lines. This is control evidence; the final-tree soak remains required.

Acceptance checkpoint: the final quick gate, Ruff and contract comparison pass
(`contracts unchanged`). The dimension-portable peer-sync fixture is committed
as chitta-field `0878ca7`. Job 22916128 is still running the full compute gate;
its July acceptance and final-tree soak are sequenced after a successful gate.
No phase 2b completion or sub-two-second result is claimed yet.

### Phase 2b legacy inventory correction (2026-09-19)

Job 22916128 passed the full gate and fresh p21 soak, but July acceptance
failed: the legacy open replayed 12,234 records in 104,497 ms and retained
120,590 memories; the first certified reopen applied zero records and retained
120,590 memories, but still spent about 91.1 s in WAL replay. Only three
segments were pruned. This is a failed performance gate, not completion.

Inspection found 4,736 of the July corpus's 4,995 segments are empty 56-byte
V3 headers. The previous scanner omitted these from its inventory and replay
omitted their writers from coverage. Replay now captures validated ranges
while decoding, including an empty range represented by first_seqno and
last_seqno = first_seqno - 1, and records zero coverage for empty writers.
The next family commit reuses those ranges after checking size; full and
cortical coverage still jointly bound skipping and pruning. Torn, changing,
foreign-lineage and active-writer segments do not gain a new certificate from
this scan. This avoids a second full payload scan at commit. Startup logs
report distinct segment files opened and certified files skipped, and the July
harness includes those counts in its report. No segment format changes.

The instance lock now has an explicit unlock guard, including failed-open
paths, and ChittaField releases it after shutdown flush/sync before Drop
returns. A same-process live holder is diagnosed as a self-holder bug rather
than treated as stale. Regression tests cover duplicated descriptors surviving
the guard, failed open followed by immediate acquisition, and certified empty
writers; the existing partial-snapshot chaos test has no retry added.

Validation job 22916145 stopped at two test compile errors (the new lock wrapper
and replay argument); both were corrected before resubmission. Job 22916146
validates three consecutive Rust runs, the full compute gate, July legacy /
forced-commit / certified cold and warm opens, and a new 36,000-write p21 soak.
Artifacts: `/projects/caeg/scratch/kbd606/tmp/p22cert-67j5h75x`. Results pending.

Correction checkpoint: quick gate PASS (79 pages checked, zero failures);
contract check PASS (`contracts unchanged`). Compute acceptance remains pending;
no phase 2b completion or new replay timing is claimed.

### Phase 2b acceptance correction (2026-09-19, job 22916146)

The first Rust run failed only `second_open_of_the_same_store_is_refused`:
310 passed, one failed, two ignored. The lock correctly rejected the second
open, but the new same-PID diagnostic omitted the established phrase
`another chitta-field instance`. Restore that phrase while retaining
`instance lock self-holder bug`; neither the test nor lock behavior changes.
This was a diagnostic compatibility regression, not a reproduced lock leak.

Job 22916149 reruns three consecutive Rust suites, the full compute gate,
July legacy/forced-commit/certified opens, and the 36,000-write p21 soak.
Artifacts: `/projects/caeg/scratch/kbd606/tmp/p22retry-vpgupjaq`.
Results remain pending; no acceptance claim is made by this correction.

Correction checkpoint: submodule `56386e9`; quick gate PASS; contracts unchanged.
The three Rust results and full acceptance remain pending in job 22916149.

### Phase 2b deterministic sealing fixture (2026-09-19)

Job 22916149 passed Rust run 1 (311 tests, two ignored), then failed run 2
(310 passed, one failed, two ignored). The failure at wal_certificate.rs:193
was an absent inventory entry, not an instance-lock failure. The fixture
created the closed foreign writer and the active writer within one filesystem
mtime tick. The p21 sealing fence intentionally requires a foreign segment's
mtime to be strictly older; equal timestamps must remain uncertified.

The fixture now explicitly sets equal mtimes and checks rejection, then sets
the closed segment two seconds older and checks eligibility before replay.
It retains the corrupted-bytes proof that a certified segment is never opened.
No production sealing rule, instance-lock test, or chaos test was weakened;
there is no retry or sleep in the fixture.

Quick gate PASS; contracts unchanged. Job 22916152 runs three complete Rust
suites, the full compute gate, July legacy/forced-commit/cold/warm reopen
acceptance, and the 36,000-write p21 soak on this corrected tree. Artifacts:
`/projects/caeg/scratch/kbd606/tmp/p22t-74w59g2s`. Results are pending;
phase 2b is not yet accepted. The shutdown-wait, clean-snapshot/load-path,
and lazy code-navigation follow-ups remain open.


### Phase 2b reserved sequence ranges and foreign archive (2026-09-19)

Job 22916152 passed quick/contracts, Rust 311/311 three times (two ignored),
full gate (37/37 ctests and 41 hook suites), and the 36,000-write soak with
30 compactions and no vanished segments. July failed its latency gate:
legacy replay 48.197 s / 4,995 files opened; after the forced commit,
replay 36.2 s / 280 files opened / zero certified skips. Both opens retained
120,590 memories; the legacy apply count was 12,234 (11,727 UpdateState),
and the committed-family reopen correctly applied zero records.

The failed replica's manifest contained 4,877 ranges. Full and cortical
coverage vectors were identical. Of the 280 remaining paths, 161 had a
certificate whose first_seqno was the first actual record (e.g. 157,837,517)
while the segment name/header reserved sequence 1. The validator correctly
required the reserved lower bound, so those certificates were rejected.
The other 119 paths were 117 foreign-lineage files and two empty writer
tails. This was not lagging cortical coverage or an incorrect stat path.

Inventory now preserves the header's reserved lower bound, accepting a
monotonic first record at or above it. This conservatively encloses the
actual record range and preserves filename validation, last-sequence
coverage, exact size checks and the strict sealed-writer fence. A regression
fixture reserves sequence 1 and first appends at 100; both inventory paths
must certify it, and replacing its bytes with invalid data proves a fully
covered replay does not open it. Existing lock and chaos tests are unchanged.

The first replay moves sealed V3 segments fenced by foreign vector-space
identity into segments/foreign/, with an audit line per rename and fsync of
both directories. The open writer and unsealed files remain untouched;
archive collisions fail rather than overwrite either copy. Legacy V1/V2
files still decode because they have no lineage stamp. Regression coverage
checks byte preservation, exclusion from the next replay and collisions.
No WAL format or RPC contract changed.

Job 22916159 validates this correction: Rust three times, full compute gate,
July legacy/forced-commit/cold/warm opens and a fresh 36,000-write soak.
Artifacts: `/projects/caeg/scratch/kbd606/tmp/p22fix-jvt99ial`.
Accepted 2026-09-19: legacy replay took 49.735 s, opened 4,995 files and
applied 12,234 records (11,727 UpdateState). After one forced commit,
4,877 certified files were pruned and foreign files were archived. Certified
cold/warm replay took 6/7 ms, opened only 2/3 tail files and applied zero
records; all three opens retained 120,590 memories. Rust passed 311/311
three consecutive times, the full gate passed (37 ctests and 41 hook
suites), and the durability soak retained 36,000/36,000 writes through
30 compactions. Phase 2b is complete; later restart-budget items remain open.

## Phase 2a: interruptible shutdown waits (2026-09-19)

The daemon's maintenance, backfill, distillation, hint-enrichment and ledger
loops now wait on a stop condition. The signal handler writes a nonblocking
self-pipe; a notifier thread wakes the condition variable without invoking
C++ synchronization or logging from signal context. Shutdown RPCs and the
existing maintenance restart path use the same wakeup. Every background
worker join and queue/pool drain now logs its duration. The pipe remains
open until process exit to avoid a signal writing into a reused descriptor.

Frozen-cut replicas on compute, identical delays after readiness:

| Delay after ready | Before SIGTERM to exit | After SIGTERM to exit | Normal stop before / after |
| --- | --- | --- | --- |
| 0 s, first start | 1.327 s | 1.286 s | yes / yes |
| 65 s, background loops active | 15.070 s | 1.307 s | no / yes |
| 0 s, next restart | 2.239 s | 1.282 s | yes / yes |

All twelve logged joins/drains were below 1 ms in each revised trial. The
measured process-exit maximum is 1.307 s, below the 2 s target. The control
65-second trial had no normal-stop marker; its duration matches the existing
15-second watchdog window, but its log contains no watchdog message.
These trials isolate sleeping background loops: they do not establish a
bound for an active external LLM call, heavy handler or slow NFS operation.
The existing watchdog remains; this step does not add a shutdown checkpoint
or alter persistence or RPC contracts.

Reproduce on compute with `python3 benchmarks/storage/shutdown.py --output PATH`,
where PATH is a new, short directory under project scratch. Reports include the daemon binary hash, individual
join logs and normal-stop markers. Artifacts for job 22916169:
`/projects/caeg/scratch/kbd606/tmp/p22sd-tyxcphtc`; control reports:
`/projects/caeg/scratch/kbd606/tmp/sdbtqvku14r`; revised reports:
`/projects/caeg/scratch/kbd606/tmp/sdabziyy14l`. Quick gate, Rust tests and
37/37 ctests passed. Job 22916169 completed successfully: full gate PASS,
including the hook suites; contracts unchanged. The shutdown-wait step is
validated on this replica.


## Replay-loop profiling checkpoint (2026-09-19)

The live-shaped replay regression is not yet reproduced or fixed in this
checkpoint. Instrumentation gated by `CHITTA_PROFILE_REPLAY=1` now reports
`decode_ns`, `verify_ns`, `apply_ns`, and `other_ns` per decoded operation kind.
Reader totals include covered records; `replay_apply` still counts only
operations actually applied. Sorting, segment metadata operations and profiler
overhead are outside the per-record totals and must be compared against the
whole `wal_replay` phase before attributing the regression.

With `CHITTA_PROFILE_SNAPSHOT=1`, normalization reports cache loading, vector
normalization, signatures, binary codes, ANN repair and trim/cache saving
separately. These changes do not alter replay or normalization behavior.

`benchmarks/storage/replay_profile.py --prepare --output PATH` starts a frozen
replica through `eval-replica.sh`, ingests this repository using `learn_codebase`,
and stops only its own scratch daemon with SIGKILL after the existing periodic
fsync interval. This is a workload generator, not a durability proof. Run again
with `--source PATH/m --output CONTROL` to replay a copy of that stopped store.
Reuse that source for the optimized comparison. Reports include per-kind timing,
normalization subphases, memory/code counts, daemon hash and WAL fingerprints.

Compute job 22916172 prepares and measures the control, then runs Rust tests.
Artifacts: `/projects/caeg/scratch/kbd606/tmp/p22rp-j893v8ok`; prepared source:
`/projects/caeg/scratch/kbd606/tmp/rpj893v8/m`; control:
`/projects/caeg/scratch/kbd606/tmp/rcj893v8`. Both jobs completed successfully;
the control measured WAL replay at 30,835 ms and normalization at 112 ms.


## Ingestion replay: source-file invalidation profile (2026-09-19)

The prepared frozen replica contains 226,474 decoded WAL records, including
178,864 AddTriplet and 2,370 InvalidateTripletsBySourceFile records. The full
loop profile attributes **29.763 s** to source-file invalidation alone. Across
all kinds, decode costs 0.127 s, verification 0.119 s, application 30.273 s,
and other reader work 0.214 s. This reproduces a replay bottleneck in the
cortical application path; it does not reproduce the live restart's exact
19,000-record workload or its slow normalization. Memory count is 134,805;
code context reports 104,965 symbols and 10,379 files.

The measured fix replaces the full triplet-vector scan on each source-file
invalidation with lazy source-file postings. Build once when first used,
append positions when replay adds facts, discard on compaction, index rebuild,
or preparation for serialization. Positions preserve legacy duplicate-ID
behavior and invalidation order; shared source strings avoid additional path
copies. The index is runtime-only, absent from both WAL and snapshot formats.
Its allocation is included in the memory census. The cost changes from one
whole-store scan per invalidation to one initial scan plus matching postings.

The regression test compares against the original scan across interleaved
add/invalidate operations, reused IDs, repeated and missing paths, timestamp
zero, cleanup, deduplication, and deserialization. No record reordering or
weakened CRC/hash verification is involved. Broader base-state-only replay and
post-readiness derived-index repair remain separate work; normalization was
already below its target on this corpus.

Acceptance job 22916189 compares a fresh eval-replica copy of the exact same
prepared source against the control's WAL hashes, per-kind decoded/applied
counts, memory count, and symbol/file counts, then requires replay below
3 seconds and normalization below 1 second. Rust runs three times and the
full compute gate follows. Artifacts:
`/projects/caeg/scratch/kbd606/tmp/p22ri-6s_6egtl`; candidate:
`/projects/caeg/scratch/kbd606/tmp/ri_6egtl`.

| Same-WAL measurement | Control | Indexed invalidation |
| --- | ---: | ---: |
| WAL replay | 30.835 s | 1.139 s |
| Normalize | 112 ms | 104 ms |
| Source-file invalidation apply (2,370 records) | 29.763 s | 85.5 ms |
| Decoded records | 226,474 | 226,474 |
| Memories after normalize | 134,805 | 134,805 |
| Code files / symbols | 10,379 / 104,965 | 10,379 / 104,965 |

The candidate passes both timing targets (27.1x faster replay); source WAL
hashes and every per-kind decoded/applied count match exactly. This is a fresh
replica copy of the same prepared ingestion corpus, not a certified reopening
with the tail removed. Validation: three consecutive Rust runs each passed 314 tests (2 ignored);
quick gate passed; contracts unchanged. The full compute gate in job 22916189
is still running at this checkpoint; collect it before merging.


## Phase 2a WAL-budget checkpoint — revised validation pending (2026-09-19)

A full shutdown checkpoint is rejected. Replica job 22916871 measured an
explicit family save at 44.4 seconds and a budget-triggered save at 34.5 seconds.
The 15-second watchdog interrupted shutdown at 15.1 seconds; restart replayed
805 records. Its log rejected the newest snapshot family and selected an older
family. Normalize took 21.2 seconds, Turbo 8.1 seconds and event-tape organs
5.5 seconds. These results do not establish a cache bug in a completed family;
the next experiment isolates a successful family commit before changing caches.
The accepted flush-only shutdown baseline remains 1.282–1.307 seconds.

The pending implementation checks CHITTA_CHECKPOINT_WAL_MB (default 16 MiB)
and CHITTA_WAL_SNAPSHOT_RECORDS (default 20,000) on the Rust maintenance worker.
It never initiates a budget checkpoint within 60 seconds of the last completed
family or previous budget attempt. Existing timer checkpoints remain. Monotonic
WAL accounting survives rotation and includes recovered uncovered debt. A family
acknowledges only its starting watermark after the manifest commits; writes
racing the save remain debt. Manifest errors propagate. The existing family
writer saves normalize, LSH, Turbo, organs and HDC caches. Shutdown flushes and
syncs the tail, without requesting a new family. No format change is intended.

The revised benchmarks/storage/checkpoint.py uses a 1 MiB byte budget, 160
observations, a completed maintenance checkpoint, then a state-only strengthen
write before SIGTERM and restart. Background writes remain enabled. This tests
cache reuse for a state-only tail, not arbitrary mixed writes. Acceptance checks
that replay equals the recorded post-cut WAL debt, memory counts match,
normalize is under one second, LSH/Turbo/organs all report cache hits, field_store
is under 15 seconds, and flush-only shutdown is under two seconds without the
watchdog. It preserves each daemon log before restart truncates it. Shutdown
during an already-running family save remains unverified: close still joins
that worker, so this experiment cannot establish that latency bound.

Build job 22916872 and the dependent acceptance run use logs under
/projects/caeg/scratch/kbd606/tmp/p22cpfix-nu0h4nlf. Acceptance runs Rust three
times, quick/contracts, the replica at /projects/caeg/scratch/kbd606/tmp/cpfix01,
and the full gate. Results must be collected before this implementation is
committed or accepted. Last verified Rust head: a495ecf; parent b2628282 includes
the merged upstream documentation. No new implementation commit exists yet.


## Phase 2a pre-listen index loading (2026-09-19)

The accepted checkpoint implementation is parent fad7672e / field c7a0116
(the preceding validation-pending note predates that acceptance). This step
measures each operation between FieldStore construction and normal RPC dispatch,
including constructor, set_mind_path, handler setup and Subconscious startup.
The socket retains its existing single responder owner throughout.

Repository and code-navigation sidecars now open on the maintenance thread after
the normal responder starts serving. Atomic publication gates all code-index
read and mutation handlers until both opens finish. Those handlers return
`error=loading`, `phase=code_indexes`, and `retry_after_s=1`. Recall serves its
memory results and omits optional repository suggestions during index loading.
No Rust store format or tool schema changes. Background loading still incurs
the existing parse/rebuild cost and can delay a maintenance-thread shutdown
join; this change does not establish a shutdown bound during index loading.

`benchmarks/storage/startup_indexes.py` uses eval-replica with the frozen cut
and a fixed 69,242,781-byte synthetic sidecar (4,096 symbols), since the frozen
store copy contains no code-navigation sidecar. No live index is read. Control
job 22916876 measured 2,730 ms from field ready to normal dispatch: repository
open 464 ms, code-navigation open 2,017 ms, handler construction 184 ms.
Memory count stayed 134,805. The original control assertion rejected a swap of
the top two recall results; all five returned IDs were identical. The harness
now compares nonempty sorted IDs, while preserving full-response equality as
a separate diagnostic. Deferred replica acceptance in job 22916878 passed: field-ready to serving
229 ms (down from 2,730 ms); set_mind_path 0 ms; repository/code open
487/2,029 ms after serving. The raw client observed 69 loading responses before
a successful code query returned symbols. Memory count remained 134,805; all
five recall IDs matched before, during and after loading. Recall during loading
took 665 ms versus 653 ms before restart (warm post-load recall 50 ms). Maximum
probe duration, including the one recall sample, was 668 ms. This fixture proves
the scheduling change; it does not reproduce the live index's 19-second cost.
Job 22916878 passed the quick gate, Rust tests, all 37 C++ tests, and
contract verification (unchanged). At this thread checkpoint its hook suites
are still running; full-gate acceptance is pending, not claimed. Logs are under
/projects/caeg/scratch/kbd606/tmp/p22lazy-g3a14i4h; collect full.log and
validate-job.log before merging. Shellcheck was unavailable and skipped by
the quick gate.


### Section-lazy loading prerequisite (2026-09-19)

The preceding pre-listen validation is complete: job 22916878 ended with
`VALIDATION_COMPLETE` and `full gate: PASS`, including 37/37 C++ tests and the
hook suites. Its earlier pending-hook note above is superseded.

V23 already maps the immutable full snapshot and decodes independent sections
on a four-worker pool. The outstanding delay is the publication barrier: every
section and startup sidecar finishes before `ChittaField::open` returns. Moving
only `FullSnapshot::load` to a worker would not make recall ready sooner.

This prerequisite gives the section decoder ownership of the mapping, section
ranges, and completed batch results. Batches can select sections by name without
redecoding completed sections. Applying results and reporting errors remains in
file order, including duplicate sections and the `triplets_clean` marker. Tests
exercise every truncation against the historical streaming reader, deliberately
schedule the clean marker before preceding sections, and unlink the snapshot
between batches. The production loader still finishes every batch before
publication: this commit does **not** claim section-lazy serving or the 3-second
target, and does not change the file format or RPC contracts.

Runtime integration must preserve these dependencies:

- WAL `ApplyCtx` currently borrows memory, keyword, triplet, symbol, code-file,
  association and registry state together. A deferred snapshot section cannot
  be installed over a section already updated by replay. Route each operation
  once to its owning state, or retain its bounded replay tail until that state
  is decoded; test mixed memory/code/ledger WAL against eager replay.
- Recall uses more than payloads and embeddings: acknowledgement scores,
  refreshed state and utility posteriors influence ranking; association edges
  support graph expansion. Required sections must be classified from the actual
  recall path. Optional tools must return explicit loading until their sections
  and derived indexes are ready.
- `.pld` contains the only payload content in stripped snapshots. Its existing
  corruption/missing-sidecar family fallback must precede publication of recall
  results. Late decode errors also need an explicit recovery policy; publishing
  a partially validated family must not silently bypass older-family fallback.
- Checkpointing, maintenance, mutations and shutdown must not serialize default
  empty sections while deferred sections still own durable data. Preserve the
  shared-mutex discipline and keep completion joins off the responder thread.

`benchmarks/storage/startup_sections.py` records process age from Linux `/proc`
start ticks (excluding replica-copy/setup time), first health, first store-ready
health, first recall completion and memory/recall-ID equality for three pinned
queries across restart. Raw Unix-socket calls avoid CLI preflight hiding loading.
The optional `--require-early-ready` flag enforces store-ready <3 seconds and
first correct recall <5 seconds; baseline runs report those checks independently
without claiming they pass. It uses only a private copy of the frozen cut.
Validation logs: `/projects/caeg/scratch/kbd606/tmp/p22sections-ij7w1fbj`
(job 22916881); results pending. The first attempt (22916880) stopped on a
probe portability error: this Python build lacks `CLOCK_BOOTTIME`. The probe
now reads `/proc/uptime`, on the same boot-time clock as `/proc/<pid>/stat`;
the failed attempt stopped its private replica and produced no accepted result.

The frozen-cut baseline in `p22sections-ij7w1fbj/before2/report.json` measured
process-to-store-ready **14.610 s**, first recall **15.280 s**, and **134,805**
memories before and after restart. Snapshot decoding took **2,490 ms**;
Turbo loading dominated at **8,125 ms** (startup-index phase **8,433 ms**).
The storage-persistence query returned the same five IDs before and after;
two further queries returned identical empty lists. The first assertion wrongly
required every query to be nonempty; the corrected harness requires the primary
query to be nonempty and compares all three results, including empty results.
This is a baseline, not an early-readiness acceptance result. The decoder remains
eager in production; staged publication and deferred Turbo work remain pending.

Decoder prerequisite validation: quick gate **22916883 passed**; job
**22916884** passed **15/15** focused snapshot tests, the C++ build, contracts
unchanged, and the corrected replica content comparison. Its `after3/report.json`
measured store-ready **14.860 s**, first recall **15.530 s**, and **134,805**
memories, with identical recall IDs. Full gate 22916884 subsequently passed
(37/37 C++ tests and the hook suites); collector 22916896 completed. These
numbers do not meet the early-readiness targets; production still loads eagerly.


### Staged publication: optional Turbo work (2026-09-19, validation pending)

Production `cf_open` now returns without waiting for Turbo warmup. The existing
maintenance thread captures a snapshot cache identity and mutation watermark,
then loads/prepares the index without a field lock. Publication checks the index
epoch and rejects an older result; writes during preparation remain in the delta
and are scored by the existing search path. A missing or corrupt cache uses the
owned-input rebuild path. Rust `ChittaField::open` remains eager for synchronous
store callers.

Optional startup computation owns neither the field nor its instance lock.
The maintenance thread can stop while native preparation is still running;
a late publication only touches that abandoned index slot. This avoids adding
Turbo preparation to the shutdown join budget. Focused tests exercise racing
upsert/delete, invalidation, corrupt caches and stopping blocked optional work.
The replica probe waits for complete warmup for its reference answers, compares
first-answer IDs against those references, and records deferred-phase durations.

This boundary does **not** yet implement staged durable-section publication.
All durable snapshot sections and WAL replay remain complete before store-ready,
so checkpoints cannot serialize an incomplete store. Keyword reverse, HDC,
spans, symbols and organs remain eager. Deferring those requires ordered replay
per section, capability-specific loading replies, and a checkpoint barrier until
all durable sections have been published. Sections used by recall updates must
be loaded before serving recall or merged without replacing those updates.
No early-readiness acceptance is claimed until the replica timing and ID gates
pass. Validation: job 22916901, followed by full gate 22916902, logs under
`/projects/caeg/scratch/kbd606/tmp/p22stage-10lxrehc`.

Validation checkpoint: job 22916901 passed the focused deferred-cache test (1/1) and the C++ build, then stopped on a SyntaxError in the modified benchmark harness. The syntax is corrected and parse-checked. Retry 22916904 and dependent full gate 22916902 are pending; no new replica timing or early-readiness acceptance is claimed. Logs are under `/projects/caeg/scratch/kbd606/tmp/p22stage-10lxrehc`.

Validation correction (2026-09-19): job 22916904 tested the old Rust archive.
`cargo test --release` rebuilt test executables but not the static library that
CMake links. Its 14.17 s store-ready / 14.84 s first-recall result had no deferred
phase and is not evidence for this change. Job 22916906 explicitly runs
`cargo build --release` before relinking and measuring. The probe now supports
`--require-deferred-turbo` so an eager binary cannot pass the deferral gate.


Publication checkpoint (2026-09-19): full gate 22916902 passed, including
37/37 C++ tests. That gate predates the corrected static-library rebuild;
22916906 remains the validation authority for the linked Turbo deferral, with
logs in `/projects/caeg/scratch/kbd606/tmp/p22publish-dthr6v_b`. The replica
probe now requires exactly 134,805 memories, not merely equal before/after
counts, and prints whether a deferred Turbo phase was observed.

Dependency audit for the next publication step: `recall_semantic_ctx` directly
uses payloads, states, semantic and artifact indexes, realm membership, ack
scores, recall provenance, scoring and learners. It also enqueues strengthening,
co-retrieval pairs and recall windows. Consequently, a minimal snapshot group
must include these scoring inputs, and maintenance must not drain effects into
an unpublished section. Keyword reverse postings are used by remove/update,
not just queries. Spans are separately persisted and `cf_close` flushes them;
an unloaded span store must never be flushed as empty. The pending snapshot
reader must consume each section once in file order (including the triplet
clean marker), replay each section's WAL suffix before publishing it, and keep
checkpoint/flush operations behind completion. This is an implementation plan,
not a claim that staged durable-section publication is complete.


#### Verified Turbo publication boundary (2026-09-19)

The production `cf_open` now publishes the field before Turbo preparation.
The synchronous Rust open remains eager. Maintenance captures an owned cache
load plan under a short read guard; validation and native preparation run
without that guard. Publication checks the index epoch and mutation watermark,
and changes accepted during loading remain in the search delta. Shutdown can
stop maintenance without joining blocked optional work; an abandoned job owns
neither the field nor its instance lock.

Correctly rebuilt job 22916906 measured the following against the frozen cut:

| Measurement | Eager reference | Deferred Turbo |
| --- | ---: | ---: |
| Process start to store ready | 14.86 s | 6.44 s |
| Process start to first correct recall | 15.53 s | 7.11 s |
| Memories | 134,805 | 134,805 |

All saved query IDs, including the first recall, match the eager report at
`p22sections-ij7w1fbj/after3/report.json`. Turbo completes after publication
(8,619 ms), with its native preparation taking 8,295 ms. Three consecutive
Rust runs passed 319 tests each (two ignored), including racing cache mutations,
invalidated cache plans, corrupt-cache fallback and optional-work cancellation.
Quick gate passes and contracts are unchanged. Job 22916906 completed its full
gate: 37/37 C++ tests and 41 hook suites passed (`p22publish-dthr6v_b`).
The separate strengthened reference check, job 22916911, failed: two saved
queries had empty result sets. Matching those sets does not demonstrate
positive recall coverage.

**The staged-publication acceptance is still failing:** 6.44 s exceeds the 3 s
store-ready target and 7.11 s exceeds the 5 s first-recall target. This is the
Turbo boundary only. Snapshot decode remains eager (2,173 ms, including triplet
index rebuilding at 1,452 ms and triplet deserialization at 633 ms). Other eager
phases include keyword reverse postings (788 ms), HDC (209 ms), event organs
(174 ms) and symbols (80 ms). These times overlap where workers run concurrently.

The replica harness now accepts a saved eager `--reference` and checks every
query ID against it. `--require-early-ready` requires that reference plus log
lines for Turbo, event organs, keyword reverse postings, HDC, spans and symbols;
missing publication phases cannot pass just because startup becomes faster.
Remaining durable-section publication requires an explicit readiness barrier,
ordered WAL application to deferred sections, guarded side-effect draining and
checkpoint/span-flush protection, as described in the dependency audit above.


### Consume-once section publication checkpoint (2026-09-19)

The mapped decoder now publishes a selected group only once. All bodies in a
selected group must decode successfully before any member is applied; duplicate
sections retain file order. A later decode/finish cannot overwrite a section
already changed by replay. The triplet body and clean marker form one group
because they modify the same destination. State sanitization runs when states
publish, not again after replay. Three regression tests exercise replay-update
preservation, duplicate triplet ordering, and corrupt-group atomicity. This is
the decoder prerequisite; production still waits for the remaining durable
sections. It does not meet the early-readiness acceptance by itself.

The benchmark now sends `no_learn=true` and records its recall parameters, so
its own queries do not train the store between measurements. Saved references
must match those parameters and query names. Diagnostic job 22916916 returned
five stable IDs before/after restart for each of `storage persistence`,
`WAL replay`, and `snapshot checkpoint`; those are the required positive probes.
`session handoff` and `memory recall` remain parity probes for empty results.
The diagnostic (seven queries total) exited 1 under the deliberately strict
all-positive check: three queries were empty. Its measured 7.29 s ready /
7.98 s first recall and 134,805 memories describe the existing Turbo build,
not a new publication speedup. Logs: `p22once-t4r7jlp5/probes`.

Validation job 22916915 passed: Rust 322/322 three consecutive runs (two
ignored each), quick gate PASS, contracts unchanged. Full gate 22916918 is
pending; it runs on the rebuilt library after that successful validation. A fresh eager reference
with the recorded non-learning parameters is still required for final
acceptance. Existing saved reports cannot serve as that reference.


### Keyword reverse publication checkpoint (2026-09-19, Rust and quick verified)

The pending daemon path defers keyword reverse-map construction to the existing
maintenance thread, before Turbo warmup. Forward postings continue serving BM25
reads. Preparation holds an upgradable read guard to prevent intervening writes,
builds an owned reverse map, and publishes it under a short exclusive guard.
Cancellation is checked while acquiring/upgrading the guard and during preparation;
the maintenance thread retains ownership so shutdown can join it before releasing
the store lock. The eager Rust open path continues building the reverse map inline.

Snapshot bodies omit this reverse map. Deleting or replacing a previously loaded
document before rebuilding it formerly left stale forward postings. The pending
fix scans postings for that document only when its reverse entry is missing; new
documents do not take that fallback. This preserves replay and pre-maintenance
mutation semantics, but a mutation-heavy replay still needs measurement because
repeated fallback scans can be expensive. Regression tests cover exact BM25 score
and ID parity with eager reconstruction, cancelled preparation preserving the
existing index, and same-process reopen after a cancelled startup helper.

Validation initially exposed a compile failure: `ProfiledRwLock` did not expose
`try_upgradable_read_for`, required by the cancellable maintenance publication.
Jobs 22916918 and 22916921 failed for that reason; the former still passed all
37 C++ tests and 41 hook suites. The wrapper now exposes timed acquisition of
the native upgradable guard and records acquisition latency. This preserves the
same guard through timed upgrade attempts, avoiding a mutation gap.

Retry 22916924 passed Rust three times: 325 passed, zero failed, two ignored
each (76.55 s, 76.60 s, 73.80 s). The quick gate passed and contracts are
unchanged. Final-tree job 22916925 is running the full gate and the
frozen-replica keyword/Turbo diagnostic.
Logs are under `/projects/caeg/scratch/kbd606/tmp/p22kw-i8lodn_2`; the corrected
new diagnostic directory is `/projects/caeg/scratch/kbd606/tmp/kwn249b`.
The prior build measured 7.29 s ready / 7.98 s first recall with 134,805 memories;
those are not patch results. Full-gate and timing results remain pending; no
speedup is claimed.

Production durable-section publication remains unfinished: ordered replay must
not be overwritten by late snapshot decoding, section-dependent tools need
readiness barriers, and checkpoint/span flush must wait for durable state.
Organs, HDC, spans, and symbols still require staging. A fresh eager reference
with identical non-learning probes is required before the under-3-s readiness
and under-5-s first-recall acceptance can pass.


### Staged secondary construction (2026-09-19, validation pending)

The production FFI open now publishes without building symbol postings, loading
span/HDC sidecars, or constructing the event-tape organs. Maintenance initializes
each once and logs its duration. The existing keyword reverse and Turbo work
also remain after publication. Health and embedding/LSH recall remain available;
other tool calls return `loading`, phase `secondary_indexes`, and retry-after
until these secondary stores are initialized. The additive C ABI readiness query
is `cf_startup_indexes_ready`; tool schemas and the disk format are unchanged.

Initialization owns only its baseline inputs, not the field or instance lock.
Any direct mutation or snapshot access initializes the same cell first, so a
late maintenance result cannot overwrite intervening writes. Cancellation can
drop the store while a detached initializer finishes its private inputs.

`CHITTA_STARTUP_EAGER=1` selects the eager control for same-binary replica
comparisons; production defaults to staged open. Job 22916930 measured eager
store-ready / first recall at 14.51 / 15.18 s, and staged at 5.86 / 6.52 s.
Both had 134,805 memories and identical recall IDs. All requested deferred phases
were logged. The latency targets remain unmet. The staged pre-ready snapshot
phase took 2,431 ms, embeddings 665 ms, normalization 92 ms, and PLD 81 ms.

That job passed Rust three times (328 passed, 2 ignored each), quick, and
contracts, but failed eight of 37 ctests; all 41 hook suites passed. The chaos
clients received a bare secondary-loading result outside the CLI's retry
envelope. The pending fix uses the existing structured tool-result envelope.
The global-lock fixtures now wait for startup indexes before asserting
steady-state locking; writes during secondary loading remain retryable.

The responder now binds before embedding-model initialization as well as before
store open, reporting phase `snapshot`. The benchmark separately requires a
first loading health answer within one second. Detached validation job 22916936
in `/projects/caeg/scratch/kbd606/tmp/p22detach-eim75ur8` checks the fixed tree
with Rust three times, the full gate, contracts, and the same eager reference;
its results are pending. These changes have not yet been committed.

Normalization remains before ready because it prepares LSH; PLD remains there
because it holds recall text. Full snapshot section decoding remains on the
publication path. Deferring it must preserve WAL mutations rather than replace
them with a late snapshot section; this part is unfinished.
