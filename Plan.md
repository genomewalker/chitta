# Evolve selection and verification — 2026-09-14

Scope: fix/evolve-select in this worktree only; evolve package/tests, EVOLVE.md,
and this decision log. No deployment, live writes, pushes, or agent delegation.

Decisions:
- Normalize legacy cards to metric_only/none; derive internal evidence from
  explicit evidence provenance (empty/unknown evidence receives no bonus).
- Retain UCB shape, weight utility by V/P/I, and exclude exhausted candidates
  from exploitation. Survey candidates obey the existing hypothesis quota.
- History is an effort floor, matched by normalized mechanism (legacy ID fallback).
  Refuted resolutions count; compile failures alone do not refute a mechanism.
  Null-choice surveys record one effort bump per abandoned candidate.
- Survey runs read-only in the isolated pinned-base worktree before bet/spec,
  capped at min(survey-minutes, remaining cycle budget); strict JSON validates
  choice, complete abandonment accounting, reasons, and finite tractability.
- SELF_CHECK names a test in the touched module's tests directory; verify its
  definition/body intersects committed diff. Missing proof overrides to
  inconclusive:no_self_check. Existing measurement verdict rules stay intact.
- Existing paper cards need source=hypothesis and numeric blast_radius repairs.
  Query rewriting: metric_only/none, radius 2; typed preference graph:
  metric_only/none, radius 3; slot uniqueness: self_verifying/none, radius 3.
  All use external evidence. Preserve original path descriptions as cost.scope;
  normalize percentage-point gains to fractional units (2 points = 0.02).

Completed:
- Schema/defaults, V/P/I scoring, mechanism-level history floor and quota-safe
  shortlist; all three existing paper cards now normalize successfully.
- Bounded strict-JSON survey for both implementers; null choices bump effort;
  malformed/time-limited/dirty surveys cannot implement. Local skip evidence
  survives failed memory writes.
- Committed test-body matching for Python, native and shell SELF_CHECK IDs;
  missing checks override favorable replica deltas and prevent publication.
- Documentation updated, including native test names and structural-check limits.

Review decisions: one survey counts at most once per mechanism even if multiple
candidate titles alias it. Empty/unknown evidence is not internal evidence.
Native tests can use ordinary C++ functions without a new test framework.
A failed verdict memory write must preserve the original local skip artifact.

Final gates (2026-09-14):
- Required bioinfo Python unittest discover tests: 130 tests, OK (10.074s).
- Ruff check chitta-mcp: all checks passed; format --check: 52 files formatted.
- python3 -m evolve.selector --dry-run: exit 0, per-proposal V/P/I printed.
  Top: MDL evidence coverage (small), score 0.87455, V=.70 P=1 I=1.2.
  Paper cards: query rewriting 0.83290 (.70/1/1), slot uniqueness 0.83278
  (1/1/1), preference graph 0.83267 (.70/1/1). All effective effort none.
- bash scripts/evolve-cycle.sh --dry-run: exit 0; SURVEY and 'Survey time limit:
  20.0 minutes, included in the total cycle budget.' Printed survey→choose/skip→
  bet→spec→implement→SELF_CHECK→gates→replica→verdict plan; no spec generated.
- AGENTS gates: all 12 hook scripts pass; SMRITI 46 tests OK; no shell edits.
  Hook session-card fixture initially collided on the harness's shared temporary
  socket (Errno 98). Removing the outer socket override for that self-isolating
  fixture passed all six cases; no hook changes. Expected fixture FAIL text in
  SMRITI output is not a failing unit test.
- git diff --check passed; only authorized evolve/tests/docs and Plan.md changed.

Full local gate logs: /tmp/evolve-select-full-tests.log,
/tmp/evolve-select-selector-dry-run.log, /tmp/evolve-select-cycle-dry-run.log,
/tmp/evolve-select-hook-tests.log, /tmp/evolve-select-session-cards.log,
/tmp/evolve-select-smriti-tests.log. No deployment, push or live writes.

Implementation notes: initial fixture expectation treated differently titled
cards with the same mechanism as independent effort; corrected to mechanism-level
history. A detected rewrite error was corrected before tests. Ruff is in the
bioinfo environment rather than PATH; all lint findings were fixed.

---

# Daemon task ledger — 2026-09-13

Scope: replace SQLite on fix/ledger, only in this worktree. No deployment,
installed files, live writes or pushes. Commit after gates pass.

## Decisions

- One ledger_op RPC, {op,args}; preserve Python signatures and row keys.
- Atomic row-change batches in existing ledger/task_records MsgEvent WAL events.
  Indexed C++ tables rebuilt once at startup; no event scans on requests.
- Backward-compatible V23 snapshot section for task-record and session events.
  Existing full snapshots omit these registries but skip covered WAL operations.
  Restore the section before WAL suffix replay. No new WAL opcode.
- Shared session lifecycle for direct and queued RPCs updates bindings/leases.
- Extract stdlib socket/HTTP clients from server.py. Hooks: bounded deadline,
  no daemon spawning, ambiguous-write retries or SQLite fallback.
- Migration: SQLite URI mode=ro, primary-key insert-if-absent, preserve row values.
- Dict-backed fake for fast tests; scratch integration for all tables, contention,
  WAL/snapshot restart, queue lifecycle, migration and hook timings.

## Gates

- [x] Read constraints, clean fix/ledger worktree, inspect APIs/persistence.
- [x] Implement persistence, handler, client and migration.
- [x] Update tests/docs; capture old signatures and JSON key sets.
- [x] Rust build/tests, embedding identity; C++ build/ctest inside worktree.
- [x] Python suites, PyPy imports, Ruff, hook shell tests and SMRITI.
- [x] Real-copy migration, restart and timings; record evidence.
- [x] Final commit gate passed; Rust submodule committed as c43639c. Root commit includes this completed plan.

Sandbox launcher lacks bubblewrap; approved commands are used with all writes
restricted to this worktree and scratch.

## Findings and validation status

- Real source COPY has 53 threads, 22 bindings, 7 leases, 6 inbox, 803 artifacts
  (prompt expected 52/7/6/803). Migration + rerun + initial full row equality pass.
- First crash check found no missing/extra rows but 1-ULP timestamp changes from
  serde_json parsing; enabled float_roundtrip and added an exact timestamp test.
  Corrected WAL/snapshot/queue restart proofs now preserve every value.
- Final Rust suite: 257 passed, 1 ignored. C++ build and all 15 CTests passed.
- Python 117 passed; SMRITI 46 passed; PyPy 3.9 hook imports passed.
- Shell suite passed after correcting harness overrides that masked two fixture
  paths (test_registry_call and test_session_start_cards). No shell source edits.
- New client test isolates HOME as well as endpoint. All integration daemons use
  private /tmp state, no queue initially; queue phase enables only private queue.

## Final evidence

See Documentation.md and docs/ledger-migration-evidence.json. Exact migration
and WAL/snapshot/queue restart comparisons pass. serde_json float_roundtrip fixes
the timestamp mismatch. Final gates: 15 CTests, 257 Rust tests (+1 ignored), 117
Python tests, 46 SMRITI tests, 11 shell scripts, Ruff and PyPy imports. All 21
PyPy lifecycle timings are below 150 ms (medians 131.57/121.83/124.29 ms).
