# Prompt hook evolution evidence

Proposal: 69a8f047e822354f. Base: 54bc7e6be5e4695bf6df3a57611f34ab1c7fa949.

## Result and scope

Fixed the confirmed last-session rendering defect. The proposed fixed ~2 s
floor was not reproduced, so no latency optimization is claimed or implemented.
The <900 ms median target and preregistered -1200 ms gain remain unmet. The
caller owns frozen-replica evaluation, the verdict, and human review/merge.

Only prompt continuity parsing, its regression tests, and documentation changed.
Lane scheduling, correction checks, C2/admission, the six-process fallback,
and the `[admit]` / `t:` contracts are unchanged. No native build is needed.

## Mechanism investigation

The old continuity path took the first nonblank recall line. The daemon prints
`Found N results ...` before memory results, so the hook injected a heading and
lost the actual memory line. The replacement selects the first canonical
`#<id> [pct%] [type] content` line with nonblank content. Header-only, empty,
warning-only, and metadata-only output is suppressed; populated output retains
the memory, with the existing 150-byte bound. The SessionStart recap card is a
separate path and was not changed.

Timestamped `bash -x hooks/prompt-core.sh` traces showed no independent fixed
2 s wait. One RPC took 2014 ms with its correction lane reporting 2001 ms;
other observed gaps included heartbeat work (230–268 ms) and session-summary
recall (299 ms). The fallback waits only for launched lane PIDs; the RPC path
does not call `_wait_lanes`. Its zero-ms corrk result is a completed daemon lane,
not evidence of an orphaned shell process. The C2 retry remains conditional on
missing hybrid signal. These observations do not identify the original floor's
root cause, and do not justify changing those mechanisms.

Bash here is 4.4.20, which does not expose EPOCHREALTIME. The first trace attempt
therefore had no timestamps; an early BASH_XTRACEFD assignment also complained
until the fd was opened for the child. The useful trace used
`PS4='+T$(date +%s%N) ${LINENO}: '` and fd 9 opened at invocation. This adds
considerable overhead: trace totals are not benchmark results. Trace only the
core, since the adapter execs a new shell. Raw traces stay in /tmp because recall
can contain unrelated live memory text.

## Before/after diagnostic benchmark

Run from the worktree, before and after the code change, without editing the
benchmark:

```bash
env -u CHITTA_HEADLESS -u CC_SOUL_HEADLESS \
  CHITTA_BENCH_BIN=/home/kbd606/.claude/bin/chitta \
  bash scripts/bench-recall-lanes.sh 5
```

Each arm has 15 runs (five repetitions of each of three fixed queries), in the
benchmark's off/on order. Realm: brahman. Budget: benchmark default 6000 ms.

| Arm | Before median / p95 ms | After median / p95 ms | Empties before / after |
|---|---:|---:|---:|
| Standalone (`off`) | 1311 / 3009 | 1344 / 3129 | 0 / 0 |
| RPC (`on`) | 1210 / 3105 | 1212 / 2893 | 0 / 0 |

Logs: `/tmp/chitta-evolve-before.log`, `/tmp/chitta-evolve-after.log`.
The initial run inherited both HEADLESS aliases set to 1 and returned all empty;
that bypass-only run was discarded, not counted as fast recall. A subsequent
attempt failed its read-only status probe. The successful before run overlapped
part of the tracing work; these are shared-live-daemon diagnostics, not a
controlled evaluation. Lane contention and timeout tails were observed. There
is no measured improvement attributable to the continuity fix.

The worktree has no bin/chitta. The existing live CLI was used through the
benchmark's read-only allowlist; HOME, mind, queue, and runtime were temporary.
No installations, service operations, live stores, push, or merge were performed.
No holdout results were read and no evaluation files were modified.

## Validation

- Existing prompt lane suite passed before edits.
- New end-to-end continuity cases failed on the old parser, then passed after
  the fix, in both RPC and standalone modes. A large output budget and a
  positive soul-context assertion prevent truncation or an early exit from
  disguising failures. Existing ablation, C2, telemetry, and RPC fallback cases
  remain passing.
- All seven `hooks/tests/*.sh` suites passed. The isolated post-bash test first
  failed because its default CLI was absent under temporary HOME; rerunning
  with CHITTA_BIN=/bin/true passed without any source changes to that test.
- MCP: 99 tests passed; SMRITI: 46 passed; Python hook suite: four passed.
  Python suites used CPython 3.12.3 at
  `/maps/projects/fernandezguerra/apps/opt/conda/envs/bioinfo/bin/python3`.
- Shell syntax, whitespace, and frozen-path checks are recorded in Plan.md.
  ShellCheck is not available on PATH and was not installed.

Tests ran with temporary HOME (including .claude/mind), runtime, mind, queue,
and task ledger, with inherited CHITTA_/CC_SOUL_ settings cleared. The live
measurement flags were cleared only for those measurement subprocesses.
No native sources changed, so Rust/CMake build gates do not apply.

## AGENTS.md review

Before: clarity 3/5, completeness 3/5. After: clarity 4/5, completeness 4/5.
These are implementer ratings based on this task, not an external evaluation.
Hard constraints stayed first. Added supplied-worktree reuse, isolated hook
state, dual-headless bypass diagnosis, absent worktree CLI handling, safe
post-bash test setup, trace entrypoint/clock details, and honest measurement
reporting. Removed duplicate deployment commands, unused model roster and
effort claims, duplicate sqz instructions, and the overbroad command-prefix
warning. The root recovery instruction now agrees with the stream deployment
ban. The combined instruction files are 12,905 bytes (below 13,000).

The existing official AGENTS.md source was checked during this edit:
https://learn.chatgpt.com/docs/agent-configuration/agents-md . New operational
advice above comes from local source inspection and this run's observed failures.
