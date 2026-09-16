# chitta Hooks System

Status as of 2026-09-16.

chitta integrates with Claude Code and Codex through the hooks system, enabling
automatic context injection and lifecycle management.

**This page is the enforcement reference.** A hook is a deterministic gate;
`CLAUDE.md` and `AGENTS.md` therefore point here rather than restating hook rules
as prose. Per the [Claude Code memory
doc](https://code.claude.com/docs/en/memory), [34](#ref-34) "to block an action regardless…
use a PreToolUse hook" — CLAUDE.md instructions "are not a hard enforcement
layer." When a convention needs to actually hold, add it here, not there.

**Sources for the 2026-09-13 documentation pass** (all verified that day):
Anthropic, *The new rules of context engineering for Claude 5 generation models [37](#ref-37)*
(claude.com/blog, 2026-07-24);
[Claude Code memory](https://code.claude.com/docs/en/memory) (target under 200
lines per CLAUDE.md; imports expand inline and do not save context);
[Prompting Claude Fable 5.1](https://platform.claude.com/docs/en/build-with-claude/prompt-engineering/prompting-claude-fable-5-1) [35](#ref-35)
(effort default `high`, levels `low|medium|high|xhigh|max`; re-sweep per model
generation);
[Skill authoring best practices](https://platform.claude.com/docs/en/agents-and-tools/agent-skills/best-practices) [36](#ref-36)
(`name` ≤64 chars, `description` ≤1024 chars in third person stating what and
when, SKILL.md body under 500 lines, match freedom to fragility);
Codex [AGENTS.md](https://learn.chatgpt.com/docs/agent-configuration/agents-md) [38](#ref-38)
(combined `project_doc_max_bytes` 32 KiB; global, then git-root, then cwd, closer
files overriding) and its approvals reference [39](#ref-39);
`gpt-6-astra` ([announcement](https://openai.com/index/gpt-6-astra/) [41](#ref-41), 2026;
the previously cited 2026-09-04 publication day is unverified).

Community-verified Codex defects, GitHub issues Sept 2026, cited in
`codex-plugin/AGENTS.md`: #13386 [42](#ref-42) and #37956 [43](#ref-43) (doc truncation is silent and drops
the end of the file, so the project keeps combined `AGENTS.md` bytes under ~10 KB
and puts critical rules first); #34289 [44](#ref-44) (no `PostToolUse` exit code, see the
PostToolUse warning above); #26602 [45](#ref-45) (`-a/--ask-for-approval` must precede `exec`);
#41378 [46](#ref-46) (`bearer_token_env_var` propagation); #41600 [47](#ref-47) (streamable-HTTP session
leak); #43194 [48](#ref-48), #43335 [49](#ref-49), #42449 [50](#ref-50) (context-management state loss — worktree files, not
model memory, hold run state); #44305 [51](#ref-51) (tool-output amplification).

---

## Table of Contents

- [Overview](#overview)
- [Hook Files](#hook-files)
- [Hook Events](#hook-events)
- [PreToolUse — pre-tool-hook.sh](#pretooluse--pre-tool-hooksh)
- [Code-Intel Shadow Logging and Enforcement](#code-intel-shadow-logging-and-enforcement)
- [Environment Variables](#environment-variables)
- [Configuration](#configuration)
- [Transparent Memory](#transparent-memory)
- [Proactive Learning](#proactive-learning)
- [Subconscious Daemon](#subconscious-daemon)
- [Custom Hooks](#custom-hooks)
- [Troubleshooting](#troubleshooting)

---

## Overview

Hooks are shell commands that execute in response to Claude Code events. chitta uses hooks for:

1. **Context injection** — Soul state and relevant memories appear automatically
2. **Transparent memory** — Memories surface without explicit MCP calls
3. **Proactive learning** — Corrections, preferences, and milestones detected automatically
4. **Session continuity** — State saved and restored across sessions via ledger
5. **Tool safety** — Destructive commands blocked, expensive reads truncated
6. **Background processing** — Subconscious daemon management

### Two Hook Systems

chitta provides hooks in two forms:

| System           | Files                                                                               | Used When                        |
|------------------|-------------------------------------------------------------------------------------|----------------------------------|
| **Plugin hooks** | `hooks/hooks.json` + `hooks/*.sh`                                                   | Installed as Claude Code plugin  |
| **Settings hooks** | `hooks/*.sh` + `~/.claude/settings.json`                                          | Standalone installation          |

The `smart-install.sh` script configures the appropriate system automatically.

### Hook Output Format

All lifecycle hooks write JSON `hookSpecificOutput` schema on stdout. This is not the old "SSL format" (`[conf%:type]`) from earlier versions — that format is obsolete.

---

## Hook Files

| Script                  | Event            | Purpose                                                                              |
|-------------------------|------------------|--------------------------------------------------------------------------------------|
| `session-start-hook.sh` | SessionStart     | Realm detection, code re-indexing, soul context injection, ledger restore, git diff  |
| `prompt-hook.sh`        | UserPromptSubmit | Wraps `prompt-core.sh`: six-lane realm-scoped recall, admission filtering, code symbol injection, anticipation prediction, learning hints |
| `prompt-core.sh`        | UserPromptSubmit | The recall fan-out and admission policy itself. See the [recall pipeline page](https://genomewalker.github.io/chitta/recall.html) |
| `stop-hook.sh`          | Stop             | Extract `[SOLUTION]`, `[GOTCHA]`, `[PREFERENCE]`, `[DECISION]`, `[FAILURE]`, `[PATTERN]`, `[LEARN]` blocks; anticipation recording; ledger save |
| `pre-compact-hook.sh`   | PreCompact       | Save ledger checkpoint before context compaction                                     |
| `pre-tool-hook.sh`      | PreToolUse       | Safety checks, file dedup, large-file truncation, Write guards, Agent routing, ScheduleWakeup budgeting |
| `post-bash-hook.sh`     | PostToolUse:Bash | Check the command result, append a `bash_outcome` event to the outcome ledger        |
| `outcome-ledger.sh`     | Library          | Fail-open JSONL append of `injected` / `recall_empty` / `bash_outcome` / `session_end` events |
| `session-end-hook.sh`   | SessionEnd       | Session teardown and the closing ledger event                                        |
| `resume-inject-hook.sh` | SessionStart:resume | Inject the recap capsule when a session resumes                                   |
| `compact-restore-hook.sh` | SessionStart:compact | Restore context after a compaction                                             |
| `subagent-stop-hook.sh` | SubagentStop     | Capture what a subagent learned, asynchronously                                      |
| `file-changed-hook.sh`  | FileChanged      | Re-index a changed file, asynchronously                                              |
| `log-bash-history.sh`   | PostToolUse (async) | Append every Bash command to history for pattern learning                         |
| `memory-intercept.sh`   | PostToolUse:Write (async) | Capture Write operations to learn what you build                           |
| `artifact-trace.sh` | PostToolUse:Write (async) | Register newly written scripts as artifact signals with path, hash and purpose |
| `distill.sh`            | Background       | Transcript distillation into compressed wisdom nodes                                 |
| `subconscious.sh`       | SessionStart     | Start/stop/restart/status for the chittad daemon                                     |

**Where these run.** Inside `hooks.json`, lifecycle handlers resolve against
`${CLAUDE_PLUGIN_ROOT}` — the plugin cache — while the `PreToolUse` matchers and
most `PostToolUse` matchers resolve against `${HOME}/.claude/hooks`. A source
checkout is neither, so editing one changes nothing until you install.
`scripts/dev-install.sh` symlinks both live paths back at a checkout; re-run it
after a plugin update, which can replace those symlinks with a fresh clone.

**Communication:** All scripts use a Unix domain socket to talk to the daemon (fast path). Falls back to the `chitta` thin client if the socket is unavailable. Socket path is derived from the mind path using a djb2 hash: `/tmp/chitta-{hash}.sock`.

---

## Hook Events

### SessionStart

Status 2026-09-13: SessionStart concurrent lanes preserve output order and enforce the hook budget; 10 isolated live-read runs improved median/p95 from 3116.5/4137 ms to 739.5/1573 ms (992 bytes each, 0 failures); the 300 ms/call stub completes in 1077 ms. [Attribution and gates](../hooks/tests/session_start_latency.md).

#### Native hook paths: final worktree measurements (2026-09-15)

Prompt cleanup, recall-response validation, and telemetry use bash/sed/jq;
registration, task cards, and heartbeats use the native CLI. Failed or unreachable
heartbeat RPCs retain durable queueing. Tests now stub those CLI operations and
verify registration, concurrent recall, offline queueing, and unchanged card text.
RPC validation requires exactly one JSON response; empty responses trigger fallback.

The unchanged `scripts/bench-recall-lanes.sh 5` ran three queries five times per
arm, with both HEADLESS aliases unset, a warm live daemon, isolated HOME/mind/queue,
and a CLI wrapper allowing only reads. Values are the hook's `t:total` in ms.

| Prompt arm | Runs | Before median / p95 ms | Final median / p95 ms | Final empties |
|---|---:|---:|---:|---:|
| RPC off | 15 | 842 / 1134 | 693 / 843 | 0 |
| RPC on | 15 | 822 / 1063 | 608 / 717 | 0 |

The benchmark command and capability/status probes succeeded. Its unchanged runner
swallows individual hook exit statuses, so a separate per-hook failure count is
unavailable. Shared node/daemon load is uncontrolled; these are live diagnostics,
not a controlled speedup claim. **The prompt median target of <500 ms is unmet.**

Ten SessionStart runs used `source:startup`, `session_id:probe-0` through `probe-9`,
the worktree cwd, and no transcript. All state was temporary; maintenance scripts
were excluded via a temporary plugin root, live read APIs were allowed, and write
APIs were no-ops. Times include the whole process tree, measured by wall clock.

| SessionStart | Runs | Before median / p95 ms | Final median / p95 ms | Final failures / empties |
|---|---:|---:|---:|---:|
| Startup | 10 | 693 / 834 | 599 / 677 | 0 / 0 |

Final samples (rounded ms): 677, 592, 650, 512, 637, 596, 617, 543, 500, 602.
Median is computed before rounding; p95 uses nearest rank. SessionStart meets
its <700 ms median target.

**Remaining prompt costs.** A separate five-run RPC-on diagnostic inserted sparse
phase timestamps into a temporary hook copy using the same read-only isolation.
For `how does chitta semantic recall use the field store`, the middle run by total
reported 747 ms (phase boundary sum 745 ms plus 2 ms to collect total telemetry):

| Sequential phase | Measured ms |
|---|---:|
| Input, query/turn/queue preparation, and dispatch setup | 112 |
| Recall RPC plus jq validation and lane-file writes | 255 |
| Lane collection, C2 extraction, and recall merge | 37 |
| Candidate filtering: confidence/token pipelines, metadata checks, deduplication, anchor logging | 174 |
| Lane selection and admission summary | 20 |
| Exposure and correction bookkeeping | 28 |
| Regex-gated intent detection | 19 |
| Narrative, predicates, habits, goals, curiosity, and inbox enrichment | 92 |
| Output assembly up to total checkpoint | 8 |

That run's overlapping daemon lanes reported sem=7, ctx=2, hyb=197, kw=42,
corr=226, corrk=0 ms; these are concurrent and must not be added together.
The final rendering interval was another 47 ms after the total checkpoint
(including the 2 ms above). `t:total` excludes final JSON emission and EXIT
telemetry. Five diagnostic checkpoint totals were 745, 806, 712, 778, 663 ms;
instrumentation and changing load mean this table does not decompose the separate
608 ms benchmark median.

The final `grep -n 'python3' hooks/prompt-core.sh hooks/lib.sh` leaves only explicit
RLM [71](#ref-71) exploration, classifier invocation after required regex evidence, periodic
hint extraction (every sixth turn with transcript/model), and `registry_call`
(used by compatibility/lifecycle callers, absent from the native prompt path).
Installing the classifier model no longer starts Python on every ordinary turn;
model-plus-regex decisions remain unchanged. Explicit RLM mode still requires Python.

[Detailed implementation and validation](../hooks/tests/hook_nopython_latency.md).
Raw benchmark/harness evidence: `/tmp/chitta-nopython-finish/` and
`/tmp/chitta-nopython-evidence/` on the measurement node.

**What chitta Does:**
1. Auto-install binaries if needed (`smart-install.sh`)
2. Start subconscious daemon (`subconscious.sh start`)
3. Detect current realm/project
4. Incremental code re-indexing (only changed files)
5. Auto-register transcript for distillation
6. Load soul context (nodes, triplets, confidence, status)
7. Surface user profile (expertise, preferences, style)
8. Load active goals
9. Surface behavioral corrections
10. Load ledger checkpoint (pending tasks, next steps, mood)
11. Detect git changes since last session

### Stop

**What chitta Does:**
1. Read Claude's response from stdin
2. Extract tagged blocks: `[SOLUTION]`, `[GOTCHA]`, `[PREFERENCE]`, `[DECISION]`, `[FAILURE]`, `[PATTERN]`, `[LEARN]`
3. Auto-detect missed learning opportunities (corrections, preferences not explicitly tagged)
4. Record anticipation patterns (context to action)
5. Verify predictions from earlier anticipation
6. Auto-checkpoint every 5 turns or on meaningful work
7. Save ledger with session state

### UserPromptSubmit

**What chitta Does:**
1. Extract user message from stdin JSON
2. Context recovery: if user says "continue"/"resume", re-inject full soul context
3. Detect learning opportunities (corrections, preferences, frustration, milestones)
4. Determine proactive surfacing tags
5. Run `full_resonate(query)` via daemon socket
6. Filter results: only ≥25% relevance, strip type prefixes, max 500 chars
7. Surface code symbols if query is code-related
8. Run `anticipation_predict` for context-based predictions
9. Output combined context

#### Automatic learning admission

> **Automatic learning admission.** Compression of
> chat-shaped evidence did not establish useful learning selection. The Bash
> distillation tap and native shadow machinery are removed; existing storage,
> deduplication and recall ranking continue. Admission means eligibility for
> retrieval after deduplication, with no utility-posterior hard gate. Historical
> shadow logs remain untouched and inert. See the
> [retirement status and pending paired 20-task experiment](EVOLVE.md).
> Outcome hooks remain active, recording associational evidence for offline
> analysis; null exit codes are excluded from known outcomes, while `likely_fail`
> heuristics can still count as failures in the current joiner.

#### Daemon task ledger

Threads, inbox items, artifacts, session bindings and exclusive
thread leases are daemon-owned records. Hooks call the native `chitta` CLI
(`session_register`, `session_heartbeat`, `ledger_op`) with bash/jq rendering;
they do not start the Python registry or open a local SQLite ledger. `task_ledger.py` preserves its
public API and renderers. Its single `ledger_op` RPC stores atomic row-change
batches in the Rust event WAL; a snapshot section preserves ledger and native
session events. Indexed maps are rebuilt on startup. Read pages are capped at 100;
the Python client follows pages for larger or unlimited lists.

`session_registry.py` remains a compatibility client for Python callers.
The hook path calls `chitta session_register`, `chitta session_heartbeat` and
`chitta session_deregister` directly. Those handlers update durable bindings and leases,
including when invoked by the private mind's `queue.jsonl` processor. The
Python compatibility `heartbeat --queued` path only appends a queue item; it
performs no RPC. Its 100 ms daemon-wait and 75 ms RPC budgets describe that
client, not the native hook path. Native hook calls use their enclosing shell
timeouts and queue failed heartbeats. Unavailable or warming daemons return empty results; registration
reports `registered:false`. No hook starts a daemon or falls back to SQLite.

The endpoint is `CHITTA_SOCKET_PATH` when explicitly set; otherwise the client
resolves the socket from `MIND_PATH` / `CHITTA_DB_PATH` / `CHITTA_MIND` and the
runtime directory. `CHITTA_RPC_PORT` plus optional `CHITTA_RPC_HOST` selects HTTP
when an explicit socket is absent. Hook tests must isolate these endpoint settings
as well as HOME, runtime, mind and queue.

Migrate the legacy SQLite file once against the intended daemon:

```bash
python3 chitta-mcp/task_ledger.py migrate --from /path/to/legacy-ledger.db
```

Migration opens the source with SQLite URI `mode=ro`, preserving every row's keys,
nulls, timestamps and metadata. It imports all five tables by stable primary key;
existing daemon records win, so reruns neither duplicate records nor overwrite
newer changes. A failure exits nonzero and can be safely resumed. SQLite is used
only by this explicit migration command; the old ledger-path environment variable
has no effect on normal operation.

#### Prompt latency investigation and session continuity (2026-09-13)

Proposal `69a8f047e822354f` reported a ~2 s floor despite fast recall lanes.
The unchanged worktree benchmark did **not** reproduce a fixed floor: 15 runs
per arm measured median/p95 totals of 1311/3009 ms (standalone) and
1210/3105 ms (RPC), with zero empties. Timestamped core traces showed actual
recall waits, including a 2014 ms RPC with a 2001 ms correction lane, plus
shell/heartbeat/enrichment overhead. A status probe also timed out. These live
measurements do not establish the preregistered gain or a root cause for the
original floor. That earlier investigation left scheduling unchanged and did
not meet the <900 ms target. Its full evidence is in
`git show 7ba22801:Documentation.md`; the follow-up below supersedes its status.

The empty `[last-session] Found 1 results ...` message had a confirmed cause:
`prompt-core.sh` selected the first nonblank recall line, which is the summary
header, and discarded the memory body. Continuity now requires a canonical
memory result with nonblank content before emitting `[last-session]`. Empty,
warning-only, summary-only, and metadata-only responses produce no heading.
Regression coverage exercises both RPC and standalone recall paths.
SessionStart now also requires a canonical memory line with nonblank content,
after removing episodes. A header or weak warning cannot emit a realm recall
card; empty scoped results still retry unscoped, and a single surviving memory
is sufficient. Cards render memory lines first within the 1200-byte cap.

#### Concurrent prompt work and hook noise metric (fix/hooks, 2026-09-13)

Final paired diagnostic (unchanged benchmark, N=5 per query, 15 runs per arm):

```bash
env -u CHITTA_HEADLESS -u CC_SOUL_HEADLESS \
  CHITTA_BENCH_BIN=/home/kbd606/.claude/bin/chitta \
  bash scripts/bench-recall-lanes.sh 5
```

| Arm | Before median / p95 ms | Final median / p95 ms | Empties before / final |
|---|---:|---:|---:|
| Standalone (off) | 1502 / 5783 | 1076 / 3047 | 0 / 1 |
| RPC (on) | 1368 / 3011 | 1094 / 5092 | 0 / 0 |

**Targets remain unmet:** RPC median is above 900 ms, p95 above 2000 ms;
standalone also produced one empty. All paired commands completed successfully
and passed their status/capability probes, but the unchanged benchmark swallows
individual hook exit statuses, so a separate per-hook failure count is unavailable.
The final run had no concurrent local test or trace workload. Shared live daemon
load is uncontrolled; these data do not establish an accepted latency gain.

Logs: `/tmp/fix-hooks-before.log`, `/tmp/fix-hooks-after-final.log`.
Intermediate after runs were also retained: concurrency alone measured off
1194/3308 and on 1122/2995 ms; the first run including the hash fix measured off
1214/3999 and on 1135/6549 ms, with zero empties in both intermediate runs.
The baseline's tail overlapped the first trace; the first hash-fix benchmark
overlapped the evaluator smoke's tail and subsequent local tests.

The heartbeat now runs independently into `$_ld/heartbeat`, with its output
descriptor redirected at launch and no prompt-side wait. Session-summary recall
starts alongside the query lanes into `$_ld/session`; only its rendering
consumer joins it. EXIT cleans the shared scratch directory, including early
empty-recall exits. An empty lane PID list cannot accidentally wait for unrelated
jobs. The six-process fallback, lane timeouts, C2, admission, ledger events, and
`[admit]`/`t:` formats are preserved. A stub handshake proves heartbeat and
continuity overlap the RPC and that continuity is joined before rendering.

Timestamped `bash -x hooks/prompt-core.sh` traces used fd 9 and
`PS4='+T$(date +%s%N) ${LINENO}: '` (Bash 4.4, no EPOCHREALTIME), with the
benchmark's temporary HOME/mind/queue and read-only CLI wrapper. Before tracing
showed a 212 ms registry call and about 194 ms around continuity recall. It also
exposed socket hashing's per-character command substitutions; `printf -v`
replaces those subprocesses without changing DJB2. The hash regression covers
empty input, punctuation, long paths, and 32-bit overflow. A later trace hit the
3000 ms RPC timeout and fell back, so daemon waits remain a material tail risk.
Trace totals are not benchmark numbers. Raw traces remain in /tmp. The final
post-hash trace attempt failed its read-only daemon status probe before tracing;
the valid before/after traces cover the concurrency change.

`benchmarks/noise.py` now includes `hook_total_ms`. One observation is the median
of the three fixed queries from `scripts/bench-recall-lanes.sh`; N observations
supply sample SD and the existing 2-SD acceptance threshold. The report also
retains the median of those observations and every query total. Timings come
only from the existing `t:` total field; empty, ambiguous, failed, or timed-out
hook executions fail evaluation rather than becoming zero-ms samples.

The hook runner isolates HOME, runtime, mind, queue, and daemon endpoint, clears both
HEADLESS aliases, and uses the benchmark's read-only RPC allowlist. Full
calibration includes the hook panel by default. For hook-only calibration,
source a frozen replica's env (socket, snapshot ID, replica mind), then use
`--hook-only --hook-runs N`. Timing calibration needs no agent execution; live
smoke remains `acceptance_ready=false`, so `noise.py band hook_total_ms` refuses it.

Only one live evaluator smoke was run, during implementation; full calibration
was not run:

```bash
env -u CHITTA_HEADLESS -u CC_SOUL_HEADLESS \
  CHITTA_BENCH_BIN=/home/kbd606/.claude/bin/chitta \
  bash scripts/eval-noise.sh --hook-only --hook-runs 5 \
  --output /tmp/fix-hooks-noise.json
```

The 15 hook invocations produced five panel medians:
3635, 2400, 2160, 1273, 1292 ms. Median: 2160 ms; mean: 2152 ms;
sample SD: 971.223 ms; descriptive 2-SD threshold: 1942.446 ms.
This proves metric plumbing only, not a calibrated gain. It preceded the final
socket-hash measurement and is not the final before/after benchmark.

Validation: all 11 `hooks/tests/*.sh` suites, 99 MCP tests, and 46 SMRITI tests
pass with isolated state. Changed shells pass `bash -n`; `noise.py` passes ruff
and imports on PyPy 3.9.18. The evaluator tests cover panel aggregation, isolated
launch, missing/failed output, and live-band rejection. The initial SMRITI run
caught older programmatic callers lacking `hook_runs`; they now retain their
offline behavior. No native code changed, so no cargo/cmake commands were run.

### PostToolUse

**What chitta Does:**
- **post-bash-hook.sh**: Records significant Edit/Write operations as signals for background learning
- Note: `capture-hook.sh` is currently disabled (exits immediately). Write safety checks are handled by `pre-tool-hook.sh`, not the post-bash hook.

**Codex exit codes are unknown, not zero.** Codex `PostToolUse` fires for failures
too, but its payload carries no exit code and `PostToolUseFailure` never fires
(Codex issue #34289). The tell is the payload shape: `tool_response` is the
aggregated output **string** rather than an object, which is how
`post-bash-hook.sh:44` detects a Codex call.

For that shape the hook records `"exit_code": null` in the `bash_outcome` ledger
event — explicitly unknown — and adds `"likely_fail": true` when the output text
matches a failure heuristic (`No such file`, `command not found`, `Permission
denied`, a Python traceback, `fatal:`, `FAILED`, a non-zero `Exit code N`).

Consumers must treat null as unknown and never as success. Both already do:
`chitta-mcp/outcome_ledger.py` counts only entries whose `exit_code` is not null
and declines to issue a verdict for a window in which nothing is known, and
`chitta-mcp/saddle_detector.py` has a `failed()` helper that falls back to
`likely_fail` when the code is null. `hooks/tests/test_post_bash_payloads.sh`
covers all three Codex cases (null exit recorded, failure text flagged, clean
output not flagged).

Before this, a missing field defaulted to 0 and every Codex failure was booked as
a success — the same defect class the comment at `post-bash-hook.sh:33-35` records
for Claude Code, where the hook saw only exit 0 for ten days until
`PostToolUseFailure` was registered.

### Recall scheduling telemetry

The prompt hook appends a compact timing field to its admission summary:

```text
[admit] C2:KNOWN(55%) sem:1 hyb:1 | drop conf:2 | t:sem=412,hyb=3105!,kw=88,total=3270
```

Times are wall milliseconds. `!` means that lane either reached `timeout` or
returned an empty output file. Only attempted lanes are shown; `total` is hook
wall time measured just before context emission.

The outcome ledger's `injected` event carries the same data as structured
`lane_ms` and `lane_timeout` objects plus `hook_ms`. If recall completes without
usable context, including after the cross-realm fallback, the hook appends a
`recall_empty` event with those timing fields even though it emits no context.
This keeps empty prompt-hook runs diagnosable without changing fail-open output.

The prompt hook's six recall lanes run as one `recall_lanes` RPC by default
(`CHITTA_RECALL_LANES_RPC=1`, since 2026-09-13; set `0` for the six-process
path). Measured on the live daemon, 3 queries × 20 runs per arm: median total
1496 → 1201 ms, p95 3208 → 3087, zero empties either way; the RPC alone returns
all lanes in ~190 ms. Ablated lanes are omitted from the request, the
cross-realm `xr` lane remains a separate fallback, and an unavailable or invalid
fan-in response falls open to the process fan-out for that prompt. The
remaining ~2 s of hook time is sequential pre-lane work (heartbeat, session
recall, C2), not recall — see the evolve backlog.

### PreToolUse

Handled by `pre-tool-hook.sh`. See the [full section below](#pretooluse--pre-tool-hooksh).

### PreCompact

**What chitta Does:**
1. Save current state to ledger checkpoint
2. Record that compaction is happening
3. Ensure work state is preserved for continuation

---

## PreToolUse — pre-tool-hook.sh

`pre-tool-hook.sh` is the main safety and intelligence orchestrator. It handles these tool matchers:

```
Read | Edit | Write | Bash | Agent | ScheduleWakeup
```

### Bash Matcher

**Stage 1 — Safety blocks (exit 2, command is cancelled):**

| Pattern                      | Block reason                    |
|------------------------------|---------------------------------|
| `rm -rf /` or `rm -rf ~`    | Destroys root or home           |
| `chmod -R 777 /`            | Exposes entire filesystem       |
| `dd` to a raw disk device   | Overwrites disk directly        |

**MCP transport guard (structured deny, exit 0):** Bash command text containing
`pkill`, `kill`, or `killall` targeting `chitta-mcp` (including the
`chitta-m[c]p` spelling) is denied. This covers explicit `--http` targets
and broad matches that include the HTTP transport. A `kill` PID-selection
pipeline may pass only with an explicit `grep -v -- --http` exclusion;
filtering the kill command's output does not qualify. Compound commands are
checked independently so an exclusion elsewhere does not unlock a broad kill.
The reason points to `scripts/dev-install.sh`. The explicit hook-environment
bypass is `CHITTA_ALLOW_MCP_KILL=1`; headless mode does not bypass this guard.

**Stage 2 — Find/grep/ls fallback strategy:**

- Unbounded `find` on `/` or `~` gets scoped: `chitta recall` is called first to get a memory hint; `find` is limited to a specific directory with `-maxdepth 3`. Set `CHITTA_DEEP_SEARCH=1` to allow root-wide search.
- Grep content mode is capped at 50 matches via `head_limit` injection (not 200 — the 200 figure in older docs was wrong).
- Glob results are capped at 100 entries via `head_limit` injection.

**Stage 3 — Bash pattern recall (2s timeout, realm-scoped):**

Detects command patterns and injects relevant corrections and gotchas from memory:

| Pattern detected | Memory tags queried |
|-----------------|---------------------|
| R / Rscript     | r-lang, bioconductor |
| Python / conda  | python, conda, environment |
| git             | git, version-control |
| cmake / make    | cmake, build        |
| daemon / systemctl | daemon, service  |

**Task ledger pre-stage:**

Detects trackable commands (`sbatch`, `srun`, `nohup`, python scripts, bash scripts) and snapshots the current file state for provenance tracking before the command runs.

### Read Matcher

1. **Per-turn dedup**: A sentinel file `$MIND/.soul_injected_<session>_<turn>` ensures soul recall is injected only once per turn, not on every Read call.
2. **Realm detection**: Calls `chitta realm_detect` before injecting any recall. If realm detection fails, injection is skipped entirely to prevent cross-project memory bleed.
3. **Code-intel advisory**: If the file is indexed in chitta, suggests using `read_symbol` or `smart_context` instead of reading the whole file. Advisory only appears if chitta has indexed the file.
4. **Large-file truncation**: Reads of files >200 lines are truncated to the first 150 lines. (Older docs said ">500 lines → head -200"; the actual thresholds are 200 and 150.)
5. **Read dedup cache**: Tracks file reads by mtime hash at `$MIND/.read_cache_<session>`. Repeat reads of unchanged files return a 13-token `§ref:HASH§` reference via sqz instead of full content.
6. **Strict-mode enforcement for indexed files**: When enforcement is active, reading a fully-indexed file is blocked with a suggestion to use symbol-level tools instead. Bypass for the session with `CHITTA_ALLOW_READ=1` or by creating the flag file `$MIND/.allow_read_<session_id>`.
7. **System-path exception**: Enforcement is skipped for paths under `/site-packages`, `/usr/lib`, `/opt/*/lib`, and similar system locations — those files are never indexed.

### Write Matcher

1. **Blocks temp patch scripts**: Rejects Write calls to paths matching `/tmp`, `*/scratch`, `*patch*`, `*fix_*`, `*edit_*` and tells Claude to use the `Edit` tool directly instead.
2. **Delegates to file_patch for large files**: If the target file exists and is ≥50 lines, routes through the `file_patch` MCP tool rather than a full rewrite.

### Agent Matcher

1. **Haiku routing**: Agents whose `subagent_type` or `description` matches
   search/research patterns are rerouted to Haiku 4.5 (`claude-haiku-4-5`) with a
   ≤200 word limit injected. Two cases are never rewritten:
   - an Agent call that already carries an explicit `model` (`sonnet`, `opus`,
     `haiku`, or `fable` — all four pass through untouched);
   - `subagent_type: "fork"`, because a fork inherits the parent model by
     definition and the Agent tool ignores `model` for forks.

   Bypass the routing entirely with `CHITTA_AGENT_NO_FORCE=1`.
2. **Subagent count tracking**: Increments a per-session counter.
   - Warns at `CHITTA_AGENT_WARN` (default: 20).
   - Hard advisory at `CHITTA_AGENT_LIMIT` (default: 50).

### ScheduleWakeup Matcher

Guards autonomous loops (agents that reschedule themselves):

- Warns at `CHITTA_LOOP_WARN` iterations (default: 10).
- Blocks at `CHITTA_LOOP_LIMIT` iterations (default: 20).

This is separate from the Agent subagent count above.

---

## Code-Intel Shadow Logging and Enforcement

`pre-tool-hook.sh` logs every Read/Edit decision to `$MIND/.hook_shadow.jsonl`.

### Log Fields

| Field      | Description                                      |
|------------|--------------------------------------------------|
| `ts`       | Unix timestamp                                   |
| `tool`     | Tool name (`Read`, `Edit`, etc.)                 |
| `file`     | Absolute path to the file                        |
| `lines`    | Line count of the file                           |
| `indexed`  | Whether chitta has indexed this file (`true`/`false`) |
| `decision` | `allow` or `deny`                                |
| `reason`   | Short string explaining the decision             |
| `enforced` | Whether the hook was in enforce mode             |

### Auto-Activation of Enforcement

Enforcement auto-activates when **both** conditions are met:

1. Shadow log has **≥100 entries**
2. Shadow log is **≥3 days old**

No manual environment flip is needed. The log accumulates in shadow mode (decisions logged but not enforced), then silently switches to enforce mode once the threshold is met.

### Enforcement Controls

| Variable                 | Effect                                                            |
|--------------------------|-------------------------------------------------------------------|
| `CHITTA_HOOK_ENFORCE=1` | Force enforce mode on immediately (skip the 3-day wait)          |
| `CHITTA_HOOK_ENFORCE=0` | Force shadow-only mode (disable enforcement even after threshold) |
| `CHITTA_ALLOW_READ=1`   | Bypass Read deny for this session                                 |

The flag file `$MIND/.allow_read_<session_id>` is an equivalent per-session bypass for `CHITTA_ALLOW_READ=1`.

### Log Rotation

The shadow log auto-rotates when it reaches **10 MB**. The current log is renamed to `.hook_shadow.jsonl.1` and a fresh log starts.

### Review Data

```bash
./scripts/hook-stats.sh   # decisions, reasons, tool split, enforce status
```

---

## Environment Variables

Every `CHITTA_*` variable below also works under its pre-rename `CC_SOUL_*`
name, except the new explicit `CHITTA_ALLOW_MCP_KILL` bypass
(see [docs/RENAME.md](RENAME.md)).

| Variable                      | Default      | Description                                                              |
|-------------------------------|--------------|--------------------------------------------------------------------------|
| `CHITTA_HOOK_ENFORCE`        | (auto)       | `1` = force enforce; `0` = force shadow only                             |
| `CHITTA_ALLOW_READ`          | `0`          | `1` = bypass Read deny and indexed-large deny for this session           |
| `CHITTA_ALLOW_MCP_KILL`      | `0`          | `1` in the hook environment bypasses the MCP transport kill guard; no legacy alias |
| `CHITTA_DEEP_SEARCH`         | `0`          | `1` = allow root-wide `find` (removes `-maxdepth 3` scoping)             |
| `CHITTA_STRICT_MODE`         | `0`          | `1` = enforce symbol-level flow for indexed files                        |
| `CHITTA_AGENT_NO_FORCE`      | `0`          | `1` = disable Haiku routing for search/research agents (explicit `model` and `subagent_type: fork` are exempt regardless) |
| `CHITTA_AGENT_WARN`          | `20`         | Subagent count at which a warning is issued                              |
| `CHITTA_AGENT_LIMIT`         | `50`         | Subagent count at which a hard advisory fires                            |
| `CHITTA_LOOP_WARN`           | `10`         | ScheduleWakeup iterations before warning                                 |
| `CHITTA_LOOP_LIMIT`          | `20`         | ScheduleWakeup iterations before block                                   |
| `CHITTA_SUBAGENT_BASH_RECALL`| `0`          | `1` = run Bash recall for subagent calls (adds ~2s per call, default off)|
| `CHITTA_MAX_WAIT`            | `5`          | Max seconds to wait for daemon responses                                 |
| `CHITTA_CACHE_TTL_MIN`       | `60`         | Prompt-cache TTL in minutes; the `[cache-expired]` banner fires only past it (was a fixed 5 min) |
| `CHITTA_MIN_QUERY_TOKENS`    | `1`          | Turns with fewer *distinctive* tokens (5+ chars, not in the generic-word list) than this run only the correction lanes; "Do all", "is it working now", "what else is missing" pull nothing. Exactly one distinctive token raises the admission floor to 70 |
| `CHITTA_KW_SINGLE_TOKEN_MIN` | `60`         | Keyword rows need this BM25 [24](#ref-24) confidence when the turn has fewer than two distinctive tokens |
| (no knob)                     | built in     | UNKNOWN-band anchor: a hybrid/keyword row is admitted only if it shares a *distinctive* turn token (5+ chars, not in the generic-word list in `prompt-core.sh`); "manage better short messages" anchors nothing, "session registry sqlite" does |
| `CHITTA_PRETOOL_MIN_SIM`     | `0.6`        | `pre-tool-hook` "BEFORE RUNNING" injection requires this semantic similarity; the tag fallback (similarity 0) is never injected |
| `CHITTA_FILE_TRACES`         | `1`          | On the first Read of a file per session, `pre-tool-hook` adds `[traces]`: up to two correction/wisdom/signal/preference memories whose text names that file (one keyword RPC, exact-name filter). Stigmergy per SwarmWorld [14](#ref-14) (arXiv:2608.26081): reuse starts by observing the artifact. `0` disables |
| (no knob)                     | built in     | `artifact-trace.sh` (PostToolUse Write) stores `[artifact] <path> sha:<8> purpose:<first comment>` as a shared signal for scripts written outside temp/scratch dirs, so later sessions fork the script instead of rewriting it. It is the trace the row above surfaces |
| `CHITTA_CLI_AUTOSTART`       | unset        | `chitta mcp` no longer spawns a daemon when it cannot connect; set `1` to restore that for ad-hoc setups. The systemd unit owns the daemon |
| `CHITTA_STORE_LOCK`          | `1`          | Store directory lock; `0` lets two processes open the same mind dir (never in production) |
| `CHITTA_SOCKET_PATH`         | derived      | Daemon socket override, honoured by the CLI and by `get_socket_path` in every hook; the SMRITI runner and `eval-replica.sh` set it so hooks talk to the frozen replica |
| `CHITTA_QUEUE`               | `<mind>/queue.jsonl` | Fire-and-forget write queue shared by hooks, MCP and the daemon; derived from the mind dir (`/tmp` does not persist across nodes). `CHITTA_QUEUE_PATH` is a legacy alias, `CHITTA_NO_QUEUE=1` disables consumption |
| `CHITTA_RECALL_LANES_RPC`    | `1`          | One `recall_lanes` RPC for all prompt-hook lanes (default on since 2026-09-13; `0` = six CLI processes) |
| `CHITTA_MCP_LAG_INTERVAL_MS` | `250`        | MCP asyncio scheduling-delay sampling interval in milliseconds            |
| `CHITTA_MCP_LAG_WARN_MS`     | `200`        | Scheduling delay that increments `over_count` and logs a warning          |
| `CHITTA_LEAN`                | `false`      | Ultra-lean context mode (stats only)                                     |
| `DEBUG_SOUL`                  | `0`          | `1` = enable debug output to stderr                                      |
| `SUBCONSCIOUS_INTERVAL`       | `60`         | Daemon cycle interval in seconds                                         |

This table covers the enforcement and budget knobs only. The hook scripts read
far more than this — around 97 distinct `CHITTA_*` / `CC_SOUL_*` names, most of
them internal. Enumerate the full set with:

```bash
grep -ohE 'CHITTA_[A-Z_]+|CC_SOUL_[A-Z_]+' hooks/*.sh | sort -u
```

The user-facing subset, with the old-to-new name mapping, is tabulated in
[docs/RENAME.md](RENAME.md). Recall-specific knobs — pool depth, the pre-filter,
lane ablation, the confidence band — are documented on the
[recall pipeline page](https://genomewalker.github.io/chitta/recall.html).
The MCP `health_check` tool includes process-local `mcp_loop_lag.max_lag_ms`
and `mcp_loop_lag.over_count` counters; the two knobs above also honor their
`CC_SOUL_MCP_LAG_*` aliases.

**Testing manually:** `CHITTA_HOOK_ENFORCE=1 bash hook.sh Read` will NOT work because the prefix assignment is not exported to nested bash. Use `export CHITTA_HOOK_ENFORCE=1` first.

---

## Configuration

### Plugin Mode (hooks.json)

```json
{
  "hooks": {
    "SessionStart": [
      {
        "matcher": "*",
        "hooks": [
          {"type": "command", "command": "${CLAUDE_PLUGIN_ROOT}/hooks/subconscious.sh start"},
          {"type": "command", "command": "${CLAUDE_PLUGIN_ROOT}/hooks/session-start-hook.sh"}
        ]
      }
    ],
    "UserPromptSubmit": [
      {
        "matcher": "*",
        "hooks": [
          {"type": "command", "command": "${CLAUDE_PLUGIN_ROOT}/hooks/prompt-hook.sh", "timeout": 10}
        ]
      }
    ],
    "Stop": [
      {
        "matcher": "*",
        "hooks": [
          {"type": "command", "command": "${CLAUDE_PLUGIN_ROOT}/hooks/stop-hook.sh"}
        ]
      }
    ],
    "PreCompact": [
      {
        "matcher": "*",
        "hooks": [
          {"type": "command", "command": "${CLAUDE_PLUGIN_ROOT}/hooks/pre-compact-hook.sh"}
        ]
      }
    ],
    "PreToolUse": [
      {
        "matcher": "Bash",
        "hooks": [
          {"type": "command", "command": "${CLAUDE_PLUGIN_ROOT}/hooks/pre-tool-hook.sh Bash", "timeout": 10}
        ]
      }
    ],
    "PostToolUse": [
      {
        "matcher": "Bash",
        "hooks": [
          {"type": "command", "command": "${CLAUDE_PLUGIN_ROOT}/hooks/post-bash-hook.sh", "timeout": 5}
        ]
      }
    ]
  }
}
```

The PreToolUse section handles matchers `Read`, `Edit`, `Write`, `Bash`, `Agent`, and `ScheduleWakeup` — not only Bash.

### Settings Mode (~/.claude/settings.json)

```json
{
  "hooks": {
    "SessionStart": [{
      "matcher": "*",
      "hooks": [
        {"type": "command", "command": "~/.claude/hooks/subconscious.sh start"},
        {"type": "command", "command": "~/.claude/hooks/session-start-hook.sh"}
      ]
    }],
    "UserPromptSubmit": [{
      "matcher": "*",
      "hooks": [
        {"type": "command", "command": "~/.claude/hooks/prompt-hook.sh"}
      ]
    }],
    "Stop": [{
      "matcher": "*",
      "hooks": [
        {"type": "command", "command": "~/.claude/hooks/stop-hook.sh"}
      ]
    }],
    "PreCompact": [{
      "matcher": "*",
      "hooks": [
        {"type": "command", "command": "~/.claude/hooks/pre-compact-hook.sh"}
      ]
    }],
    "PreToolUse": [{
      "matcher": "Read|Edit|Write|Bash|Agent|ScheduleWakeup",
      "hooks": [
        {"type": "command", "command": "~/.claude/hooks/pre-tool-hook.sh", "timeout": 10}
      ]
    }]
  }
}
```

### Hook Configuration Fields

| Field     | Description                                              |
|-----------|----------------------------------------------------------|
| `matcher` | Regex pattern to filter events (`*` = match all)         |
| `type`    | Hook type: `command`                                     |
| `command` | Shell command to execute                                 |
| `timeout` | Timeout in seconds (optional)                            |

---

## Transparent Memory

**Transparent memory** is the prompt-hook path that injects retrieved memories without an explicit agent tool call.

### How It Works

1. **User sends message**: "How should I handle rate limiting?"

2. **Hook receives JSON on stdin**:
   ```json
   {"message": "How should I handle rate limiting?"}
   ```

3. **Hook runs resonance via daemon socket**:
   ```bash
   rpc_call "full_resonate" '{"query":"How should I handle rate limiting?","k":3}'
   ```

4. **Output injected into Claude's context**:
   ```
   In Project X, used exponential backoff for rate limiting...
   Rate limiting gotcha: always return 429, not 500...
   Redis INCR with EXPIRE for distributed rate limiting...
   ```

5. **Claude sees memories as part of the conversation** — no explicit recall needed.

### Filtering and Formatting

- **Relevance threshold**: Only ≥25% relevance results pass
- **Type stripping**: `[72%] [wisdom]` prefix removed — just content
- **Truncation**: Max 500 chars total injection
- **Tag exclusion**: Auto-captured signals (`auto:cmd`, `auto:file`, `auto:edit`) are excluded

---

## Proactive Learning

The hooks detect learning opportunities in user messages and inject hints for Claude to act on.

### Detection Patterns

| Pattern        | Detection                                          | Action                     |
|----------------|----------------------------------------------------|----------------------------|
| **Correction** | "no", "actually", "that's wrong", "not quite"      | Hint: use `learn_correction` |
| **Preference** | "I prefer", "always", "never", "more concise"      | Hint: use `learn_preference` |
| **Frustration**| "stuck", "confused", "frustrated", "tedious"       | Hint: use `learn_approach`   |
| **Milestone**  | "it works", "shipped", "released", "deployed"      | Hint: use `learn_milestone`  |

### Auto-Learning (Stop Hook)

If Claude didn't use a learning tool but the user clearly corrected or expressed a preference, the stop hook auto-stores the learning:

```bash
chitta remember --content "[correction] WRONG: ... CORRECT: ..." \
  --tags "correction,auto-learned" --type wisdom --visibility 2
```

### Anticipation

1. **UserPromptSubmit**: Calls `anticipation_predict` to suggest the likely next action
2. **Stop**: Records what Claude actually did via `anticipation_observe`
3. **Verification**: If prediction matched, calls `anticipation_success` to strengthen the pattern

---

## Subconscious Daemon

The subconscious daemon runs background processing without consuming main context tokens.

Status 2026-09-14: query embedding uses one model with `CHITTA_EMBED_CONTEXTS` contexts (default 4, clamp 1..16) and an exact-query LRU (`CHITTA_EMBED_CACHE`, default 512, 0 disables; health_check reports hits/misses/coalesced); the isolated 768-d eval replica passed 17 CTests and bit identity across contexts, but repeated 12-call recall_lanes median total was 1794.5 ms (target <1500 ms unmet); see measurements (recorded in the 2026-09-14 stream notes; the numbers are the ones quoted here).

### Lifecycle

```
SessionStart
     │
     ▼
subconscious.sh start
     │
     ├─▶ Kill stale daemons
     ├─▶ Acquire atomic lock
     ├─▶ Start chittad daemon
     ├─▶ Wait for socket + heartbeat
     └─▶ Log: "[subconscious] Started (pid=12345, socket=..., heartbeat=ok)"

(Daemon runs independently)
     │
     ├─▶ Every 1 second:   Check for work
     ├─▶ Every 30 seconds: Process embedding queue (batch of 20)
     ├─▶ Every 30 minutes: Hygiene cycle (decay, prune, consolidate)
     ├─▶ Every 60 minutes: Theme maintenance (split, merge, reassign)
     └─▶ Pattern detection: Corrections, preferences, frustration, milestones
```

### Managing the Daemon

```bash
./hooks/subconscious.sh status    # Check status (PID, socket, managed/MCP-spawned)
./hooks/subconscious.sh health    # Health check with auto-recovery
./hooks/subconscious.sh stop      # Graceful stop then force kill
./hooks/subconscious.sh restart   # Stop then start

tail -f ~/.claude/mind/.subconscious.log   # View logs
```

**Socket path:** `/tmp/chitta-{djb2_hash(MIND_PATH)}.sock`
**PID file:** `/tmp/chitta-{djb2_hash(MIND_PATH)}.pid`

---

## Custom Hooks

### Adding a Custom Hook

1. Create your script in `scripts/`:

```bash
#!/bin/bash
input=$(cat)
# Output appears in Claude's context as <system-reminder>
```

2. Make it executable: `chmod +x scripts/my-custom-hook.sh`

3. Add to `hooks/hooks.json` (plugin mode):

```json
{
  "hooks": {
    "UserPromptSubmit": [
      {
        "hooks": [
          {
            "type": "command",
            "command": "${CLAUDE_PLUGIN_ROOT}/scripts/my-custom-hook.sh",
            "timeout": 5
          }
        ]
      }
    ]
  }
}
```

### Hook Input

**UserPromptSubmit:**
```json
{
  "message": "User's message text",
  "context_window": {"remaining_percent": 85}
}
```

**PostToolUse:**
```json
{
  "tool_name": "Bash",
  "tool_input": {"command": "ls -la"},
  "tool_response": "...",
  "cwd": "/path/to/project",
  "session_id": "abc123"
}
```

**SessionStart:**
```json
{"transcript_path": "/path/to/transcript.jsonl"}
```

### Hook Output

- **stdout** — Injected as `<system-reminder>` in Claude's context (JSON `hookSpecificOutput` schema)
- **stderr** — Logged but not shown to Claude
- **Exit code 2** — Blocks the tool call and shows the hook's stdout as an error explanation

### RPC from Custom Scripts

```bash
# Via Unix socket (fast path)
echo '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"recall","arguments":{"query":"test","limit":3}}}' \
  | nc -U /tmp/chitta-HASH.sock

# Via thin client (fallback)
echo '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"recall","arguments":{"query":"test","limit":3}}}' \
  | ~/.claude/bin/chitta
```

---

## Troubleshooting

### Hooks Not Running

```bash
# Plugin mode: check hooks.json
jq . hooks/hooks.json

# Settings mode: check settings.json
jq '.hooks' ~/.claude/settings.json

# Verify scripts are executable
ls -la hooks/*.sh

# Ensure daemon is running
./hooks/subconscious.sh status
```

### Slow Hook Execution

1. Increase timeout in configuration
2. Check daemon responsiveness: `./hooks/subconscious.sh health`
3. Set `CHITTA_MAX_WAIT=10` for longer daemon response timeout

### Resonance Not Working

```bash
# Check daemon socket exists
ls /tmp/chitta-*.sock

# Test daemon responds
echo "stats" | nc -U /tmp/chitta-*.sock

# Test resonance directly
echo '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"full_resonate","arguments":{"query":"test","k":3}}}' \
  | nc -U /tmp/chitta-*.sock
```

### Daemon Not Starting

```bash
pgrep -f "chittad daemon"                            # stale processes?
rm -f /tmp/chitta-*.sock /tmp/chitta-*.pid /tmp/chitta-*.lock   # clean up
ls -la ~/.claude/bin/chittad                          # binary exists?
cat ~/.claude/mind/.subconscious.log                  # daemon logs
~/.claude/bin/chittad daemon --path ~/.claude/mind --foreground  # manual start
```

### Debug Mode

```bash
export DEBUG_SOUL=1
claude
# or test a hook directly:
DEBUG_SOUL=1 ./hooks/prompt-hook.sh prompt "your test query"
```

Debug output (stderr) shows: query, realm, boost k, extra tags, raw full_resonate results with relevance scores, symbol search results, and the final injected context.

### Checking Shadow Log Enforcement Status

```bash
# Review decisions and enforce status
./scripts/hook-stats.sh

# Count entries in shadow log
wc -l ~/.claude/mind/.hook_shadow.jsonl

# Check log age
stat ~/.claude/mind/.hook_shadow.jsonl
```

Enforcement auto-activates when the shadow log reaches ≥100 entries AND is ≥3 days old. Use `CHITTA_HOOK_ENFORCE=1` to force it on early, or `CHITTA_HOOK_ENFORCE=0` to keep it in shadow mode.

2026-09-14 — Read-path scratch A/B (133,673 memories): recall_lanes median total 1/6/12 callers 1044/1233.5/1644.5 → 572/96.5/94.5 ms; hook off median/p95 1113/6031 → 788/1803 ms, on 1210/2094 → 871/4268 ms (15 runs/arm, zero empties); zero recall-worker fdatasync calls; active stacks show no fsync/Turbo convoy (isolated shared waits 1/0/1); on-arm hook p95 regressed.

2026-09-14 — Saddle advisory wired into Bash PreToolUse and Stop. After existing
safety/find checks and task pre-staging, `saddle_detector.py check --session SID
--cmd COMMAND` reads at most the last 1 MiB of the outcome ledger, selecting this
session's failures from the last seven minutes. Three similar failures trigger
one additionalContext notice per saddle ID in `$CHITTA_DB_PATH/.saddle_<sid>`
(default mind: `~/.claude/mind`). A matching explicit success closes the saddle;
unknown Codex exit codes count only when likely_fail is true and otherwise never
close it. Post-Bash now stores a 160-byte stderr/output excerpt for failures.
Both checks use `timeout -s KILL 0.3s python3 -S`, including interpreter startup,
and fail open. Stop prints one summary line and appends a `saddle` event before
transcript/daemon early exits; available persisted summaries include that line.
Very busy ledgers can evict relevant events from the bounded tail; failed or
budget-expired checks emit nothing. The detector does not invoke recall_analogy.

2026-09-14 — Saddle hook isolated timing, final 25 interleaved runs/arm, both
headless aliases unset, benign non-trackable Bash command and no live RPC:
no-ledger baseline median/p95 45.16/65.80 ms; populated no-saddle full PreToolUse
143.81/179.68 ms; added median 98.65 ms, paired added p95 123.79 ms. The 150 ms
added-overhead gate passes, as does the full-hook median gate; full-hook p95 is
179.68 ms and is not a sub-150 ms tail-latency guarantee. Regression coverage is
`hooks/tests/test_saddle_hook.sh` (shape/session/time, Codex unknowns, dedupe,
combined advisories, Stop telemetry, malformed/bounded ledger and timeout).


2026-09-16 — Bash PreToolUse now calls `saddle_check` in `hooks/lib.sh`, using
Bash and one jq process instead of starting Python. It reads a 1 MiB tail (plus
one boundary byte), then `tail -n 4097` limits parsing to 4096 complete records.
The Python offline/Stop check shares the record limit. Session/shape/time gates,
Unicode normalization, SequenceMatcher matching blocks and thresholds, Codex
unknown exits, and nonblocking flock dedupe are preserved. Tail parsing and
comparison use `timeout -s KILL 0.3s jq`; failures/timeouts emit nothing and do
not consume the notice marker. Stop retains its once-per-turn Python summary.
`hooks/tests/test_saddle_parity.sh` compares notice bytes and marker contents on
81 ledgers; `test_saddle_hook.sh` also checks that PreToolUse never starts Python.

Isolated 25 interleaved runs/arm, both headless aliases unset, benign
non-trackable Bash command, temporary HOME/mind, stub CLI and no live RPC:

| Timing (ms) | Python before median / p95 | Bash+jq after median / p95 |
|---|---:|---:|
| No-ledger baseline | 42.83 / 58.06 | 43.92 / 61.94 |
| Populated no-saddle PreToolUse | 139.49 / 164.79 | 67.97 / 85.87 |
| Added overhead | 96.66 / 109.36 | 24.05 / 30.66 |

Added median is the difference of arm medians; added p95 is the p95 of paired
on-minus-off samples. Both targets (<25 ms median, <40 ms p95) pass; the existing
relative regression bounds remain unchanged. Evidence: `/tmp/saddle-before.log`
and `/tmp/saddle-sixth.log` on the measurement node.

## Hook pruning audit (2026-09-16)

The nine scripts below had no callers in `hooks.json`, the install manifest,
other hooks, `scripts/`, `codex-plugin/`, `chitta-mcp/`, or this reference.
The audit also checked repository-wide references and each script's git history.
Historical changelog mentions describe old releases, not current hook wiring.
No event registration or install-manifest entry changes in this cleanup.

| Script | Verdict | Evidence / purpose |
|---|---|---|
| `bridge-holes.sh` | Moved to `scripts/` | Added by `013e909b` as G9 graph bridging; standalone `--dry-run`, `--realm`, `--max` interface, no lifecycle caller. |
| `debug-recall.sh` | Moved to `scripts/` | Added by `9a4efca6` as the G0 recall diagnostic; compares fetched candidates and final recall results for an explicit query. |
| `enrich-code.sh` | Delete | Old temp-file symbol enrichment adapter, last functional change `c820af65`; header claims a daemon caller, but no source reference remains. |
| `evolve-topology.sh` | Moved to `scripts/` | Added by `013e909b`, maintained in `33aacc65`; explicit topology mutation with ledger gate and `--dry-run`, not an event hook. |
| `ledger-autosave.sh` | Delete | Old January/February ledger wrapper sends an obsolete combined `ledger` action; current lifecycle hooks use `ledger_save`/`ledger_load`. |
| `post-edit-hook.sh` | Delete | Historical May CEC/reindex hook; no current event or install entry. Current file-change reindexing is handled by `file-changed-hook.sh`; no new Edit event is enabled. |
| `reparse-transcripts.sh` | Delete | Old whole-history backfill, last maintenance `c9defdbe`; truncates the queue and duplicates current transcript registration/parsing. It also exits at its first zero-valued post-increment under `set -e`. |
| `settle-predictions.sh` | Moved to `scripts/` | Added by `cd8a7d1e` alongside distillation predictions; explicit expired-prediction sweep with realm and dry-run options. |
| `yajna-batch.sh` | Delete | Old batch wrapper for absent `yajna_list`, with UUID-only result parsing and daemon autostart; last changes concern executable mode and historical startup safety. |

The four retained operator tools now live at `scripts/<name>.sh`, with their
executable modes and explicit operator interfaces preserved. They are not
installed or automatically invoked; none sources `hooks/lib.sh` or appears in
the install manifest.

`lib.sh` now owns the identical JSON escaping, transcript-path decoding,
lifecycle realm lookup, and DJB2 hashing implementations. The overwritten first
`daemon_available` definition and the unreachable Python failure notification
inside PostToolUse's success-only branch were removed. Queue serialization was
already centralized; its native enqueue and documented jq bootstrap fallback
remain intact. Different prompt/lifecycle realm policies and the manager's
socket-directory creation policy remain distinct.

Inline legacy aliases are retained where removal would change behavior: the
alias shim intentionally leaves both names untouched when both are supplied,
and an explicitly empty `CHITTA_*` still falls through to a nonempty `CC_SOUL_*`
at those read sites. Entrypoint checks before sourcing the shim also need both
names. Remaining Python calls implement reachable RLM, classification, hint,
provenance, transcript snapshot, Stop saddle, lifecycle compatibility, or
operator/background paths; they are not dead hot-path fallbacks.

Shell inventory (`wc -l hooks/*.sh`, excluding tests): **46 scripts / 9,696 lines
before**, **37 scripts / 8,585 lines after** (530 lines removed, 581 lines moved
to four operator tools in `scripts/`). The supplied
9,744-line estimate differs from the checked-out `31e3a26a` baseline. One focused
helper regression test was added; the existing 19 hook tests are unchanged.

Validation: all 20 hook tests passed after helper extraction. After deletions,
19 passed in the full run; `test_session_start_concurrency.sh` failed its
immediate post-kill process-state assertion, then passed unchanged in isolation
(300 ms calls completed in 1.077 s; 350 ms deadline assertions passed). All
46 SMRITI tests, both full CI Ruff commands, touched-shell `bash -n`, and
` shellcheck -x --severity=warning` passed. MCP ran 136 tests with only the known
`BoundedSessionManager._session_owners` error. Tests used the prescribed bioinfo
CPython and isolated hook state. No native code changed or native build ran.
After the operator relocation, all 20 hook tests and all 46 SMRITI tests passed;
shell syntax and warning-level ShellCheck passed for all four moved tools.
MCP again ran 136 tests with only the same pre-existing `_session_owners` error.

**Contract check: `contracts unchanged`.** The original check failed because
122 stale CLI-help snapshots remained in the baseline. Merging the orchestrator's
`a28ead65` removed those snapshots; the check now passes with
`CHITTA_PY=/maps/projects/fernandezguerra/apps/opt/conda/envs/bioinfo/bin/python3`.
The install-manifest snapshot was regenerated and remains byte-identical because
none of the relocated tools was listed. Hook registrations are unchanged.

## Operator tools moved from hooks/ (2026-09-16)

- `scripts/bridge-holes.sh`: Bridge sparsely connected memory-graph entities with suggested insights; supports `--dry-run`, `--realm`, and `--max`.
- `scripts/debug-recall.sh`: Compare over-fetched candidates with final recall results for a query; supports `--limit` and `--fetch`.
- `scripts/evolve-topology.sh`: Evolve conductor visibility matrices using archive fitness, ledger and stability gates; supports `--dry-run` and `--realm`.
- `scripts/settle-predictions.sh`: Confirm expired open predictions without correction references; supports `--dry-run` and `--realm`.

<!-- BEGIN CITATIONS -->
## References

- <a id="ref-14"></a>**[14]** Subhadeep Pal, Fiona Y. Wang, and Markus J. Buehler. SwarmWorld: Stigmergic technological evolution in societies of language-model agents. arXiv:2608.26081 (2026). [source](<https://arxiv.org/abs/2608.26081>) [source](<https://arxiv.org/html/2608.26081>)
- <a id="ref-24"></a>**[24]** Stephen Robertson and Hugo Zaragoza. The Probabilistic Relevance Framework: BM25 and Beyond. Foundations and Trends in Information Retrieval 3(4), 333–389 (2009). [source](<https://doi.org/10.1561/1500000019>)
- <a id="ref-34"></a>**[34]** Anthropic. How Claude remembers your project. Claude Code documentation (accessed 2026-09-16). [source](<https://code.claude.com/docs/en/memory>)
- <a id="ref-35"></a>**[35]** Anthropic. Prompting Claude Fable 5.1. Claude Platform documentation (accessed 2026-09-16). [source](<https://platform.claude.com/docs/en/build-with-claude/prompt-engineering/prompting-claude-fable-5-1>)
- <a id="ref-36"></a>**[36]** Anthropic. Skill authoring best practices. Claude Platform documentation (accessed 2026-09-16). [source](<https://platform.claude.com/docs/en/agents-and-tools/agent-skills/best-practices>)
- <a id="ref-37"></a>**[37]** Thariq Shihipar. The new rules of context engineering for Claude 5 generation models. Anthropic blog (2026-07-24). [source](<https://www.claude.com/blog/the-new-rules-of-context-engineering-for-claude-5-generation-models>)
- <a id="ref-38"></a>**[38]** OpenAI. Custom instructions with AGENTS.md. Codex documentation, ChatGPT Learn (accessed 2026-09-16). [source](<https://learn.chatgpt.com/docs/agent-configuration/agents-md>)
- <a id="ref-39"></a>**[39]** OpenAI. Codex Security. Codex documentation, ChatGPT Learn (accessed 2026-09-16). [source](<https://developers.openai.com/codex/security/>)
- <a id="ref-41"></a>**[41]** OpenAI. GPT-6 Astra: A new generation of intelligence. OpenAI (2026; exact publication day unverified). [source](<https://openai.com/index/gpt-6-astra/>)
- <a id="ref-42"></a>**[42]** openai/codex issue contributors. AGENTS.md is silently truncated and instructions near the end ignored. GitHub issue #13386 (accessed 2026-09-16). [source](<https://github.com/openai/codex/issues/13386>)
- <a id="ref-43"></a>**[43]** openai/codex issue contributors. Docs: project_doc_max_bytes semantics are undocumented — the 22.5KB root AGENTS.md leaves ~9.7KB before nested files are truncated. GitHub issue #37956 (accessed 2026-09-16). [source](<https://github.com/openai/codex/issues/37956>)
- <a id="ref-44"></a>**[44]** openai/codex issue contributors. Hooks: PostToolUse payload carries no failure signal, and PostToolUseFailure never fires. GitHub issue #34289 (accessed 2026-09-16). [source](<https://github.com/openai/codex/issues/34289>)
- <a id="ref-45"></a>**[45]** openai/codex issue contributors. Docs list --ask-for-approval as global, but codex exec rejects the post-subcommand form. GitHub issue #26602 (accessed 2026-09-16). [source](<https://github.com/openai/codex/issues/26602>)
- <a id="ref-46"></a>**[46]** openai/codex issue contributors. Streamable HTTP MCP bearer_token_env_var reported as unset although present in parent shell. GitHub issue #41378 (accessed 2026-09-16). [source](<https://github.com/openai/codex/issues/41378>)
- <a id="ref-47"></a>**[47]** openai/codex issue contributors. MCP Streamable HTTP: sessions are opened but never terminated (1.8% DELETE ratio), exhausting remote server worker pools. GitHub issue #41600 (accessed 2026-09-16). [source](<https://github.com/openai/codex/issues/41600>)
- <a id="ref-48"></a>**[48]** openai/codex issue contributors. Experimental context management: native notes/history return 404 on Pro + Astra, while new_context can discard task state. GitHub issue #43194 (accessed 2026-09-16). [source](<https://github.com/openai/codex/issues/43194>)
- <a id="ref-49"></a>**[49]** openai/codex issue contributors. Token-budget new windows omit notes content, so the first LLM request has no task state. GitHub issue #43335 (accessed 2026-09-16). [source](<https://github.com/openai/codex/issues/43335>)
- <a id="ref-50"></a>**[50]** openai/codex issue contributors. Codex Desktop compaction requires unavailable notes tool, loses checkpoint, and repeats token-heavy work. GitHub issue #42449 (accessed 2026-09-16). [source](<https://github.com/openai/codex/issues/42449>)
- <a id="ref-51"></a>**[51]** openai/codex issue contributors. Codex tool loop causes context snowballing and multi-million-token input amplification. GitHub issue #44305 (accessed 2026-09-16). [source](<https://github.com/openai/codex/issues/44305>)
- <a id="ref-71"></a>**[71]** Alex L. Zhang, Tim Kraska, and Omar Khattab. Recursive Language Models. arXiv:2512.24601 (2025; revised 2026). [source](<https://arxiv.org/abs/2512.24601>)
<!-- END CITATIONS -->
