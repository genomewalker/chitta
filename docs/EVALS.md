# Evaluation trust layer

Status as of 2026-09-16

2026-09-16 — Phase 7 replica canary: `scripts/nightly-replica-canary.sh`
copies the selected evaluation family into a fresh private mind, pins its socket,
and runs three read-only golden panels using the noise calibration's
hybrid/depth-20/no-reranker configuration. It compares their mean with the
lower `golden.ndcg.band_95` bound and exits nonzero on regression, invalid scores,
uncalibrated configurations, or snapshot mismatch. Reports go to
`/projects/caeg/scratch/kbd606/tmp/canary/<UTC-date>.<unique>/report.json`;
`CHITTA_CANARY_SOURCE` and `CHITTA_CANARY_REPORT_ROOT` support isolated runs.
The optional current-truth runner must accept `--socket`/`--output` and emit
scalar `current_truth.*` metrics matching `noise.json`; an available but
uncalibrated/incompatible runner fails closed. This revision has no such panel,
so its absence is explicitly recorded, not counted as a pass.

2026-09-16 — Canary evidence: family `bbcaed33`, golden mean **0.492311**,
calibrated band **[0.490965, 0.494474]**, PASS. Dated report:
`/projects/caeg/scratch/kbd606/tmp/canary/2026-09-16T093217Z.XnrfeW/report.json`.
Individual samples were 0.446157, 0.525503, 0.505273: variability exceeded the
historical band, so this is a mean-score regression check, not evidence of
stability or equivalence. Controlled fixtures verified PASS/exit 0,
REGRESSION/exit 1, nonfinite-score rejection, snapshot-mismatch rejection, and
scratch cleanup. `scripts/install-evolve-timers.sh` writes a separate
`chitta-replica-canary.timer`, enabled only with `--enable-canary`; it has no
live-daemon dependency. No unit was installed or enabled during validation.
The phase's one-week canary soak remains outstanding.

Automatic-learning harness built; the prospective 20-task panel remains pending — [protocol](../benchmarks/learning/protocol.md).
2026-09-15 23:15 CEST: **official cohort cut recorded** in `benchmarks/learning/cohort.json`
(`cut_timestamp_ms` 1789506106539, family `da86decb`, realm `project:cc-soul`).
The immutable source copy lives at
`/projects/caeg/scratch/kbd606/tmp/learning-cut-20260915-frozen` (read-only);
`learning-cut-20260915` is the launcher's working copy and may be overwritten.
The task panel is now collected prospectively: every graded prompt from a real
session after the cut is a candidate for `freeze.py task add` / `task validate`;
`freeze.py freeze` runs once 20 are validated.

The evolution loop is propose → implement in an isolated worktree → gates →
evaluate against a frozen-snapshot replica → verdict → human merge. Evaluation
changes need a separate, explicit approval. Nothing here starts a daemon, merges,
or publishes a candidate.

2026-09-16 — The
`scripts/chaos-replica.py`/CTest cases caught NFS post-unlink WAL data loss and
non-idempotent queue replay, leading to the authorized production fixes. The
final gate passes all nine full-copy cases and 25/25 CTests. See
[the measured per-case table](FIELD_PERF.md#2026-09-16-phase-7-recovery-gates) and
[recovery implementation](../Documentation.md).
Run a real-copy case with the configured worktree binaries and the required
CPython, for example `python3 scripts/chaos-replica.py --source
/projects/caeg/scratch/kbd606/tmp/chitta-eval-mind --cases snapshot,second
--report /tmp/chaos.json`. Omit `--cases` to run all nine cases, including the
production binary format probe under embedding/matrix load. `--cases` permits independent investigations;
a multi-case run stops at its first failed invariant to avoid using contaminated
state. CTest registers each case independently. Raw logs and scratch results
remain untracked; the dated evidence summary is committed.

## 2026-09-16: current-truth panel and provenance coverage

Status 2026-09-16: the 50-question current-truth panel and read-only provenance
census are implemented. The full historical F2 fixture gate and exact
session-or-source coverage remain unavailable for the reasons below. The
prospective automatic-learning experiment remains pending its 20 frozen tasks;
this retrieval panel supplies no causal learning verdict.

The questions were authored directly from repository files at
`1c82589b93bbe350319678db66a9d720385cdc4b` before consulting recall, with pinned
citations, deterministic matchers, 30 visible / 20 procedural holdout questions,
and 10 predeclared abstentions. [Protocol](../benchmarks/current_truth/README.md),
[panel](../benchmarks/current_truth/questions.json),
[runner](../benchmarks/current_truth/run.py).

Three final repetitions used a private scratch copy of frozen learning family
`da86decb` (sequence 206502617, manifest generation 39285), created by
`scripts/eval-replica.sh` from `learning-cut-20260915-frozen`, port 17436.
The daemon was stopped after evaluation. Configuration: realm `project:cc-soul`,
fused strategy, limit 3, `--no-learn`, no external reranker or LLM. The dated
[baseline](../benchmarks/current_truth/baseline-2026-09-16.json) binds panel and
binary hashes, family identity, configuration and every result ID. This family
is different from the historical `noise.json` family; do not compare their bands.

| Panel | Run 1 | Run 2 | Run 3 | Denominator |
|---|---:|---:|---:|---|
| Visible answerable p@3 | 5/24 | 5/24 | 5/24 | 24 answerable questions |
| Holdout answerable p@3 | 5/16 | 5/16 | 5/16 | 16 answerable questions |
| All answerable p@3 | 10/40 | 10/40 | 10/40 | 40 answerable questions |
| Visible abstention | 6/6 | 6/6 | 6/6 | Trap avoidance |
| Holdout abstention | 4/4 | 4/4 | 4/4 | Trap avoidance |
| All abstention | 10/10 | 10/10 | 10/10 | Trap avoidance |
| Total correct | 20/50 | 20/50 | 20/50 | Visible 11/30; holdout 9/20 |
| Wrong-confident | 0 | 0 | 0 | Predeclared traps only |

Here p@3 follows the requested any-top-three question-success rule (usually
called hit@3), not document precision. Abstention means no trap matched, not
proof that a missing fact was recognized. Misses cost 0 and wrong-confident
answers cost 2 in the supplementary utility score. Numeric matchers are
contextual; regex/literal scoring still cannot reliably judge negation.

The decision's F2 line records aggregate scores and themes, **not the five
original prompts**. Their location was requested; five explicitly reconstructed
fixtures are provided so the missing evidence is not silently fabricated:

| Reconstructed fixture | Run 1 | Run 2 | Run 3 |
|---|---|---|---|
| cache-ttl-variable | correct | correct | correct |
| prompt-median | miss | miss | miss |
| session-start-median | miss | miss | miss |
| cached-startup | miss | miss | miss |
| running-binary-install | miss | miss | miss |

These 1/5 results cannot certify the historical five-probe exit gate. Replacing
the reconstructions with original prompts is a separately reviewed frozen-panel
change, followed by recalibration.

`current_truth.p3`: mean **0.25**, sample SD **0**, descriptive band **[0.25, 0.25]**,
2-SD threshold **0**. `current_truth.abstain`: mean **1**, SD **0**, band **[1, 1]**,
threshold **0**. These are three warm repetitions, not evidence of equivalence.
An initial smoke before citation-only line corrections scored 9/40, 10/40, 10/40;
query text, matchers and splits were unchanged. The final panel hash was then
calibrated again. Recall-side runtime state can change even with `--no-learn`.
Existing protected `benchmarks/noise.json` is unchanged; the task explicitly
allowed the dated baseline fallback. Select it explicitly:

```bash
python3 benchmarks/current_truth/run.py --dry-run
# Export the private replica.env; this command never starts a daemon.
bash scripts/eval-noise.sh --current-truth-only --current-truth-runs 3 \
  --output /tmp/current-truth-noise.json
python3 benchmarks/noise.py band current_truth.p3 \
  --file benchmarks/current_truth/baseline-2026-09-16.json
```

### Read-only live provenance census

The [audit script](../scripts/provenance-coverage.py) and its
[frozen aggregate report](../benchmarks/current_truth/provenance-2026-09-16.json) used only
`list_memories_brief`, `memory_provenance`, and `query_graph` through the CLI.
It never opened the live store or used recall. It enumerated all visible realms,
continued through the server's 100-row cap until an empty page, deduplicated IDs,
and evaluated creation timestamps for the preceding seven days.

Audit interval: **2026-09-16T09:26:53.089184+00:00–2026-09-16T09:29:02.823897+00:00**. Unknown creation times: **0**; future creation times: **0**.

| Writer | Overall covered / total | Last 7 days covered / total |
|---|---:|---:|
| hook_compliance | 360/360 | 0/0 |
| hook_regex | 6,848/6,848 | 476/476 |
| hook_regex+native_distiller | 2/2 | 0/0 |
| hook_turn_ingest | 2,911/2,911 | 0/0 |
| mcp_tool | 314/314 | 0/0 |
| native_distiller | 37,729/38,274 | 2,332/2,332 |
| unknown | 0/86,679 | 0/146 |
| **All writers** | **48,164/135,388 (35.575%)** | **2,808/2,954 (95.058%)** |

**These are observable-provenance lower bounds.** A nonempty `source` label
identifies its writer and qualifies as source evidence; it never becomes a
synthetic session. Numeric `source`/`derived_from` references count only when
the target memory exists in the enumerated population. No text heuristics or
inferred sessions are used. Unknown writers remain unknown; combined labels
occupy one bucket rather than double-counting a memory.

The public metadata API does not expose the stored `source_session` field:
0 audited memories exposed it. Observable session triplets were also 0, but
**the actual stored-session count is unavailable, not zero**. The historical
offline 1,236-session statistic is therefore not directly comparable. Exact
session-or-source union coverage requires a read API exposing that field;
changing daemon APIs is outside this phase's write scope and frozen contracts.
The script explicitly reports `coverage_is_lower_bound=true` and
`session_coverage_complete=false`. This live interval census is not atomic;
concurrent inserts/deletes may affect pagination.

```bash
python3 scripts/provenance-coverage.py --socket /explicit/live/socket \
  --output /tmp/provenance-coverage.json
```

Validation: **149 MCP tests, 46 SMRITI tests, 10 new panel/audit tests**, all
`hooks/tests/*.sh`, CI-scope Ruff check/format, `bash -n` and ShellCheck on the
touched shell script, daemon-free panel dry-run, and **contracts unchanged**.
MCP subprocess fixtures required `CHITTA_CODEX_BIN=codex` to avoid selecting the
host executable; the card hook test required removing an inherited shared
socket override. Initial failures and the successful isolated retries are
recorded rather than treated as production fixes. New benchmark files and the
audit script are listed in `EVAL_IMMUTABLE.txt`; CI changes only extend Ruff paths.

## Evaluation stack

| Layer | Evidence | Limitation |
| --- | --- | --- |
| Golden recall | 30 queries in `hooks/grade-recall.py`; frozen gold IDs; mean nDCG@20 | Retrieval quality is not task success. Match depth, strategy, reranker, and snapshot across comparisons. |
| SMRITI | 15 tasks, off/on/ablation conditions, hidden checks, repeated trials | Small handcrafted panel; visible tasks are available for development. |
| Injection ledger | `injected_confirmed` joins planted IDs to events during the agent call | An unconfirmed memory-on sample cannot establish memory benefit; the join is by time window. |
| Noise calibration | Repeated golden scores and repeated fixed SMRITI panels | Real calibration must use the replica and real agent. Echo is harness validation only. |
| Immutability | Ref-to-ref protected-path check | A trailer records approval; it does not authenticate the reviewer. Human merge and trusted required CI remain necessary. |
| Task mining | Convention-changing commit pairs and saddle episodes | Candidates require manual reconstruction and leakage review. Mining executes no candidate or ledger command. |

The existing `scripts/eval-replica.sh` manages the replica. Source the environment
file it produces, including `CHITTA_EVAL_SOCKET`, `CHITTA_EVAL_SNAPSHOT_ID`, and
`CHITTA_EVAL_MIND`, before real evaluations. That script is unchanged by this work.
The caller must ensure that the socket and metadata describe the same frozen
snapshot. Compare baseline and candidate under the same configuration and task
panel, using fresh replicas when trial planting or learning could affect state.

## Held-out split

`benchmarks/smriti/split.json` assigns `example-001`–`example-009` to `visible`
and `example-010`–`example-015` to `holdout`. The latter contain the non-deducible
conventions that have moved ΔSR. All 15 remain in the repository: this is a
procedural holdout, not a filesystem security boundary. Keep the holdout and
hidden answers out of proposal/implementation agent context; reserve them for
verdict evaluation. Previously exposed tasks cannot become unseen by rotation.

```bash
python3 benchmarks/smriti/runner.py --trials 3 --dry-run
python3 benchmarks/smriti/runner.py --split holdout --trials 5 --agent claude-code
python3 benchmarks/smriti/runner.py --split all --trials 3 --dry-run
python3 benchmarks/smriti/scorer.py benchmarks/smriti/results/RUN.jsonl
```

The runner defaults to `--split visible`; explicit `--task` cannot bypass it.
Use `--split-file` for a separately curated task directory. Its assignments must
exactly cover that directory. New result rows preserve both their split and a
SHA-256 of the assignments. Resume refuses a different or missing manifest hash.
The scorer reports each split alongside the existing aggregate metrics. Legacy
rows use the current manifest with an explicit fallback label; do not compare
such assignments across rotations as if they were historical truth.

```bash
python3 benchmarks/smriti/split.py rotate --count 2 --seed nightly-rotation --date 2026-10-01
```

Rotation exchanges N tasks in **each** direction, retaining split sizes. SHA-256
ordering of seed, date, and task ID makes the result reproducible from the same
starting manifest across Python versions. `split-history.jsonl` records source
assignments for moved tasks and before/after hashes. History is written before an
atomic manifest replacement; if interrupted, compare the last event's hashes
with the manifest before retrying. Serialize rotations and review/commit the
manifest and history together. A rotation is a protected eval change.

## Noise and acceptance

```bash
# Replica dry harness: defaults to 5 golden passes, 3 visible tasks, 3 trials.
bash scripts/eval-noise.sh --agent echo
# Real calibration: only the caller runs this, on the replica.
bash scripts/eval-noise.sh --agent claude-code
# Select a fixed panel, for example when calibrating holdout verdicts.
bash scripts/eval-noise.sh --agent claude-code --split holdout --tasks 3 --trials 5
# Explicitly allowed fallback when no eval socket is set:
bash scripts/eval-noise.sh --agent echo --golden-runs 2
python3 benchmarks/noise.py band golden.ndcg
python3 benchmarks/noise.py band smriti.on.sr
python3 benchmarks/noise.py band smriti.on.tokens
```

The helper uses stdlib statistics and the grader's actual `grade_one` scoring
function. It fixes depth 20, hybrid retrieval, and no reranker. This differs from
the grader's historical canonical depth-10/reranker configuration; do not reuse
noise bands across them. Recall is explicitly `--no-learn`, and transport errors
abort scoring instead of turning into zero scores. Calling the grader's CLI
`main()` would also store gap memories; calibration deliberately avoids it.

The report `benchmarks/noise.json` contains provenance, raw observations, mean,
sample SD (N−1 denominator), a descriptive normal 95% band `mean ± 1.96 × SD`,
and `accept_delta = 2 × SD`. The band describes run variation, **not** a confidence
interval for the mean; a tiny K is provisional. SR bands can extend beyond [0,1]
because they are untruncated normal approximations. SMRITI's observation unit is
the SR or mean token count of the entire fixed panel in one trial, separately for
off and on. This avoids mistaking variation between task difficulties for noise.

The loop's rule is an improvement **strictly beyond 2 × SD** of the matching
metric: positive for nDCG/SR, negative for tokens. `band` prints that positive
threshold. It refuses echo, live-smoke, incomplete, or unconfirmed calibration;
echo's deterministic zero SD must never authorize evolution. This rule is a
practical gate, not a paired statistical test or a correction for repeatedly
trying many candidates. Record the tested metric, task panel, split hash, and
snapshot; holdout outcomes still need human review.

Real calibration requires the replica mind for injection confirmation and rejects
the default live mind. Echo prints simulated planting/recall/cleanup operations,
makes no agent calls, and measures word-count token proxies. It still runs fixture
checks. No real agent calibration was run for this change. The historical ±0.027
recall observation is context only, not a current threshold. The two-pass live golden smoke attempt timed out before completing a pass;
its golden mean, SD, and band are unavailable rather than zero. The 3-task ×
3-trial echo panel completed for both conditions: SR 0 and mean token proxy
38.3333, each with SD 0. These are harness observations, not acceptance bands.
See the checked-in report for the transport error and raw observations. A live smoke
run has `snapshot_id: null` because the live store is not a frozen snapshot.

## Immutability gate

```bash
bash scripts/check-eval-immutable.sh BASE HEAD
```

`benchmarks/EVAL_IMMUTABLE.txt` protects the gold IDs and grader, every
`benchmarks/smriti/tasks/*/hidden/**`, split assignments, noise report, outcome
ledger hook, and replica script. It also protects the manifest, checker, and CI
workflow themselves. Patterns use Git's rooted glob syntax. Additions, deletions,
and both sides of a rename count as changes. The checker reads the **union** of
base and head manifests, so removing a pattern in the candidate cannot disable
it. Invalid refs or absent policy fail closed.

Only this line in the **head commit's** message overrides a protected change:

```text
Eval-Change-Approved: Reviewer Name
```

Approval on a parent commit does not carry forward. Do not self-authorize a
candidate's eval edits. The `eval-immutable` CI job runs on PRs targeting main
whose source branch starts with `evolve/` or `auto/`, comparing the PR's exact
base and head SHAs. It runs the checker from the base checkout, not PR-supplied
checker code. On initial rollout, when the base has no checker, CI requires the
approval line without executing the candidate. Configure this as a required
check and retain human review of changes to CI itself.

## Adding tasks

1. Mine into scratch outside the source repository:
   ```bash
   python3 benchmarks/smriti/mine_tasks.py /path/to/repo --out /scratch/mined-tasks
   python3 benchmarks/smriti/mine_tasks.py /path/to/repo --from-saddles --out /scratch/mined-tasks
   ```
   Use `--ledger PATH` for a copied ledger. Git mining reads HEAD history without
   merges, selects messages containing convention/rename/default/contract/format,
   and requires changed source literals/constants/flags with shared evidence in
   changed test assertions. It saves a sparse pre-change fixture and an actual
   post-change test under `hidden/`, with commit IDs and evidence in `task.json`.
   It skips symlinks and submodules. It never checks out or runs source history.
2. Review the candidate. Add its dependencies, reconstruct an isolated runnable
   fixture, reduce the hidden test, and supply the exact planted convention.
   Saddle reports expose command heads (at most 80 characters), not full commands
   or working directories: recover these and a failing fixture before execution.
   The report exposes only the top 8 episodes; total episodes and emitted count
   are reported separately. Missing ledgers are unavailable, not zero failures.
3. Rewrite the prompt around the task's intent without revealing the answer.
   Keep answer-bearing tests in `hidden/`, never the visible fixture. Confirm the
   fixture fails and a reviewed solution passes. Check that the task truly needs
   an external convention rather than ordinary code exploration.
4. Promote the reviewed task manually to `benchmarks/smriti/tasks/`, removing
   candidate-only metadata to fit the schema. Assign it in `split.json`, add
   leakage/solution coverage, and run the SMRITI unit tests. Promotion changes
   protected files and requires the approval trailer.
5. Recalibrate on the frozen replica for any changed task panel or metric
   configuration before using its noise threshold.

The read-only history demo emitted **5** candidates into
`/projects/caeg/scratch/kbd606/tmp/mined-tasks`. One useful example is
`candidate-f6d63bd34dec`: the cc-soul → chitta rename, with test assertions about
`CC_SOUL_ABLATE_LANES` and `CHITTA_ABLATE_LANES`. The live saddle report contained
**2 failing events, 0 episodes, and 0 candidates**. The failure hook was only
registered on 2026-09-11, so this is sparse evidence, not proof that failures are
rare. Candidate files and ledger reports stay in scratch.

## Gates

```bash
bash -n scripts/eval-noise.sh
bash -n scripts/check-eval-immutable.sh
(cd benchmarks/smriti && SMRITI_SCRATCH=/tmp python3 -m unittest)
ruff check benchmarks/noise.py benchmarks/check_eval_immutable.py benchmarks/smriti
```

The tests include deterministic rotations, default-visible CLI selection,
per-split scoring and resume identity, sample statistics and echo isolation,
synthetic Git commits that exercise the approval rule, and read-only task mining.
Existing harness unit tests use a fake memory CLI and ledger; no live daemon is
required. CI also imports the new modules at the Python 3.9 deployment floor.

## Utility posteriors: when to flip `CHITTA_UTILITY_RECALL`

> Status as of 2026-09-14: still **off**. Per-memory Beta posteriors only became
> meaningful on 2026-09-13, when `post-bash-hook.sh` started receiving Claude
> Code's `PostToolUseFailure` payloads and Codex's exit-less shape; everything
> recorded before that is success-only and must not be trusted.

Flip procedure (earliest 2026-09-21, one week of real failure data):

```bash
# 1. how much failure signal exists now
python3 chitta-mcp/outcome_ledger.py report | head -20
# 2. recompute posteriors from the ledger and apply them (rows before 2026-09-13
#    carry no failures; `credit` never counts an unknown exit as success)
python3 chitta-mcp/outcome_ledger.py credit --apply
# 3. compare golden and SMRITI on the frozen replica with the flag on vs off
CHITTA_UTILITY_RECALL=1 bash scripts/eval-noise.sh --agent claude-code --tasks 3 --trials 3 --output /projects/caeg/scratch/kbd606/tmp/noise-utility-on.json
# 4. accept only if smriti.on.sr / golden.ndcg improve beyond benchmarks/noise.json bands; then set the flag in the chittad drop-in and restart
```


2026-09-14 — Analogy lane smoke set: `benchmarks/analogy/tasks.json` has 20
handwritten, live-triplet-grounded queries (14 proportional, six structural),
with labels frozen before evaluation. On frozen replica bbcaed33, manifest
38047, every grounding fact and probe/expected memory was independently verified
present (`coverage.json`). Default `run.py` invokes `chitta recall_analogy --json`
through CHITTA_EVAL_SOCKET, with a 2 s cap: hit@1=0/20, hit@3=0/20, median
7.058 ms, **20 CLI command-rejection errors** (`results.json`); that latency is
client rejection, not retrieval. A separate `--transport rpc` diagnostic on the
same socket yielded hit@1=0/20, hit@3=0/20, median 8.259 ms, zero timeouts:
10 proportional calls returned unrelated candidates, four were empty, and six
structural calls reported the lane unavailable (`results-rpc.json`). Errors count
as misses; the diagnostic excludes CLI startup. **hit@3=0.00 < 0.30: the analogy
lane is not ready for a hook.** No lane tuning or immutable eval edits were made.
These correlated forward/reverse smoke tasks do not establish broad accuracy.

2026-09-15 — **KEEP `recall_analogy` as explicit directed relation transfer**, per decision memo §2: on a private, freshly copied bbcaed33 family (generation 38047), the independent exact relation-join baseline and proportional RPC both achieve hit@1=14/14, hit@3=14/14, negative abstentions=14/14, unsupported answers=0, missing grounding=0. RPC errors=0; median latency=0.333 ms over 28 calls (baseline 1.627 ms over 14 positive two-graph-RPC joins; these timings have different workloads). All 14 negative argument triples are distinct and target existing subjects with other outgoing edges. Complete answer sets, citations, inputs and hashes: `benchmarks/analogy/baseline.json`, `results.json`, `replica-selection.txt`. Structural relevance tasks and endpoint VSA [10](#ref-10) ranking are retired; indexed exact-subject joins bypass the 10,000-fact bound and reject legacy ID-collision neighbours, with supporting edges and explicit `reason` abstentions. HDC and `query_graph` are unchanged; no graph cleanup or deployment. This is the memo's small screening test, not broad analogy accuracy.


## Explicit relation transfer — 2026-09-15

**Decision: keep `recall_analogy` as explicit directed relation transfer.**
The [retirement record and decision reference](EVOLVE.md)
requires hit@3 ≥12/14, 14/14 negative abstentions, and zero unsupported
answers. The final run meets all three thresholds; `query_graph` remains available.
The endpoint supports proportional relation transfer only; shape matching and VSA ranking are removed. Exact a→b
predicates transfer as a union to actual outgoing neighbors of c; results carry
supporting edges and rank by edge weight, then recency. Missing source/target
relations produce explicit abstention reasons. No reverse/fuzzy relation is inferred.

| Final benchmark | Exact relation-join baseline | `recall_analogy` RPC |
| --- | ---: | ---: |
| Positive queries | 14 | 14 |
| Hit@1 | 14/14 | 14/14 |
| Hit@3 | 14/14 | 14/14 |
| Negative abstentions | 14/14 | 14/14 |
| Unsupported returned answers | 0 | 0 |
| Missing grounding | 0 | 0 |
| RPC errors | 0 | 0 |
| Median latency (ms) | 1.526 | 0.190 |

Provenance: selected replica family **bbcaed33**, snapshot sequence **206208172**,
manifest generation **38047**, copied and selector-verified before scratch daemon
startup. The daemon opened only the private copy, with a private socket/runtime/port,
queue disabled, autonomous work disabled, and `.quiesce` present. Startup can write
private WAL bookkeeping; the input was a fresh family copy, not a reused warmed
store. No graph facts were planted, repaired, or cleaned up.

The independent `baseline.py` enumerated complete valid answer sets with temporal
indexed graph reads, cross-checked against `query_graph`, and froze 14 distinct
negative argument triples before `run.py` invoked the endpoint. Exact subject
checks reject stale index entries caused by legacy reused triplet IDs. Original
positive queries and grounding labels are preserved; missing grounding counts as
failure. Every returned answer and supporting target edge/memory citation is audited.
Baseline latency covers two graph RPCs and the Python join for positives; endpoint
latency covers one RPC per query across all 28 cases. These single-run figures
have different workloads and do not establish a speedup or broad generalization.

Artifacts: [baseline](../benchmarks/analogy/baseline.json),
[RPC results and keep decision](../benchmarks/analogy/results.json),
[family selection](../benchmarks/analogy/replica-selection.txt), and
[contract/reproduction](../benchmarks/analogy/README.md). Results bind the baseline
and task files by SHA-256 and record the private socket and snapshot identity.

Validation: Rust `./build.sh test --release` completed with **264 passed,
2 ignored, 0 failed**, including the duplicate-triplet-ID regression; binary and
doc-test targets also passed. `ctest` completed with **17 passed, 1 skipped
(`embed_pool_test`), 0 failed**. Six benchmark unit tests, the updated hook
regression, and touched-Python Ruff checks passed. Earlier isolated hook retries
and SMRITI tests passed. The existing MCP suite remains **129/130 passing**, with
one HTTP SDK/session-manager error (`BoundedSessionManager._session_owners`);
no MCP implementation was changed. A redundant Rust invocation overlapped the
surviving earlier job and failed linking; it was stopped after the original job
completed successfully. The owned scratch daemon was stopped after evaluation.

## 2026-09-16: replication-weighted recall (default off)

`swarmworld-replication-rank` now counts **distinct identifiable source sessions**,
not confirming memories, recall events, or writer labels. For a memory, take the
union of sessions in its own provenance, its direct confirmers' provenance, and
observations preserved by dedup. Provenance follows numeric `source`,
`derived_from`, and `merged_from` memory references with cycle detection;
`source_session`, `replication_session`, nonnumeric `derived_from` session targets,
and explicit `source=session:<id>` provide session identities. Bare `source`
labels (`distillation`, `mcp_tool`, etc.) do not. Repeated evidence from one session
counts once; unknown evidence contributes no invented session, and the final
neutral count is at least one. Confirmation chains do not recursively amplify
support. Deleted evidence and invalidated/superseded triplets are excluded.

The count is cached on `MemoryState`, skipped in serialization and reconstructed
after snapshot/WAL replay. Mutation paths refresh affected provenance dependents;
foreign WAL batches rebuild the derived counts. Recall reads only the cached
integer. Existing WAL-backed triplets preserve incoming dedup sessions, including
sessions attached after the daemon's remember call has returned a merged ID.
No snapshot version, migration, or FFI layout change is needed. Historical dedup
sessions already discarded by old binaries cannot be recovered.

`replication_max` in `scoring.json` is overridden by `CHITTA_REPLICATION_MAX`.
Zero disables the factor. With the proposed enabled value **1.15**, session counts
1, 2, and >=3 yield multipliers **1.00, 1.10, and 1.15** respectively, using the
concave saturation curve `1 + (max-1)*n*(5-n)/6`, `n=min(max(count,1),3)-1`.
It multiplies the secondary term inside `relevance * (0.7 + 0.3 * secondary)`;
it is not a 15% multiplier on the final score. The shipped default remains **0**:
the frozen evaluation family initially showed no memories with two identifiable
sessions, so it cannot establish ranking benefit or independent-writer diversity.
Session independence is a provenance proxy, not proof that authors reasoned
independently; distillation across separate sessions still needs a writer audit.

The experiment
uses an isolated copy selected by `scripts/eval-replica.sh`, never the live mind.
The before/after arms are the same compiled binary with max 0 / 1.15; golden
scoring imports `hooks/grade-recall.py` through `benchmarks/noise.py::golden_runs`,
with `CHITTA_EVAL_SOCKET`, hybrid strategy, depth 20, no reranker, three runs each.
This also avoids the grader CLI's optional gap-memory writes. The fixed
`python3 benchmarks/noise.py band golden.ndcg` threshold is **0.0017901251267712533**
(2 SD; reference mean 0.4927195290508894, SD 0.0008950625633856266).

Snapshot `bbcaed33`, manifest generation 38047, sequence 206208172. Offline audit
of a separate copy: **133,712 live memories; 1,236 with an identifiable session;
0 with >=2 replications**. There is no replicated cohort on which to test writer
dominance. Historical unknowns remain neutral; no writer label is converted into
a synthetic session. The top 20 are tied at the neutral floor (ascending numeric
ID breaks ties), not twenty demonstrated independent observations.

| Memory ID | Replications | Kind |
| --- | ---: | --- |
| 7890705326276612 | 1 | wisdom |
| 7890705326276613 | 1 | insight |
| 7890705326276629 | 1 | wisdom |
| 7890705326276649 | 1 | goal |
| 7890705326276651 | 1 | goal |
| 7890705326276674 | 1 | wisdom |
| 7890705326276677 | 1 | wisdom |
| 7890705326276680 | 1 | wisdom |
| 7890705326276693 | 1 | episode |
| 7890705326276695 | 1 | episode |
| 7890705326276696 | 1 | episode |
| 7890705326276698 | 1 | result |
| 7890705326276700 | 1 | task |
| 7890705326276712 | 1 | wisdom |
| 7890705326276726 | 1 | wisdom |
| 7890705326276729 | 1 | wisdom |
| 7890705326276730 | 1 | episode |
| 7890705326276749 | 1 | belief |
| 7890705326276757 | 1 | wisdom |
| 7890705326276759 | 1 | belief |

| Arm | Run 1 nDCG@20 | Run 2 | Run 3 | Mean | Sample SD |
| --- | ---: | ---: | ---: | ---: | ---: |
| Before (`replication_max=0`) | 0.5000783626 | 0.4999525031 | 0.4999525031 | 0.4999944563 | 0.0000726650 |
| After (`replication_max=1.15`) | 0.5000783626 | 0.4999525031 | 0.4999525031 | 0.4999944563 | 0.0000726650 |

Mean delta **0.0000000000**, within the fixed **0.0017901251** noise threshold.
The paired samples are identical. The current before mean differs from the older
noise calibration mean; acceptance uses the contemporaneous arm delta, not that
absolute difference. This is a compatibility result, not evidence of a recall
gain: the enabled arm has no eligible replicated memories. Keep default **off**
and set the proposal's `prior_effort` to `some`. No writer-dominance claim is
possible until a nonempty replicated cohort exists.

Restart identity: **20/20 nonempty queries** retained identical ordered result IDs
(depth 20, first 20 frozen golden queries). Each capture followed the same three
complete golden passes and a 30-second startup wait. The first capture used max 0;
the final binary's restart had `CHITTA_REPLICATION_MAX` **unset**, exercising the
shipped zero default. Its three verification warmup scores also matched the table.
The launcher recopied the same selected family for each arm/start; the Rust
regression tests separately exercise WAL-only and saved-snapshot recovery of new
replication evidence. All private daemons were stopped after measurement.

| Gate | Result |
| --- | --- |
| Rust `build.sh build --release` | Passed; final source |
| Rust `build.sh test --release` | Passed on final source: 282 passed, 2 ignored |
| Embedding identity | Compiled constants match unchanged reference `768:nomic-embed-text-v1.5` |
| CMake release build + ctest | 16/16 passed, matching GGUF supplied for embed-pool integration |
| Recall identity across restarts | 20/20 ordered ID lists identical |
| Hook suites | 18/18 passed; three rerun after clearing inherited overlong socket path |
| MCP Python suite | 126 passed, 1 existing SDK error: mcp 1.27.2 lacks `_session_owners` in `create_http_session_manager`; involved sources unchanged from HEAD |
| SMRITI Python suite | 46 passed |
| Immutable evaluation inputs / diff whitespace | Unchanged / passed |

An initial overlapping Cargo archive/audit build failed with a missing object;
subsequent build/test commands were serialized. The first metric invocation
refused to run without an exported private socket; only the corrected socket-bound
runs appear in the table. Neither failure is counted as a measurement.

<!-- BEGIN CITATIONS -->
## References

- <a id="ref-10"></a>**[10]** Pentti Kanerva. Hyperdimensional Computing: An Introduction to Computing in Distributed Representation with High-Dimensional Random Vectors. Cognitive Computation 1, 139–159 (2009). [source](<https://doi.org/10.1007/s12559-009-9009-8>)
<!-- END CITATIONS -->
