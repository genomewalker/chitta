# Phase 7 recovery fixes

Worktree: `feat/chaos-tests`, continuing `528e2a1c` and `ea61ed39`.

## WAL segment recovery

`OpLog::recover_segment` reads the accepted segment prefix through the open descriptor in 64 KiB chunks. Newly created and reopened WAL descriptors now permit reading. Recovery preserves the segment header, first sequence number, filename and chain tip. It fsyncs a temporary replacement, renames it over the missing segment, and fsyncs the segment directory before accepting more writes. The recovery log reports the copied byte count. A failed append's partial record and remaining writer buffer are excluded; torn-tail replay remains unchanged. Unreadable descriptors or failed durability operations return errors rather than acknowledging discarded history.

Rust proof: five pre-unlink records plus seven later records survive replay, including a reopened writer descriptor; a store-level test checks all twelve distinct memory IDs and contents without snapshot coverage. The daemon harness also checks a WAL-only pre-unlink memory as well as post-unlink writes. The existing `read_path_tests::append_recovers_when_the_segment_is_deleted_underneath` regression now requires the preserved prefix instead of explicitly expecting its loss.

## Queue applied-ack ledger

Each lane stores JSON ack IDs in `<queue>.applied-acks`, independently of snapshots and WAL compaction. A successful checked `cf_sync` precedes each ack publication; publication writes a temporary file, fsyncs it, renames it and fsyncs its parent. Recovery filters applied IDs before requeueing, and dispatch filters duplicate IDs in the current batch. Coalesced saves also retain their terminal ack decisions so recovery cannot resurrect a superseded save. Ledger errors pause the lane with `.processing` retained. A nonempty queue claim prunes the ledger to IDs also present in the incoming batch; empty claims preserve the most recently completed batch. Slow-lane handoff is synced under the existing lane mutex before recording its fast-lane ack.

This covers the tested crash after durable application/ack publication but before `.processing` removal, including a stale processing file after snapshot compaction. It is not a transaction across store and filesystem: a crash between store mutation/sync and ledger publication can still replay that unacknowledged item. Old IDs are deliberately forgotten when a later batch rotates; producers must not reuse IDs across that retention boundary. Entries without ack IDs retain the existing checkpoint behavior.

Queue path selection, constructor paths, rename/claim placement, and daemon configuration are unchanged for the Phase 6 placement merge.

## Production functions touched

- Rust `log.rs`: `OpLog::open_with_chain`, `OpLog::append` (diagnostic), `OpLog::sync` (diagnostic), `OpLog::recover_segment`, `create_segment_v3`. `segment_vanished` and torn-tail replay are unchanged.
- C++ `queue_processor.cpp`: `QueueProcessor::recover_processing`, `QueueProcessor::run` (including `flush_slow` and its local `LockProfGuard` destructor), `QueueProcessor::run_slow`.
- New local C++ ledger helpers: `ack_sync_path`, `ack_replace`, `queue_ack_id`, `AppliedAcks::{AppliedAcks,contains,persist,rotate,record}`; `QueueAckError` distinguishes durability failures from item failures.

## Additional diagnostic

An added WAL-only restart control changes observe confidence from 0.8 to 1.0 with **no pending or processing queue file**. The same change occurred with recovery logging zero requeued items and one skipped ack. `put_memory` initializes confidence in memory, whereas WAL `PutPayload` replay constructs `MemoryState::new` with default confidence 1.0. This separate initial-state persistence issue is outside the two specified fixes. The snapshot-based queue proof checks exact identity and state. Since `get` reports wall-time-decayed strength, the native fixture disables decay for just that memory using the existing `cf_update_state` API while the scratch daemon is stopped. It then captures state before fault injection; equality has no numeric tolerance. A full-copy intermediate failure differing only by one strength ULP is retained as evidence of this clock-dependent oracle. The diagnostic failure and no-requeue control will be retained with the evidence.

## Validation

Rust library suite: 289 passed, 2 ignored (53.26 s). CTest: 25/25 passed (100.33 s), using the real local embedding model for the format probe. MCP: 149 passed. SMRITI: 46 passed. All 21 hook scripts and touched-Python Ruff passed. The final full-copy harness passed all nine cases. WAL recovery: 11.868 s, 19,622 bytes restored; queue replay: 11.472 / 12.354 s with exact identity/state and ledger pruning verified. See `docs/FIELD_PERF.md` and `docs/chaos-2026-09-16.json` for the complete table and retained failures. Production commit: `b4c06294`; Rust submodule commit: `765b68e`. No service changes, installation, push, live daemon fault, or external worktree changes.
