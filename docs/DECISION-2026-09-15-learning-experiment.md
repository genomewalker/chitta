# Decision 2026-09-15: automatic-learning experiment protocol

> Status as of 2026-09-15: **agreed between Fable (orchestrator) and Codex
> gpt-6-astra (consult)**. Supersedes the sketch in
> `DECISION-2026-09-15-mdl-analogy.md` section 1 and the pending block in
> `EVOLVE.md`. Nothing is built yet; branch `feat/learning-experiment`.

## Agreed protocol (Fable's seven points with Codex's amendments accepted)

1. **Tasks.** 20 self-contained prompts with deterministic graders, frozen
   before any recall is inspected, each with transcript sha256, timestamp,
   selection rule, dependencies and initial-state hashes. Graders must fail on
   the initial state and pass on a known-good state (pattern:
   `benchmarks/smriti/mine_tasks.py:212`). Prompts that need conversation
   context or uncommitted session state that cannot be reconstructed are
   excluded.
2. **Cohort.** The free-form automatic writers: queue `observe` rows with
   provenance `source=distillation` (`chitta/src/queue_processor.cpp:460`) and
   native-distiller learnings identified by `derived_from` provenance across
   all their kinds (`chitta/src/native_distiller.cpp:45,185`). Category alone
   never decides membership. Explicit memories, corrections, source episodes
   and the deterministic value facts (`native_distiller.cpp:316`) stay in both
   arms. Per-task eligible ID lists are frozen with evidence; an unresolved
   classification blocks the verdict.
3. **Arms.** One immutable, hashed source replica family; fresh A/B instances
   cloned per task and trial (`scripts/eval-replica.sh`), private HOME, DB,
   queue, runtime dir, socket and port, `CHITTA_REALM=project:cc-soul`
   pinned, identical binaries and hooks, utility recall OFF. Recall-side
   mutations are reset between trials; the harness verifies removed IDs cannot
   resurface through graph or correction lanes.
4. **Runner.** Headless Claude Code with model, version, turns, budget and
   timeout pinned; both headless aliases unset (`hooks/prompt-core.sh:5`);
   paired arm order randomised; **three trials for every task from the start**
   (120 runs), no outcome-dependent expansion. Capture hook output and each
   trial's `CHITTA_DB_PATH/outcome_ledger.jsonl` to verify cohort exposure in A
   and zero exposure in B; a genuinely empty recall is an outcome, not an
   error. Ledger command credit is never the grader.
5. **Leakage.** Creation-time filtering is necessary but not sufficient: later
   distillation strengthens older memories (`native_distiller.cpp:137`), so a
   task is valid only with recoverable pre-task content, weights and graph
   state. **Decision: collect the 20 tasks prospectively.** The cohort cut is
   the moment the harness freezes `cohort.json`; tasks are the next graded
   prompts from real sessions after that cut, so no historical reconstruction
   is needed. Runner isolation: no transcripts, no live MCP, no future git
   objects, graders installed only after execution.
6. **Decision rule.** Δ = Σ over tasks of (mean success A − mean success B)
   over three trials; retain automatic admission iff Δ ≥ 3 (nine net
   successes out of 60). Report every paired outcome, cohort and total
   injection counts, failures and empties, median and p95 recall and hook
   latency. Invalid isolation, provenance or telemetry yields no verdict. A
   valid failure retires every tested free-form writer, queue admission
   included, not only the native store call.
7. **Sequencing.** Build the harness now on `feat/learning-experiment`; run
   once 20 prospective tasks are frozen (expected late September). The utility
   flip (`EVALS.md` "Utility posteriors") stays a separate golden/SMRITI
   comparison, earliest 2026-09-21, and is not blocked by this experiment.

## Pre-registered secondary metrics (added 2026-09-15 after reading SwarmWorld, arXiv:2608.26081)

SwarmWorld found that shared societies beat isolated best-of-N search on
portfolio breadth and resilience while isolated search kept the best single
artifact, and that ~95% of first reuse came from observing an artifact rather
than being told about it. Two secondary metrics are therefore recorded and
reported, without changing the primary rule:

- **Best-of-three per arm.** For each task, max success over the three trials
  in A and in B. Hypothesis H2 (pre-registered): B's best-of-three is not worse
  than A's even when mean Δ favours A. If H2 holds, shared memory widens what
  gets done rather than raising the ceiling, and the orchestrator should keep
  using isolated parallel streams for "find the best" tasks.
- **Artifact reuse rate.** Fraction of arm-A trials in which the agent read or
  executed an artifact (script, file, command) that a pre-task memory named,
  measured from the trial's hook shadow log and Bash history. This tests
  whether the cohort transfers through artifacts (stigmergy) or through prose.

## Harness (minimal)

`benchmarks/learning/`: `protocol.md`, `tasks.json`, `cohort.json`,
`fixtures/`, `hidden/` (graders), `freeze.py` (extraction, cohort
classification, freeze validation), `runner.py` (isolation, replicas,
execution, grading, paired report), `tests/test_protocol.py` (leakage,
isolation, aggregation), `results/<run>/`; entry point
`scripts/eval-learning.sh` reusing the replica launcher.

Estimate: 16–24 engineering hours plus 6–20 serial execution hours.
Largest invalidation risk: future information retained in older memories or
shared state despite creation-time filtering; the prospective design is the
mitigation.

## Codex's memo (verbatim)

# Automatic-learning experiment protocol

1. **AMEND — task source.** Freeze 20 self-contained prompts, deterministic graders, transcript SHA-256, timestamps, selection rule, dependencies, and initial-state hashes before recall inspection. HEAD alone misses uncommitted session changes; reconstruct those or exclude the task. Require pre-fail/post-pass grader validation ([benchmarks/smriti/mine_tasks.py:212](benchmarks/smriti/mine_tasks.py:212)). Reject prompts requiring unavailable conversation context.

2. **AMEND — cohort.** Target free-form automatic admission, matching the proposed retirement. Queue provenance records `source=distillation` ([chitta/src/queue_processor.cpp:460](chitta/src/queue_processor.cpp:460)); native learning records `derived_from`, with kinds including belief/preference/milestone, not just wisdom ([chitta/src/native_distiller.cpp:45](chitta/src/native_distiller.cpp:45), [chitta/src/native_distiller.cpp:185](chitta/src/native_distiller.cpp:185)). Use writer evidence plus provenance; category alone cannot establish authorship. Preserve explicit memories, corrections, episodes, and deterministic value facts—the latter have a separate operational write ([chitta/src/native_distiller.cpp:316](chitta/src/native_distiller.cpp:316)). Freeze per-task eligible ID lists, evidence, and unresolved classifications; unresolved treatment membership blocks a verdict.

3. **AMEND — arms.** Clone one immutable, hashed source family into fresh A/B instances for every task/trial; never take two independent live snapshots. Replica startup already quiesces background work ([scripts/eval-replica.sh:285](scripts/eval-replica.sh:285)). Give each trial private HOME, DB, queue, runtime directory, socket, and port; pin `CHITTA_REALM=project:cc-soul`, identical binaries/hooks, and utility OFF. `CHITTA_NO_QUEUE` does not suppress hook enqueue writes ([hooks/lib.sh:264](hooks/lib.sh:264)). Reset recall mutations between trials; verify removed IDs cannot surface through graph/correction lanes.

4. **AMEND — runner.** Pin model/version, turns, budget, dependencies, and timeout explicitly; SMRITI’s command currently specifies only JSON output ([benchmarks/smriti/runner.py:332](benchmarks/smriti/runner.py:332)). Unset both headless aliases ([hooks/prompt-core.sh:5](hooks/prompt-core.sh:5)). Randomize paired arm order. Use three trials throughout: 120 runs, avoiding outcome-dependent expansion. Capture actual hook output plus each trial’s hook-written `CHITTA_DB_PATH/outcome_ledger.jsonl`; it is not daemon-owned ([hooks/outcome-ledger.sh:16](hooks/outcome-ledger.sh:16)). Verify telemetry completeness and A’s cohort exposure/B’s zero exposure; genuine empty recall remains an outcome. Ledger command credit is not the grader ([chitta-mcp/outcome_ledger.py:56](chitta-mcp/outcome_ledger.py:56)).

5. **AMEND — leakage.** Creation-time filtering is necessary but insufficient: later distillation strengthens older memories ([chitta/src/native_distiller.cpp:137](chitta/src/native_distiller.cpp:137)). Require recoverable pre-task content, weights, and graph state, across all accessible realms; otherwise use prospective tasks after freezing the cohort. Enforce filesystem/tool isolation from transcripts, live MCP, future Git objects, and hidden graders. Install grading checks only after execution ([benchmarks/smriti/runner.py:433](benchmarks/smriti/runner.py:433)).

6. **AMEND — decision.** Keep the ≥3/20 screening threshold ([docs/EVOLVE.md:64](docs/EVOLVE.md:64)). With three trials define Δ = Σtask(mean success A − mean success B); retain iff Δ≥3, equivalent to ≥9 net successes/60. Report all paired outcomes, cohort/total injection counts, failures/empties, and median/p95 recall and hook latency ([hooks/prompt-core.sh:158](hooks/prompt-core.sh:158)). Invalid isolation/provenance/telemetry yields no verdict. A valid failure retires the tested free-form writers, including queue admission if included—not merely the native write at [chitta/src/native_distiller.cpp:153](chitta/src/native_distiller.cpp:153).

7. **AMEND — sequencing.** Build on `feat/learning-experiment` now; target completion by September 20, conditional on historical reconstruction. Defer rather than weaken validity if prospective collection is necessary. Utility stays a separate comparison, earliest September 21 ([docs/EVALS.md:202](docs/EVALS.md:202)); existing noise calibration wraps `benchmarks/noise.py` ([scripts/eval-noise.sh:7](scripts/eval-noise.sh:7)), not this task panel.

## Minimal harness files

Under `benchmarks/learning/`:

- `protocol.md`: frozen selection, exclusions, scoring, validity rules.
- `tasks.json`, `cohort.json`: graders, initial-state references, provenance, hashes.
- `fixtures/`, `hidden/`: reproducible inputs and protected graders.
- `freeze.py`: extraction, cohort classification, freeze validation.
- `runner.py`: isolation, replicas, execution, grading, paired report.
- `tests/test_protocol.py`: leakage, isolation, aggregation checks.
- `results/<run>/`: manifests, raw events, per-task table.

Plus `scripts/eval-learning.sh`: entry point reusing the replica launcher.

**Estimate:** 16–24 engineering hours, plus 6–20 serial execution hours; historical-state recovery may extend this.

**Largest invalidation risk:** future information retained in older memories or shared state despite creation-time filtering.

## Amendment — 2026-09-15: prospective post-cut treatment (follow-up 3)

This amendment supersedes the historical-cohort and ambiguous-exclusion rules
above; the original decision and memo remain unchanged as history.

- `cohort` capture records only the cut timestamp and hashed store manifest.
  Every record created at or before the cut is baseline in both arms, with no
  historical writer classification.
- For each task T strictly after the cut, the treatment is records created in
  (cut,T] by queue/observe `source=distillation` or native-distiller
  `derived_from`. Combined source=distillation and derived_from lineage remains
  automatic (the labelled bash observe path can emit both). The requested native
  writer includes operational value facts
  at `native_distiller.cpp:326`, superseding their earlier exemption. Explicit
  remember/learn writes, corrections, [artifact]/[done] hook signals and episodes
  stay in both arms through T.
- Task freeze inspects the later immutable source through a quiescent probe;
  it retains the original cut manifest separately. Post-cut records without
  writer provenance are hard errors listed by ID and kind. Missing timestamps,
  contradictory evidence or unavailable required parent evidence also block
  freeze. No unlabelled population is dropped from both arms.
- Official task freeze inventories all store realms to cover global fallback
  and graph neighbors, while task recall remains pinned to project:cc-soul.
  Both arms remove all records created after T. B additionally removes only
  that task's automatic cohort. Exact-ID lookup, hybrid, graph and correction
  lane checks verify both exclusion sets. The task artifact and manifest pin
  per-task lists and timestamps, and exposure is checked per task: A must expose
  its cohort at least once, B never, and neither arm may expose future IDs.
- Timestamp filtering cannot undo later changes to older content, weights or
  graph state. Recoverable pre-task state remains required for a valid model
  verdict; creation-time exclusion does not certify that separate condition.
- Read-only live inspection of project:cc-soul since 2026-09-15T00:00Z found
  269 automatic records
  (19 native learnings, 250 native value facts, zero source=distillation rows)
  and two unlabelled wisdom records. Enumeration drift makes this diagnostic
  only. `hooks/distill.sh` omits `--source distillation` in this branch; fixing
  that writer belongs to the orchestrator on main. No hook edit is included.

Implementation and count-only diagnostic:
`benchmarks/learning/protocol.md` and
`benchmarks/learning/evidence/live-postcut-2026-09-15.json`.
