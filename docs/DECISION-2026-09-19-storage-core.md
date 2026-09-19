# Storage core baseline and runtime design review

Status as of 2026-09-19: **phase 1 merged (afda805e) and live since 09:18Z; phase 2b authorised.**
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
