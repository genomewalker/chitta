# SessionStart latency evidence — 2026-09-13

Scope: `hooks/session-start-hook.sh`, its new concurrency regression test, and
one status line in `docs/HOOKS.md`. No shared helper or task_ledger module change.
Independent reads and registry registration run in private `/tmp` result lanes;
ordered assembly preserves the existing sections. The two task renderers share
one Python process. Correction JSON is parsed once, followed by parallel tag
lookups. Sentinel cleanup overlaps retrieval. An outer process-group deadline
bounds setup, lane waits and assembly; nested timeouts use `--foreground` so
children cannot escape that deadline. Session environment retains the original
hook caller PID. Registry stdin is explicitly inherited by its background lane.

## Measurement protocol

Baseline is the branch's original `session-start-hook.sh` (git blob `40a2634a`).
Run the hook directly under Bash, never through an adapter. A separate fd 9
records xtrace with `PS4='+T$(date +%s%N) ${BASHPID} ${BASH_SOURCE}:${LINENO}: '`;
fd 9 is opened before exporting `BASH_XTRACEFD=9`. No EPOCHREALTIME is used.
One trace precedes each arm's ten untraced runs; timing uses Python's monotonic
clock around `subprocess.run`, including shell startup and captured output.

Both HEADLESS aliases are unset. Each arm uses `probe-ss-1` through `probe-ss-10`
and exactly this input (cwd is the physical worktree path):

```json
{"session_id":"probe-ss-N","cwd":"/maps/projects/caeg/scratch/kbd606/tmp/codex-wt-fix-session-start","hook_event_name":"SessionStart","source":"startup"}
```

The live CLI is `/home/kbd606/.claude/bin/chitta`. Its socket is resolved before
changing HOME. The wrapper forwards only `realm_detect`, `ledger_load`,
`soul_context`, `sql_query`, `recall`, `triplet_history` and `query_triplets`,
using that explicit live socket. Other CLI operations return success without
writing. HOME, XDG_RUNTIME_DIR, CHITTA_DB_PATH and CHITTA_QUEUE are scratch paths.
CHITTA_SOCKET_PATH is a bound, non-listening scratch socket, isolating Python
registry/task-ledger RPCs. A scratch plugin directory contains the original
Python modules, with no maintenance scripts. Correction surface counts reset
before each run. The detected realm is `project:codex-wt-fix-session-start`.

This measures live retrieval with isolated hook writes and empty task cards;
it does **not** reproduce live-home NFS unlink latency or a populated daemon
registry/task ledger. The user's original NFS measurements (691 + 614 ms for
cleanup) remain a separate observation. The new 300 ms stub test exercises
populated task cards and registration as well as all output sections.

## Untraced results

| Arm | Runs | Median | p95 (nearest rank) | Empty outputs | Nonzero exits |
| --- | ---: | ---: | ---: | ---: | ---: |
| Before | 10 | 3116.5 ms | 4137 ms | 0 | 0 |
| After | 10 | 739.5 ms | 1573 ms | 0 | 0 |

Before samples in ms: 4137, 3666, 3981, 3927, 3120, 2956, 2887, 3113, 2973, 2801.
After samples in ms: 1573, 952, 891, 890, 929, 589, 553, 529, 485, 488.
All 20 outputs contain 992 bytes; every corresponding before/after output is
byte-identical. The median is below 1500 ms; p95 remains slightly above it.
Live measurements include cache warming and daemon variability; these are
observed timings, not a frozen-replica quality evaluation.

## Baseline trace: every gap over 50 ms

Traced total: 7340 ms. Locations below refer to the original hook; `lib.sh:86`
is the shared registry timeout. Multiple numbers on one row are separate gaps,
in occurrence order. All 26 qualifying gaps are included. In a pipeline, Bash
sometimes attributes the wait to the writer (`echo`) or reader (`head`); the
operation column names the work that actually consumed the interval.

| Original location | Operation | Gaps, ms |
| --- | --- | --- |
| lib.sh:86 | Python registry registration | 145.7 |
| session-start-hook.sh:317 | Python render_inbox | 150.9 |
| session-start-hook.sh:323 | Python render_threads | 161.3 |
| session-start-hook.sh:330 | soul_context | 55.4 |
| session-start-hook.sh:425 | Scoped project recall | 1040.5 |
| session-start-hook.sh:435 | Fallback project recall | 1054.4 |
| session-start-hook.sh:458 | Correction recall | 173.6 |
| session-start-hook.sh:468 | Python correction filter | 89.8 |
| session-start-hook.sh:570 | Python correction enumeration, loop substitution | 86.6 |
| session-start-hook.sh:474 | Five Python ID extractions | 94.8, 81.5, 80.0, 72.3, 84.6 |
| session-start-hook.sh:475 | Five Python text extractions | 88.6, 84.7, 74.4, 78.6, 101.1 |
| session-start-hook.sh:479 | Five tag RPC / Python parsing pipelines | 95.2, 87.1, 79.4, 98.1, 86.9 |
| session-start-hook.sh:502 | Compliance recall, head reader waits | 450.0 |
| session-start-hook.sh:532 | Cache recall, head reader waits | 380.6 |

## After trace attribution

Traced total: 3877 ms. These durations include xtrace/date subprocess overhead
and overlap; do not add them. A final `:` in each lane marks completion in the
trace without introducing a timing subprocess during ordinary hook execution.

| Lane | Duration, ms |
| --- | ---: |
| Cleanup, strict mode and turn reset | 74.1 |
| Registry | 226.3 |
| Soul context | 63.7 |
| SQL themes / kinds / counts / recent | 15.5 / 13.6 / 14.4 / 17.4 |
| Ledger load | 19.5 |
| Combined task renders | 143.0 |
| Correction recall | 413.6 |
| Correction parsing, parallel tags and ordered suppression | 435.2 |
| Probe triplets | 14.8 |
| Compliance recall | 973.1 |
| Cache recall | 1074.1 |
| Scoped / fallback recall | 2011.9 / 2015.5 |

Both project recall lanes reached their preserved 2-second per-call timeout
in this traced run. Every global trace gap over 50 ms was a retrieval wait:
159.3 ms awaiting correction recall at `_read_lane`; 110.7 ms until compliance
completed; 121.9 ms until cache completed; 1109.2 ms until project recall timed
out. Concurrent trace interleaving places the last three gaps after another
lane's completion marker; the marker itself is not the expensive operation.

## Validation

- `bash -n` on the changed shell files; `git diff --check`.
- All 12 `hooks/tests/*.sh` suites pass, with existing suites unchanged.
- New regression: 300 ms per CLI call, exact populated section text/order,
  one process for both renderers, and completed registry registration: 1077 ms.
- Scoped/fallback selection, independent renderer failure, 64-bit correction
  IDs, tag suppression and the five-surface limit, compact restoration and
  clear resume text all pass.
- A 350 ms hook budget bounds a stalled registry and TERM-ignoring CLI calls;
  CLI descendants are dead or reaped on return. Tests capture both stdout and
  stderr, detecting children that keep either pipe open.
- Ruff passes on all embedded Python; PyPy 3.9.18 compiles it and imports the
  unchanged `task_ledger` module.
- MCP: 117 tests pass using existing cached MCP 1.27.2 via PYTHONPATH. The first
  run with the conda environment's older SDK failed its HTTP-session test
  because `_session_owners` was absent. Nothing was installed or changed there.
- SMRITI: 46 tests pass. Its printed synthetic `FAIL` outcomes are fixtures;
  the unittest suite itself reports OK.

No binaries, services, installed hooks, live mind files, or external messages
were changed. Raw traces contain memory text and are kept out of the commit.
