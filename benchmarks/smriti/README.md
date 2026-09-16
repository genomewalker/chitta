# SMRITI-Bench

*Status as of 2026-09-02 (v2 matrix run, `results/a7e42269.jsonl`): 15 tasks × {off, on, ablate:semantic, ablate:hybrid, ablate:keyword} × 3 trials = 225 runs. off 33/45 (73%), on 41/45 (91%): ΔSR +17.8 pp; ΔT −1,675 tokens/task; MUI v2 0.53 (sr_lift 0.16, cost 0.38); injection confirmed 44/44. All lift from non-deducible conventions (005, 009–011, 013, 014). Ablating the hybrid lane drops injection confirmation to 23/45 — it is the delivery channel for planted memories; semantic and keyword ablation barely move outcomes. Anomaly: example-012 fails 0/3 in every condition including memory-on with injection confirmed (memory delivered, not acted on) — task under review. n=45/condition: still indicative.*

*Status as of 2026-09-02: v2 -- MUI replaced with a ledger-grounded metric
(`scorer.mui_credit`), 6 more non-deducible-convention tasks
(`example-010`..`example-015`, corpus now 15), ablation wired for real, and
`--resume`. **MUI v2**: for each memory-on run, credit requires
`injected_confirmed` AND the run passed AND (a paired memory-off trial for
the same task FAILED -- `sr_lift_credit` -- OR used ≥1.5× the tokens --
`cost_credit`); `mui` = fraction of on-runs credited, `sr_lift_credit` +
`cost_credit` sum to it. The old surface-echo check survives as
`echo_rate`, kept for reference. Recomputed on the existing full matrix
(`results/a1aa8c0f.jsonl`, unchanged data, just rescored): `mui=0.630`
(17/27), `sr_lift_credit=0.148` (4/27 -- exactly the 4 off-failures, so it
reproduces ΔSR's own signal from the ledger rather than an echo),
`cost_credit=0.481` (13/27 -- on-runs where memory was confirmed injected,
passed, off *also* passed, but off cost ≥1.5× more), `echo_rate=0.0`
still, confirming the old proxy was measuring nothing. **New tasks**:
`example-010` mandated file header, `011` mandated handler-function
prefix, `012` mandated credential env-var name, `013` mandated log-line
format, `014` mandated CLI-subcommand order, `015` mandated retry
count/backoff constant -- each a convention with no way to deduce the
"right" answer from the fixture alone, the same shape as `005`/`009` in
the first matrix, the only two tasks that showed a real `off`-failure
gap. **Ablation**: real now, not skipped -- `ablate:<lane>` sets
`CHITTA_ABLATE_LANES=<short-lane-name>` on the agent subprocess
(`runner.ABLATE_LANE_ALIASES` maps semantic/keyword/graph/hybrid/context/
corrections to chitta's own sem/kw/hyb/ctx/corr hook-lane names; short
names also accepted directly, plus `xr`), consumed by
`hooks/prompt-core.sh` (landed by a parallel agent this same session --
see "Ablation matrix" below for the mechanism and `ablate:all`'s sanity
check). **`--resume <results.jsonl>`**: an interrupted `run_all` continues
from what's recorded, skipping any task×condition×trial already in the
file and appending the rest to it (same `run_id`). `--trials` is still
required, still no default. Not yet done: a real (non-dry-run,
non-`echo`) matrix over the ablation conditions or the 6 new tasks -- see
"Running" below; this status entry is scaffold + a re-score of existing
data, not a new empirical result.*

*Status as of 2026-09-02 (earlier same day): first full matrix run
(`results/a1aa8c0f.jsonl`):
9 tasks × off/on × 3 trials, injection confirmed 27/27. Memory-off 23/27 pass,
memory-on 27/27 (ΔSR +14.8 pp); paired token ratio on/off median 0.52
(ΔT −2,137 tokens/task). The off-failures were exactly the non-deducible
conventions (009 exit-code contract 0/3→3/3, 005 CLI default 2/3→3/3). The
MUI echo-proxy reads 0.0 and is broken (planted text is never literally
echoed) -- to be replaced by ledger-based injected∩outcome credit. n=27 per
condition: indicative, not conclusive; ablation still unwired.*

*Status as of 2026-09-01: v1.1 -- real `ChittaAdapter` (live daemon) and
`ClaudeCodeAdapter` (`claude -p --output-format json`), 9 hand-authored tasks
(`example-001`..`example-009`, each with a `solution/` and, as of v1.1, a
`hidden/`), `--trials` required on the CLI. A live pilot on v1 caught a real
leakage bug -- both `off` and `on` passed because the agent read the
expected answer straight out of a visible test file -- fixed in v1.1 by
moving every convention-revealing test into `hidden/`, applied only after
the agent's turn ends (see "Threats to validity" > "Fixture leakage via a
visible test"). v1.1 also records and reports whether a planted memory was
actually confirmed injected into the agent's session (`injected_confirmed`,
"Threats to validity" > "Unconfirmed injection"). Ablation is still not
wired daemon-side -- `ablate:<lane>` conditions are skipped, not faked (see
"Ablation matrix" below).*

An outcome-grounded memory benchmark for coding agents. Named for *smṛti*
(स्मृति), Sanskrit for memory/recollection.

## Motivation: the proxy-target gap

Existing agent-memory benchmarks — [LoCoMo](../locomo/README.md) [17](#ref-17),
LongMemEval [16](#ref-16) — score conversational QA recall: given a long dialogue history,
can the system answer a question about it, measured by F1 against a reference
answer. That is the wrong target for a coding agent's memory system. A coding
agent's memory exists to make the agent *better at doing the next task*, not
better at reciting the previous one. QA-recall F1 is a proxy that can rise
while the thing that actually matters — does the agent ship a working fix
faster and cheaper because it remembered something — stays flat or gets
worse (e.g. a memory system that retrieves plausible-sounding but stale
context can raise "recall quality" scores while actively misleading the
agent on a real task).

SMRITI-Bench closes that gap by scoring the same axis chitta is built for:
does injected memory change whether an objective, automated check passes,
and at what token cost. No LLM-judge, no reference-answer F1. A task either
passes its `check_cmd` or it doesn't.

## What it measures

For each task, the agent is run under multiple **memory conditions**
(`off`, `on`, `ablate:<lane>` — see Ablation matrix below). Each run
produces one pass/fail outcome (from `check_cmd`) and one token count.
Aggregated across conditions:

- **SR** (success rate) per condition, with a Wilson [2](#ref-2) 95% interval (small-n
  task counts make a naive proportion misleading).
- **ΔSR** = SR(`on`) − SR(`off`) — memory's effect on task success. The
  headline number.
- **ΔT** = mean tokens(`on`) − mean tokens(`off`) — memory's effect on cost.
  A memory system that adds ΔSR but doubles ΔT is not free.
- **MUI** (memory utility index) = among `on`-condition passes, the fraction
  where a planted memory is demonstrably used. This is a proxy, not a
  verified causal link — see the MUI subsection below and `scorer.py`'s
  `memory_was_used`.
- **Per-lane ablation ΔSR** = SR(`on`) − SR(`ablate:<lane>`) for each
  retrieval lane, isolating that lane's marginal contribution.

## Task format

`schema/task.json` is the JSON Schema. A task is:

```json
{
  "id": "example-001",
  "repo_fixture": "repo_fixture",
  "prompt": "...instruction handed to the agent...",
  "check_cmd": "python3 -m unittest test_mymodule -v",
  "planted_memories": [
    {"content": "...", "realm": "...", "type": "wisdom"}
  ],
  "tags": ["python", "convention"],
  "expected_tokens_budget": 4000
}
```

`repo_fixture` is a path (copied into the run workdir) or a `git:<url>#<sha>`
ref (cloned and checked out). `check_cmd` runs from that workdir after the
agent finishes and after `hidden/` is applied (see below); exit 0 is pass.
`check_cmd` must run with the stdlib only -- this environment has no
`pytest` installed on any of its pythons (verified: conda python3,
`/usr/bin/python3`, `python3.11`, `python3.12` all lack it), so every task
in the corpus uses `python3 -m unittest`, not pytest.

Each task also has two directories, neither declared in `schema/task.json`
(they're fixed-name siblings of `repo_fixture`, the same way `repo_fixture`
itself is just a conventional string, not a schema-typed structure):

- **`solution/`**: the fixed version of whatever file(s) the fixture's stub
  left broken. Copying `solution/` over a materialized fixture
  (`shutil.copytree(..., dirs_exist_ok=True)`) must make `check_cmd` pass.
- **`hidden/`**: every test that would reveal the planted convention's exact
  answer (a literal expected string, an exit code, anything an agent could
  read straight off instead of reasoning about or remembering). `hidden/`
  is copied into the workdir by `runner.apply_hidden_checks` **after the
  agent's turn ends**, immediately before `check_cmd` runs -- it is never
  part of what the agent sees. `repo_fixture/` itself carries no test, or
  only a convention-agnostic one; never one that states the answer. See
  "Threats to validity" > "Fixture leakage via a visible test" for why this
  exists.

`tests/test_scaffold.py`'s `TestTaskCorpus` enforces, for every task: schema
validity, that both directories exist, that no visible `repo_fixture` file
or the prompt contains the convention's answer-revealing terms
(`LEAK_CHECK_TERMS`), that no visible test defines a test method at all,
and the solution/hidden mirror invariant (`check_cmd`, with `hidden/`
applied, FAILS on the *untouched* fixture and PASSES once `solution/` is
copied over it -- a task whose check passes with no work at all is broken).

**Solvability invariant**: a task must be *attemptable* with `memory=off`
by an agent that reads the fixture and reasons carefully -- there must be a
reasonable, complete generic solution the agent can actually write, and
exploring the repo (not guessing at a hidden test) must be how a careful
agent would discover the convention if it discovers it at all. This is
**not** the same as guaranteeing `memory=off` passes: for a genuinely
arbitrary institutional convention (underscores vs hyphens, exit code 3 vs
1) there is no way to *deduce* the "right" answer from first principles --
only from exploring the repo carefully enough, or from memory. `off`'s
success rate is expected to be positive but below 100% for a well-designed
convention task; `on`'s success rate approaching 100% is what a working
memory system should look like. A task that a generic implementation
literally cannot compile/run at all, or that requires information nowhere
in the repo and not inferable from its domain, is testing information
injection, not memory utility, and does not belong in this benchmark.
`planted_memories` encodes non-obvious project convention or history -- the
kind of thing a human teammate would say "oh yeah, we decided that last
week" about -- that a memory system could plausibly have retained from a
prior session on this repo.

## Protocol

For each task × condition × trial:

1. Materialize `repo_fixture` into a fresh scratch workdir (`runner.py:materialize_fixture`).
2. Plant `planted_memories` via a `MemoryAdapter` (no-op under `off`),
   capturing the ids `ChittaAdapter.plant` returns.
3. Invoke the agent via an `AgentAdapter`. Memory is **not** hand-injected
   into the prompt — see "How memory reaches the agent" below.
4. Run `check_cmd` against the workdir; record pass/fail.
5. Append one record to `results/<run_id>.jsonl`: `run_id, task_id,
   condition, trial, passed, tokens_used, input_tokens, output_tokens,
   cache_read_tokens, transcript, recalled_context_preview,
   planted_memory_contents, timestamp, dry_run`.
6. Tear down the workdir and forget every id `plant` returned
   (`ChittaAdapter.teardown` — one `chitta forget --id` per planted memory;
   there is no bulk forget-by-realm RPC, only forget-by-id and
   forget-by-pattern, and per-id is the precise, order-independent choice).

`scorer.py` reads a `results/<run_id>.jsonl` and prints the metrics above.

### How memory reaches the agent

`ChittaAdapter` does not hand `ClaudeCodeAdapter` a hand-built memory string
to prepend to the prompt. For real runs, memory reaches the agent the way it
would in actual use: chitta's own hooks (`hooks/prompt-core.sh`'s
`UserPromptSubmit` handler, plus `session-start-hook.sh` /
`resume-inject-hook.sh`) recall from the live daemon when `claude -p` runs.
Two adapter-level env vars steer that (`MemoryAdapter.agent_env`, set on the
`claude -p` subprocess):

- **`on` / `ablate:<lane>`** (`ChittaAdapter.agent_env`): sets
  `CHITTA_REALM=<realm_prefix>`. `detect_realm()`
  (`chitta/src/rpc_server.cpp:1416`) checks `CHITTA_REALM` before the
  `.cc-soul-realm`-file and git-repo-name fallbacks, so this pins the
  agent's hooks to exactly this trial's planted realm without touching the
  materialized fixture's files.
- **`off`** (`NullAdapter.agent_env`): sets `CHITTA_HEADLESS=1`. This is
  **not** a benchmark-specific workaround — it's chitta's existing global
  kill switch, used today to keep multi-agent "room" participants quiet.
  Every memory-injecting hook no-ops immediately when it's set (verified:
  `grep CHITTA_HEADLESS hooks/*.sh` hits `prompt-core.sh`,
  `session-start-hook.sh`, `resume-inject-hook.sh`, `post-bash-hook.sh`,
  `pre-tool-hook.sh` — every hook that could inject memory). We considered
  and rejected the alternative the task brief floated (an empty/nonce realm
  for `off`): live testing showed `chitta recall --realm <realm-with-no-hits>`
  falls back to unfiltered cross-realm results rather than returning nothing
  (the daemon's own cross-realm-fallback behavior, mirrored at the hook
  level in `prompt-core.sh:410-411`) — an empty realm does not reliably mean
  zero injection. `CHITTA_HEADLESS=1` does.

`ChittaAdapter.context_for` still makes a real `chitta recall --realm
<realm_prefix>` call and records what it returns as
`recalled_context_preview` in the result record, but only as a diagnostic —
it is never prepended to the agent's prompt. Prepending it there *as well
as* letting the hooks inject it would double-inject and would substitute a
hand-rolled proxy for the real recall-routing path that ablation is
supposed to isolate. Because of the same cross-realm-fallback behavior
noted above, treat `recalled_context_preview` as "what recall returned",
not "what this task's planted memories were" — on a realm with few or no
scoped hits it can legitimately include unrelated content pulled in from
elsewhere.

## Ablation matrix

Conditions are a flat string (`off`, `on`, `ablate:<lane>`, `ablate:all`)
parsed by `runner.parse_condition`, resolving to a `MemoryAdapter`.

**Wired via env, not a daemon flag.** chittad still has no `--ablate-lane`
RPC flag, but it doesn't need one: `ChittaAdapter.agent_env` sets
`CHITTA_ABLATE_LANES=<comma-list>` on the `claude -p` subprocess, and
`hooks/prompt-core.sh`'s recall call skips any lane named there before
issuing it — the ablation happens in the hook that would have made the
recall call, not in chittad itself. `runner.ABLATE_LANE_ALIASES` maps a
benchmark-facing lane name to chitta's own short hook-lane name; the short
name also works directly. `ablate:all` sets every lane at once
(`CHITTA_ABLATE_LANES=sem,ctx,hyb,kw,corr,xr`) and should behave like
`off` in effect — `scorer.py` reports the gap between them as
`ablate_all_vs_off_sr_gap`, a sanity check on the wiring itself rather
than a memory-effect number: a gap far from 0 there means the env var
isn't reaching every injection path, not that ablation "found" something.
An unrecognized lane name raises `ValueError` immediately from
`parse_condition`, before any subprocess runs.

| Condition | Benchmark lane name(s) | `CHITTA_ABLATE_LANES` value | What's disabled | Isolates |
|---|---|---|---|---|
| `off` | — | (unset; `CHITTA_HEADLESS=1` instead) | all memory | baseline — can the agent solve it cold |
| `on` | — | (unset) | nothing | full system upper bound |
| `ablate:semantic` | `semantic` / `sem` | `sem` | embedding/ANN recall | value of fuzzy semantic match |
| `ablate:keyword` | `keyword` / `kw` | `kw` | BM25 [24](#ref-24) lane | value of exact term overlap |
| `ablate:graph` / `ablate:hybrid` | `graph`, `hybrid` / `hyb` | `hyb` | hybrid/triplet recall path | value of multi-hop + fused retrieval |
| `ablate:context` | `context` / `ctx` | `ctx` | recency-weighted thread-context recall | value of conversational continuity |
| `ablate:corrections` | `corrections` / `corr` | `corr` | `[correction]`-kind deterministic lookup | value of the durable-correction fast path |
| `ablate:xr` | `xr` | `xr` | cross-realm recall | value of cross-project retrieval |
| `ablate:all` | — | `sem,ctx,hyb,kw,corr,xr` | every lane | sanity check: should ≈ `off` |

`graph` and `hybrid` are both aliases for chitta's single `hyb` hook lane
— there is no separate triplet/PageRank [70](#ref-70) lane distinct from the hybrid
path (this differs from the original design sketch, which assumed one).
Each single-lane ablation keeps every other lane on, so its ΔSR is a
marginal contribution, not that lane's total value (lanes overlap — see
Threats to Validity).

## MUI: what "used" means

`scorer.mui_credit` is the headline metric as of v2, replacing the v1
surface-echo proxy. For each memory-on run, credit = 1 iff all of:

1. `injected_confirmed` — the planted memory actually reached the child
   session, not just was planted (README "Unconfirmed injection" below).
2. the run passed.
3. at least one paired memory-off trial for the same task (matched by
   trial number when the exact trial exists in the results, else every
   off trial recorded for that `task_id` — see `scorer.score`) either
   **failed** (`sr_lift_credit`: memory changed the outcome) or used
   **≥1.5× this run's tokens** (`cost_credit`: memory changed the cost,
   even on a task exploration alone can already solve — see the first
   pilot's finding that memory's first measurable effect was cost, not
   success, on exploration-solvable tasks).

`mui` = fraction of on-runs credited (`sr_lift_credit` + `cost_credit`,
which are mutually exclusive as computed — cost is only checked when no
paired off trial failed — so they sum exactly to `mui`); `mui_n` is the
denominator (total on-runs, not just on-wins). This directly measures
what the benchmark cares about — did memory demonstrably change the
outcome or its cost — rather than whether the agent quoted the memory's
wording, so it doesn't share the old proxy's failure modes (a quoted-but-
irrelevant memory scoring as "used"; an internalized-but-unquoted one
scoring as "unused").

`scorer.memory_was_used` (the v1 proxy: does the first 40 characters of a
planted memory's content appear, case-insensitively, in the transcript)
is kept and reported as `echo_rate`, for reference only. It reads 0.0
across every on-condition win in `results/a1aa8c0f.jsonl` — real agents
apply a planted convention without repeating its exact wording, so the
echo check was never going to register anything on real data.

A more faithful MUI still would ablate the *specific* planted memory (not
a whole lane) per task and diff outcomes — expensive (task_count ×
memory_count extra runs) and still a v2+ follow-up (open design question
below); `mui_credit`'s lane-level pairing is the practical middle ground
for now.

## Threats to validity

- **Memory leakage between runs.** If a run's planted memories or agent
  actions aren't fully torn down before the next condition on the same
  task, a later `off` run can accidentally benefit from an earlier `on`
  run's residue. Mitigation: realm-scoped planting keyed by `run_id` +
  `task_id` + `condition` + `trial`
  (`project:smriti-<task_id>-<run_id>-<condition>-t<trial>`), unique per
  trial and never reused, and `ChittaAdapter.teardown` forgets every
  planted id after each trial by exact id (verified live: planting and
  forgetting a probe memory, then confirming a `--realm`-scoped and a
  `--tag smriti-bench` query both return nothing afterward — no leaked
  memories from either this corpus's tests or the ad-hoc probes used to
  verify the CLI's actual JSON shapes). Also verified live: `--realm`
  filtering *is* a hard, exact-match filter when the realm has scoped hits
  (probe memory in realm A was invisible to a query scoped to realm B) —
  see "How memory reaches the agent" above for the one case where it isn't
  (realm has zero hits).
- **Non-canonical realm names — the second failure a live pilot caught.**
  v1's realm names had no colon (`smriti-<task>-<run>-<cond>-t<n>`). A pilot
  run showed memory=on delivered **zero** planted memories: the child
  session's `hooks/prompt-core.sh` `realm_detect_once` greps the *output* of
  `chitta realm_detect` for a `word:token` shape
  (`grep -oE '[a-z][a-z0-9_]*:[A-Za-z0-9_./-]+'`) before trusting it, found
  no match against the colon-free realm string, and fell back to `brahman`
  — the outcome ledger's `injected` ids for that run were 3 unrelated
  memories that happened to rank well in the unscoped fallback, not the
  planted ones. Two independent fixes: (1) `hooks/prompt-core.sh` now takes
  an explicit `CHITTA_REALM` env verbatim, ahead of the `chitta
  realm_detect` grep-shape path entirely (patched by the project owner,
  live). (2) The benchmark's realm names are now canonical `project:*`
  shape (`project:smriti-<task>-<run>-<cond>-t<n>`) throughout --
  `remember --realm`, `recall --realm`, and the `CHITTA_REALM` env — the
  same shape `detect_realm()` (`chitta/src/rpc_server.cpp:1416`) itself
  ever produces (`project:<repo>` or `brahman`), so nothing downstream that
  assumes that shape (any hook not yet audited the way `prompt-core.sh` was,
  ranking/boosting, per-realm indexing) can silently misroute a benchmark
  realm the way the ungrep'd colon-free form did. This is exactly why
  `injected_confirmed` (below) is essential rather than a nice-to-have: SR
  numbers from the earlier non-canonical runs were measuring the fallback
  behavior, not memory, and looked no different for it — only the ledger
  join catches that.
- **Prompt-ordering effects.** Running all `off` conditions before all `on`
  conditions (or vice versa) confounds condition with any position-dependent
  drift in the agent or environment. Mitigation: `runner.run_all` interleaves
  conditions within each task (task loop outer, condition loop inner);
  a v1 with real agents should additionally randomize condition order across
  repeated trials.
- **Non-determinism.** LLM agents are not deterministic even at temperature
  0 in practice, and `check_cmd` results can flake (network, timing). A
  single run's pass/fail is a noisy sample of the agent's real success
  probability on that task. Mitigation: Wilson intervals in `scorer.py`
  make the noise visible instead of hiding it behind a point estimate;
  a v1 protocol should run each task × condition ≥5 times before trusting
  a per-task ΔSR.
- **Lane overlap in ablation.** chitta's recall lanes are not independent
  (e.g. semantic and keyword both surface the same high-signal memory via
  different paths), so `ablate:<lane>` ΔSR values are not additive and
  should not be summed to "explain" the `on`-vs-`off` gap.
- **Fixture leakage via a visible test — the failure that actually
  happened.** A 2026-09-01 live pilot (real `claude -p`, example-001,
  `off` vs `on`, 1 trial each) ran clean end-to-end but both conditions
  PASSED, making ΔSR unmeasurable. The transcript showed why: the agent's
  own reasoning said it matched "the convention shown in the tests, where
  the expected output is hello_world" -- it read the exact expected string
  straight out of `repo_fixture/test_mymodule.py`, which was materialized
  into its workdir and fully visible during its turn. Memory couldn't show
  a lift because the fixture itself already gave the literal answer away;
  this generalizes beyond example-001 -- example-005's old
  `test_report.py` asserted the literal CSV output, example-006's asserted
  the literal tab-separated string, example-009's asserted the literal exit
  code 3, and example-007's `zephyr_tool.py` had an explanatory code
  comment ("Known bug: above 4 workers, zephyr_tool silently drops...")
  stating the exact gotcha outright. **Fix**: every convention-revealing
  test moved into `tasks/<id>/hidden/`, applied by
  `runner.apply_hidden_checks` only after the agent's turn ends, never
  materialized into the agent's workdir (see "Task format"); the
  explanatory comment in `zephyr_tool.py` removed (the mechanism stays in
  the code, which a careful agent reading the source could still find --
  what's gone is the narration stating it as a documented fact); a decoy
  stale root-level `example-004/repo_fixture/app.ini` (`greeting = hello`)
  added alongside the real `config/app.ini` (`greeting = hola`) so a naive
  "check the obvious location first" implementation concretely fails
  instead of accidentally succeeding by having only one config file to
  find. `tests/test_scaffold.py`'s `TestTaskCorpus` now enforces this
  automatically for every task: `test_every_task_has_a_hidden_dir`,
  `test_no_visible_test_file_defines_a_test`, and
  `test_no_visible_fixture_file_states_the_convention` (greps every visible
  `repo_fixture` file and the prompt for each task's `LEAK_CHECK_TERMS`).
  More generally: any `check_cmd` or fixture file that accidentally states
  the planted convention (a docstring, a comment, a decoy that isn't
  actually wrong) breaks the solvability invariant the same way. Every new
  task must be checked for this before being counted -- `LEAK_CHECK_TERMS`
  is the automated part of that check, not a substitute for reading the
  task with this failure mode in mind.
- **Unconfirmed injection.** A `passed=True` record under `on` only counts
  as evidence memory helped if the planted memory actually reached the
  child `claude -p` session. `runner.run_one` records
  `injected_confirmed`: `True`/`False` when something was planted (`on`,
  and eventually `ablate:<lane>`), by checking whether an `outcome_ledger`
  `injected` event during the agent's exact wall-clock call window
  (`hooks/prompt-core.sh`'s tap into `$CHITTA_DB_PATH/outcome_ledger.jsonl`,
  default `~/.claude/mind/outcome_ledger.jsonl`) names one of the ids
  `ChittaAdapter.plant` returned; `None` when nothing was planted (`off`).
  There's no reliable child-session id to join on (the runner doesn't know
  the `claude -p` subprocess's own session id in advance), so the join is
  purely by time window -- a race is possible in principle (another
  concurrent process's injection landing in the same window) but is not a
  practical risk for a benchmark that runs one agent call at a time.
  `scorer.py` reports `injection_confirmation_rate` per condition (and
  `on_injection_confirmation_rate` at headline level) but does **not**
  filter `delta_sr` by it -- a low confirmation rate is a reason to
  distrust `delta_sr`, surfaced for a human to see, not silently corrected
  for.

## Files

| Path | Purpose |
|---|---|
| `schema/task.json` | JSON Schema for a task |
| `runner.py` | orchestrates task × condition × trial runs, stdlib only |
| `scorer.py` | reads `results/*.jsonl`, prints metrics, stdlib only |
| `tasks/example-001/` .. `example-015/` | 15 tasks, each `repo_fixture/` (broken stub, no convention-revealing test) + `task.json` + `hidden/` (the convention-revealing test, applied only at check time) + `solution/` (the fix) |
| `tests/test_scaffold.py` | schema validation, `hidden/`+`solution/` presence, no-visible-leak grep (`LEAK_CHECK_TERMS`), no-visible-test-defines-a-test, `check_cmd` fail-before/pass-after-`solution/` (with `hidden/` applied) for every task, `EchoAdapter`+real-`ChittaAdapter` end-to-end plumbing (incl. `injected_confirmed`), ablation-skip check, `--dry-run` smoke check, `--trials`-required check |
| `results/` | `runner.py` output, one JSONL per invocation |

Reused from `../locomo/`: the `chitta <tool> --json --key value` CLI
invocation convention (`ChittaAdapter` mirrors `locomo/evaluate.py`'s
`call_chitta`).

## Running

```bash
cd benchmarks/smriti

# Fake agent, real local daemon plant/recall/forget, no cost -- smoke-tests the harness.
python3 runner.py --task example-001 --agent echo --trials 3

# Real agent. --trials has no default -- README "Threats to validity"
# recommends >=5 before trusting a per-task delta_sr; a single trial is a
# coin flip, not a result. Costs real LLM calls.
python3 runner.py --agent claude-code --trials 5 --timeout 600
python3 scorer.py results/<run_id>.jsonl

# Include ablation conditions (now real, scored runs -- see "Ablation matrix").
python3 runner.py --agent claude-code --trials 5 \
    --condition off --condition on \
    --condition ablate:semantic --condition ablate:keyword \
    --condition ablate:graph --condition ablate:context \
    --condition ablate:corrections --condition ablate:all

# An interrupted run (killed, timed out, node died) can be continued without
# redoing already-recorded task x condition x trials -- appends to the same
# file, same run_id.
python3 runner.py --agent claude-code --trials 5 --resume results/<run_id>.jsonl

# Print the exact chitta/claude commands every task x condition would run,
# without executing any of them (no daemon calls, no LLM calls, no results file).
python3 runner.py --agent claude-code --trials 1 --dry-run

python3 -m unittest tests/test_scaffold.py -v
```

## Open design questions

1. **Ablation env vars — resolved for v2: `CHITTA_ABLATE_LANES`.**
   `ChittaAdapter.agent_env` sets it directly on the agent subprocess;
   `hooks/prompt-core.sh` reads it and skips any named lane's recall call.
   No daemon-side `--ablate-lane` flag was needed after all — the ablation
   happens at the hook that would have issued the recall, not inside
   chittad (see "Ablation matrix"). Still open: a real (non-`echo`,
   non-`dry-run`) matrix with ablation conditions included hasn't been
   run yet — only the wiring itself has been exercised
   (`tests.test_scaffold.TestAblationWiring`, a `--dry-run` smoke check).
2. **Per-memory MUI vs per-lane MUI — still per-lane for v2.**
   `scorer.py`'s `ablation_delta_sr` is per-lane (`on` SR minus
   `ablate:<lane>` SR for each lane present in the results); `mui_credit`
   (the new headline MUI) is also lane-agnostic — it credits a run based
   on paired off-trial outcome/cost, not on which lane surfaced the
   memory. Per-memory MUI (ablate one specific planted memory per task,
   not a whole lane) would be more faithful but costs task_count ×
   memory_count extra runs — still deferred, no longer blocked on #1 now
   that ablation itself is wired.
3. **Task corpus size and sourcing — updated for v2: 15 hand-authored
   tasks.** `example-001`..`example-009` (v1: naming, forbidden dependency,
   error-handling pattern, config location, CLI flag default, output
   format, a fake tool's documented gotcha, an algorithmic-complexity
   constraint, an exit-code contract) plus `example-010`..`example-015`
   (v2, all non-deducible institutional conventions: a mandated
   generated-file header, a mandated handler-function name prefix, a
   mandated credential env-var name, a mandated log-line format, a
   mandated CLI-subcommand registration order, a mandated retry
   count/backoff constant), all Python + stdlib `unittest`. In the first
   full matrix, only the two arbitrary-convention tasks that existed
   then (`005`, `009`) showed a real `off`-failure gap — the six new
   tasks are built the same way, to give ΔSR more surface to move on.
   Mining tasks from real "convention changed" commit pairs remains a
   future extension — harder to guarantee the solvable-without-memory
   invariant against real history than to hand-author it.
4. **`ClaudeCodeAdapter` usage/tokens — resolved.** `tokens_used` =
   `input_tokens + output_tokens`, deliberately excluding
   `cache_read_input_tokens` (verified live against the real `claude -p
   --output-format json` envelope: top-level `usage` object has
   `input_tokens`, `output_tokens`, `cache_read_input_tokens`, and separately
   `cache_creation_input_tokens`). `results/*.jsonl` records all three of
   input/output/cache_read separately (`scorer.py` reports per-condition
   means for each) so `delta_tokens` can be recomputed a different way later
   without re-running anything.
5. **Repeated-trial default — resolved: no default, required.**
   `runner.py --trials` is `required=True` on the CLI; omitting it is a hard
   argparse error, not a silent fall-through to 1. `run_all(...)` (the
   Python API) also takes `trials` as a required keyword arg with no
   default, and raises `ValueError` on `trials < 1` — a single trial is a
   coin flip, not a result.

<!-- BEGIN CITATIONS -->
## References

- <a id="ref-2"></a>**[2]** Edwin B. Wilson. Probable Inference, the Law of Succession, and Statistical Inference. Journal of the American Statistical Association 22(158), 209–212 (1927). [source](<https://doi.org/10.1080/01621459.1927.10502953>)
- <a id="ref-16"></a>**[16]** Di Wu, Hongwei Wang, Wenhao Yu, Yuwei Zhang, Kai-Wei Chang, and Dong Yu. LongMemEval: Benchmarking Chat Assistants on Long-Term Interactive Memory. ICLR (2025); arXiv:2410.10813 (2024). [source](<https://arxiv.org/abs/2410.10813>)
- <a id="ref-17"></a>**[17]** Adyasha Maharana, Dong-Ho Lee, Sergey Tulyakov, Mohit Bansal, Francesco Barbieri, and Yuwei Fang. Evaluating Very Long-Term Conversational Memory of LLM Agents. ACL (2024); arXiv:2402.17753. [source](<https://arxiv.org/abs/2402.17753>) [source](<https://aclanthology.org/2024.acl-long.747/>)
- <a id="ref-24"></a>**[24]** Stephen Robertson and Hugo Zaragoza. The Probabilistic Relevance Framework: BM25 and Beyond. Foundations and Trends in Information Retrieval 3(4), 333–389 (2009). [source](<https://doi.org/10.1561/1500000019>)
- <a id="ref-70"></a>**[70]** Sergey Brin and Lawrence Page. The Anatomy of a Large-Scale Hypertextual Web Search Engine. Computer Networks 30, 107–117 (1998). [source](<https://research.google/pubs/the-anatomy-of-a-large-scale-hypertextual-web-search-engine/>)
<!-- END CITATIONS -->
