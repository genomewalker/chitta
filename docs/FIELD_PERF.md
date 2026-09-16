# chitta-field performance

### Phase 3 source-index measurement (2026-09-16)

Paired private copy of `learning-cut-20260915-frozen`, family `da86decb`:
current-truth rose from 20/50 (visible 11/30, holdout 9/20) to 36/50
(20/30, 16/20). Environment-variable and hook-location classes improved;
the 40/50 gate remains unmet. Original five probes are preserved verbatim in
`benchmarks/current_truth/original_probes.json`; its explicit deterministic
rubric scored 0/5 before and 3/5 after, distinct from the historical human
verdict of 2/5. The reconstructed frozen probes remain a separate report.
Golden nDCG@20 fell from 0.480335 to 0.449095 (three runs each), outside the
paired baseline noise band. Source rows occupy result slots previously used by
memory IDs, which this golden panel grades; this is still a failed gate.
Baseline ordered restart identity was 16/20, and baseline prompt median was
935 ms with a 354 ms noise allowance. These are measurements, not a claim that
Phase 3 is qualified for deployment.
The indexed prompt median was 958 ms (+23 ms, inside that allowance).
Verification: 26 CTests, 149 MCP tests (explicit stub binary), 46 SMRITI tests
and all hook scripts passed. Replica chaos passed eight cases; its NFS lock
case was skipped on the initial XFS scratch directory and must be rerun on NFS.

### Phase 3 anchors and final paired checks (2026-09-16)

The derived Rust anchor index adds no snapshot fields. Lifecycle tests cover
foreign replay, load, changed/missing files and forgetting. A private-daemon
fixture also passed current-state rendering, missed-watcher detection,
same-scope supersession, independent scopes and delayed old events.

On the same `da86decb` copy, current-truth remains **36/50** versus **20/50**:
visible **20/30** versus **11/30**, holdout **16/20** versus **9/20**. The 16
gains comprise seven environment-variable/default questions, seven hook
questions, one operational-safety question and the rename date; no question
regressed. Original strict probes are **3/5** (deploy, PostToolUse and field
performance); reconstructed frozen probes are **1/5**. Neither probe rubric is
the historical memo's human useful-hit judgment.

Final-binary golden nDCG@20 over three runs was **0.452860, 0.449226,
0.449226**, mean **0.450437**, below the paired baseline mean **0.480335** and
its noise band. The final fixed-source ordered restart comparison was **16/20
before and 15/20 after** (same queries, script and replica copy), failing the
replacement gate. Earlier comparisons scored 13/20 while sources were changing
and 16/20 before the final learning-order fix; those results are retained rather
than selecting the passing repeat. Prompt median was **965 ms** versus
**935 ms**, within the baseline's 354 ms allowance. The preceding binary's
three-run golden mean was 0.445206 and prompt median was 893 ms.

The complete final NFS chaos run passed **9/9**, including the unchanged 8-second
hook deadline, with all three thread caps set to one. The initial XFS run's
NFS skip was also rerun alone on NFS and passed. Rust release tests: **291
passed, 2 ignored**; CTest **26/26**, MCP **149**, SMRITI **46**, hooks **27/27**.
Format ID remains `9230643459983636874`; the embedding identity sidecar remains
absent. Current-truth, original probes, golden recall and restart identity
remain unqualified. Co-retrieval learning runs after source merging, so source
IDs and displaced memory IDs do not receive memory co-occurrence updates.

Continuation qualification is also unmet: the authorized transcript directory
contains two conversational sessions and one title-only file. The construction
script produced one consecutive pair; the strict production-selector replay
scored **0/1**, short of **18/20**. Builder/scorer tests and the synthetic ledger
capsule checks pass, including short completion invalidation, but do not replace
the missing real cases. The extraction/scoring rule is documented in
`docs/HOOKS.md`; transcript data and fixture output are outside the repository.

## Acknowledged-write durability

Status 2026-09-16 (Phase 6 source audit). A successful response is not a universal
fsync guarantee. The dispatcher in `field_handler.hpp` calls `FieldStore::sync()`
after exclusive write handlers release the C++ mutex. It bypasses that call for
lock-free/subprocess handlers. The inventory below covers all 188 registered
handlers outside its read-only set, including aliases and conservative write
classifications. A read-only operation within `ledger_op` skips the sync.

| Write RPCs | Acknowledgement boundary |
| --- | --- |
| `compact_wal` | Completed snapshot/manifest commit and covered-WAL compaction; handler checks the result. |
| `learn_codebase`, `distill_now`, `consolidation_pass` | No dispatcher fsync. WAL-backed changes use append/OS flush and the WAL maintenance timer; snapshot-only state changed by these operations still needs a snapshot. Preview/disabled/no-op calls may write nothing. |
| `log_event`, `log_event_ex`, `log_decision`, `predicate_run` | Primary tape/predicate state is snapshot-resident, not a WAL-append acknowledgement. Incidental WAL-backed side effects use timer sync. Do not treat these as durable event-ledger APIs. |
| `ack_memory`, `add_delegation`, `add_observation`, `add_probe`, `agent_disable`, `agent_upsert`, `anticipation_observe`, `anticipation_record_outcome`, `anticipation_success`, `approve_memory`, `assert_fact`, `assoc_decay` | Dispatcher calls WAL fsync before returning. |
| `attach_debt_evidence`, `batch_forget`, `branch_create`, `branch_resolve`, `calibration_record`, `checkpoint`, `cleanup`, `clear_codebase`, `close_intervention`, `close_rederive`, `compact_context`, `connect` | Dispatcher calls WAL fsync before returning. |
| `connect_temporal`, `consolidate_similar`, `consolidation_auto`, `consolidation_merge`, `create_episode`, `curiosity_note_gap`, `curiosity_resolve`, `cycle`, `dedupe_symbols`, `defer_debt`, `densify_backfill`, `describe_symbol` | Dispatcher calls WAL fsync before returning. |
| `disable_source`, `distill_set_model`, `dream_cancel`, `dream_force_woke`, `dream_start`, `dream_wander`, `embed_symbols`, `enroll_wisdom_lineage`, `executor_flush`, `export_soul`, `export_training_pairs`, `file_index_all` | Dispatcher calls WAL fsync before returning. |
| `file_index_session`, `file_restore`, `flush_embeddings`, `forget`, `forget_kind`, `full_resonate`, `goal_complete`, `goal_progress`, `goal_set`, `grow`, `habit_observe`, `habit_strengthen` | Dispatcher calls WAL fsync before returning. |
| `habit_weaken`, `health_check_start`, `hygiene_run`, `impl_start`, `import_soul`, `ingest_source`, `insight_promote`, `learn_outcome`, `ledger_append`, `ledger_compile`, `ledger_delete`, `ledger_op` | Dispatcher calls WAL fsync before returning. |
| `ledger_save`, `lineage_expiry_check`, `link_evidence`, `log_exposure`, `long_task_complete`, `long_task_event`, `long_task_start`, `long_task_update`, `mark_memory_invalidated`, `memory_lock`, `memory_outcome`, `memory_unlock` | Dispatcher calls WAL fsync before returning. |
| `msg_ack`, `msg_ack_all`, `msg_respond`, `msg_send`, `nack_memory`, `narrative_log`, `observe`, `pending_embed_ids`, `pin_memory`, `predicate_attach`, `probe_calibrate`, `probe_seed` | Dispatcher calls WAL fsync before returning. |
| `profile_observe`, `profile_update`, `promote_memory`, `propose_change`, `prune_episodes`, `queue_experiments`, `realm_add`, `realm_remove`, `realm_set`, `realm_visibility`, `rebuild_fts_index`, `reconcile_pass` | Dispatcher calls WAL fsync before returning. |
| `reconsolidate`, `record_attribution`, `record_feedback`, `record_surprise`, `reembed_memories`, `register_debt`, `register_task`, `reject_memory`, `remap_realms`, `remember`, `remember_batch`, `repl_execute` | Dispatcher calls WAL fsync before returning. |
| `repl_session_delete`, `repl_session_set`, `resolve_contradiction`, `resolve_debt`, `resolve_merge`, `resolve_probe`, `restore_code_intel_confidence`, `retract_fact`, `sadhana_checkpoint`, `sadhana_pause`, `sadhana_resume`, `sadhana_set_goal` | Dispatcher calls WAL fsync before returning. |
| `sadhana_set_interval`, `sadhana_set_max_turns`, `sadhana_set_model`, `sadhana_start`, `sadhana_stop`, `save_spectral_snapshot`, `seed_hdc_geometry`, `semantic_backfill`, `session_deregister`, `session_heartbeat`, `session_register`, `session_sync` | Dispatcher calls WAL fsync before returning. |
| `set_affect`, `set_criterion`, `set_evidence_type`, `set_memory_type`, `set_priority_tier`, `skill_deprecate`, `skill_upload`, `span_backfill`, `span_backfill_memories`, `stageb_set_surface`, `start_intervention`, `strengthen` | Dispatcher calls WAL fsync before returning. |
| `suggestion_resolve`, `suggestion_track`, `tag`, `think_wander`, `tick_lineage_staleness`, `transcript_register`, `transcript_remove`, `transcript_update`, `transition_wisdom_lineage`, `trigger_add`, `trigger_dismiss`, `trigger_fire` | Dispatcher calls WAL fsync before returning. |
| `trim_realm_names`, `triplet_supersede`, `unpin_memory`, `update`, `update_scorer_model`, `update_source_weight`, `update_task`, `update_wisdom_lifecycle`, `upsert_wisdom_candidate`, `weaken`, `wiki_export`, `witness_memory` | Dispatcher calls WAL fsync before returning. |

For the dispatcher-synced rows, this protects WAL-backed mutations on a healthy
filesystem, not arbitrary C++ configuration, subprocess results, exports or
snapshot-only organs. `FieldStore::sync()` currently discards `cf_sync`'s error
code, so an I/O failure can still produce an apparent success; correcting that
API is outside this phase's file scope. The WAL sync interval defaults to
`CHITTA_WAL_SYNC_MS=200` ms. This is a scheduling interval, not a hard bound
during I/O stalls.

`scripts/stress-embed-recall.py --durability-test --mind PRIVATE_COPY --label kill
--output /tmp/p6-kill.json` acknowledges a unique `remember`, kills that checked
scratch process, restarts the **same** copied store, and asserts the memory is
retrievable by ID. A process SIGKILL does not evict the kernel page cache and
cannot establish power-loss safety; the timer-synced case must not be asserted
lost. Never use `eval-replica.sh start` between ack and verification: it replaces
the copy from its source and would invalidate this test.

## Phase 6 follow-up verification (2026-09-16)

Base: merged `main` at `fd2797f1`; Rust submodule `5c5b5dd`. Native GGUF
embedding remains 768-dimensional nomic-embed-text-v1.5, four contexts, one
opt-in document worker, and `CHITTA_RUNTIME_LOCAL=1`. The private NFS copy was
selected from learning-cut-20260915-frozen: family `da86decb`, manifest generation
39285, snapshot sequence 206502617. This differs from the earlier Phase 6 copy;
these measurements are a new run, not a controlled before/after comparison.

The timing runner waited until native tests, Rust tests and chaos finished.
The node's reported load averages at measurement were 86.89/89.88/84.34.

| Measurement | Result |
| --- | --- |
| Remember writes | 200/200; zero errors |
| Pending embeddings after / drain time | 0 / 64.44 s |
| Idle recall p50 / p95 (40 samples) | 88.9 / 134.1 ms |
| Recall during writes p50 / p95 (18 samples) | 77.0 / 113.9 ms |
| Full loaded-window recall p50 / p95 (590 samples) | 78.7 / 162.8 ms |
| Remember acknowledgement p50 / p95 | 986.1 / 1636.8 ms |
| Cached restart | 16.098 s; 5 s gate unmet |
| Distinct-query ordered IDs across restart | 20/20 |
| Fixed-query full response / ordered IDs across restart | 20/20 / 20/20 |

The full-window 150 ms stress gate **failed**; the write-active subset alone
passes. No runtime-placement or worker default was enabled.

Validation: frozen-replica chaos **9/9** (361.985 s summed case time), CTest
**25/25** (103.35 s, real-model pool test included), Rust **289 passed / 2 ignored**,
hooks **23/23**, MCP **149**, SMRITI **46**, and contracts unchanged before commits
and after stress. Shell syntax, warning-level ShellCheck and touched-Python Ruff
checks pass. The first CTest attempt failed with an older `python3` selected by
`/usr/bin` in synthetic chaos helpers; explicitly configuring CMake with Conda
Python fixed this. Its queue-log assertion and registration timing failure did
not recur in the corrected run; the standalone queue-isolation rerun also passed.

Timestamp checks covered 205 mixed C++/C/raw-fd/concurrent/partial lines and
5,528 nonempty daemon log lines, all dated. A real scratch daemon refusing a
foreign recorded lock holder produced one dated cross-host incident. The soak
remains unestablished. Raw JSON/logs remain untracked under `results/followup/`
and `/tmp/p6-*`; all scratch daemons were stopped.

Remaining work: atomic queued mutation plus `ack_id` receipt requires a store
transaction API; Phase 7's ledger was retained without duplication. Automatic
cross-node client fallback was deferred as allowed: safe completion also needs
MCP's bridge, CLI and socket-only helpers, including suppression of secondary-node
local starts (transport diff: zero lines). See `docs/CLI.md` for the requested
primary-node guard and its service-manager restart limitation. Shared lifecycle
and FileChanged markers still reside on NFS. Phase 6 is not complete.

## Phase 6 measurements and remaining gates

Status 2026-09-16: single sequential control/treatment runs on separate private
NFS copies of family `bbcaed33`, generation 38047. Both use the local
768-d nomic GGUF with four contexts (`CHITTA_WITH_LLAMA_CPP=ON`), with runtime
placement off in both arms to isolate worker scheduling. Both admitted all 200
concurrent remembers, reported zero RPC errors and reached zero pending
embeddings. The 90-second post-ack sampling period includes asynchronous
embedding work; the treatment backlog drained at 62.4 seconds. These are
descriptive runs, not a noise-calibrated causal claim. Preliminary Ollama
fallback runs were discarded after correcting a CMake compiler-reset issue.

| Measurement | Default workers (off) | One bounded document worker |
| --- | ---: | ---: |
| Idle recall p50 / p95, 40 samples | 92.0 / 123.2 ms | 79.2 / 105.7 ms |
| Recall during writes p50 / p95 | 124.4 / 624.7 ms (n=3) | 75.2 / 128.1 ms (n=15) |
| Full loaded-window recall p50 / p95 | 78.2 / 139.0 ms (n=837) | 76.0 / 137.7 ms (n=863) |
| Remember acknowledgement p50 / p95 | 430.9 / 754.5 ms | 787.8 / 1387.6 ms |
| Cached restart to successful recall | 20.608 s | 20.001 s |

The recall target passes in the treatment run; the small write-active sample
counts limit percentile precision. Write acknowledgements are slower. The
cached-start ≤5 s exit gate **does not pass**. The historical 9.5 s operational
figure below was not reproduced by this worktree on these copies.
`benchmarks/field-perf/run.sh --restart --mind PRIVATE_COPY --label LABEL
--output /tmp/restart.json` measures existing-copy cached readiness, rejects
`warming_up` replies, and compares 20 distinct queries before/after restart.
`scripts/stress-embed-recall.py` multiplexes 200 concurrent remember RPCs over
one connection (the daemon accepts at most 32 connections), samples recall on
another connection, and reports both the write-active and full sampling windows,
errors and remaining pending embeddings. It never accepts warm-up replies as
successful writes and keeps sampling until embeddings drain (300-second limit)
and the minimum post-ack interval elapses. Raw output belongs in /tmp or
untracked results, never git.

Validation on 2026-09-16: release Rust 285 passed (2 ignored), unchanged
768/nomic/format-1 identity, all 16 real-GGUF CTests, 149 MCP tests, 46 SMRITI
tests, all 21 hook scripts, CI ruff and touched-shell checks passed. The final
contract snapshot printed **contracts unchanged**. Focused
fixtures cover both placements, queue alias precedence, bounded admission,
reader progress, and ledger append-before-offset crash recovery. An actual
hook-to-daemon probe checkpointed a local tail to the private NFS ledger with
its offset. An acknowledged durable remember survived immediate SIGKILL on the
same copy; a timer-synced symbol also survived, which is permitted.

The broader 20-distinct-query restart diagnostic is **not consistently stable**:
initial pristine GGUF control/treatment both matched 20/20, the final pristine
treatment matched 16/20, and post-load repeats matched 12/20 with workers off and
11/20 with workers on/local placement. These failures are reported rather than
counting only passing attempts; the default-control repeat does not isolate a
worker regression. A further treatment repeat matched 18/20. Universal ordered-ID
identity remains unestablished. Separately, the established fixed query
`chitta recall performance lock contention`, realm `project:cc-soul`, limit 5,
`no_learn=true`, passed **20/20 full-JSON response pairs** across restart with
workers off and with one bounded worker/local placement on. The restart runner
reports both the fixed-query gate and the broader diagnostic; the former does
not imply that all queries are stable.

Runtime placement remains default-off. Queue recovery is currently checkpointed
at-least-once: no atomic store API couples an arbitrary queued mutation to its
`ack_id` receipt. Exact idempotence across a crash in that gap remains an unmet
Phase 6 gate; Phase 7's applied-ack sidecar does not close the mutation/receipt
crash gap. The follow-up migrates transient markers in prompt-core, SessionStart,
stop-core and pre-tool-hook (see `docs/HOOKS.md` for the groups). Files shared
with other lifecycle hooks, and file-changed-hook's `.reindex_*` marker, remain
on NFS; file-changed-hook is outside the follow-up scope. Do not enable local
placement as a completed migration until those gates are resolved.

The fortnight instrument is `scripts/report-runtime-incidents.py chittad.log`.
It reports open failures, stale-lock replacements, cross-host lock holders,
repeated starts within five minutes, and lockprof holds strictly over 150 ms.
Daemon stderr now carries ISO-8601 UTC timestamps, including Rust/C diagnostics.
Use `--host` when analyzing logs from another host. Undated records remain
`undated`; a log with no matches does not prove a fortnight of coverage.

Status as of 2026-09-16. The dated results below preserve the `fix/field-perf` measurements on private eval copies, including unmet targets and the ancillary MCP SDK failure. Two original JSON artifacts are absent; their committed tables are linked instead. Current live startup is about 9.5 s with sidecar hits, about 20 s on the first start after deployment or a format change; see [startup and recovery](CLI.md#startup-sidecars-and-instance-lock). These current operational figures do not replace the historical control/experiment measurements below.

## 2026-09-16: Phase 7 recovery gates

Final authorized fixes: **9/9 full-copy chaos cases passed**, **25/25 CTests
passed** (100.33 s), and **289 Rust library tests passed, 2 ignored** (53.26 s).
Production commit: `b4c06294`; Rust submodule: `765b68e`.
[Implementation and touched functions](../Documentation.md) describe the merge
boundary; [dated JSON evidence](chaos-2026-09-16.json) preserves both failures
and final results.

| Case | Result | Native fixture recovery | Full replica recovery |
|---|---|---:|---:|
| SIGKILL during save | PASS | 1.129 s | 19.957 s |
| Second opener | PASS | 1.193 s | 1.221 s |
| Unlinked active WAL | PASS | 1.187 s | 11.868 s |
| Stale / foreign lock on NFS | PASS | 1.178 s | 11.752 s |
| ENOSPC during save | PASS | 1.240 s | 60.998 s |
| Queue processing replay | PASS | 1.166 s | 12.354 s |
| Restart during prompt hook | PASS | 0.503 s | 0.894 s |
| HTTP MCP session restart | PASS | 0.002 s | 0.002 s |
| Binary format probe under load | PASS | 0.016–0.024 s | 0.021 s |

The WAL case restored **19,622 bytes** on the full replica (**733 bytes** on the
native fixture), including a WAL-only pre-unlink memory. Both repeated queue
replays preserved exact identity and state: **11.472 / 12.354 s** on the full
copy and **1.150 / 1.166 s** on the native fixture. The queue proof also checks
same-batch duplicate IDs, pruning on the next batch, and ack retention for
coalesced saves. Its test helper disables decay for that one memory through the
existing state-update FFI while the scratch daemon is stopped; this keeps the
`get` view's wall-time-dependent strength stable without an equality tolerance.

These are single shared-node observations; CTest and the final full-copy run
used independent scratch stores concurrently. Recovery includes startup and
invariant readback; ENOSPC includes the later successful save. Hook time measures
the interrupted hook's exit, MCP time measures the stale-session call after
listener readiness, and format time is the maximum of three real-model probes.
The ledger survives the tested post-ack crash and snapshot compaction; it is not
a transaction spanning store mutation and ack publication. The separate WAL-only
confidence reconstruction diagnostic is documented in the implementation report.
No services, installed binaries, source corpus, frozen evaluation files, or
embedding identity were changed. The independent canary soak remains outside
this completed recovery gate.

### Original failure evidence

Initial tests on `feat/chaos-tests`, before the authorized production fixes,
exposed two defects. The original failure evidence follows. Cargo release: **288 passed, 2 ignored**. Native CTest:
**23/25 passed**, with `chaos_wal_test` and `chaos_queue_test` failing. MCP (149),
SMRITI, every hook fixture, CI Ruff check/format, and touched-shell parse and
ShellCheck gates passed. No production recovery logic had changed at that point.

Each CTest case uses its own 128-memory native snapshot family, copied by
`eval-replica.sh`. The full-corpus runs use a separate NFS copy of `bbcaed33`,
private ports/sockets and isolated hook HOME. Subsequent starts reopen the
faulted copy; they never refresh it from the source. All process kills target
captured scratch PIDs. The HTTP test restarts an owned MCP process, not a live
systemd unit. CI reports NFS-only lock tests as skipped on non-NFS filesystems.

| Case | Invariant / outcome | Native fixture recovery | Full replica recovery |
|---|---|---:|---:|
| SIGKILL during save | PASS: every acknowledged ID/content recovered; manifest unpublished at kill | 1.124 s | 21.918 s |
| Second opener | PASS: refused with recorded holder PID | 1.192 s | 1.128 s |
| Unlinked active WAL | **FAIL: an acknowledged post-unlink write disappeared** | ready 1.074 s, invariant failed | daemon-ready 9.342 s, invariant failed |
| Stale / foreign lock on NFS | PASS: dead same-host holder replaced on a new inode and logged; foreign holder refused | 1.114 s | 41.560 s |
| ENOSPC during save | PASS: manifest unchanged, acknowledged prefix recovered, next save succeeded | 1.214 s | 55.775 s including successful resave |
| Queue processing replay | **FAIL: replay created a second memory with identical content after a snapshot** | ready 1.110 s, invariant failed | ready 10.592 s, duplicate IDs |
| Restart during prompt hook | PASS: fails open within external 8 s budget; next hook completes recall | 0.485 s | 1.071 s |
| HTTP MCP session restart | PASS: stale session returns explicit 404 within 5 s; new session/tool call succeeds | 0.001 s | 0.002 s |
| Binary format probe under load | PASS: production `spawn_format_id_probe`, four real GGUF embedding workers and an atfork stall sentinel | 0.02016–0.02029 s (<1 s) | same native process test |

Timings are single observations, not latency distributions. Second-opener time
includes a readback; hook time measures the interrupted hook's exit, and MCP time
measures the next call after the replacement listener is ready. Full-replica WAL
9.342 s is the daemon's logged startup time, not successful durable recovery.
The lock test holds the old inode while recording a verified dead PID: it models
a retained NFS server lock; merely writing an unlocked stale file would not
exercise the recovery branch.

`save_fault_test.cpp` is a test-only preload library, never linked into release
executables: `CHITTA_CHAOS_STORE` scopes interception to one scratch store;
`CHITTA_CHAOS_PAUSE_ARM` stops at a temporary snapshot write, and
`CHITTA_CHAOS_ENOSPC_ARM` returns real ENOSPC from that write and records a hit.
This run used that deterministic error seam, not a privileged tmpfs/loop mount.

The NFS failure is consistent with silly-renaming: the open unlinked inode can
retain a nonzero link count, so append misses deletion; sync detects the missing
path only after the first new operation was written to the abandoned inode.
Queue replay is checked by exact identity **and state**, not only daemon health:
a weaker ID-only check hid a confidence change; snapshotting before replay then
exposed duplicate IDs. These failures originally exceeded the tests-only scope;
the user subsequently authorized both production fixes. The independent week-long
canary soak remains outstanding. The original failure assertions are retained.
See [implementation and proof boundaries](../Documentation.md) and
[machine-readable evidence](chaos-2026-09-16.json).

## Measurement boundary

The supplied live baseline (134,129 memories, 7.95 GB RSS, 62 s first recall, hybrid p95 not supplied) is context, not the control arm. The scratch family is `bbcaed33`, generation 38047, snapshot sequence 206208172, with its selected WAL. It reports 133,724 records (133,712 live in detailed health), approximately 1.37M triplets, and 768-dimensional `nomic-embed-text-v1.5` embeddings. Both primary arms load the same validated family. Autonomous work is quiesced; this does not reproduce live writer contention.

`benchmarks/field-perf/run.sh` copies the manifests, selected snapshot family, WAL, migration markers and lite encoder according to `scripts/eval-replica.sh`, validating selection before and after copying. Each child owns a private store, runtime directory, socket and RPC port with `CHITTA_NO_QUEUE=1`. The runner only terminates its own child. It leaves the copy available for audit and never opens a daemon on the source.

Measurements use socket JSON-RPC, excluding CLI process-launch overhead. Readiness is the first non-warming health response; the daemon also logs process-start-to-dispatch time. First recall is issued immediately, before a common 30-second settling interval. Hybrid runs before other timed workloads so they cannot consume its refresh backlog. Each workload has 20 repetitions; p95 is nearest rank (19th sample). Query: `chitta recall performance lock contention`; realm: `project:cc-soul`; limit: 5; `no_learn=true`. Lanes: sem, ctx, hyb, kw, corr. Both primary arms enable profiling. Results retain IDs and schema shapes, never memory text.

The control is `e109286` chitta-field / `f8686fed` superproject plus load/recall timers and allocation diagnostics, measured before optimizations. Wall-clock performance varies with this shared node's load. A single 20-sample workload is evidence for this replica/query, not a general latency guarantee.

## Before and after

Primary control: [BEFORE JSON](../benchmarks/field-perf/results-before.json). Primary final implementation: [AFTER table, label `primed`](../benchmarks/field-perf/table-primed.md). The original primed JSON is not present in this checkout; the committed table preserves its reported measurements. Earlier `after` / `final` artifacts are intermediate steps, identified below.

| Metric | before |
|---|---:|
| Ready (ms) | 35928.8 |
| First recall (ms) | 6440.6 |
| RSS (MiB) | 4322.4 |
| hybrid p50 / p95 (ms) | 33.7 / 245.1 |
| keyword p50 / p95 (ms) | 6.3 / 8.5 |
| fused p50 / p95 (ms) | 33.6 / 42.4 |
| smart p50 / p95 (ms) | 11.9 / 17.8 |
| recall_lanes p50 / p95 (ms) | 33.6 / 35.6 |
| Load snapshot (ms) | 3657 |
| Load pld (ms) | 120 |
| Load emb (ms) | 222 |
| Load lite_encoder (ms) | 15 |
| Load hdc [10](#ref-10) (ms) | 247 |
| Load triplets (ms) | 3665 |
| Load turbo (ms) | 5926 |

| Metric | primed |
|---|---:|
| Ready (ms) | 28712.3 |
| First recall (ms) | 92.4 |
| RSS (MiB) | 3220.2 |
| hybrid p50 / p95 (ms) | 34.7 / 64.1 |
| keyword p50 / p95 (ms) | 6.3 / 6.8 |
| fused p50 / p95 (ms) | 34.1 / 35.8 |
| smart p50 / p95 (ms) | 12.2 / 18.0 |
| recall_lanes p50 / p95 (ms) | 35.0 / 41.6 |
| Load snapshot (ms) | 3982 |
| Load pld (ms) | 131 |
| Load emb (ms) | 251 |
| Load lite_encoder (ms) | 22 |
| Load hdc (ms) | 276 |
| Load turbo (ms) | 6456 |
| Load triplets (ms) | 3311 |

The process-ready log reports 35,912 → 28,696 ms; the tables use the external readiness probe. One/five/fifteen-minute load averages were [67.63, 69.67, 70.09] BEFORE and [56.68, 55.73, 57.18] AFTER.

| Requested target | Final observation | Verdict |
|---|---|---|
| First recall < 2× warm p50 | 92.4 / 34.1 = 2.71× | Not met |
| Hybrid p95 < 400 ms | 64.1 ms | Met |
| Hybrid p50 unchanged or better | 33.7 → 34.7 ms | Not demonstrated; final median is slightly higher |
| RSS reduced by ≥25% | 4322.4 → 3220.2 MiB (25.50% reduction) | Met |
| Same hybrid IDs/order | 20/20 identical | Met for this query |

First recall improved 98.57%, but remaining cold-query/competitive-refresh work still exceeds the requested ratio against repeated warm queries. Host variation does not establish median parity.

Clean checkpoint restart with the final binary ([committed table](../benchmarks/field-perf/table-clean-final.md); the original JSON is not present in this checkout):

| Load measurement | BEFORE legacy family | AFTER same legacy family | AFTER clean checkpoint |
|---|---:|---:|---:|
| Triplet migration (ms) | 3665 | 3311 | 0 |
| Ready (ms) | 35928.8 | 28712.3 | 28533.2 |


## Design and evidence by item

1. **Startup phases and readiness.** `chitta-field/src/field.rs:654` times snapshot decoding separately from `.pld` and `.emb`; `field.rs:1270` times lite/HDC loads; `field.rs:1473` times triplet migration; `src/hnsw.rs:3112` times Turbo construction. Lines have the requested `[chitta-field] load phase=<name> ms=<n>` form. `chitta/src/simple_cli.cpp:892` logs process-start-to-ready immediately before normal request dispatch. These phases overlap and do not add up to total startup: WAL replay, derived-map reconstruction, event tape/CDAWG [6](#ref-6), model setup and other initialization remain in the wall time.

2. **Warm before serving.** `chitta-field/src/field.rs:1265` starts Turbo and lite work beside HDC loading after replay/normalization, before store locks exist, and joins both before open returns. `hnsw.rs` publishes a completed immutable index, with no age-only maintenance rebuild on an unchanged corpus. The C++ loader also primes the encoder queue while the store loads and joins before leaving warming (`simple_cli.cpp:1674`). Before publishing Turbo, every bounded refresh worker executes a read-only search with a corpus vector, initializing the pool and search scratch without touching learned state. This last step reduced first recall from 118.5 to 92.4 ms in sequential measurements. No `.turbo` format is introduced. The startup regression test asserts that opening a persisted fixture returns with Turbo already available.

3. **Triplet migration.** `src/organ/triplet.rs:140` tracks whether the live SPO set is clean. Duplicate insertion/replay and invalidation clear that flag. Snapshot writes clean the cloned triplet organ and persist the optional V23 `triplets_clean` section (`src/snapshot.rs:1267`); an old V23 without the section still migrates once. Dirty WAL replay forces migration again. Borrowed dedup keys preserve the highest-weight winner without allocating three strings per row. The clean-checkpoint restart measures the migration portion as 0 ms; decoding and rebuilding derived triplet maps remain part of loading. This avoids recurring dedup, not every triplet-related allocation.

4. **Hybrid tail.** The control's first recall spent several seconds in competitive-weight refresh, with the first control diagnostic showing approximately 3.07 + 3.15 seconds in its two semantic lanes. The primary BEFORE log has a maximum single refresh stage of 4.20 seconds. Early warm hybrid samples remained around 245 ms while later samples fell near 33 ms: they were paying the outstanding refresh work. `src/store.rs:3008` captures an immutable fresh-Turbo view under a short semantic guard, runs independent neighbor searches in a bounded pool off all store guards, then applies weights using the existing two-phase logic. The original single-query arithmetic, deletion filtering and candidate order are retained. Dirty, scalar and HNSW [4](#ref-4) routes fall back to the existing search. A bitwise score/ID test covers serial versus parallel behavior. A four-query batching experiment was discarded because it changed float rounding without a measured latency gain.

   `CHITTA_RECALL_PROFILE=1` enables Rust semantic search / CW refresh / scoring timers and C++ setup, semantic lanes, keyword, HDC, bridge/fusion, rerank, prefilter, metadata, format and span timers (`src/profile.rs:1`, `chitta/src/handlers/field_memory_recall.cpp:17`). Output stays on stderr. In the verified run, CW refresh peaked at 12.63 ms; rerank/prefilter/format were not dominant. The supplied live 500–1100 ms tail was not reproduced in the quiesced scratch daemon, so this result does not establish that all live contention is resolved.

5. **Memory.** `src/profile.rs:44` emits capacity estimates for payloads, states, embeddings, HNSW, HDC, triplets, spans, CDAWG, semantic caches, lite encoder, keyword and episode structures. Each guard is held alone. `chitta/src/handlers/field_system.cpp:149` adds a `memory_breakdown` line to detailed health; request `health_check` with `details=true` to bypass the atomic fast path. The shared FFI additions are `cf_memory_breakdown` and `cf_recall_profile_enabled`; allocated C strings retain the existing `cf_free_string` ownership rule.

   The largest avoidable allocation found was the keyword reverse index: 13,426,880 per-document term entries replicated strings for just 108,299 distinct terms. `src/organ/keyword.rs:27` uses runtime `u32` term IDs, reclaiming IDs when the last posting disappears; serialized postings and BM25 [24](#ref-24) scoring stay unchanged. Episode HDC uses exact bit-plane counters instead of a `u16` per dimension (`src/hdc.rs:567`), including saturation behavior. Triplet source paths share `Arc<str>` (`src/organ/triplet.rs:16`): 804,781 paths referenced only 6,197 distinct strings, approximately 79.35 MB repeated bytes versus 0.61 MB unique. Their custom serde encoding is exactly the former String representation. Subject/predicate/object strings were left alone because the reverse keyword map offered the larger measured saving.

| Component allocation estimate (MiB) | BEFORE | AFTER |
|---|---:|---:|
| payloads | 168.31 | 168.31 |
| states | 62.25 | 62.25 |
| embeddings | 399.99 | 399.99 |
| hnsw | 0.00 | 0.00 |
| hdc | 269.50 | 269.50 |
| triplets | 712.42 | 627.04 |
| spans | 0.00 | 0.00 |
| cdawg | 85.89 | 85.89 |
| semantic_caches | 50.74 | 50.74 |
| lite_encoder | 11.09 | 11.09 |
| keyword_postings | not captured | 223.34 |
| keyword_reverse | 672.09 (legacy estimate) | 66.99 |
| episode_hdc | 51.70 (legacy estimate) | 7.30 |

   Estimates exclude allocator headers/fragmentation, Turbo internals, several other organs, the embedding model and thread stacks. They are allocation attribution, not additive RSS measurements. The reported RSS comes separately from `/proc/<pid>/status`. HNSW is zero for this replica's flat-scan route; zero spans means this copied family has no loaded span store. Legacy keyword/episode figures are explicit counterfactual capacity estimates on the final data, not component counters captured in BEFORE.

6. **Reproduction.** See [benchmark instructions](../benchmarks/field-perf/README.md). `run.sh <label> --profile` writes `results-<label>.json`, a standalone Markdown table and an ignored daemon log. `--checkpoint` saves only the scratch child after all measurements. A subsequent `--source <scratch>/chitta-field` makes another private copy and measures the clean marker. `compare.py` checks the same family/query/realm, response shapes and ordered IDs; its nonzero exit on adaptive differences is intentional and must not be treated as success.

Rust implementation commit: `bc709a3` (committed before the superproject).

## Compatibility and validation

- Release Rust build and `./build.sh test --release` passed: **277 passed, 2 existing ignored** (279 total; 44.84 s). The exact bitwise search comparison is included.
- CMake configured inside this worktree from the production `CHITTA_*`, compiler, Python and BLAS cache values, overriding only `CHITTA_FIELD_ROOT` to this checkout. Dependency sources are read from the production build; generated files stay here. Final C++ build passed and **all 18 CTests passed**, using the existing local GGUF for `embed_pool_test` (18.56 s total).
- The supplied worktree had no `target/release/.chitta-embed-identity` stamp and the wrapper did not create one. Its absence remains unchanged. Generated `embed_config.rs` verifies dimension 768 and model `nomic-embed-text-v1.5`, matching the production identity read before building. No embedding-identity migration or model change was performed; see [identity evidence](../benchmarks/field-perf/embed-identity.json).
- Fixed-input Rust regressions cover serial/parallel bitwise scores and IDs; stale-view safety and dirty-route fallback; warm-on-open; absent/present V23 marker; duplicate replay and invalidation; keyword legacy encoding plus removal/reindex/rebuild; shared source-path String encoding; and episode-counter growth/saturation. Original V23 loads and the new checkpoint reloads. Existing snapshot sections and WAL records retain their wire representation; `triplets_clean` is an optional named section old V23 readers skip.
- [Contract comparison](../benchmarks/field-perf/comparison.json): top-level structured key sets are unchanged for `recall`, `smart_recall`, `hybrid_recall`, `recall_keyword`, and `recall_lanes`. Keyword/fused/hybrid ordered IDs match 20/20. Smart matches 8/20 and combined lanes 3/20, with 4 variants already present within each BEFORE workload. The pre-existing route learner seeds from time and episode ID (`src/learner/route.rs:75`). Optional `affect` appears on different first selected sem/ctx rows in the final combined-lane contract probe; strict sampled-shape equality therefore fails there. No recall response construction or schema keys were changed by this patch, but deterministic parity for adaptive routes is **not established**. `compare.py` exits 1, as recorded.
- Changed shell syntax, touched-Python ruff and both `git diff --check` checks passed. All 15 hook scripts passed after rerunning the self-isolating session-cards test without a conflicting outer socket override; all 6 of its fixtures passed. SMRITI: 46 passed.
- Additional MCP suite: 130 tests, one error in unmodified HTTP session integration. Available SDK 1.27.2 lacks `_session_owners`, which existing `create_http_session_manager` expects. This is an outstanding ancillary gate failure; no out-of-scope SDK installation or MCP code change was made. [Ancillary exit codes](../benchmarks/field-perf/ancillary-checks.json) preserve this failure.


## Intermediate measurements

| Artifact label / step | Ready (ms) | First recall (ms) | Hybrid p50 / p95 (ms) | RSS (MiB) |
|---|---:|---:|---:|---:|
| before / instrumentation control | 35928.8 | 6440.6 | 33.7 / 245.1 | 4322.4 |
| warm / eager Turbo + borrowed dedup | 50357.5 | 345.0 | 49.5 / 343.3 | 4266.8 |
| memory / episode counters + clean marker | 38625.4 | 343.9 | 58.8 / 336.7 | 4233.4 |
| after / parallel refresh | 34057.2 | 102.4 | 43.5 / 64.4 | 4217.5 |
| final / compact keyword reverse map | 26572.4 | 111.6 | 37.3 / 60.1 | 3312.4 |
| optimized / shared paths + encoder prime + rejected batching | 30951.8 | 110.0 | 49.0 / 70.4 | 3223.5 |
| verified / exact arithmetic restored | 28758.1 | 118.5 | 40.8 / 65.0 | 3219.7 |
| primed / warm workers (primary AFTER) | 28712.3 | 92.4 | 34.7 / 64.1 | 3220.2 |

These are sequential implementation measurements on a shared node, not an additive attribution experiment. `optimized` contains the discarded four-query batching experiment; `verified` restored exact single-query arithmetic. The primary final comparison uses the final implementation only. The clean restart uses a newly checkpointed family and is a load-path check, not the same-family retrieval control.

2026-09-16 — Snapshot decode (V23 unchanged): four bounded Rayon workers decode immutable mmap section ranges, pre-size root maps, and overlap triplet-index reconstruction; keyword reverse reconstruction overlaps Turbo/HDC/lite startup. Two cold-process starts on the same scratch checkpoint plus one-row replay delta (Turbo cache hit both arms): snapshot **9195/7819 → 4263/3567 ms**, field_store **14999/14575 → 9516/9263 ms**. Targets <2500/<9000 ms were **not met**; triplet reconstruction remains 2944/2325 ms. The repeated-query restart gate is **20/20 byte-identical CLI JSON pairs**; the broader 20-query diagnostic is 19/20 after versus 18/20 in the warmed control (not a claim of universal determinism). Release build, 279 Rust tests (2 ignored), and all 16 CTests pass; current-main binary opens the new writer's checkpoint. Embedding constants remain 768/nomic-embed-text-v1.5/format-1; the worktree identity stamp was absent before/after and the main stamp hash is unchanged. OS caches and shared-host load were uncontrolled; overlapping phases must not be summed.

## Historical Phase 3 preparation checkpoint, 2026-09-16 (not qualified)

This records the earlier pre-authorization checkpoint and its different
`bbcaed33` family. The source-index and anchor work and comparable `da86decb`
measurements above supersede its implementation status and gates.

The `feat/freshness` worktree adds Markdown heading extraction to the existing
code-intel API and task-ledger handoff capsules. It does not yet implement the
repository recall merge, startup/query hash validation, or the derived anchor
index. The real-thread continuation gate is unmeasured: all 54 threads in the
inspected replica have empty metadata; the four synthetic capsule checks are
mechanics coverage only.

Measurements on private copies of eval family `bbcaed33`, using the frozen
panels and no-learn recall:

| Measure | Before | Current worktree | Interpretation |
|---|---:|---:|---|
| Current truth | 13/50 | 14/50 | Below 40/50; no gain claimed with recall unchanged |
| Reconstructed F2 fixtures | 1/5 | 1/5 | Original five prompts still unavailable |
| Golden nDCG@20, three-run mean | 0.499994 | 0.502660 | Baseline already outside stored band; first-run variation |
| Prompt median, five fixed-query panels | 892 ms | 912 ms | +20 ms, within baseline 2-SD allowance 250.4 ms |

The before daemon was the installed binary running only on the scratch copy;
the after daemon was built in this worktree. This is qualification evidence,
not a controlled causal attribution. The compiled vector-space ID matches
`9230643459983636874`; the worktree embedding stamp was absent before and after.
Rust release tests: 289 passed, 2 ignored. All 25 CTests passed (the initial
`embed_pool_test` skip was resolved by supplying the existing GGUF model).
MCP: 149 passed; SMRITI: 46 passed. All 23 hook shell tests, CI ruff check/format,
touched-shell syntax and CI ShellCheck warning gate passed. Contracts printed
`contracts unchanged`. The capsule round-tripped through the real scratch ledger
and survived a cold daemon restart; this remains a synthetic action check.

Ordered IDs were identical for **11/20 distinct queries** across that restart;
this fails the 20/20 gate. The recall implementation was not changed. The
full-replica chaos run passed snapshot and second-instance cases, then failed
WAL on a 60-second RPC timeout. A retry passed WAL, lock, disk and queue cases,
then failed prompt-hook recovery on an 8-second timeout. The final attempt to
run hook/MCP/format cases timed out during replica setup (120 seconds), followed
by an NFS cleanup error; no additional cases ran. A clean 9/9 run is not
established. These are retained failures, not acceptance evidence. Raw logs and
reports stay outside git under `/tmp/chitta-p3-freshness`.

<!-- BEGIN CITATIONS -->
## References

- <a id="ref-4"></a>**[4]** Yu. A. Malkov and D. A. Yashunin. Efficient and robust approximate nearest neighbor search using Hierarchical Navigable Small World graphs. IEEE TPAMI 42(4), 824–836 (2020); arXiv:1603.09320 (2016). [source](<https://arxiv.org/abs/1603.09320>) [source](<https://doi.org/10.1109/TPAMI.2018.2889473>)
- <a id="ref-6"></a>**[6]** A. Blumer, J. Blumer, D. Haussler, R. McConnell, and A. Ehrenfeucht. Complete inverted files for efficient text retrieval and analysis. Journal of the ACM 34(3), 578–595 (1987). [source](<https://doi.org/10.1145/28869.28873>)
- <a id="ref-10"></a>**[10]** Pentti Kanerva. Hyperdimensional Computing: An Introduction to Computing in Distributed Representation with High-Dimensional Random Vectors. Cognitive Computation 1, 139–159 (2009). [source](<https://doi.org/10.1007/s12559-009-9009-8>)
- <a id="ref-24"></a>**[24]** Stephen Robertson and Hugo Zaragoza. The Probabilistic Relevance Framework: BM25 and Beyond. Foundations and Trends in Information Retrieval 3(4), 333–389 (2009). [source](<https://doi.org/10.1561/1500000019>)
<!-- END CITATIONS -->

## Ordered recall identity across restart

Run the shared Phase 1 gate separately from write stress, after building this
worktree's `bin/chittad` and `bin/chitta` with the pinned 768-dimensional
`nomic-embed-text-v1.5` identity. It requires the frozen replica family, so it is
an explicit replica gate rather than a replica-free CTest.

```bash
export OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1 RAYON_NUM_THREADS=1
identity_root=$(mktemp -d /tmp/chitta-restart-identity.XXXXXX)
/maps/projects/fernandezguerra/apps/opt/conda/envs/bioinfo/bin/python3 \
  scripts/restart-identity.py \
  --source /projects/caeg/scratch/kbd606/tmp/learning-cut-20260915-frozen \
  --mind "$identity_root/restarts" --restarts 3 \
  --report "$identity_root/restarts.json" > "$identity_root/restarts.log"
/maps/projects/fernandezguerra/apps/opt/conda/envs/bioinfo/bin/python3 \
  scripts/restart-identity.py \
  --mind "$identity_root/control" --within-process \
  --report "$identity_root/control.json" > "$identity_root/control.log"
```

The committed `scripts/restart-identity-queries.json` is the first twenty distinct
queries of the golden panel. Each run uses unscoped hybrid recall at depth 20,
`no_learn`, and `explain`. There are no warm-up panel calls. Every ordered ID list
must match: 20/20 on each of three consecutive restarts, plus 20/20 for the
within-process control. Missing results, failed RPCs, or missing base/variant
embeddings fail the gate. Score deltas remain visible even when IDs agree.

The tool owns a fresh scratch mind, private HOME, socket and port. `--mind` must
not exist; `--port` can select a particular private port, otherwise the tool
selects one. It invokes `eval-replica.sh start` once to copy the selected frozen
family, then `eval-replica.sh restart` to reopen the same store without recopying.
It stops its daemon on completion and leaves reports and the copy outside git.

`CHITTA_RECALL_NOW` pins Rust recall scoring to the run's initial Unix milliseconds
(or `--now-ms`); writers retain their real clocks. Pinning evaluation time avoids
changing production relevance through quantization. The report records that value
for paired experiments. `CHITTA_RECALL_EMBED_WAIT_MS` is 10000 for the gate
(`--embed-wait-ms`, bounded to 1–60000); production keeps its 50 ms default.
A longer wait allows complete inference under load, and the gate still rejects a
missing embedding instead of accepting matching keyword fallbacks. See the
[environment table](HOOKS.md#phase-6-runtime-placement-and-embedding-workers-2026-09-16).

Each JSON report retains complete RPC results, score components, exact native f32
embedding bytes, and candidate lanes before fusion. To diagnose a failure, compare
clock-sensitive factors, then embedding availability/bytes, then lane candidate
sets, then state changes and exact-score ties. Optional `.lsh`, `.turbo`,
`.turbo.meta`, and `.organs` rebuild experiments belong only on the owned copy.
