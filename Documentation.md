# Daemon task ledger — implementation and evidence

Status: 2026-09-13, implemented on `fix/ledger`; gates passed. No deployment,
service operations, live daemon writes, or push. Scratch daemons used private
`/tmp/chitta-ledger-*` homes, runtime directories and minds, and were stopped.

## Per-constraint report

| Constraint | Implementation and evidence |
|---|---|
| 1. One durable store | `chitta/include/chitta/task_ledger.hpp:148` validates and persists each complete row-change batch before publishing it. `chitta/src/handlers/field_task_ledger.cpp:14` emits the existing `ledger/task_records` MsgEvent WAL operation. The optional V23 `ledger_session_events` section is saved at `chitta-field/src/snapshot.rs:1283`, captured at `chitta-field/src/store.rs:8838`, and restored at `chitta-field/src/field.rs:718`. No WAL opcode or old positional snapshot layout changes. Startup rebuild is at `chitta/src/handlers/field_task_ledger.cpp:5`. Forced-kill recovery passed for WAL-only, snapshot plus WAL suffix, and queued lifecycle. Snapshot compaction deleted one covered WAL segment before restart. |
| 2. Same Python API, fail-open | All 22 original public function signatures are unchanged; `connect()` and the schema are removed. Wrappers and pagination: `chitta-mcp/task_ledger.py:22`. `chitta-mcp/daemon_client.py:233` reuses transports extracted from server.py without MCP imports, process spawning, SQLite fallback or ambiguous-write retries. Hook waits are 75 ms per RPC / 100 ms cumulative per registry operation (`daemon_client.py:219`); an HTTP worker also bounds DNS/body stalls. Missing/warming daemons yield empty/None/false results and `registered:false`. Regression cases: `chitta-mcp/tests/test_task_ledger_client.py:40`. |
| 3. Minimal RPC and bounded reads | One new RPC, `ledger_op`, is in `chitta/src/rpc_server.cpp:698` and the daemon registration table. Indexed current-row maps are in `chitta/include/chitta/task_ledger.hpp:17`; equality predicates select indexed candidates, glob filters inspect candidate artifact rows, and lineage follows primary keys. No request scans event history. Lists use the central work-policy clamp (`field_task_ledger.cpp:20`) and pages of at most 100. Reads take the shared RPC lock (`field_handler.hpp:549`); mutation batches take its exclusive lock. Legacy Python limits/unlimited lists follow pages. |
| 4. Sessions and queue | `chitta-mcp/session_registry.py:168`, `:292`, `:328` call native registration/heartbeat/deregistration RPCs. Realm detection stays local to the hook's environment/config/git root (`session_registry.py:145`), preserving the old client-side CLI behavior. Native handlers update binding and lease records (`chitta/src/handlers/field_session.cpp:897`, `:917`, `:1001`). Queue processing shares those handlers under its existing RPC lock (`chitta/src/queue_processor.cpp:593`) without recursively acquiring it. Queued heartbeat makes no RPC, then queue consumption renews the binding and lease; queued deregistration closes the binding and removes the lease. Native `session_list` survives snapshot restart. |
| 5. Idempotent migration | `chitta-mcp/task_ledger.py:204` implements `migrate --from`, using SQLite URI `mode=ro`, `query_only`, a consistent read transaction and explicit close. `TaskLedger::run` import (`task_ledger.hpp:389`) inserts by primary key only if absent. Every original row value is retained; reruns cannot overwrite newer daemon values. The integration test executes the actual CLI twice (`task_ledger_integration_test.py:163`) against a COPY of the real database and compares every field. |
| 6. Tests | The inference/selection tests use `chitta-mcp/tests/fake_ledger.py:12`, a dict-backed RPC fake. Exactly one real-daemon ledger integration test is registered in CTest: `chitta/src/task_ledger_integration_test.py:24`; it begins with `CHITTA_NO_QUEUE=1`, and enables only its private queue in the queue phase. `chitta/src/task_ledger_test.cpp` covers failed persistence, invalid import rejection, index maintenance, atomic lease batches and replay. `chitta-field/src/ffi.rs:7305` checks snapshot/WAL suffix and exact floating-point timestamps. |
| 7. Docs and cleanup | `docs/HOOKS.md:161` has a dated daemon-ledger section, endpoint configuration, fail-open/queue behavior and migration command. The old ledger environment variable no longer influences production code. SQLite is imported only inside the explicit migration function. |

## RPC list and wire shapes

New tool: `ledger_op`, arguments `{"op": "...", "args": {...}}`.
The daemon structured response is `{"value": <operation result>}`. This internal
envelope lets the client preserve scalar, boolean and null results as well as rows.
List operations return `{"rows": [...], "after": <cursor-or-null>}` inside that
envelope; the Python API returns its original plain list. Lineage pages use
`{"rows": [...], "next_id": "..."}` and a client cycle guard.

Operations:

- `thread_create`, `thread_get`, `thread_list`, `thread_update`, `thread_seal`
- `session_bind`, `session_get`, `session_list`, `session_touch`, `session_close`
- `lease_claim`, `lease_release`, `lease_list`
- `inbox_push`, `inbox_list`, `inbox_ack`
- `artifact_register`, `artifact_list`, `artifact_link`, `artifact_lineage`
- `import`, `counts`

Existing RPCs retained: `session_register`, `session_heartbeat`,
`session_deregister`, `session_list`; transcript registration still uses the
existing transcript RPC. Renderers remain local formatting functions.

## Python JSON key-set diffs

`chitta-mcp/tests/ledger_contract.json` freezes the original SQLite implementation's
22 signatures and all row/lease-result keys. The integration test checks them
against the daemon-backed implementation. Signature differences: **none**.

| Functions | Return shape | Added keys | Removed keys |
|---|---|---|---|
| thread_get, thread_list | thread row / list | none | none |
| session_get, session_list | binding row / list | none | none |
| lease_list | lease rows | none | none |
| inbox_list | inbox rows | none | none |
| artifact_list, artifact_lineage | artifact rows | none | none |
| lease_claim (success and conflict) | result dict | none | none |
| thread_create, inbox_push, artifact_register | ID string (None offline) | n/a | n/a |
| thread_update, thread_seal, session_bind, session_touch, session_close, lease_release, inbox_ack, artifact_link | boolean | n/a | n/a |
| render_inbox, render_threads | text | n/a | n/a |

Exact unchanged key sets:

- **threads**: `created_at, last_active_at, metadata_json, parent_thread_id, realm, sealed_at, status, thread_id, title, topic_fingerprint`
- **thread_sessions**: `client, ended_at, last_active_at, metadata_json, project_dir, session_id, started_at, status, thread_id, transcript_path`
- **thread_leases**: `acquired_at, expires_at, generation, last_heartbeat_at, session_id, thread_id`
- **inbox**: `acked_at, created_at, delivered_at, delivery_state, digest, event_type, item_id, payload_json, target_realm, task_id, thread_id`
- **artifacts**: `artifact_id, created_at, kind, md5, metadata_json, mtime, parent_artifact_id, path, size, task_id, thread_id`
- **lease_claim success**: `claimed, expires_at, generation, session_id, thread_id`
- **lease_claim conflict**: `claimed, expires_at, generation, owner_session_id, reason, thread_id`

## Migration and restart proof

The copied source passed `PRAGMA integrity_check`. Its SHA-256 is
`42ba58c855aecf50f1607d18b00311730ff47b97b601bbcb3ba7c115b566de9a`.
Only that source file was copied/read; migration wrote solely to the scratch daemon.

| Table | Source / first import | Second import additions |
|---|---:|---:|
| threads | 53 | 0 |
| thread_sessions | 22 | 0 |
| thread_leases | 7 | 0 |
| inbox | 6 | 0 |
| artifacts | 803 | 0 |

The prompt expected 52 threads and 7 bindings; the actual copy already contained
53 and 22. All 891 rows were imported and compared field-for-field, including
metadata strings, nulls, states and fractional timestamps. The test then exercised
new create/bind/lease/inbox/artifact operations, pagination, mutation, lease
conflict/force/release, lineage cycles, direct native session lifecycle, and:

1. SIGKILL the owned daemon after acknowledged writes; start it against the same
   private path; all ledger rows compare equal.
2. Add 100 synthetic memories to satisfy the existing compaction safety guard;
   compact to a full snapshot and delete a covered WAL segment; write a suffix;
   SIGKILL/start; all ledger rows and native session_list survive.
3. Enable the private queue; enqueue heartbeat and deregistration; verify lease
   renewal/removal and binding liveness; SIGKILL/start and compare rows again.

The first restart attempt exposed 1-ULP timestamp drift in serde_json's default
float parser, with no missing records. `chitta-field/Cargo.toml:18` enables
`float_roundtrip`; the corrected binary passes exact equality. This is why the
store dependency feature is part of the change.

## Timings and gates

Seven runs per operation, actual session_registry.py CLI under PyPy 3.9.18,
including interpreter startup, against the scratch Unix socket. All calls returned
success; no queue fallback was counted as a successful direct operation.

| Operation | Median ms | Maximum ms |
|---|---:|---:|
| register | 131.57 | 143.05 |
| heartbeat | 121.83 | 136.03 |
| close | 124.29 | 142.66 |

All 21 measurements are below 150 ms. Initial medians were about 250 ms;
measured import costs motivated lazy hashing/UUID/HTTP/logging imports and a fast
path for canonical registry CLI arguments. argparse retains help/error handling.
Timings cover the isolated Unix socket fixture, not a loaded cluster or HTTP RTT.

- Rust release build passed; `./build.sh test --release --lib`: **257 passed,
  1 ignored, 0 failed**.
- Embedding identity: **768:nomic-embed-text-v1.5**. This fresh worktree initially
  had no target directory; its marker was seeded from the canonical build before
  building and remained byte-identical afterwards. The compiled configuration
  and both CMake/Rust dimensions are 768; the canonical marker is unchanged.
- C++ configured with the canonical CHITTA settings and compiler/BLAS/Python
  paths, substituting this worktree's chitta-field archive and build directories.
  Canonical dependency source trees were read; build outputs remained local.
  Final CTest: **15/15 passed** (the original 13 plus two ledger tests).
- Python unittest discovery: **117 passed**; SMRITI: **46 passed**.
- PyPy 3.9.18 imports passed for daemon_client, task_ledger, session_registry,
  thread_inference, resume_selector, resume_capsule and poller.
- Ruff check and format check passed across chitta-mcp, hooks and the integration
  test (**63 files formatted**).
- `bash -n` passed for hook and hook-test shell files; all **11 shell test scripts**
  passed. Two initial failures came from outer harness overrides masking the
  tests' own fixtures; rechecking without those overrides passed.
- `git diff --check` passed in both repositories.

Machine-readable, sanitized counts/restart results and all timing samples are in
`docs/ledger-migration-evidence.json`. Raw build/test logs and the private database
copy are retained only under the ignored `chitta/build/ledger-evidence/` directory.

## Review boundaries

No live migration or deployment was performed. Historical ledger event batches
remain in the snapshot section; startup replay cost grows with retained ledger
history, while request reads use indexed current records. List pagination is a
sequence of reads, so concurrent updates can change the view between pages.
The public API retains its prior signatures and row keys; internal RPC paging and
envelopes are hidden by the client.
