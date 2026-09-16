# Phase 5b shell-policy retirement — 2026-09-17

Phase 5b meets the measured line and median-latency gates. Top-level
`hooks/*.sh` falls **8,988 → 2,696 lines**. All 70 adjacent fixture pairs match;
no golden fixture bytes changed. Prompt, SessionStart, Stop and Bash meet the
requested budgets on the same private frozen replica. Hooks and daemon ship
together; there is no older-RPC compatibility implementation in the hooks.

## Verified steps

| Step | Commit | Top-level shell lines |
|---|---|---:|
| Phase 5 merge, starting point | `0fe85a96` | 8,988 |
| 1: prompt fan-out, fusion, admission and local CLI fallback removed | `a979edcb` | 8,325 |
| 2: Stop/SessionStart ledger compatibility removed | `a81c297c` | 8,033 |
| 3: ancillary native policy and thin local envelopes | `31236c0e` | 2,696 |
| 4: this report, hook documentation and changelog | this documentation revision | 2,696 |

Counts include comments and blank lines, using `wc -l hooks/*.sh`. Policy moved
into C++ native helpers, not a replacement shell implementation in Python.
Python modules provide local input/output and transport. No Rust, recall scoring,
`ablate-organs.py`, snapshot/WAL format or advertised RPC schema changed.
`contract-snapshot.sh check` prints **contracts unchanged**.

## Family boundaries

| Family | Shell lines before → after | Moved to daemon | Retained locally and why |
|---|---:|---|---|
| Prompt | 1,937 → 8 | Query gates, scheduling, enrichment, fusion/admission and output assembly through one `prompt_context` | JSON/markup envelope, local state and transcript size, queue/marker acknowledgement, optional local inference process transport |
| Shared library | 698 → 442 | Deleted dead query/session/admission helpers | Queue/runtime paths, native heartbeat transport, Stop marker parsing and local accounting still have callers |
| SessionStart | 833 → 8 | Startup/clear/compact selection, corrections, memory import and all cards | Project/transcript paths, git inspection, process identity, local markers and maintenance launches |
| Stop | 1,143 → 1,030 | Ledger/checkpoint/card/capsule assembly | Bounded transcript slicing, marker extraction, git paths, cursor/queue ACK, local context safeguards and existing local saddle telemetry |
| PreToolUse | 657 → 85 | Search/read policy, dedup, routing, wakeup budget and saddle decisions | `safety_check` remains shell by design; hashes, file metadata and enforcement markers are local |
| PostToolUse Bash | 324 → 8 | Outcome classification, milestones, job/habit policy and recall notices | Payload envelope, local outcome append, git/provenance/file metadata; Codex unknown exits remain null |
| PreCompact / restore | 627 → 20 | Checkpoint and continuity selection/rendering | Bounded transcript input and local file/queue transport |
| Distillation / Dream sweep | 667 → 29 | `distill_now` owns distillation; native sweep eligibility and cross-session synthesis | Staging header, transcript scan/counts, lock, temporary synthesis file and processed-session ACK |
| Persona / run ledger | 209 → 16 | Catalog/selection and task/realm classification | CLI arguments, raw transcript and timestamp; no embedded persona fallback |
| MEMORY.md interception | 109 → 8 | Import eligibility and enhanced memory text | Read/write the requested local file after native acknowledgement |
| Tool spans / subagent completion | 236 → 16 | Reward/outcome classification and observation/summary assembly | Bounded Stop snapshot, transcript existence and span JSONL append |
| Shepherd | 360 → 16 | Active task enumeration and error/completion policy | Terminal/pane addressing, layout/screen capture, alert transport and local poller invocation |
| FileChanged / codebase read | 95 → 16 | Re-index decisions and throttling | File existence/path, local rate marker and queue ACK; edits/deletions still refresh the index |
| Bash history | 65 → 8 | Command eligibility | Append to the local history file |
| Post-commit maintenance | 38 → 5 | Cleanup selection and crystallization plan | Git changed paths, bootstrap file existence and queue ACK |
| Codex wrappers | Stop 10 → 4; others unchanged | Shared prompt/PreToolUse/compact policy uses the native paths above | Frontend schema normalization, native session-registry identity and lifecycle envelopes; Stop defaults are applied once in the shared core |
| Other retained helpers | unchanged | No duplicate daemon assembly retained | Artifact hashes/comment metadata, offline code export, operator service manager, hook-name manifest, immutable outcome append and evaluation transport need local resources |

Native read plans use private operations within the existing `ledger_op` schema.
Their returned writes pass through the existing local durable queue. Unsupported
queue tools now use allowlisted `hook_apply`, explicitly classified as a write;
pure assembly plans are reads. No plan does a store sync merely to render output.

The audit removed nonfunctional SQL topology/probe calls and queued
`file_watch_register`: the current daemon registers none of those operations.
Shepherd reads the existing native task store instead of an unregistered list RPC;
its event payload is a string and tags are an array, matching the native handler.
Run-ledger retains its historical `n_wisdoms:0` placeholder: the old query-less
recall was rejected, so it never supplied a session-specific count. Adding such a
count is outside this retirement. The unsafe optional hook SQZ formatter and
unused lean selector are retired. Current alias, ablation and safety controls
are described in [HOOKS.md](HOOKS.md).

## Parity and gates

Each implementation step has before/after parity controls. The final Step 3
check compares the previous committed hooks against the new hooks adjacent on
one daemon: ten fixtures × (four warmups + three repetitions) = **70 identical
pairs**, with `--require-pipeline --require-ledger`. The report step uses a 20-pair before control and a 70-pair after check
around documentation changes. The ten fixtures cover both prompt
frontends, SessionStart, both Bash/Stop frontends, positive handoff and compact.

Tests that previously simulated a missing native implementation now require
`[chitta] daemon unavailable; context not loaded.` and exit zero. They no longer
expect shell lane fan-out, standalone admission, shell cards or local LLM
extraction. Native fixtures retain lane/admission expectations, u64 IDs, frozen
card bytes, correction suppression, delayed-read concurrency, queued registration,
lease and transcript records, and bounded process-group timeout behavior. The
chaos fixture's barrier now recognizes stdin JSON-RPC; its kill/recovery assertions
remain. All 81 saddle cases compare native policy with the Python reference under
a fixed clock; compiling the fixture cannot age a future event into the present.

Verified: **32 hook shell tests, 159 MCP tests, 46 SMRITI tests, nine hook Python
tests, CI Ruff check/format, changed-shell bash syntax/ShellCheck, and 33 CTests**.
The final native run had two synchronization failures under combined load:
`daemon_isolation_test` read a log before its expected path appeared, and
`task_ledger_integration_test` observed no inferred task yet. Both passed unchanged
on isolated rerun; an earlier complete native run passed all 33. These failures
are retained in evidence, not hidden by changing assertions.
The final shell suite also hit the saddle integration fixture's 3-second
subprocess deadline while another fixture was compiling. The unchanged saddle
test passed in isolation, including its timeout and latency assertions; the
other 31 shell tests passed in the original run. Both logs are retained.

The documentation control exposed a helper lifecycle regression: new Python
launchers had used separate process groups, unlike the old shell background jobs.
That let private notifier processes survive fixture cleanup. The launcher now
preserves the frontend's process group; prompt helpers reuse it. A regression
test checks the actual helper PID/group. All 134 verified notifier processes from
this stream's private homes were terminated individually; no live process was
touched. Clean parity and timing runs after that fix supersede earlier timings.

## Full wall time

Five measured repetitions after four warmups, `bench-hook-parity.py measure
--repeat 5`, no concurrent worktree build/test workload. Values are milliseconds;
p95 is the largest observation with this sample size. Targets apply to medians;
they are not p95 guarantees. Bash is measured as the whole hook process, including
startup, not as added overhead above an empty shell.

| Fixture | Phase 5 final median | Phase 5b median | Phase 5b p95 |
|---|---:|---:|---:|
| `prompt` | 414.7 | 194.693 | 264.031 |
| `codex-prompt` | 456.9 | 194.931 | 336.432 |
| `session-start` | 304.0 | 200.101 | 409.120 |
| `bash` | 94.9 | 83.397 | 94.480 |
| `codex-bash` | 94.7 | 64.529 | 221.939 |
| `stop` | 845.0 | 691.566 | 2366.373 |
| `codex-stop` | 790.3 | 615.021 | 13735.637 |
| `session-handoff` | 335.2 | 177.077 | 393.533 |
| `stop-handoff` | 777.9 | 610.999 | 762.909 |
| `session-compact` | 295.3 | 171.845 | 208.402 |

The Phase 5 column is the historical report, not a newly paired estimate of effect.
Final gates: prompt ≤400, SessionStart ≤400, Stop median ≤720, Bash full-wall
median ≤90 ms. Every listed frontend/positive variant meets its median budget;
tail observations exceed these median targets, including 2,366.373 ms for Stop
and 13,735.637 ms for Codex Stop. The harness recorded no process timeout or
nonzero exit. Shared-node load and five samples limit statistical conclusions.

### Attribution and the Stop regression

Phase 5 reported Stop 716.8 →845.0 ms. The native capsule renderer itself does
not explain that 128.2 ms increase: the isolated capsule experiment measured
73.455 /74.392 /71.553 ms for Phase 5-before /Phase 5-after /retired, including
transport. Final ledger assembly spans have a 0 ms median (millisecond resolution).

The investigation found avoidable surrounding work: the 150 ms compatibility
probe could time out and trigger duplicate shell work, and pure ledger assembly
operations were missing from the read classification, forcing field-store sync.
The global-lock tests now count actual sync calls with the lock on and off.
Step 2 also decodes its snapshot once, skips empty span capture and avoids redundant
queue-directory creation. Step 3 removes shell span policy and repeated client
process work, decodes context-window fields in the existing input pass, skips
canonical-path subprocesses for an explicit plugin root, and removes duplicate
Codex Stop normalization already performed by the core. Handoff response validation
and extraction use one JSON pass. The final Stop 691.566 ms meets the 720 ms gate. The evidence supports
these removed costs; it does **not** justify attributing the entire historical
128.2 ms to one renderer or one sync call.

Prompt native medians are 0 ms embedding-cache lookup, 82 ms retrieval
including embedding, and 4 ms admission. Full wall time includes Python,
CLI, local acknowledgement and native enrichment/rendering beyond those spans.

An early five-repeat run missed Bash at 97.715 ms. Process tracing showed three
`dirname` execs plus `realpath` even with an explicit plugin root. The envelope
now uses that root directly and retains Python's alias handling. These initial
comparisons preceded the helper-lifecycle fix and are not final qualification.
The final Bash median is 83.397 ms; its maximum is
94.480 ms. Earlier missed gates and a 4550.318 ms handoff-Stop
outlier remain in evidence.

After the lifecycle fix, one run still measured Codex Stop 726.080 ms even though
both Stop fixtures have identical payloads and now execute the same core.
A separate seven-pair helper startup experiment measured 132.232 ms with site
initialization versus 101.251 ms with `-S`. Snapshot/commit/thread inference use
only standard-library and explicit local imports; the final hook skips site
initialization for those subprocesses. All nine hook Python tests also pass with
`-S`. This removes a measured fixed startup cost; it does not assign every
run-to-run difference or tail outlier to Python. Final Codex Stop median is
615.021 ms, and positive handoff-Stop is 610.999 ms.

## Replica and reproduction

Only `/tmp/chitta-p5b-retire/replica`, a COPY of
`/projects/caeg/scratch/kbd606/tmp/learning-cut-20260915-frozen`, was used for
experiments through `scripts/eval-replica.sh`. Private port 17436; snapshot
`da86decb`, seqno 206502617, manifest generation 39285. No live-mind writes, install,
service operation, push or Rust/recall-scoring edits occurred.

`CHITTA_RECALL_NOW=1789516800000`, `CHITTA_RECALL_EMBED_WAIT_MS=10000` on the
scratch daemon; parity pins `CHITTA_HOOK_NOW` to the same milliseconds. Measure
mode unpins presentation timing, as in Phase 5. All runs set
`OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1 RAYON_NUM_THREADS=1`. Python and CXX
come from the specified bioinfo conda environment.

```bash
P5_PY=/maps/projects/fernandezguerra/apps/opt/conda/envs/bioinfo/bin/python3
export OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1 RAYON_NUM_THREADS=1
"$P5_PY" scripts/bench-hook-parity.py check \
  --replica /tmp/chitta-p5b-retire/replica --cli "$PWD/bin/chitta" \
  --reference-hooks /tmp/chitta-p5b-retire/step3-reference/hooks \
  --work /tmp/chitta-p5b-retire/verify-work --results /tmp/chitta-p5b-retire/verify \
  --repeat 3 --warmup 4 --require-pipeline --require-ledger
"$P5_PY" scripts/bench-hook-parity.py measure \
  --replica /tmp/chitta-p5b-retire/replica --cli "$PWD/bin/chitta" \
  --work /tmp/chitta-p5b-retire/timing-work --results /tmp/chitta-p5b-retire/timing \
  --repeat 5 --warmup 4 --require-pipeline --require-ledger
```

Raw evidence/logs remain untracked under `/tmp/chitta-p5b-retire/evidence`:
`step3-stdlib-parity`, `step3-qualified-final`, earlier `step3-lifetime-timing` and `step3-envelope-timing`
and `step3-qualified-timing` (including the missed Bash gate and leaked-helper runs), `capsule-profile.json`, native/hook/MCP gate logs and
`step4-*` controls. `Plan.md` remains untracked. Only this report summarizes
results in the repository.
