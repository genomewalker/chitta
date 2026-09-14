# Evaluation trust layer

Status as of 2026-09-13

The evolution loop is propose → implement in an isolated worktree → gates →
evaluate against a frozen-snapshot replica → verdict → human merge. Evaluation
changes need a separate, explicit approval. Nothing here starts a daemon, merges,
or publishes a candidate.

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

