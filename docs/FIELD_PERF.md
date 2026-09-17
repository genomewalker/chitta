# chitta-field performance

## Phase 1 synchronization inventory — 2026-09-16

This inventory covers the registered handler surface and its maintenance,
queue, backfill, distillation and subconscious callers. An FFI call releases
its Rust guards before returning; separate FFI calls are not one transaction.

| Mutable state / caller | Synchronization and publication | Evidence |
| --- | --- | --- |
| Task-ledger tables, indexes and revisions | Dedicated transaction mutex covers read/validate, WAL append and publication; reads return copies. Persistence callbacks must not reenter the ledger. | `chitta/include/chitta/task_ledger.hpp:78`, `chitta/src/handlers/field_task_ledger.cpp:23` |
| Query embedding LRU and in-flight requests | Cache mutex and shared futures; inference outside the mutex; hit/miss counters atomic. | `chitta/include/chitta/rpc/field_handler.hpp:101` |
| Health/soul memory and spectral caches | Separate mutexes, single-flight refresh outside locks, stale values while refreshing. One store lifetime per daemon. | `chitta/src/handlers/field_system.cpp:19`, `chitta/src/handlers/field_system.cpp:61` |
| Distillation model and enabled flag | `distill_mutex_` for the string; atomic flag. | `chitta/include/chitta/rpc/field_handler.hpp:235` |
| Embedding jobs and cached vectors | Separate queue/cache mutexes; pressure flags atomic. | `chitta/include/chitta/embed_queue.hpp:254`, `chitta/include/chitta/embed_queue.hpp:261` |
| Embedding contexts / model | Per-context mutexes and bounded semaphore; model immutable after construction. | `chitta/include/chitta/vak_llama.hpp:86` |
| Subconscious queues / stats | Separate event, embedding, suggestion, anticipation and tool-sequence mutexes; counters/timestamps atomic. | `chitta/include/chitta/mind/subconscious.hpp:116`, `chitta/include/chitta/mind/subconscious.hpp:276` |
| Subconscious callbacks / load probe | Setter and copy-out mutex; invoke copies outside that mutex; initial callbacks installed before starting workers. | `chitta/include/chitta/mind/subconscious.hpp:226`, `chitta/src/simple_cli.cpp:401` |
| Sadhana state and subscriptions | Manager mutex and subscription mutex; counters atomic. Construct and set stream callback before readers start; handler pointer atomic. | `chitta/include/chitta/sadhana/sadhana_manager.hpp:218`, `chitta/include/chitta/sadhana/sadhana_manager.hpp:224`, `chitta/src/simple_cli.cpp:291` |
| Queue counters and paths | Counter values atomic; pointer/path publication uses a dedicated mutex and copy-out snapshot. File scans and FFI calls occur after releasing it. | `chitta/include/chitta/rpc/field_handler.hpp:208` |
| Write notification callback | Mutex on setter/copy; callback invoked outside lock so reentrant replacement is safe. | `chitta/include/chitta/rpc/field_handler.hpp:220` |
| Registrations, schemas, recall callback, embed/subconscious/load pointers | Published during construction/startup before serving; thereafter immutable. Owners remain alive while workers drain. | `chitta/include/chitta/rpc/field_handler.hpp:162`, `chitta/src/simple_cli.cpp:274` |
| RPC budgets and maintenance load/wait counters | Atomics; immutable configured limits. | `chitta/include/chitta/rpc/work_policy.hpp:101`, `chitta/include/chitta/rpc/field_handler.hpp:762` |
| Consolidation admission | Atomic in-flight flag; request-local work and Rust-owned stored state. | `chitta/src/handlers/field_memory_recall.cpp:2165` |
| RPC / compaction lifetime | Foreground pool explicitly drained before referenced locals die; owned compaction thread joined before pool and handlers are destroyed. | `chitta/src/simple_cli.cpp:264`, `chitta/src/simple_cli.cpp:1335` |
| Queue lane / applied-ack state | One worker owns each lane. Existing checked `cf_sync` precedes applied-ack fsync/rename. Phase 1 changes acquisition only. | `chitta/src/queue_processor.cpp:182`, `chitta/src/queue_processor.cpp:937` |
| Maintenance / distillation / backfill | Handler factories obey the startup switch; Rust collect/plan/apply phases own their state. Belief read/decide/write sequences lack a store-level compare-and-apply transaction. | `chitta/src/distillation.cpp:78`, `chitta/include/chitta/rpc/field_handler.hpp:352`, `chitta/src/simple_cli.cpp:534` |
| Subconscious direct store writes | Startup injects a null global-mutex pointer when the shared switch is off; local queue locks remain. | `chitta/src/subconscious.cpp:106`, `chitta/src/simple_cli.cpp:284` |

The acknowledgement policy is independent of acquisition: historically exclusive
write classes call `FieldStore::sync()` after dispatch, while existing lock-free
and subprocess classes retain the boundaries below. Bypassing a mutex must not
silently bypass that sync. Multi-FFI belief maintenance remains unqualified for
production without the global lock; component locks alone do not prevent stale
read/decide/write decisions.

Initial baseline on frozen snapshot da86decb: distinct-query restart identity
15/20 then 18/20; both fixed-query controls 20/20. The lead confirmed the drift
on unchanged main and replaced this stream's identity gate with no worse than
before on the same copy/script plus fixed control 20/20. Reports remain outside
git. The global-lock default remains subject to the 300-second stress gate.

Step (b) validation, 2026-09-16: CTest 26/26, MCP 149/149, all 23 hook scripts,
CI Ruff and chaos 9/9 pass. Same-copy distinct restart identity 20/20, fixed
response and ordered-ID controls both 20/20. Current-truth remains 20/50;
golden nDCG moves from 0.5708 to 0.5773 (both below the grader's 0.7 target).
The inherited primary-node hook fixture initially saw the old binary; it passed
with exit 75 after rebuilding the merged Phase 6 daemon.

## Named Rust lock timings — 2026-09-16

All 76 parking_lot component RwLocks in `chitta-field/src/field.rs` and the
standard-library archive RwLock use guards from `chitta-field/src/profile.rs`.
Acquisition attempts, wait and hold maxima are always measured, including timed
read failures and unwinding. The archive retains standard-library poisoning.
`[lockprof] RUST component=... mode=... held_us=... wait_us=...` is emitted on
first use, new lifetime maxima, each 4096th completion, and every wait or hold
over 50 ms. The guard releases its component before writing diagnostics.
No profiling environment variable is required and no RPC or file format changes.

Reports distinguish observed stress-interval timings from lifetime maxima that
can include startup. Every over-threshold completion is logged even when its
component had a larger startup maximum. Timing includes scheduler delays; these
are real periods during which other clients cannot acquire the component.

Step (c) validation, 2026-09-16: Rust release build and 291 tests pass (two
existing fixtures ignored), CTest 26/26, MCP 149/149, hooks 23 scripts, SMRITI
46/46, CI Ruff and chaos 9/9. Embedding identity remains 768 /
nomic-embed-text-v1.5 / text-format 1, format ID 9230643459983636874.
The same-copy distinct-query restart result is 15/20 against the 18/20 baseline:
the revised no-worse gate is unmet. Both fixed-query controls remain 20/20.
Current-truth is still 20/50 and golden nDCG is 0.5772. These are observations,
not qualification to remove the global lock. The profiler observes 33 exercised
components, with startup-inclusive maximum hold 705.761 ms and wait 71.131 ms.

## Global-lock switch validation — 2026-09-16

`CHITTA_GLOBAL_LOCK=1` retains the historical dispatcher policy; exactly `0`
removes `rpc_mutex_` from every RPC, queue dispatch and background lock factory,
including maintenance, distillation, backfill and subconscious callbacks.
Configuration is frozen at startup. Unset or invalid values retain the global
lock. This keeps removal disabled by default while the exit gate is unmet.
The previous write classification still controls WAL sync independently of
mutex ownership. Four policy tests cover both values, unset/invalid input,
publication of configuration, real contention and the actual `cf_sync` calls.

All three migrated workload classes ran for 300 seconds with 12 persistent
writer clients and 12 reader clients on private copies, with the switch set to
0. The first ledger stage preceded the all-tools bypass; the two memory classes
used the final bypass. Background backfill and Rust touch flushing remain active
on these quiesced copies. No client exceeded its 30-second deadline.

| Initial 300 s class run | Acknowledged writes | Reader iterations | Maximum RPC (ms) | Rust hold / wait maximum (ms) | Holds >50 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| ledger | 15,103 | 3,197 | 2,567.902 | 348.833 / 219.516 | 2,237 |
| observe | 21,113 | 11,023 | 1,928.914 | 242.633 / 831.098 | 4,945 |
| remember | 22,825 | 9,023 | 1,131.022 | 350.516 / 717.085 | 4,350 |

All three runs had zero client errors, exact memory counts and no malformed Rust
timing records. Ledger replay verified all 15,103 acknowledgements. Initial
memory runs failed an overly strict replay comparison: the remember diagnostic
showed only access_count 16,342→16,363 and last_accessed_ms advancing while
verification itself called touching reads. The observe failure did not retain
field differences and remains unexplained by that initial report. These initial
failures are preserved; they are not clean replay results. The stress tool's
corrected comparison and follow-up measurements are documented separately below.

A preceding connection-per-call smoke run reset a connection after 88 ledger
acknowledgements. Persistent-client runs avoid connection churn; the tool keeps
that mode available as a separate diagnostic. An observe smoke also exposed a
Rust/native stderr interleaving. The profiler now emits each short record in one
write; the final observe and remember runs recorded 27,214 and 20,329 intact
timing lines respectively.

Final gates: Rust release build and 291 tests pass (two existing ignored), CTest
30/30, MCP 149/149, hooks 23/23, SMRITI 46/46, CI Ruff and chaos 9/9 with the
switch off and BLAS/OMP/Rayon threads limited to one. An initial CTest ledger
takeover fixture hit its 75 ms fail-open client budget; its unchanged retry and
the final full suite passed. Same-original-copy restart identity is **20/20
after versus 18/20 before**, with both fixed controls **20/20**. Separate initial
stress-copy identities were ledger 18/20, observe 18/20 and remember 15/20;
the last failed its individual no-worse gate. Current-truth remains 20/50;
golden nDCG is 0.5691 versus 0.5708 before (both below the grader's 0.7 target).
Embedding identity and contracts are unchanged.

**Removal is unqualified for every class:** every full workload exceeded the
50 ms Rust hold gate. Maintenance/control, sadhana, code-intelligence and deletion
workloads have not each received a dedicated 300-second mixed-client run.
Factory tests and chaos coverage do not substitute for those runs. The live-week
hold gate is also unmeasured; deployment and live writes are outside this stream.

## Reproducing mixed RPC stress — 2026-09-16

`scripts/stress-rpc.py` requires an explicitly named private replica, validates
its metadata, daemon argv, socket peer and worktree executable, and refuses to
run unless `CHITTA_GLOBAL_LOCK=0`. Start each class from a fresh copy of the
stopped evaluation replica family, using its own mind and port:

```sh
export PATH=/maps/projects/fernandezguerra/apps/opt/conda/envs/bioinfo/bin:$PATH
export CHITTA_LIVE_MIND=/absolute/path/to/stopped-eval-replica
export CHITTA_EVAL_MIND=/tmp/my-private-lock-stress
export CHITTA_EVAL_PORT=17439
export CHITTA_BIN="$PWD/bin/chitta" CHITTAD_BIN="$PWD/bin/chittad"
export CHITTA_GLOBAL_LOCK=0 CHITTA_NO_ASSOC_LEARN=1
export OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1 RAYON_NUM_THREADS=1
bash scripts/eval-replica.sh start
python3 scripts/stress-rpc.py --mind "$CHITTA_EVAL_MIND" \
  --handler-class remember --report /tmp/my-lock-stress.json
```

Defaults are 12 writers, 12 readers and 300 seconds. Repeat with `ledger` and
`observe` on separate copies. Each client uses a persistent socket; optional
`--connection-mode per-call` also stresses connection churn. Each complete RPC
has a monotonic 30-second deadline, including send, receive and JSON parsing.
The tool checks exact memory counts, unique acknowledgements, payload and state
for every recall hit (including newly published memories), then SIGKILLs only
the validated scratch daemon and checks every acknowledgement after WAL replay.
It reports interval and lifetime Rust lock maxima separately; absent or malformed
profiling records cannot qualify a run. Exit 0 requires every exit gate, including
identity; short runs and `--skip-identity` are diagnostic and always exit 1.

The identity panel runs before any workload creates memories. To use the lead's
no-worse exception, record the baseline before changing the binary using
`scripts/stress-embed-recall.py --restart-only --mind COPY --label before
--output /tmp/before.json`, then supply `--identity-baseline /tmp/before.json`
when measuring that same copy after the change. The ordered query list and both
fixed-query controls are checked; a baseline containing a `mind` field must name
this copy. Without a baseline, the distinct-query gate requires 20/20. Never
invoke `eval-replica.sh start` between the before/after measurements: it recopies
the family. Reports, replica contents and logs stay outside git.

Replay comparison excludes wall-time-decayed strength. All other payload and
authored state must match exactly; access counts and timestamps must not regress.
Decay rate may change only when access state advances. This matters because
`get` and `expand_memory` enqueue touches and the Rust worker persists them even
while a verification pass is reading other rows. The verifier therefore proves
authored-state durability and monotonic access state, not byte identity of
read-induced bookkeeping. A real captured access-only mismatch is accepted;
injected payload/confidence changes and count/timestamp regressions are rejected.
Socket-stall, non-replica and absent/malformed-profile negative checks also pass.

Final corrected-verifier repeats ran concurrently for 300 seconds each on their
already-mutated private copies, with 12 writers and 12 readers per daemon.
Both passed counts, hit payload/state checks and every acknowledgement after
SIGKILL/WAL replay, with zero client errors and zero malformed timing records:

| Repeat class | Writes replayed | Reader iterations | Maximum RPC (ms) | Interval Rust hold / wait maximum (ms) | Holds >50 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| observe | 18,901 | 11,539 | 3,291.325 | 727.692 / 1,331.772 | 4,663 |
| remember | 21,378 | 9,383 | 1,149.374 | 736.738 / 669.846 | 3,995 |

These repeats used `--skip-identity` because the original-copy identity panel
had already finished and these copies contained stress writes. They exited 1,
correctly: diagnostic runs cannot meet the full gate and the hold limit was
exceeded. Their timing differences from the sequential initial runs are not
performance deltas: corpus size and concurrent load differed. Initial stress
copy identity comparisons against the original-copy baseline are diagnostic,
not the lead's required same-copy comparison; the latter is the separate final
20/20 versus 18/20 measurement above. No class is qualified for default removal.
### Phase 3 review follow-up (2026-09-16)

Merged main's recall diagnostics and deterministic measurement behavior, with
Rust `847e7e3` and superproject `68cd0a37`. Recall now accepts `sources=false`
for memory-only evaluation. Source rows retain their citations, but their
uncalibrated BM25 ranks no longer force calibrated confidence to one:
`source_hits` reports their count separately. A private-replica check with
three sources preserves memory order and `max_relevance=0.813876867` in both
modes; before the confidence fix, source merging forced it to `1.0`.

The paired native binaries use a fresh private `da86decb` copy from
`learning-cut-20260915-frozen`, pinned recall time and a bounded embedding wait.
The before binary has no source switch, so its golden memory-only control used
an empty source-root registry. The same copy was then registered to this
worktree under `project:cc-soul` for default current-truth evaluation. Both
the source-toggle build and final build use `sources=false` for golden recall.

| Gate | Before | After |
| --- | --- | --- |
| Golden nDCG@20, three runs | 0.480414590, 0.480414590, 0.480414590 | 0.480414590, 0.480414590, 0.480414590 |
| Current-truth, default sources | 36/50; visible 20/30, holdout 16/20 | 36/50; visible 20/30, holdout 16/20 |
| Original five strict probes | 3/5 | 3/5 |
| Ordered identity, three restarts | 20/20 each | 20/20 each |
| Within-process identity | 20/20 | 20/20 |
| Chaos, single-threaded BLAS/OMP/Rayon | 9/9 | 9/9 |
| Prompt-hook median | 890 ms | 929 ms |
| Same 20 continuation pairs | 0/20 | 0/20 |

No current-truth question outcome or class moved; wrong-confident answers remain
zero. Prompt median increased 39 ms, within the before-run 241 ms noise allowance.
The golden band stored in `benchmarks/noise.json` is **0.490965206–0.494473852**,
measured on family `bbcaed33`; both requested `da86decb` arms miss it. The source
toggle removes the mechanical displacement without tuning anchor demotion.
The current 36/50 retention gate passes; the original 40/50 and five-probe 5/5
goals remain unmet. No frozen questions or acceptance bands were changed.

Native verification passes: Rust **293 tests**, 2 ignored (the same Rust
artifact in both binaries); **26/26 CTests**, **159 MCP tests**, **46 SMRITI
tests**, CI Ruff and shell syntax/ShellCheck. Format identity remains
`9230643459983636874`; the embedding identity sidecar remains absent. Contract
checks print `contracts unchanged` after the authorized recall input update.
The before hook run passed 27/27; the concurrent after run passed 26/27, with
the saddle timing case at 174.21 ms incremental p95 against its 150 ms limit.
Rerunning that case alone after the stress runs passed at **36.76 ms**; both
observations are retained, with no timeout or threshold changes.

The continuation builder read 641 transcripts across 382 project directories
with conversation records, producing 258 eligible consecutive pairs. The newest
20 all lack an explicit final plan line or usable ledger next action: strict
production-selector replay scores **0/20**, before and after. Four builder/scorer
tests pass, including project isolation and branch rejection. This is a failed
18/20 gate, not evidence that historical sessions wrote these capsules. Pair IDs,
transcript SHA-256 digests, ground truth and individual failure reasons stay in
`/projects/caeg/scratch/kbd606/tmp/continuation-fixture/`, outside Git.

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

<!-- ORGAN-ABLATION-TABLE -->
## Organ ablation 2026-09-16

Three repetitions per arm; missing calibration or invariants block retirement.
When controls exceed a declared band, differences are descriptive and cannot
be attributed to the ablation. No organ or tool is deleted by this runner.
Equivalence requires complete panel, identity and consumer-test evidence.
A valid calibrated-panel or invariant failure is sufficient for not equivalent.

Golden controls: 0.500078362622, 0.500078362622, 0.500078362622; spread 0; frozen margin 0.00179012512677.

Unqualified raw observations remain in the report and are not equivalence estimates.

| Organ | Dependency class | Panels moved (Δ; margin) | Verdict |
|---|---|---|---|
| `session_registry` | event/API | not qualified; raw observations retained | unqualified; consumer API test evidence missing: session_registry: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `transcript_registry` | event/API | not qualified; raw observations retained | unqualified; consumer API test evidence missing: transcript_registry: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `task_registry` | event/API; write path (retain) | not qualified; raw observations retained | unqualified; consumer API test evidence missing: task_registry: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `user_model_registry` | event/API | not qualified; raw observations retained | unqualified; consumer API test evidence missing: user_model_registry: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `theme_organ` | event/API | not qualified; raw observations retained | unqualified; consumer API test evidence missing: theme_organ: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `analytics_registry` | event/API; write path (retain) | not qualified; raw observations retained | unqualified; consumer API test evidence missing: analytics_registry: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `msg_registry` | event/API; snapshot section retained | not qualified; raw observations retained | unqualified; consumer API test evidence missing: msg_registry: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `skill_registry` | event/API | not qualified; raw observations retained | unqualified; consumer API test evidence missing: skill_registry: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `agent_registry` | event/API | not qualified; raw observations retained | unqualified; [Errno 3] No such process; treatment invariants incomplete; consumer API test evidence missing: agent_registry: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `constraint_store` | event/API | not qualified; raw observations retained | unqualified; consumer API test evidence missing: constraint_store: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `trigger_store` | event/API | not qualified; raw observations retained | unqualified; consumer API test evidence missing: trigger_store: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `intervention_store` | event/API | not qualified; raw observations retained | unqualified; consumer API test evidence missing: intervention_store: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `agent_protocol_store` | event/API | not qualified; raw observations retained | unqualified; consumer API test evidence missing: agent_protocol_store: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `wisdom_lineage_store` | event/API | not qualified; raw observations retained | unqualified; consumer API test evidence missing: wisdom_lineage_store: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `symbol_event_log` | event/API | not qualified; raw observations retained | unqualified; consumer API test evidence missing: symbol_event_log: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `repl_sessions` | event/API | not qualified; raw observations retained | unqualified; consumer API test evidence missing: repl_sessions: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `event_tape` | event/API; write path (retain); snapshot section retained | not qualified; raw observations retained | unqualified; consumer API test evidence missing: event_tape: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `cdawg` | event/API; write path (retain) | not qualified; raw observations retained | unqualified; consumer API test evidence missing: cdawg: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `episode_hdc` | event/API | not qualified; raw observations retained | unqualified; consumer API test evidence missing: episode_hdc: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `refutation_ledger` | event/API | not qualified; raw observations retained | unqualified; consumer API test evidence missing: refutation_ledger: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `cec_policy_store` | event/API | not qualified; raw observations retained | unqualified; consumer API test evidence missing: cec_policy_store: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `decision_tape` | event/API; snapshot section retained | not qualified; raw observations retained | unqualified; consumer API test evidence missing: decision_tape: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `hypothesis_market` | event/API | not qualified; raw observations retained | unqualified; consumer API test evidence missing: hypothesis_market: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `turiya_monitor` | event/API; snapshot section retained | not qualified; raw observations retained | unqualified; consumer API test evidence missing: turiya_monitor: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `fep_prior` | event/API; write path (retain) | not qualified; raw observations retained | unqualified; consumer API test evidence missing: fep_prior: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `observer` | event/API; write path (retain) | not qualified; raw observations retained | unqualified; consumer API test evidence missing: observer: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `observer_state` | event/API; write path (retain); snapshot section retained | not qualified; raw observations retained | unqualified; consumer API test evidence missing: observer_state: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `interaction_ledger` | event/API; snapshot section retained | not qualified; raw observations retained | unqualified; [Errno 3] No such process; treatment invariants incomplete; consumer API test evidence missing: interaction_ledger: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `predicate_store` | event/API; snapshot section retained | not qualified; raw observations retained | unqualified; consumer API test evidence missing: predicate_store: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `archive` | event/API; write path (retain) | not qualified; raw observations retained | unqualified; consumer API test evidence missing: archive: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `cortical_idx` | recall-adjacent; write path (retain); index sidecar retained | not qualified; raw observations retained | unqualified; consumer API test evidence missing: cortical_idx: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `hdc_idx` | recall-adjacent; write path (retain); index sidecar retained | golden.ndcg: +0.00500551; margin=0.0017901251267712533; hook_total_ms: +31.3333; margin=1833.3949929025114 | not equivalent; consumer API test evidence missing: hdc_idx: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens; observed beyond band: golden.ndcg |
| `lite_encoder` | recall-adjacent | not qualified; raw observations retained | unqualified; consumer API test evidence missing: lite_encoder: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `sparse_encoder` | recall-adjacent; write path (retain) | not qualified; raw observations retained | unqualified; consumer API test evidence missing: sparse_encoder: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `learners` | recall-adjacent; write path (retain) | golden.ndcg: -0.002199; margin=0.0017901251267712533; hook_total_ms: +134.333; margin=1833.3949929025114 | not equivalent; consumer API test evidence missing: learners: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens; observed beyond band: golden.ndcg |
| `span_store` | recall-adjacent; write path (retain) | not qualified; raw observations retained | unqualified; consumer API test evidence missing: span_store: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `predictor` | recall-adjacent | not qualified; raw observations retained | unqualified; consumer API test evidence missing: predictor: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `surprise_store` | recall-adjacent | not qualified; raw observations retained | unqualified; consumer API test evidence missing: surprise_store: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `epistemic_debt_store` | recall-adjacent | not qualified; raw observations retained | unqualified; consumer API test evidence missing: epistemic_debt_store: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `integration_kernel` | recall-adjacent | not qualified; raw observations retained | unqualified; consumer API test evidence missing: integration_kernel: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `surprise_learning` | recall-adjacent | not qualified; raw observations retained | unqualified; consumer API test evidence missing: surprise_learning: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `wisdom_promotion` | recall-adjacent | not qualified; raw observations retained | unqualified; consumer API test evidence missing: wisdom_promotion: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `learned_scorer` | recall-adjacent | not qualified; raw observations retained | unqualified; consumer API test evidence missing: learned_scorer: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `event_api` | group (30 organs) | not qualified; raw observations retained | unqualified; consumer API test evidence missing: session_registry, transcript_registry, task_registry, user_model_registry, theme_organ, analytics_registry, msg_registry, skill_registry, agent_registry, constraint_store, trigger_store, intervention_store, agent_protocol_store, wisdom_lineage_store, symbol_event_log, repl_sessions, event_tape, cdawg, episode_hdc, refutation_ledger, cec_policy_store, decision_tape, hypothesis_market, turiya_monitor, fep_prior, observer, observer_state, interaction_ledger, predicate_store, archive: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `recall_adjacent` | group (13 organs) | golden.ndcg: +0.00463211; margin=0.0017901251267712533; hook_total_ms: +45.6667; margin=1833.3949929025114 | not equivalent; consumer API test evidence missing: cortical_idx, hdc_idx, lite_encoder, sparse_encoder, learners, span_store, predictor, surprise_store, epistemic_debt_store, integration_kernel, surprise_learning, wisdom_promotion, learned_scorer: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens; observed beyond band: golden.ndcg |
| `event_prediction` | group (4 organs) | not qualified; raw observations retained | unqualified; consumer API test evidence missing: event_tape, cdawg, episode_hdc, fep_prior: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens |
| `all` | group (43 organs) | golden.ndcg: +0.00463211; margin=0.0017901251267712533; hook_total_ms: +12; margin=1833.3949929025114 | not equivalent; consumer API test evidence missing: session_registry, transcript_registry, task_registry, user_model_registry, theme_organ, analytics_registry, msg_registry, skill_registry, agent_registry, constraint_store, trigger_store, intervention_store, agent_protocol_store, wisdom_lineage_store, symbol_event_log, repl_sessions, event_tape, cdawg, episode_hdc, refutation_ledger, cec_policy_store, decision_tape, hypothesis_market, turiya_monitor, fep_prior, observer, observer_state, interaction_ledger, predicate_store, archive, cortical_idx, hdc_idx, lite_encoder, sparse_encoder, learners, span_store, predictor, surprise_store, epistemic_debt_store, integration_kernel, surprise_learning, wisdom_promotion, learned_scorer: missing current_truth.abstain, current_truth.p3, smriti.off.sr, smriti.off.tokens, smriti.on.sr, smriti.on.tokens; observed beyond band: golden.ndcg |

### Paired prompt-hook latency

Each trial runs `bench-recall-lanes.sh 5`: 15 samples per arm. Cells list
each repetition's median/p95 in milliseconds; these are descriptive and
separate from the calibrated `hook_total_ms` statistic above.

| Ablation | Off median/p95 | On median/p95 | Empty outputs | Write/restart invariants |
|---|---|---|---|---|
| `control` | 903/1103, 938/5094, 863/1167 | 908/4922, 920/995, 878/1065 | 0 | 3/3 passed; recall identity 20/20, 20/20, 20/20 |
| `hdc_idx` | 977/3161, 924/1611, 1044/1555 | 868/1570, 884/4713, 909/1107 | 0 | 3/3 passed; recall identity 20/20, 20/20, 20/20 |
| `learners` | 841/2258, 1043/5130, 1050/1419 | 881/2306, 1042/1389, 1066/5113 | 0 | 3/3 passed; recall identity 20/20, 20/20, 20/20 |
| `recall_adjacent` | 842/1347, 930/1345, 932/1892 | 883/3310, 912/1053, 901/1809 | 0 | 3/3 passed; recall identity 20/20, 20/20, 20/20 |
| `all` | 914/1143, 891/1077, 827/4680 | 937/4971, 858/1200, 818/995 | 0 | 3/3 passed; recall identity 20/20, 20/20, 20/20 |
<!-- ORGAN-ABLATION-TABLE -->

### Organ retention and rollback justification

The checked-in noise declaration lacks `current_truth.p3` and
`current_truth.abstain`; it cannot establish equivalence on every required panel.
No deletions or contract changes are justified yet. The archive has no reader,
but `put_memory` writes it, so the explicit write-dependency exclusion applies.

`CHITTA_ABLATE_ORGANS` is parsed once per store open; unknown names reject open.
Each disabled runtime organ starts empty. New organ-owned WAL records are
suppressed; old WAL still replays into retained state. Snapshot assembly reads
that retained state through the existing codecs. The snapshot layout is unchanged.
The preserved state remains allocated: this experiment measures runtime effects,
not memory savings or the eventual startup cost of removing an organ.

`ablate-organs.py --preflight` prints the frozen margins and missing inputs.
The runner pins one `CHITTA_RECALL_NOW`, a 10000 ms embedding wait and
BLAS/OMP/Rayon thread limits of one, recording these in the report. Golden
requests save their actual `--explain` lane traces; missing embeddings fail
the trial. The identity gate is `scripts/restart-identity.py` on a fresh copy.
Run `--controls-only` first, then `--resume` with `--block groups`,
`--block largest` and `--block remaining`, using the same `--output /tmp/RESULTS`. Each arm has
three fresh copies. Controls must fit the frozen spread bands before any
treatment starts. The remaining block defers when one-minute node load exceeds
`--max-load` (default: CPU affinity count; 96 on this run).
Use `--organ NAME` for a specific arm and `--write-table` for qualified numeric
comparisons; unqualified rows explain the missing evidence. A complete
calibrated panel outside its margin can disprove equivalence even when other
panels are unavailable. `--consumer-tests FILE` supplies an organ-to-argv JSON
map; those API tests must actually pass on each ablated replica for a positive
equivalence claim. Their commands and log hashes are retained. The optional
`--smriti-agent claude-code` runs the real visible panel without `--dry-run`;
without an isolated replica agent, the report explicitly omits SMRITI.

Retention reasons below distinguish consumers from exposed APIs. API ownership
was traced from `ChittaField` through store/FFI entry points and searched in
handlers, hooks, MCP and scripts. An API declaration alone is not a consumer;
where none was found, missing equivalence evidence still prevents deletion.
No consumer API was removed by this stream.

| Remaining organ | Consumer / reason retained |
|---|---|
| `session_registry` | `cf_session_register / cf_session_list` |
| `transcript_registry` | `cf_transcript_register / cf_transcript_list` |
| `task_registry` | `cf_task_create / cf_task_get; log_event write path` |
| `user_model_registry` | resonance learner save/load in `chitta/include/chitta/resonance_learner.hpp` uses `user_model_upsert / get_latest_event` |
| `theme_organ` | `subconscious.cpp` calls `theme_maintain`; similarly named theme RPCs use core memory recall, not this organ |
| `analytics_registry` | no reader found; `queue_processor.cpp` writes analytics events through `emit_event` (outcome, calibration, habit, tokens); retain: queue write dependency and unqualified equivalence |
| `msg_registry` | `cf_emit_event / cf_get_events_by_target; task ledger and sessions` |
| `skill_registry` | `cf_skill_upload / cf_skill_search` |
| `agent_registry` | `cf_agent_upsert / cf_agent_list` |
| `constraint_store` | `assert_constraint / query_constraints` |
| `trigger_store` | `add_trigger / evaluate_triggers` |
| `intervention_store` | `start_intervention / record_attribution` |
| `agent_protocol_store` | `register_task / query_tasks` |
| `wisdom_lineage_store` | `enroll_wisdom_lineage / query_wisdom_lineages` |
| `symbol_event_log` | `log_symbol_event / query_symbol_events` |
| `repl_sessions` | `repl_execute / repl_session_get` |
| `event_tape` | `put_memory / log_event / recall_temporal_events` |
| `cdawg` | `put_memory / log_event / recall_causal` |
| `episode_hdc` | `log_event_ex / recall_hdcbind` |
| `refutation_ledger` | `log_event / refutation_stats` |
| `cec_policy_store` | `queue_experiments / executor_flush / list_policies` |
| `decision_tape` | `log_decision / recall_true_counterfactual` |
| `hypothesis_market` | `queue_experiments / hypothesis_probes` |
| `turiya_monitor` | `consolidation_pass / turiya_status` |
| `fep_prior` | `put_memory / consolidation_pass / fep_status` |
| `observer` | `put_memory canonical BM25 terms` |
| `observer_state` | `put_memory canonical BM25 terms and saved facts` |
| `interaction_ledger` | `ledger_append / ledger_compile / ledger_query` |
| `predicate_store` | `predicate_attach / predicate_run / predicate_list` |
| `archive` | `put_memory writes process genomes; no reader found (retain: write dependency)` |
| `cortical_idx` | `encode_memory / cf_search_attractor / recall_with_fallback_windowed` |
| `hdc_idx` | `put_memory / recall_hdc` |
| `lite_encoder` | `subconscious.cpp` calls `train_lite_encoder`; runtime encoder API |
| `sparse_encoder` | `encode_memory / cf_reconstruction_error / cf_search_attractor` |
| `learners` | `select_route / recommended_window / recall_semantic_ctx` |
| `span_store` | `span_link_memory / span_query / span_for_memory` |
| `predictor` | `enqueue_recall_effects / predict_needed` |
| `surprise_store` | `record_surprise / query_surprises` |
| `epistemic_debt_store` | `register_debt / query_debts` |
| `integration_kernel` | `record_feedback / get_source_weights` |
| `surprise_learning` | `record_surprise credit and feedback / surprise_learning_stats` |
| `wisdom_promotion` | `upsert_wisdom_candidate / query_wisdom_candidates` |
| `learned_scorer` | `update_scorer_model / effective_scorer_weight` |
| `payloads / states / log / id_alloc` | core remember/get/WAL durability; not retirement candidates |
| `anchors` | memory insert/delete and `source_anchors`; core source-lifecycle write/read dependency added on main |
| `retrieval_surfaces` | Stage B retrieval surface / reembedding; snapshot sidecar |
| `assoc_edges / coactivation_stats` | recall_spreading / drain_pending_recall_effects; recall and snapshot |
| `semantic_idx` | recall_semantic_ctx / put_memory; recall/write |
| `time_idx` | recall_temporal / put_memory; recall/write |
| `keyword_idx` | recall_keyword_ctx / put_memory; recall/write |
| `artifacts / artifact_paths / artifact_idx` | recall_artifact / put_memory; recall/write |
| `triplet_store` | query_subject / recall_spreading / keyed relations; recall/write |
| `symbol_idx / call_graph / code_files` | code intelligence symbol/callgraph/file APIs |
| `chunk_hash_idx / content_prov_idx` | put_memory duplicate admission; write path |
| `prov_key_idx` | provenance_lookup; protected keyed lane |
| `correction_key_idx` | correction_check; protected keyed lane |
| `task_key_idx` | task_state_lookup; protected keyed lane |
| `realm_members / kind_members` | realm and kind filtering / put_memory; recall/write |
| `session_recent` | optional write-time same-session associations |
| `hopfield` | recall_field; recall API |
| `scoring_pipeline` | semantic and keyword ranking factors; recall |
| `realm_stats / kind_stats` | put_memory embedding statistics |
| `ack_scores` | acknowledged-use ranking; snapshot |
| `recall_provenance` | cross-instance recall evidence; snapshot |

### Ablation implementation validation

Post-merge Rust `85a9205` includes main `36039ba` and preserves named lock
profiling inside the ablation wrappers; superproject `5393cf8c` includes
main `3a2a2612`. Release builds pass with 768-dimensional
`nomic-embed-text-v1.5`, text-format-1 identity; `format-id` remains
`9230643459983636874`. The build wrapper has no `.chitta-embed-identity` file.
Rust: **301 passed, 2 ignored** (main's 295 plus six ablation tests).
All **31 CTests**, MCP **159**, SMRITI unit tests **46**, and all **27 hook
scripts** pass. The primary-node hook was rechecked after rebuilding its daemon;
the saddle timing recheck measured incremental p95 **41.83 ms**, after an
initial loaded run measured 210.1 ms against its 150 ms gate. Both logs remain.
The full owned NFS-copy chaos gate passes **9/9**. An earlier XFS-copy run
passed eight and skipped the NFS-only case; it is not the 9/9 evidence.
CI Ruff, shell syntax and ShellCheck pass. Contract checks printed
`contracts unchanged` at the commits. No tests were removed.

### Pinned controls and completed measurement blocks

Runner `f3ea9cd9` fixes the clock at **1789581502687 ms**, embedding wait at
**10000 ms**, and the three thread limits at **1**. All three control golden
scores are **0.5000783626223507**: spread **0**, inside the frozen
**0.0017901251267712533** margin. All 60 scored requests per control have
complete 768-dimensional embeddings. Calibrated hook samples are
**951/795/906 ms**, spread **156 ms**, inside the **1833.3949929025114 ms** band.

The four-group block has **12/12 completed trials**, three per group.
Together with controls, all **15 trials** recovered **200/200** acknowledged
writes with exact content-value matches after SIGKILL/WAL replay, answered
**3/3** keyed lanes and passed the canonical identity gate **20/20**. There
were no trial errors or empty prompt-hook outputs.

`recall_adjacent` and `all` each score **0.5047104720407914** in every run:
golden delta **+0.004632109418440722**, outside the frozen margin, hence
**not equivalent**. `event_api` and `event_prediction` match the control's
golden score in all three runs but remain **unqualified** for equivalence.
Current-truth observations are 4/40 answerable hits for controls/event groups
and 3/40 for recall-adjacent/all; all have 10/10 correct abstentions. These
truth counts are descriptive: their margins are absent from the frozen noise
file. No isolated SMRITI agent is configured, and no consumer-test commands
were supplied. SMRITI unit-test success is not a real visible-panel result.
The eight-largest block adds **24/24 completed trials**, three per organ.
`hdc_idx` scores **0.5050838724598726** in all three: delta
**+0.005005509837521904**, outside the frozen golden band, hence **not
equivalent**. Its answerable current-truth hits are 3/40. `cortical_idx`,
`cdawg`, `span_store`, `episode_hdc`, `event_tape`, `lite_encoder` and
`sparse_encoder` each match the control golden score and 4/40 truth hits in
all three runs; their equivalence remains **unqualified** for the missing
evidence above. All eight preserve 10/10 correct abstentions.

Across the first **39 completed trials**, every run passes 200-write recovery,
three keyed lanes and canonical restart identity **20/20**, with no trial
errors or empty hook outputs.

The remaining-organ block finished on **2026-09-17**, recording **105 trials**
for all **35 individuals** without triggering the recorded node-load limit
of **96**. `learners` scores **0.49787936370452585** in every repetition:
golden delta **−0.0021989989178248792**, outside the frozen band, hence
**not equivalent**. Its three runs preserve 4/40 answerable truth hits,
10/10 correct abstentions, 200 recovered writes, three keyed lanes and
identity 20/20. The other 34 individuals in this block are **unqualified**.

The complete post-fix matrix contains **144 records**: three controls,
four groups and 43 individual organs, each with three repetitions.
**142 trials** pass 200-write recovery, three keyed lanes and canonical
restart identity **20/20**. Two trials have incomplete invariants:
`agent_registry` repetition 2 and `interaction_ledger` repetition 3 both
report `[Errno 3] No such process`; cleanup confirms the owned replica was
not running. Their other two repetitions pass. These errors are retained
without replacement and disqualify both arms. The retained start/stop logs
do not establish why those processes disappeared; the runner removed their
scratch copies without retaining the daemon logs.

All 144 records have nonempty hook outputs. Predeclared primary hook samples
range from **795 to 1194 ms**, inside the frozen band around the **884 ms**
control mean. The separate second probe for `surprise_store` repetition 2
measures **2814 ms** and remains in the raw report alongside its predeclared
first probe of **899 ms**; samples were not selected after measurement.

Final individual verdicts: **2 not equivalent** (`hdc_idx`, `learners`),
**41 unqualified**, **0 equivalent**. Group verdicts: recall-adjacent and
all organs are not equivalent; event/API and event prediction are
unqualified. Missing current-truth calibration, a real isolated SMRITI
panel and per-organ consumer tests still block positive equivalence.
The table includes numbers only for qualified comparisons and gives the
reason for each unqualified row. Raw post-fix reports remain in
`/tmp/p2-ablation-postfix`; `report.json` SHA-256 is
`7942245fa6c380d13f0c2b7ac7707679235491bb47c083e9e61b269805e408cb`.

**Retired organs: none. Removed tools: none. Retirement lines removed: 0.
Removed tests: none.** Consumers and protected dependencies above remain the
reasons to keep each organ; no positive equivalence claim is made from
incomplete evidence.

### Quarantined earlier measurements

The lead stopped the original matrix after **105 completed trials** and an
interrupted sparse-encoder trial. Its controls were
0.5000783626223507 / 0.5000783626223507 / 0.4327613581907712: spread
0.0673170044315795 against the 0.0017901251267712533 margin. The pre-fix binary's
50 ms embedding deadline could silently omit semantic lanes under load, and
the clock was not pinned. These data, including restart failures, are
**unqualified, pre-fix**, retained under `/tmp/p2-ablation-matrix` and never
pooled with the pinned runs. The earlier diagnostic pilot
(`/tmp/p2-ablation-pilot`) had a multiline-JSON assertion bug and is also
excluded; the corrected content matcher has a regression test.

<!-- BEGIN CITATIONS -->
## References

- <a id="ref-4"></a>**[4]** Yu. A. Malkov and D. A. Yashunin. Efficient and robust approximate nearest neighbor search using Hierarchical Navigable Small World graphs. IEEE TPAMI 42(4), 824–836 (2020); arXiv:1603.09320 (2016). [source](<https://arxiv.org/abs/1603.09320>) [source](<https://doi.org/10.1109/TPAMI.2018.2889473>)
- <a id="ref-6"></a>**[6]** A. Blumer, J. Blumer, D. Haussler, R. McConnell, and A. Ehrenfeucht. Complete inverted files for efficient text retrieval and analysis. Journal of the ACM 34(3), 578–595 (1987). [source](<https://doi.org/10.1145/28869.28873>)
- <a id="ref-10"></a>**[10]** Pentti Kanerva. Hyperdimensional Computing: An Introduction to Computing in Distributed Representation with High-Dimensional Random Vectors. Cognitive Computation 1, 139–159 (2009). [source](<https://doi.org/10.1007/s12559-009-9009-8>)
- <a id="ref-24"></a>**[24]** Stephen Robertson and Hugo Zaragoza. The Probabilistic Relevance Framework: BM25 and Beyond. Foundations and Trends in Information Retrieval 3(4), 333–389 (2009). [source](<https://doi.org/10.1561/1500000019>)
<!-- END CITATIONS -->
