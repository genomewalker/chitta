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
