# Autonomous evolution — 2026-09-14

Status as of 2026-09-16: history-aware selection, bounded candidate survey, mandatory
committed SELF_CHECK verification and opt-in `--candidates N` fan-out are implemented;
deployment remains human-owned. The automatic-learning cohort cut was recorded at
2026-09-15 23:15 CEST; the prospective 20-task panel remains pending (see [EVALS.md](EVALS.md)).

The loop is **propose → rank → survey in an isolated worktree → choose →
preregister a bet → implement → SELF_CHECK → gates → frozen replica measurements → verdict memory → optional branch/PR →
human merge**. Paired baseline and candidate measurements happen after gates;
only the preregistered prediction is included in the implementer's spec. The package is stdlib-only, with postponed
annotations, and imports on PyPy 3.9 and CPython 3.12.

Run a preview from this checkout:

```bash
EVOLVE_PYTHON=/maps/projects/fernandezguerra/apps/opt/conda/envs/bioinfo/bin/python3 \
  scripts/evolve-cycle.sh --dry-run
```

A preview gathers real sources, reads experiment history, runs an available
immutability check, prints the ranked backlog and survey prompt, and writes
`survey-prompt.md` and `survey-candidates.json` under `.evolve/cycles/<cycle-id>/`.
It stops before generating an implementation spec. It does not
register memories, create a worktree, invoke an implementer, evaluate, or push.
`--backlog /path/cards.json` supplies a JSON array instead of source gathering;
stored proposals are still merged. `CHITTA_BIN` can select a CLI stub for tests.

For an actual cycle:

```bash
scripts/evolve-cycle.sh --max-minutes 120 --implementer codex
scripts/evolve-cycle.sh --implementer claude --real-eval
# Explicit publishing authorization; only an accepted verdict can publish:
scripts/evolve-cycle.sh --real-eval --open-pr
```

Codex defaults to `-m gpt-6-astra -c model_reasoning_effort=high`, with
`exec -C <worktree> --approve-for-me --skip-git-repo-check`. `--model` overrides
the model. Claude uses `claude -p --output-format json` with the worktree as its working
directory; the runner extracts the final `result` string. Codex final messages
come only from its `-o` file, never from terminal logs.
Both receive the written spec as one argument, without shell interpolation.
A cycle creates `evolve/auto-<UTC-date-time>-survey` from a pinned `main`
commit under `.evolve/worktrees/`. Branches, specs, logs and verdict artifacts
remain for review; the runner does not delete implementation worktrees.

The cycle resolves the Codex CLI as `CHITTA_CODEX_BIN`, else `~/.local/bin/codex`,
else PATH: the bioinfo conda env carries an npm `@openai/codex` 0.151.0 that
rejects `gpt-6-astra`.

> Status as of 2026-09-15 — **MDL [11](#ref-11) admission compression retired.** The reported
> evaluation tally was **0 accepted / 461 judged across both models**: 237
> historical native judgments plus 224 private-replay candidates (the replay
> compared same-chunk and corpus-dictionary models on those same 224 candidates).
> This is not a claim that the full live history never accepted: the
> [decision memo, section 1](DECISION-2026-09-15-mdl-analogy.md#1-mdl-choose-c-drop-admission-compression-and-rely-on-recall-ranking)
> also identifies a separate pooled acceptance, which does not demonstrate useful
> selection. Compression measures string reuse, not whether chat-shaped advice
> improves a later decision. Native and Bash shadow taps, pooling, corpus
> dictionaries/bootstrap, Python judging, environment knobs and MDL telemetry
> proposals are retired. Admission continues to mean an eligible retrieval
> candidate after existing deduplication; storage and recall ranking continue.
> No utility posterior replaces compression as a hard admission gate.
>
> The open question is whether automatic learning improves subsequent work:
> freeze 20 independently graded tasks before inspecting retrieval, then run
> paired trials with identical model, budget and existing memories, including
> versus excluding only the preceding automatic-learning cohort. Verify actual
> injection and exclude source transcripts and future information. Retain
> automatic admission for at least three net additional successes out of 20
> (a screening threshold, not statistical proof); otherwise retire automatic
> free-form storage while preserving explicit memories and source episodes.
> This experiment remains pending; planted-memory SMRITI gains do not answer it.
> Protocol agreed 2026-09-15 (Fable + Codex): see
> `DECISION-2026-09-15-learning-experiment.md`. Tasks are collected
> prospectively after the cohort cut, three trials per task (120 runs),
> Δ ≥ 3 over 20 tasks to retain; harness on `feat/learning-experiment`.
>
> The outcome ledger remains active: hooks record injections, command outcomes
> and session ends; the offline joiner assigns per-memory Wilson [2](#ref-2)-bound credit.
> Null exit codes are excluded from the known-outcome set; the current joiner
> can still count `likely_fail` heuristics as failures. Shared-window credit
> is associational and confounded by task difficulty and co-injected memories;
> it is not causal evidence or an admission policy. Historical
> `~/.claude/mind/mdl_gate_shadow.jsonl` stays untouched and inert: retirement
> neither appends to it nor consumes it for evolve coverage proposals.

## Isolated best-of-N — 2026-09-16

`--candidates N` opts into independent Codex implementations after the survey
chooses one proposal and registers one bet. The default is 1 and retains the
single-stream execution path (including Claude support). Counts above 1 require
Codex. The wrapper forwards the option unchanged:

```bash
scripts/evolve-cycle.sh --candidates 2 --max-minutes 120
PYTHONPATH=chitta-mcp python3 -m evolve.report
# Offline report from retained artifacts, without querying memory:
PYTHONPATH=chitta-mcp python3 -m evolve.report --cycles .evolve/cycles
```

Every stream starts from the same pinned base in a separate worktree and branch,
using the same proposal, survey decision, and preregistered bet. Only its assigned
paths/branch differ in the spec. Implementations and gates run concurrently.
Each gets private HOME, CODEX_HOME, XDG config/cache/data/state/runtime, temporary
files, CHITTA_DB_PATH, socket and queue paths; inherited Chitta/Claude/Codex task
state is removed and both headless flags are set. No shared Codex configuration,
MCP connections, hook state or session directories are copied. Supply Codex API
authentication through `CODEX_API_KEY`; isolated homes do not inherit an interactive
login. Tool executables must already be available on PATH (or CHITTA_CODEX_BIN).
This isolates task state, not filesystem access: the spec also forbids inspecting
other streams or live memory. Implementation worktrees and private state remain
under `.evolve/` for review; no stream merges another stream's work.

The runner checks clean committed changes, ancestry/branch, frozen paths and the
immutable helper, existing path-dependent tests/builds, a changed SELF_CHECK test,
and cleanliness after gates. Only passing streams are measured. Each receives
the existing paired baseline/candidate evaluation on fresh copies of the same
replica family. Measurements run sequentially to avoid competing replica loads.
Selection maximizes the bet's target delta in its registered direction; equal
deltas prefer fewer added-plus-deleted lines, then the lower candidate index.
Binary changes cannot win a tie by claiming zero lines. Missing/incomparable
metrics cannot win. Existing noise-band, quality and runtime-coverage verdict
rules still apply to the selected artifact; selection alone does not accept it.
The selected branch is retained for review, and the existing explicit `--open-pr`
option can publish only an accepted selection.

One monotonic `--max-minutes` deadline includes survey, setup, implementations,
gates and evaluation. Fan-out admission requires at least ten minutes for shared
implementation/gates plus five minutes per paired candidate evaluation (twenty
with `--real-eval`). These are minimum allocations, not runtime predictions.
Below that floor the runner falls back to one stream and records the explanation.
The implementation deadline reserves those evaluation minutes; running process
groups are reaped on timeout. Existing short process cleanup and verdict-memory
bookkeeping grace periods still apply after a deadline. Long builds or measurements
can exhaust the remaining budget; every candidate timeout/failure is retained.

Verdicts and bet resolutions contain `candidates` with index, branch/worktree,
gate outcomes, changed-line count and metric delta; verdicts also record the
requested/effective counts, fallback reason and selected index. The report prints
per-cycle selected/best and first-candidate deltas, using `n/a` when no eligible
measurement exists. Failed streams are never silently converted to zero delta.
These are observations for comparing isolation over cycles, not a claim of a
measured improvement from this implementation. The proposal card's accepted-bet
rate is not produced by the existing replica evaluator, so that metric cannot
receive an acceptance without a dedicated evaluator.

## Proposal sources and scoring

* Telemetry reads `~/.claude/mind/outcome_ledger.jsonl`: empty recall divided by
  empty-plus-injected events, timeouts per attempted lane in `injected.lane_timeout`,
  and failed Bash outcomes divided by all Bash outcomes. It also asks
  `chitta health_check --json` for `rpc_over_budget` and runs the repository's
  `saddle_detector.py report --json`.
* Memory reads `chitta recall --json --realm project:cc-soul --tag <tag>` for
  `ceiling`, `todo`, `incident`, and `correction`, using the keyword strategy and
  `--no-learn`. Tracked repository `// ceiling:` and `# ceiling:` comments are
  additional memory candidates. Wrong-realm or malformed CLI replies are rejected.
* Literature cards live in `chitta-mcp/evolve/proposals.d/*.json`. See the
  [schema](../chitta-mcp/evolve/proposal.schema.json) and
  [example](../chitta-mcp/evolve/proposals.d/README.md). Hypothesis cards use
  `source: "hypothesis"`.

One `Proposal` holds id, title, mechanism, expected gain, cost, evidence, source,
`verifiability`, and `prior_effort`. New cards require the last two fields in the
schema; normalization supplies `metric_only` and `none` for old cards. The derived
`internal_evidence` boolean is true for nonempty evidence only when **every**
item explicitly names `source: telemetry`, `ledger`, or `memory`. Unknown sources,
empty evidence, papers, and URLs receive no internal bonus; a card cannot set
this boolean directly. Generated internal observations carry their provenance.

| Field | Values and utility factors | Meaning |
|---|---|---|
| `verifiability` / V | `self_verifying`: 1.0; `metric_only`: 0.7; `judgement`: 0.35 | An internal consistency check falsifies the mechanism; only a replica delta can establish it; or a human must read it. |
| `prior_effort` / P | `none`: 1.0; `some`: 0.6; `exhausted`: 0.15 | Failed effort on this exact mechanism, with history as a floor. |
| `internal_evidence` / I | true: 1.2; false: 1.0 | Prefer evidence already inside the artifact. |

The three literature cards are explicitly classified: query rewriting and typed
preference graphs are `metric_only/none`; slot uniqueness is
`self_verifying/none` because multiple current values in one slot violate the
mechanism itself. All three have external evidence. Their numeric blast radii
are 2, 3, and 3 respectively, with named code paths preserved in `cost.scope`.
The two 2-percentage-point accuracy priors are represented as fractional 0.02.
These are card priors, subject to history, not claims that the ideas succeeded.

 IDs hash normalized title plus mechanism, so duplicate observations
merge their evidence. Production cycles persist JSON bodies as `signal`
memories tagged `proposal`, exclusively in `project:chitta-evolve`.
Evolution memories are read with paginated `list_memories_brief` to avoid
silently limiting UCB history to semantic recall's top results. Memory writes
use JSON tags and whitespace-prefixed JSON content because the CLI otherwise
coerces a leading `{` into an object instead of the required string.

The score is:

```
abs(expected_delta) * confidence / (effort_h * blast_radius) * V * P * I
    + c * sqrt(log(N + 1) / (tries + 1))
```

Deltas are signed changes in bounded metric units, such as absolute rate
changes. Their sign supplies the bet's direction. Telemetry and memory gains
are explicit **priors**, not measured improvements: telemetry proposes a 20%
reduction of observed bad rates; generic memory candidates start at +0.02
nDCG with 0.3 confidence. RPC overrun and saddle candidates use a 0.1 prior.
Owners should refine mechanism cards before trusting utility estimates across
unlike metrics. Raw counter gains are not directly comparable to rate gains.

`N` and `tries` count prior implementation choices, including rejects and
inconclusive cycles; null-choice surveys do not spend the exploration quota;
repeated cycle IDs count once. Deterministic ID ordering breaks ties. Before
cycle `N+1`, `--explore-quota 0.3` forces a hypothesis if fewer than
`ceil(0.3*(N+1))` prior-plus-current choices would be hypotheses. This guarantees
the quota at each prefix when hypotheses exist, including the first cycle.
Missing hypothesis cards produce a warning and retain exploration debt.
Exhausted ideas are excluded from exploitation even if their raw UCB score is
highest; they can win only as hypotheses through the exploration quota. The
survey shortlist obeys the same eligibility rule, so surveying cannot bypass it.
`python3 -m evolve.selector --dry-run` prints rankings and each proposal's V/P/I factors and effective effort independently (set
`PYTHONPATH=chitta-mcp`). `--c` controls the UCB exploration coefficient.

## Survey and effort history

`--top-k` (default 3) sends the highest-ranked eligible proposals to the selected
implementer, before any forward bet or implementation spec. `--survey-minutes`
(default 20) is included in `--max-minutes`, capped by the remaining cycle budget.
The read-only survey inspects each candidate's named code paths, starts with our
code/telemetry/ledger/memory before literature, and stops after the shortlist or
when its deadline approaches. It must reserve time to report a decision:

```json
{"chosen": "canonical-id", "abandoned": [{"id": "other-id", "reason": "check is too costly"}], "tractability": 0.8}
```

Only this JSON object (optionally in a single `json` fence) is accepted. All
nonchosen IDs must occur exactly once with reasons, the chosen ID must be in the
shortlist, and tractability must be a finite JSON number in [0,1]. A null choice
requires zero tractability. Unknown IDs, duplicates, missing reasons, trailing
prose, timeout, or edits/commits during the survey stop implementation. Malformed
or timed-out surveys are inconclusive and do not manufacture effort evidence.

The runner saves the prompt, candidates, raw final output, and parsed
`survey.json`; the verdict memory embeds the survey. Only after a valid choice
does it register a bet and write `spec.md`/`proposal.json` for that candidate.
A null choice ends as `skipped:no_tractable_candidate`, without a bet or spec.
Each abandoned candidate gets a `prior_effort_updates` entry bumped one level
(`none → some → exhausted`, capped). These updates live in verdict history;
the card files are not rewritten by a cycle.

Selection always reads `history()`, even when a card declares `prior_effort`.
One refuted verdict/resolution for the same whitespace/case-normalized mechanism
sets at least `some`; two or more set `exhausted`. New verdicts retain the
mechanism so changing a title/ID does not erase failed effort; old verdicts fall
back to canonical proposal ID. A refuted bet resolution counts even if the
cycle was accepted or inconclusive. A compile failure or rejection alone does
not establish that the mechanism was refuted. Duplicate cycle IDs count once;
null-survey effort bumps also contribute. Card effort and recorded bumps are
floors that history cannot lower.

## Forward bets and novelty

A `forward-bet` signal memory is written **before implementation** and contains
`proposal_id`, metric, signed predicted delta, increase/decrease direction,
`band_from_noise`, registration timestamp, cycle ID and preregistered bands for
other metrics. Missing bands are recorded as null; they never imply zero noise.
`bets.resolve(bet_id, measured_delta)` accepts a scalar target delta or a map of
metric deltas. It writes a `bet-resolution` memory linked by JSON IDs.

* **Confirmed:** target movement exceeds the noise band in the predicted
  direction and its magnitude is within one band of the prediction.
* **Refuted:** a measured, banded target fails that prediction, including movement
  within noise or a different magnitude. Bet confirmation is separate from the
  cycle's accept/reject decision about useful improvement.
* **Surprise:** an unpredicted metric moves beyond its own preregistered band.
  Unknown-band metrics cannot become surprises. Surprise takes precedence if
  both the target and another metric move.

A surprise records the unexpected metric movements. Those movements alone do
not establish useful new knowledge; `novel: false` makes no novelty claim.
Compression judgments no longer certify novelty.
There is no fabricated bet outcome when target measurements or bands are missing.
Verdicts link proposal memory, canonical proposal ID and bet ID, with the base
commit, branch, spec digest, measured deltas and resolution.

## Evaluation and gates

Every generated implementation spec requires **one internal consistency test**
that would fail if the mechanism were wrong, under the touched module's `tests/`
or `test/` directory. The implementer must run it, explain the falsification, and
name it on exactly one line in its final message:

```text
SELF_CHECK: chitta-mcp/tests/test_example.py::ExampleTests::test_consistency
```

The test ID is a repository-relative file plus `::` plus the test symbol. Python
functions use `test_name`, methods use `ClassName::test_name`, C++ tests use their function name or GoogleTest `Suite.TestName`, and Rust/shell
tests use the function name. Native test files use .c/.cc/.cpp/.cxx/.rs/.sh;
new C++ checks can be ordinary test functions without adding a test framework. The runner checks
that the test exists, its declaration/body intersects the committed diff, and
its module also has a changed non-test path. An unchanged test, a name in a
comment, a test in another module, or an absent/ambiguous SELF_CHECK cannot count.
The artifact records the verified ID. Missing proof forces
`inconclusive:no_self_check` regardless of the replica delta and prevents
publication. This structural check does not prove that a test's assertions
capture the mechanism; the implementer and human review still own that judgment.
The existing `verdict_for` measurement rules otherwise remain unchanged.

The runner refuses to proceed if `scripts/check-eval-immutable.sh` exists and
fails, both before selection and after implementation. With no helper it warns
and uses a built-in changed-path prohibition covering benchmarks, graders,
gold IDs, replica/check scripts and CONTRACTS.md. It rejects uncommitted output,
branch changes, changes without the pinned base as an ancestor, and empty commits.

Changed shell scripts get `bash -n`. Changed `chitta-mcp/` files trigger the
unittest suite. Only native changes (`chitta/` or `chitta-field`) trigger native
builds: initialize the local submodule with `--no-fetch`, run
`cd chitta-field && bash build.sh build --release`, configure
`cmake -S chitta -B chitta/build`, then use the exact CLAUDE.md build command,
`cd chitta && cmake --build build --parallel`. All outputs stay in the candidate
worktree. No install, restart or deployment command from CLAUDE.md runs.
Missing local submodule objects or build prerequisites fail the gates.

`scripts/eval-replica.sh status` checks the frozen snapshot source, with `start`
called only if needed. Each measurement uses a fresh private replica clone
under `/tmp/chitta-evolve-*`, launched by that same script from the source's
committed family. The candidate clone uses built worktree binaries for native
changes. For native changes, the baseline is also built from the pinned `main` commit
in a separate detached worktree under `.evolve/baselines/`, using the same
build commands. Thus both code revisions and both snapshot identities are
controlled; an arbitrary deployed binary is never the baseline of an accept.
Private clones use isolated sockets and ports. The runner stops only its own
clones through the launcher's PID/path validation and retains failed-cleanup
paths with a warning. It never stops or replaces the shared replica or live daemon.

The trusted grader and gold IDs are copied to cycle artifacts, and run with
`CHITTA_EVAL_SOCKET` and canonical `--quiet --limit 10`. Fresh reports must name
the expected snapshot and identical version/limit/reranker/query-count settings.
Only `--real-eval` runs `benchmarks/smriti/runner.py --split holdout --trials 3
--agent claude-code`, twice for paired measurements. Missing `--split` refuses
an unpartitioned run. Success rate and `delta_sr` are read from its three-trial
JSONL results. Holdout artifacts never enter implementation prompts.

`benchmarks/noise.py band <metric>` is called when present. `noise.json` accepts
`{"metrics": {"mean_nDCG": {"band": 0.02}}}` or a direct metric map;
`noise_band` and numeric metric values are also supported. Bands are frozen at
registration. Improvement beyond the target band, without nDCG regression or
a failing grader, can be accepted; regression is rejected; movement within
noise is inconclusive. Missing comparable target metrics, bands, snapshot IDs,
canonical reports or evaluators never produces an accept.

The current grader exercises native recall. It cannot attribute gains to
installed hook/MCP changes without a dedicated adapter, so those cycles remain
inconclusive even if tests pass. Native measurements use the candidate's own
private daemon instead of crediting gains observed on the old binary.

## Safety and budgets

The canary order is **worktree → frozen replica → human → live**. The spec
requires worktree-only edits, prohibits systemd/install/pkill/push and live hook
or binary changes, and requires a commit on the assigned branch after gates.
Proposal contents are explicitly untrusted data. The implementer is responsible
for obeying this boundary; a written spec is not an OS sandbox. Use an isolated
agent environment for unattended operation.

A monotonic wall-clock budget bounds subprocesses and memory calls. On timeout,
the runner terminates only that subprocess group and its descendants; it never
uses a process-name kill. Private replica cleanup and the final verdict write
have up to ten seconds of grace each. Interrupted or unavailable evaluation
records an inconclusive verdict. Local verdict JSON is written before its memory
write, so a memory outage leaves a recoverable artifact and a nonzero exit.

The default never pushes. `--open-pr` is the caller's explicit authorization to
push the accepted branch and create a PR using a body file. A failed/rejected or
inconclusive cycle cannot publish. No automatic merge or live deployment exists.

<!-- BEGIN CITATIONS -->
## References

- <a id="ref-2"></a>**[2]** Edwin B. Wilson. Probable Inference, the Law of Succession, and Statistical Inference. Journal of the American Statistical Association 22(158), 209–212 (1927). [source](<https://doi.org/10.1080/01621459.1927.10502953>)
- <a id="ref-11"></a>**[11]** Jorma Rissanen. Modeling by shortest data description. Automatica 14(5), 465–471 (1978). [source](<https://doi.org/10.1016/0005-1098(78)90005-5>)
<!-- END CITATIONS -->
