# Phase 5 policy migration report — 2026-09-16

> Superseded 2026-09-17 by [Phase 5b shell-policy retirement](PHASE5B-POLICY-RETIRE-REPORT.md): 8,988 → 2,696 shell lines, with all requested median latency gates met. The new report has current parity and timing evidence; the Phase 5 measurements below remain historical.

The admission, lane fusion and ledger moves are verified, but **Phase 5's exit
gate is not met**. `hooks/*.sh` grew from 8,743 to 8,988 lines. Final prompt and
Bash latency miss their targets; SessionStart meets its target. The default
paths now use daemon policy, while compatibility and timeout paths retain the
previous shell implementations.

## Verified commits

| Step | Commit | Change |
|---|---|---|
| 1 | `db94d23e` | Pinned fixtures, verbatim baseline output, real subprocess statuses and parity harness |
| 2 | `14a9744f` | `prompt_context` admission and shared native CLI fallback; regenerated RPC/MCP/docs contracts |
| 3 | `206c9b7f` | One in-process lane/fusion/admission call, lane statuses and stage attribution |
| 4 | `7f3234b3` | Native ledger/card/capsule assembly, queued turns and missing `ledger_op` queue dispatch |

The native admission policy links OpenSSL Crypto to preserve the previous
MD5-derived session deduplication keys.

The only new daemon RPC is `prompt_context` (MCP advanced tier). Ledger assembly
uses additional operations of the existing `ledger_op`. No `chitta-field` or
recall scoring source changed. Identical query strings share the existing query
embedding cache; different context and temporal query strings retain their
existing vector identities. Admission preserves the current KNOWN/UNKNOWN
boundary at raw maxrel 81; THIN was already merged in the previous hook.

A functional bug was fixed during Step 4: the queue dispatcher silently ignored
`ledger_op`, including Stop's capsule `session_bind`. The real isolated daemon
test now queues a capsule, waits for it to render through the ledger and checks
native turn event storage. Queue acknowledgement and transcript event payloads
remain unchanged.

## Parity and verification

Final gate: **30 measured adjacent old/new pairs and 40 warm-up pairs matched
stdout, stderr and exit/timeout status byte for byte**. Ten fixtures cover both
prompt adapters, SessionStart, both Bash result shapes, both Stop adapters,
positive handoff, explicit visible Stop plan and post-compaction ledger cards.
The gate requires actual daemon prompt/ledger execution, matching local lane
accounting, and unchanged queueing of the prepared capsule plus `hook_turn`.
Fallback-only runs cannot qualify the native migration.

The historical committed baseline is preserved. SessionStart correction rows
changed order across restarts and between separated whole-suite passes, including
unchanged-hook controls. `--reference-hooks` therefore runs each unchanged hook
immediately before its candidate on the same daemon. Raw rows are never sorted,
normalized or rewritten to hide differences. This establishes warm fixture
migration parity, not cold/restart recall identity, which was excluded by the
phase's instructions.

Final implementation gates passed: 28 shell test scripts, 33 CTest tests,
159 MCP tests, 46 SMRITI tests, 8 hook Python tests, CI Ruff check/format,
`bash -n`, and ShellCheck at warning severity on touched shell. Existing
informational ShellCheck findings remain. Contracts printed `contracts unchanged`
for the Step 4 and final report commits; Step 2 regenerated them for
`prompt_context`.

Early verification caught and fixed an invalid fusion request that silently
selected fallback, missing timing objects in the local outcome ledger, empty
successful ledger replies being accepted, and a second batch wait after a
pipeline timeout. The harness now explicitly detects these cases. An existing
Stop heartbeat test now waits for its intentionally asynchronous marker. Load-
sensitive saddle/MCP timing failures were retained in local evidence and their
checks passed on subsequent isolated runs; no timing bounds were relaxed.

## Shell line counts

Counts include all top-level `hooks/*.sh`, including comments and blank lines;
tests and subdirectories are excluded consistently in both arms.

| File set | Before (`a8ffcf8a`) | After Step 4 |
|---|---:|---:|
| `prompt-core.sh` | 1,787 | 1,937 |
| `stop-core.sh` | 1,115 | 1,143 |
| `session-start-hook.sh` | 778 | 833 |
| `lib.sh` | 686 | 698 |
| Four named files | 4,366 | 4,611 |
| Other top-level shell files | 4,377 | 4,377 |
| **All `hooks/*.sh`** | **8,743** | **8,988** |

The <3,000 gate fails. RPC request validation and compatibility routing added
245 lines while the old policy remains available for unavailable, invalid or
older RPC implementations. Even removing all four named files would leave
4,377 lines. Meeting this gate requires further fallback retirement and policy
migration in ancillary hook families; moving shell files into subdirectories or
compressing their formatting would not resolve the architectural problem.

## Timings and attribution

All values below are milliseconds, from five adjacent measured pairs after four
warm-up pairs per fixture, on the private replica with no other test workload
running. The before arm is Step 3's unchanged hooks on the same final daemon.
With five observations, nearest-rank p95 is the maximum observed value. Shared
node load remains variable; these small samples are descriptive, not a
statistical claim of a latency improvement.

| Hook fixture | Before median / p95 | After median / p95 |
|---|---:|---:|
| `prompt` | 426.4 / 487.8 | 414.7 / 425.9 |
| `codex-prompt` | 418.9 / 426.3 | 456.9 / 634.3 |
| `session-start` | 304.3 / 353.3 | 304.0 / 348.3 |
| `bash` | 87.2 / 89.1 | 94.9 / 117.2 |
| `codex-bash` | 88.8 / 98.2 | 94.7 / 269.1 |
| `stop` | 716.8 / 725.2 | 845.0 / 913.8 |
| `codex-stop` | 703.4 / 910.0 | 790.3 / 1000.3 |
| `session-handoff` | 296.6 / 320.9 | 335.2 / 371.8 |
| `stop-handoff` | 707.6 / 841.9 | 777.9 / 890.2 |
| `session-compact` | 281.9 / 512.9 | 295.3 / 306.6 |

The controlled Step 3 fusion comparison separately measured prompt median
457.3 → 420.3 ms and Codex prompt 474.3 → 447.5 ms. Step 4's comparisons above do
not show an improvement in SessionStart and show higher Stop medians. Do not
attribute changes in untouched Bash/prompt paths to ledger assembly.

Final prompt daemon medians: primary embedding cache lookup 0 ms, retrieval
including embedding 76 ms (Claude) / 75 ms (Codex), and admission 4 ms. Lane spans
overlap and must not be summed. The approximately 335 / 378 ms remainder includes
hook setup, IPC, parsing, rendering, enrichment and scheduling; it is not measured
as shell CPU time. Sampled session-ledger and capsule assembly medians rounded to
0 ms at millisecond resolution; their client transport/setup time is outside that
span.

Bash's added overhead is measured per invocation against an empty Bash stdin
reader: 92.4 ms for Claude-shaped results and 92.1 ms for Codex-shaped results,
with reader medians 2.6 / 2.7 ms. These are separate from full-hook wall time.

| Exit target | Final median | Verdict |
|---|---:|---|
| Prompt ≤400 ms | 414.7 ms; Codex 456.9 ms | Unmet |
| SessionStart ≤400 ms | 304.0 ms | Met |
| Bash added ≤15 ms | 92.4 ms; Codex 92.1 ms | Unmet |
| Top-level shell lines <3,000 | 8,988 | Unmet |

The plan's earlier 608 / 599 / 24 ms figures are historical reference values,
not matched controls for these runs. The initial Phase 5 harness measured
prompt 594.0 ms, SessionStart 297.9 ms and full Bash hook 93.7 ms; the matched
comparisons above are the appropriate evidence for each move.

## What remains in Bash

- Frontend envelopes, local availability/safety checks, timeouts and compatibility
  fallback implementations preserve current behavior when native RPCs fail.
- Transcript slicing/parsing, git branch and changed-path inspection, session
  markers, cursor advancement and durable local queue acknowledgement depend on
  client-local files and process state.
- Query preparation, optional enrichment, correction surfacing counters and
  final end-to-end timing remain in the hook. The daemon cannot measure client
  setup/rendering time.
- Ancillary PreToolUse/PostToolUse/PreCompact, distillation and maintenance hook
  policies were not migrated by these four implementation steps. Their line and
  latency costs remain part of the unmet exit gates.

## Reproduction and evidence

The replica is a private copy of `learning-cut-20260915-frozen`, managed with
`scripts/eval-replica.sh`: snapshot `da86decb`, sequence `206502617`, manifest
generation `39285`, private port `17435`. Recall clock is
`CHITTA_RECALL_NOW=1789516800000`; parity additionally sets `CHITTA_HOOK_NOW`.
Actual timeout/deadline checks remain real. BLAS, OMP and Rayon threads are each
one; the requested conda CPython/compiler were used. Measurement unpins hook
presentation time and captures native profile responses.

Use `scripts/bench-hook-parity.py check --reference-hooks <unchanged-hooks>
--require-pipeline --require-ledger --replica <private-copy> --cli <built-cli>
--work <private-tmp-work> --results <private-results>`; `measure --repeat 5` records
latency separately. See the fixture README for setup and baseline rules.

Raw output, timings and test logs remain outside Git under
`/tmp/chitta-p5-policy/evidence/`, notably `step3-verified-*`,
`step4-parity-final`, `step4-timing-final` and `step5-parity`. Only the requested small synthetic
fixtures and baseline outputs are committed. No deployment or push was performed.
