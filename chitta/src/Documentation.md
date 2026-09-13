# Daemon fix evidence

## Scope and behavior

1. Direct tag fallback already had an exact-realm guard at branch base. This
   stream enforces realm again after tag selection, makes unknown tags empty,
   and routes field-strategy candidates through tag selection. Ordinary untagged
   field responses and brahman/unscoped behavior retain their contracts.
   A read-only live query for sota-card/project:chitta-evolve returned 3 rows,
   all in that realm; the original live leak was not reproduced today.
2. Queue defaults are <mind>/queue.jsonl, with .slow and .failed siblings.
   CHITTA_QUEUE takes precedence over the legacy CHITTA_QUEUE_PATH alias;
   CHITTA_NO_QUEUE still disables consumption/recovery. Hooks derive the same
   default from MIND_PATH, then CHITTA_DB_PATH, then the normal home mind.
   Native queue_write also uses the resolver. Help documents node persistence.
3. Small MDL evidence retains source-chunk boundaries and extends with the most
   recent N prior chunks (default 3, CHITTA_MDL_POOL_CHUNKS, 0 disables, max 32).
   The pool survives ephemeral NativeDistiller objects, is keyed by mind,
   transcript, session and realm, and retains at most 64 keys/32KB per chunk.
   Overlapping/retried chunks are not counted twice. History is memory-only;
   restart/missing history leaves the unpooled judge. Large current evidence
   is unchanged. Pooling changes only shadow judgments, never storage or LLM
   input. Shadow lines report total evidence_bytes and prior pool_chunks.
   Daemon distillation now passes its mind path through to the shadow logger.

## Validation

- Rust build.sh build --release: passed (4m40s); generated identity remains
  768:nomic-embed-text-v1.5, matching the main build marker. This checkout's
  build.sh does not create target/release/.chitta-embed-identity; generated
  embed_config.rs was checked instead. No model/dimension setting changed.
- Rust test_tag_selection_realm_contract: 1 passed, 0 failed (release).
- C++ cmake build --parallel 8: passed, matching main-cache CHITTA settings,
  BLAS flags and compilers; CHITTA_FIELD_ROOT points to this worktree.
  Cached dependency sources were copied into the worktree before configuring.
- All 7 hooks/tests/*.sh suites passed in isolated test state; bash -n passed.
- MCP unittest suite: 99 passed. SMRITI unittest suite: 46 passed.
- Ruff: passed after fixing test import order.
- MDL synthetic: recurring fact single saving -29, pooled three-chunk saving
  +79 (accepted at unchanged margin 64); unrelated saving -58 (rejected).
  Isolation, retry/rewind, N/disable and large-evidence cases passed.
- Final ctest: 100% tests passed, 0 failed out of 13 (12.95 seconds). The
  original 12 tests plus daemon_isolation_test all passed. Earlier fixture
  failures (long Unix socket path, unsupported queued remember operation)
  were corrected to a short runtime directory and supported observe operation.
- Tag proof: 18 real-daemon scoped recalls across keyword/field/fused strategies,
  matching and nonmatching query text, and project:a/project:b/brahman each
  returned exactly one row in the requested realm. Unscoped tag recall preserved
  all three realms; absent tags and absent realms returned no rows.
- Queue proof: default, explicit CHITTA_QUEUE, legacy CHITTA_QUEUE_PATH and
  CHITTA_NO_QUEUE cases passed. Each active daemon processed exactly one private
  canary. Disabled daemon processed zero and left its canary on disk. strace
  file-operation traces contain zero accesses to /tmp/chitta-queue.jsonl or any
  recovery sibling in all four cases. Private fast/slow startup recovery yielded
  two malformed-input dead letters at <mind>/queue.jsonl.failed.
- Key-set diff (populated live and scratch samples, both top-level and every
  distinct result-row key set): recall [], smart_recall [], hybrid_recall [],
  recall_keyword []. All match. The latter two have no named CLI aliases;
  safe stdin JSON-RPC mode was used through the live CLI with no_learn=true.

## Local artifacts (ignored build directory)

- chitta/build/rust-build.log and rust-test.log
- chitta/build/configure-args.txt, configure.log, cpp-build-final.log
- chitta/build/ctest.log and Testing/Temporary/LastTest.log
- chitta/build/isolation-proof3/{default,explicit,legacy,disabled}.{log,trace}
- chitta/build/key-set-diff.txt and mdl-synthetic.log
- chitta/build/gate-*.log (ruff subsequently rerun clean after import ordering)

No deployment or live-state writes were performed. The store submodule change
is regression coverage for the existing exact-realm contract; no Rust ABI or
storage-format change was necessary.
