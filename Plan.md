# Read-path locks — 2026-09-14

Branch: fix/read-path-locks. Work only in this worktree; no deployment or live writes.

## Decisions
- Baseline span_for_memory already uses only span_store.read(); preserve it and test concurrent readers.
- Defer access bookkeeping into a short mutex accumulator; maintenance drains one batched state operation, preserving counts and timestamps. Drain at snapshot and shutdown.
- WAL append flushes to OS; background timer syncs pending writes (200 ms default); explicit durable boundaries remain synchronous. Expose pending and sync counters.
- Turbo builds from an off-lock embedding snapshot on maintenance; readers clone an Arc to the previous index. Retain visibility of changed/new embeddings until rebuild.
- FFI owns a stoppable maintenance worker so no C++ lifecycle/signature change is needed.

## Gates
1. Build unmodified Rust/C++ in this worktree, pinned identity 768:nomic-embed-text-v1.5.
2. Copy selected eval replica family into private scratch dirs; baseline concurrency 1/6/12, paired hook benchmark N=5, stacks, JSON schemas.
3. Implement and test touches, timer/durable WAL, replay/crash tail, Turbo concurrency, span read lock.
4. Rust release tests; C++ ctest; required hook/MCP/SMRITI tests using isolated state.
5. Repeat scratch measurements, inspect zero recall-side fsyncs and 12-call median target <1200 ms.
6. Document evidence and limitations; commit submodule then superproject only after gates pass.

## Progress
- Constraints read; clean worktree confirmed. No baseline build artifacts exist here.
- Shell sandbox unavailable (missing bwrap); approved shell execution used within scope.

- Baseline built in chitta/build-read-path after resolving compiler-cache resets and using local dependency sources. Baseline binaries and library preserved under .read-path/.
- Short /tmp/chitta-read-path-UID symlink points into worktree runtime (Unix socket pathname exceeded its limit). All scratch daemon state remains in worktree.
- Baseline (3 rounds each) lane total medians: 1=1044 ms, 6=1233.5 ms, 12=1644.5 ms; stacks fdatasync=1/3/1, ensure_turbo=3/0/4, shared lock stalls=0.
- Baseline unchanged hook benchmark N=5 per query: off median/p95=1113/6031 ms, on=1210/2094 ms, 0 empty each.
- Additive UpdateStateBatch bypasses explicit-state timestamp deduplication; replay coverage ensures exactly-once batches. Original timestamps/counts retained.
- WAL has a separate timer worker so Turbo maintenance cannot delay group sync. Both are joined before FFI shutdown.
- Rust tests encountered implicit Arc autoref error (fixed), then missing intermediate archive object; rerunning serially.

- Preliminary test artifact: 262 passed, 0 failed, 2 ignored (43.85 s), including new touch/concurrency/SIGKILL tests. Final source has additional Turbo/FFI tests and must be revalidated.
- Required hooks now pass after removing inherited CHITTA_SOCKET_PATH for socket-owning fixtures. MCP 130/130 passes with already-installed user-site SDK 1.27.2 exposed explicitly under isolated HOME; conda SDK 1.27.0 lacks _session_owners. SMRITI 46/46 passes.
- Completed touch drains explicitly sync once off the read path, so the accepted nominal five-second crash window does not gain another WAL timer interval. Pending timer writes still group at 200 ms.
- Plasticity previews only affected histories; WAL failure leaves the learner unchanged. A published Turbo index filters stale vector rows before filling top-k, then merges current scores.
- Baseline scratch process received SIGTERM after measurements; its existing shutdown watchdog forced exit. No live process was signalled.

- Final frozen Rust source passes full release test command: 265 passed, 0 failed, 2 ignored; all binary/doc test targets pass. Final static library rebuilt from frozen source. Source hash verification reports no changes since freeze.
- C++ rebuild from final library is running; after-daemon measurements remain pending.

- Final C++ build passes. Full ctest: 16 pass, embed_pool_test initially skipped; targeted rerun with CHITTA_EMBED_MODEL passes, yielding all 17 tests passing.
- After lane total medians 1/6/12: 572/96.5/94.5 ms (before 1044/1233.5/1644.5). Target <1200 ms passes; each of the three after single calls beats its before counterpart.
- After hook N=5 per query: off median/p95 788/1803 ms; on 871/4268 ms; 0 empties. On-arm p95 regressed; recorded explicitly. Unchanged script masks per-hook exit status, so only runner success/empties are claimed.
- Original after stacks at 100 ms were idle (calls finished earlier). Replaced proof with three active samples under sustained twelve callers: shared waits 1/0/1 (isolated recall_semantic_ctx), fdatasync 0/0/0, ensure_turbo 0/0/0. 570 calls completed.
- strace across twelve recalls: zero recall-worker fdatasync; one chitta-maint and three chitta-wal syncs. Aggregate status counters include those background operations and are not per-call counts.
- All nested JSON key sets match for the five required recall APIs. Final report in Documentation.md and one dated metrics line in docs/HOOKS.md.
- Sent SIGTERM to final scratch PID after measurements. Final diff/whitespace/source-hash checks pass. Ready to commit submodule then superproject.

- Final scratch daemon shut down cleanly after SIGTERM (all workers stopped; no shutdown timeout). Submodule committed as f113ea8 on fix/read-path-locks.
