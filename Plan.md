# Automatic-learning harness

Scope: benchmarks/learning/**, scripts/eval-learning.sh, one EVALS status line, Unreleased changelog. No official cohort freeze or model trials.

Decisions
- Follow committed 2026-09-15 memo: prospective twenty-task panel, three paired trials, provenance-based cohort, fail closed.
- Use existing newline JSON-RPC read endpoints for provenance, retaining per-ID evidence. No daemon changes.
- Task snapshots contain only the initial git tree, rebuilt in an isolated repository/worktree; graders remain outside agent-visible files until execution ends.
- Source family selection/copy follows existing eval-replica launcher; all writes target new private scratch instances.
- Real model runs require OS-enforced filesystem/network isolation; missing isolation cannot yield a verdict. Dry fixture evidence is always NO VERDICT.

Progress
- Read canonical constraints, decision memo, replica launcher, native/queue writers, hook telemetry and RPC schemas.
- Host shell sandbox intermittently fails (missing bwrap); explicit /usr/bin/bwrap exists but namespace creation currently fails ENOSPC. Record as evidence; never weaken official-run isolation.

- Implemented common primitives, provenance/task freeze, runner, restricted RPC broker, OS sandbox guard, hook capture, synthetic fixture preparation and operational protocol.
- Live read-only diagnostic: 15,051 records; native learning 1,867; queue distillation 0; value facts 7,380; preserved corrections/episodes 1,288; other source 209; unresolved 4,307. Original diagnostic stays in /tmp; committed evidence is a content-free count/hash summary.
- Verified daemon pagination caps at 100; enumerate until empty. memory_provenance can emit invalid JSON; optional errors retained, exact subject/parent evidence remains required.
- Fixed truncated-hook-ID parsing against actual captures; only complete ID headers count. Hook ledger can contain a full ID while context ends with its truncated duplicate.
- All 19 new tests, 127 MCP tests, 46 SMRITI tests pass. All 18 unchanged hook shell tests pass after rerunning cards with a fresh socket (shared test override initially caused EADDRINUSE). ShellCheck 0.11.0 unpacked in /tmp only; no install.
- Final two-task/two-trial dry run uses scratch source /tmp/learning-fixture-915b and results/fixture-2026-09-15. No official freeze or Claude -p invocation.

- Final dry-run complete: eight of eight outcomes, no execution/telemetry failures; saffron A2/2 B0/2, cobalt A1/2 B1/2, delta +1. Source family unchanged; A has confirmed cohort exposure and B zero. Report regenerates from hashed task/cohort/event artifacts.
- Verdict is intentionally NO VERDICT (fixture, 2x2 panel, OS sandbox unavailable). Real model path is fail-closed and has not been exercised with a model call.

Completed: committed as 15882c02 on feat/learning-experiment. Post-commit eval-immutable check passes. Working tree contains only this requested untracked Plan.md. Orchestrator owns classification resolution, official cohort freeze and model experiment.

## Follow-up 2026-09-15
Authorized: home-audit screening mode, ambiguous exclusion from both arms,
unexpected native kinds included, external results and compact fixture evidence.
No deployments or live writes. Commit when all existing gates and new tests pass.
- Implementing config/env isolation, shadow/history audit and missing-trial scoring.
- Provenance metadata string-ID bug: use lossless JSON integer for metadata only;
  retain exact decimal-string graph queries and validate metadata ID.
- Implemented and tested home-audit mode, conservative shell audit, immutable
  audit controls, ambiguous exclusion and missing-outcome scoring.
- Final read-only diagnostic: 15,300 records, ambiguous 4,288, included 1,897,
  unexpected native kinds alias 11 / signal 10, contradictory 0. All metadata
  queries now succeed. Live store drift makes the diagnostic ineligible for cut.
- Gates pass: 31 harness, 18 hook, 127 MCP, 46 SMRITI; shell/ruff/immutability.
- Home-audit fixture running from /tmp/lrn-ha-915a/final-frozen, using default
  external output /projects/caeg/scratch/kbd606/tmp/learning-results/run-1789492840729.
  One deliberately forbidden Read: saffron trial 2 A. No model calls.
- Dry-run completed: 8 outcomes, 7 valid, 1 voided; report reproduces exactly,
  source unchanged. Saffron A1/1 observed +1 missing / B0/2; cobalt A1/2 / B1/2.
  The voided trial also retained a hook-output/ledger mismatch; no nonvoided
  errors. Compact fixture evidence records this limitation. Overall delta null.

## Follow-up 3 — 2026-09-15
Authorized: prospective post-cut automatic cohort per task, retain all pre-cut
baseline, hard-error unlabelled post-cut writers, remove future records in both
arms, per-task manifests/exposure, read-only diagnostic from midnight UTC.
Native derived operational value facts now included per the specified :326 writer.
No hook changes, live writes, deployments, model calls or push.
Implementation in progress; rerun harness and existing gates before commit.
- Completed prospective schema-2 cut/task freeze, per-task runner exclusions,
  global-fallback all-store inventory, per-task exposure and manifest validation.
- Labelled bash source=distillation plus derived_from is consistently automatic;
  the orchestrator's hook change needs no harness exemption.
- Final gates: 37 harness, 18 hook scripts, 127 MCP, 46 SMRITI, Ruff, shell checks.
  Immutable gate passes against b03a25e5 (stream base); main-only replica script
  updates make a direct current-main comparison fail. No protected paths edited.
- Live project:cc-soul diagnostic: 269 automatic (19 native learning, 250 native
  value facts), zero queue-labelled, 2 unlabelled wisdom, no contradictions.
  Enumeration drift: diagnostic only. Separate all-store live scan also drifted.
- Scratch fixture: 8 outcomes, 7 valid, 1 rejected for existing truncated baseline
  ID versus ledger mismatch. All exclusions pass, source unchanged, report exact.
  No model calls or learning verdict. Ready to commit on feat/learning-experiment.
