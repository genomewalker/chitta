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

## Merge onto the split store (2026-09-16)

Fetched submodule main `0e24c7d` and resolved the single `src/store.rs` conflict by retaining main's `mod tests;` declaration and moving `chaos_unlinked_wal_preserves_n_plus_m_memories` beside `put_test_memory` in `src/store/tests.rs`. The test is identical to `765b68e` except for removal of one indentation level.

The remaining hunks merge without conflict: descriptor recovery, readable WAL opens, recovery diagnostics and WAL regressions remain in `src/log.rs`; the preserved-prefix assertion remains in `src/read_path_tests.rs`; lock fencing and partial-snapshot tests remain in `src/field.rs::chaos_tests`. The two whole WAL/read-path files and the field test block were checked byte-for-byte against `765b68e`. Main's `src/field/opening.rs` phases, `src/ffi/` modules and production `src/store/` modules remain exactly as supplied by main.

The rerun exposed a harness teardown race: after the MCP invariant passed, reading `/proc/<owned-pid>/stat` could raise `ProcessLookupError` as the killed process disappeared. The termination predicate now accepts that alongside `FileNotFoundError`; fault injection and recovery assertions are unchanged. The first CTest run also skipped the embedding pool because its model environment variable was absent. With the cleanup fix and explicit real-model path, CTest passes all 25 tests without skips (96.73 s).

Merged validation: `./build.sh build --release` passed (5m40s), and the full `./build.sh test --release` command passed with 289 library tests, 2 ignored, no failures (53.64 s for the library suite). Native rebuild passed. Generated identity remains `nomic-embed-text-v1.5` / 768 / text format 1, and the rebuilt daemon's format ID remains `9230643459983636874`. Scratch-daemon contract snapshots are unchanged before and after the native rebuild. MCP 149/149, SMRITI 46/46, all 21 hook scripts, harness Ruff and canary shell syntax passed.

The full frozen-copy chaos harness passed 9/9. WAL restored 19,615 bytes and reopened in 11.282 s; queue replay reopened in 11.691 / 11.051 s with exact identity/state and pruning verified. Format probe maximum was 0.023 s. Full rerun report: `/tmp/p7-merge-full-final.json`; native rerun log: `/tmp/p7-merge-ctest-final.log`; Rust log: `/tmp/p7-merge-rust-test.log`. Harness scratch copies were removed on exit.

# Static site UX maintenance

The published artifact is docs/ from .github/workflows/pages.yml. This change
uses the existing static HTML/CSS structure; it does not deploy anything.

- `scripts/site_common.py` defines the menu, status date, footer, and version
  derived from the first numbered release in CHANGELOG.md.
- After menu/date/release changes, run `python3 scripts/sync-site-chrome.py`.
  Checked-in HTML keeps navigation usable without JavaScript.
- `docs/styles.css` retains the design tokens and owns responsive navigation,
  code overflow, and scrollable table regions. `docs/site-shell.css` loads last
  to keep shared controls consistent despite page-local styles.
- `docs/content-styles.css` holds deduplicated former static style attributes.
  Visualization scripts may still set runtime positions, colors, and visibility.
- Root-relative chrome/assets make the custom-domain 404 work at any URL depth.
  A site served under a path prefix would need its URLs adapted.
- Menu destinations mark themselves with `aria-current="page"`. Dream articles
  and 404 use their footer permalink because no menu item is that exact page.
- All pages have a focusable main landmark containing their h1. Tables have
  named, keyboard-scrollable regions. TOCs point to sections with real headings.
- The existing dark content palette is preserved. Shared chrome follows light
  OS preference; native controls use dark color-scheme on dark content.

Gates (no browser, network, service or third-party Python packages required):

```sh
python3 scripts/check-site.py
bash scripts/check-docs-links.sh
bash scripts/check-citations.sh
python3 -m unittest discover -s scripts/tests -p test_check_site.py
```

The legacy `python3 scripts/check-docs-links.py` entrypoint delegates to the same
recursive shell checker. It checks same-document and cross-page HTML anchors,
root-relative links, nested index pages, and percent-encoded URLs. Remote URLs
are excluded; successful local checks do not imply external-service health.

The per-page changes and exact checker table are recorded in
`docs/DOCS-AUDIT-2026-09-16.md`, under “Site UX pass”. CSS contracts and parsed
HTML do not prove visual layout, contrast, keyboard behavior in browsers,
WebGL rendering, or live backend connectivity.

After changing HTML line counts, run `bash scripts/check-citations.sh --write`
to refresh citation usage locations, then rerun the read-only citations gate.
The generated References blocks must remain intact inside the main landmark.

Superproject main merge: retained both sides of the add/add `Documentation.md` conflict and both the chaos and current-truth additions in `docs/EVALS.md`. `docs/FIELD_PERF.md` merged automatically with References last. Refreshed only citation usage line numbers in `docs/CITATIONS.md` after the merge shifted documentation lines. The merged link gate passed (102 pages, 3,110 links), citation gate passed, and site structure passed (79 pages). Main introduced no further native, harness or public-contract changes relative to the tested pointer-update commit.

# Phase 5b shell policy retirement — 2026-09-16

Step 1 makes `prompt_context` the only prompt retrieval/admission implementation.
Failed, invalid or late replies emit one unavailable line and exit successfully;
there is no batch/per-lane retry, shell fusion/admission or local CLI policy path.
Native-policy synthetic integration retains ablation, small-realm and query-gate
coverage. The failed-RPC and pipeline-timeout fixtures now expect the unavailable
line instead of legacy admission. Other shared lib helpers still have callers.

Verification: 70 adjacent unchanged/candidate output/status pairs passed on the
private frozen replica, including 40 warm-up pairs; native prompt execution and
lane accounting were required. All 28 shell tests, 159 MCP, 46 SMRITI, eight hook
Python tests, CI Ruff check/format, touched-shell syntax/ShellCheck and contracts
passed. Before controls preserved a first warm-up difference and intermittent
150 ms Stop compatibility-probe timeouts; the final control matched all bytes.
Step 2 must eliminate that probe fallback and require native ledger execution.
Evidence is untracked under `/tmp/chitta-p5b-retire/evidence`. Prompt shell:
1,937 → 1,274 lines; all top-level hooks: 8,988 → 8,325. No daemon changes yet.
