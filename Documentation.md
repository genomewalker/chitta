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

## Phase 9 — complete code index (2026-09-16)

The read-only live baseline was 39 files / 440 symbols under `chitta` and no
files under `project:chitta`. Indexing stopped at 500 files by default; watcher
incremental requests returned before extracting symbols; project labels were
exact-string filtered; provenance spawned Git once per file; symbol embedding
ran synchronously; and code-file JSON overflowed a fixed 128 KiB buffer.

The structural pass now uses deterministic Git file selection, includes
initialized submodules, honors `.gitignore` and `.chittaignore`, repairs missing
coverage on incremental requests, and extracts symbols without embeddings by
default. File listings accept bare/project-prefixed labels and grow buffers.
The Git installer preserves existing hooks and starts background indexing on
commit/checkout. Deployment is left to the orchestrator.

An empty scratch mind indexed 709/711 tracked supported files (99.72%; 588/590
without submodules), plus three untracked supported files, in 42.007 seconds.
The two excluded tracked paths are under `.scripts/`, explicitly gitignored.
The store held 9,139 unique symbols: C/C++ 2,553; Python 2,024; Rust 2,694;
Markdown 1,868. There were 10,204 extraction records before the existing store's
(kind, name, file) deduplication, and 70,615 callsites. Scratch mind files used
26,021,158 bytes before shutdown (includes WAL/source registry, excludes logs).
This is a code-only empty-mind test, not a frozen-replica recall evaluation.

Validation: Rust 296 passed / 2 ignored; native CTest; MCP 159 tests; SMRITI 46
tests; all hook shell fixtures; CI ruff check/format; bash syntax and shellcheck
on changed scripts. Scratch lifecycle fixtures cover first-event full repair,
project aliases, rename and deletion. Native collection fixtures cover >500
files, tracked ignore rules, quoting, deterministic ordering and deletion.
Git hook probes cover both events, preservation and idempotent installation.
Contracts regenerated: existing `learn_codebase` gains optional `embed`; no new
RPC in this step. Recall scoring and snapshot/WAL formats are unchanged.


## Phase 9, step 2 — scoped navigation graph

Added `code_query` and `code_path`; extended `read_symbol`, `code_context` and
`codebase_overview` instead of introducing a duplicate explanation tool. The
optional code-navigation.json sidecar stores AST evidence and symbol bodies;
resolved named endpoints are INFERRED, unresolved/ambiguous syntax remains
EXTRACTED with explicit resolution status. Query matching is lexical over code,
identifiers and paths; it adds bounded graph neighbors. C++ method ownership,
signatures, and Python docstrings come from the full AST extraction pass.
The compact repo map uses deterministic call-graph label propagation and
highest call-degree nodes. Hash checks make stale file queries/reads visible.
No recall scoring, embedding identity, snapshot or WAL format changes.

Fresh empty scratch mind: 711/713 tracked supported files (99.72%), 717 total
eligible files including untracked development sources, 29.047 s wall time,
74,867,533 mind bytes including the graph sidecar. The same two explicitly
ignored tracked files are excluded. Store unique symbols by language:
C/C++ 2,586; Python 2,027; Rust 2,694; Markdown 1,872 (the navigation graph
also preserves overloads/definitions collapsed by the store's legacy identity).
A forced incremental rebuild took 20.431 s. Twenty fixed development queries
achieved 18/20 file:symbol hits; 100 samples per run gave p95 74.383 ms before
and 66.677 ms after restart. All 20 complete query JSON responses matched
across restart. The separate strict byte proxy counted 253,347 vs 604,760
bytes (58.11% reduction), charging answer bodies even for retrieval misses.
The question set and runner are reserved for the step 4 evaluation commit.

Validation: Rust 296 passed / 2 ignored; CTest 36 passed (embedding-model test
skipped); MCP 159 and SMRITI 46 tests; all hook fixtures; CI ruff check/format;
changed generator lint and native graph tests covering confidence, receiver
ambiguity, path scope, shortest connections, reload identity, stale reads and
deletion. Contracts regenerated and checked against the private scratch daemon:
new RPCs `code_query`, `code_path`; existing code tool extensions above. Core
MCP surface is 55 tools / approximately 6,555 tokens, within its gate.


## Phase 9, step 3 — bounded hook context

Fetched and merged origin/main immediately before hook edits (merge 221b8353).
Added one small code-nav.sh helper, a one-line Read call, and a one-line
replacement for the obsolete capped session auto-index block. The install
manifest includes the helper. Indexed reads receive symbols, signatures,
caller/callee evidence and read_symbol arguments; file age and stale hashes
are explicit. Unknown repositories stay silent; known-index transport failures
are visible. Query processes have a 300 ms default deadline with 50 ms kill
grace, and UTF-8-safe whole-line output is bounded to 12,000 bytes. Session
maps appear once per session; uncapped refreshes run behind a per-repo flock.

Complete code-file restoration exposed an existing eager startup scan of all
historical repository roots, including a large shared dataset. Startup now
loads the root registry without those walks; search already refreshes the
requested realm before returning sources. Code-context compatibility counters
are preserved, and clear_codebase now clears the derived navigation sidecar.

Validation: real Read injection was 7,277 bytes; the real map was 17 lines and
appeared exactly once. The focused regression covers stale context, timeout
children, unindexed/non-code silence, session markers and uncapped refresh.
All hook suites, MCP 159, SMRITI 46, Rust 296 (2 ignored), CI ruff, changed-shell
syntax/shellcheck and the quick gate passed. Final CTest passed all 36 tests
(the embedding-model test skipped), including real RPC counter/clear tests.
An earlier cold subprocess-load timeout passed on the ordered rebuild/retest;
the separate frozen-copy format probe also passed under embedding load.

bench-hook-parity.py compared all 10 non-code fixtures for three measured
repetitions (30 paired results), with identical stdout, stderr and status.
The first cold warmup had unchanged prompt-lane deadline variance; the final
run explicitly warmed those lanes before the unchanged paired comparison.
Frozen-copy chaos passed 9/9. Restart-identity passed 20/20 ordered queries
across three restarts, with no numeric score deltas. No recall scoring change.
Contracts regenerated for the added install-manifest entry; RPC contracts
unchanged in this step. All experiments used owned scratch processes.


## Phase 9, step 4 — frozen navigation evaluation

benchmarks/codenav fixes 20 source-verified file:symbol answers from 18444e02
and store 089a056. Questions were fixed before retrieval measurements; their
SHA-256 remains 49fb3d96acd9c7b966e489e9b3dc6c57535665b21c18e0d22a1f54ab419240a3.
The runner selects answers from code_query alone. Independent byte accounting
charges query text, full pre-read blocks (including truncation notices), and
answer bodies even on misses. It conservatively charges lines the hook byte
cap could omit. Two trust tests reject unstable repeated answers and prove
that ground-truth body reads cannot convert a failed retrieval into a hit.
The package and protocol are protected by EVAL_IMMUTABLE with owner approval.

Final fresh empty-mind qualification with the benchmark package staged:
717/719 tracked supported files (99.72%), 718 eligible including untracked
Plan.md, 48.927 s wall index time, 74,966,923 mind bytes. The two missing tracked
files are explicitly gitignored. Store symbols by language: C/C++ 2,587;
Python 2,031; Rust 2,694; Markdown 1,875. Query score: 18/20 before and after
restart. Each run sampled 100 RPCs; p95 was 140.946 ms before and 133.943 ms
after restart. All 20 complete query responses matched across restart.
The byte proxy was 253,830 versus 604,946 bytes, a 58.04% reduction.
These are structural navigation measurements, not an LLM end-to-end trial.

Per-commit gates: all hook suites, MCP 159, SMRITI 46, Rust 296 (2 ignored),
CI ruff and shell checks passed. CTest passed 35 cases; the remaining existing
daemon-isolation log-order assertion failed during the parallel run and passed
in its isolated rerun (13.51 s). No test threshold or production behavior was
changed to pass it. Benchmark trust tests passed. Contracts unchanged.


Step 5 documents the hook contract/environment, navigation orientation,
changelog and verified five-step Phase 9 status. API/static generation produced
no further drift. The final quick gate (including documentation/site checks),
Rust 296 tests, all 36 CTest cases (one model-dependent skip), all hook suites,
MCP 159, SMRITI 46 and CI lint passed. The language-expansion follow-up added
by the required main merge is marked pending, separately from the five steps
in this work order. The benchmark approval trailer is carried forward because
the immutable-eval checker reads the branch head message for the entire diff.

## Phase 9 language expansion — Bash (2026-09-17)

Shell hooks previously had no structural symbols. Bash/sh now uses the pinned
`tree-sitter-bash` grammar with function signatures, source/dot imports and
literal calls. Unresolved executables remain extracted syntax; graph resolution
binds only known functions. The collector, graph language grouping and pre-read
hook accept `.sh` and `.bash`. No recall scoring changes; contracts unchanged.

The focused fixture yields 2 functions, 5 calls and 2 imports, with no heredoc
definitions. Exact signature prefixes and the real `hooks/lib.sh` library pass.
On compute, 200 fresh parses averaged 231.37 microseconds/KiB; the release daemon
is 53,171,288 bytes, an increase of 1,399,056 bytes. Configuration and build used
the cached pinned source with FetchContent fully disconnected. The quick gate
passed, including contracts. Two existing test-only ShellCheck warnings now
explicitly document intentional literal tilde paths. The initial JSON assertion
compile/lifetime defects in the new test harness were fixed and rerun. The full
gate and expanded repository coverage run follow the last language commit.

### R

R source now contributes assigned functions, named methods, S4/R6 classes,
source/package/namespace imports, calls and literal inheritance. `.R`, `.r` and
`.Rprofile` are recognized. Multiline function signatures are retained. The
pinned `r-lib/tree-sitter-r` revision builds with the existing runtime offline.
The fixture finds 5 definitions, 10 calls, 4 imports, 2 inheritance relationships
and 34 identifier references; fresh parsing averages 311.74 microseconds/KiB.
The daemon is 53,685,328 bytes (+514,040). Bash regression and R extraction
checks pass. A compute-side rebuild was required after shared-filesystem
attribute caching reused an older object; the rebuilt signature check passes.
Contracts unchanged; no recall scoring change.

R package imports also no longer use Python module-file resolution. A graph
regression keeps `library(a)` unresolved beside `a.py`, while an explicit
`source("a.py")` resolves. The quick gate passes, including unchanged contracts.
Named callback arguments remain anonymous; the R6 method fixture still extracts
its method. The final extraction and graph-scope regression suites pass.

### Julia

Julia now extracts both function forms, macros, modules, structs, abstract types,
subtype relationships, calls, `using`/`import` and literal `include` edges. Module
imports can resolve to an unambiguous module definition in the same repository
and language. Function signatures are not mistaken for callsites. The pinned
grammar builds offline with the existing tree-sitter runtime.

The fixture has 5 definitions, 4 calls, 3 imports, 1 inheritance edge and 22
identifier references. Fresh parsing averages 431.54 microseconds/KiB. The
daemon is 59,932,208 bytes (+6,246,880). Bash, R, Julia and graph-scope regression
checks pass; contracts are unchanged and recall scoring is unchanged.

Julia's quick gate passes. Extraction is now one shared static library rather
than seven repeated compilations; all native CodeIntel users link it explicitly.
The older callsite regression now runs with assertions enabled. Callsite,
repository-index, code-index and graph CTests pass (4/4).

### Fortran

A pinned offline-buildable Fortran grammar now extracts modules, programs,
subroutines, functions, derived types, `use`/include relationships, calls and
`extends`. Extensions accept upper/lower case. Symbol, call and module names
are folded consistently while signatures preserve the source spelling.
The mixed-case fixture yields 5 definitions, 3 calls, 2 imports, 1 inheritance
edge and 16 identifier references. Fresh parsing averages 292.73 microseconds
per KiB; daemon size is 63,698,552 bytes (+3,766,344). All language fixtures and
graph regressions pass; contracts and recall scoring are unchanged.

### Nextflow

The dedicated `nextflow-io/tree-sitter-nextflow` grammar was chosen over Groovy
because its AST names processes, workflows, process outputs and pipes. Its
pinned generated parser requires ABI 15, so the pinned tree-sitter runtime is
now 0.25.10; the existing grammar fixtures and callsite regression still pass.
Both `COUNT(ALIGN.out)` and `ALIGN | COUNT` produce channel links between unique
indexed producers and consumers. Graph evidence retains the routing expression's
file and line even when process definitions live in another file. The graph
restart regression verifies those links remain deterministic.

The fixture finds 5 definitions, 8 calls (including 2 channel links), 1 include
and 22 identifier references. Fresh parsing averages 213.56 microseconds/KiB;
the daemon is 64,204,504 bytes (+505,952, including the runtime upgrade). Offline
build and all language fixtures pass. The initial direct-output regression
identified the grammar's expression wrapper; handling it makes both routing
forms and the cross-file evidence check pass. Contracts and recall scoring
are unchanged.

### Snakemake

The dedicated Snakemake grammar builds offline and parses the fixture without
errors, so no Python fallback is needed. Rules, checkpoints and modules are
definitions; input/output/params sections are named children that `read_symbol`
can read directly. Python definitions and calls share the existing AST walker.
Includes and module Snakefiles become imports. Explicit `rules.NAME.output`
expressions connect unique producer and consumer rules.

The fixture yields 9 definitions, 2 calls (one rule dependency), 2 imports and
18 identifier references. Fresh parsing averages 291.94 microseconds/KiB;
daemon size is 65,027,832 bytes (+823,328). All language fixtures and graph
regressions pass; contracts and recall scoring are unchanged.

### Perl

Perl navigation now extracts packages and subroutines with package scope,
function/method calls, `use`/`require`, and literal `use parent`/`use base`
relationships. A package inheritance edge starts at its named package and
retains the actual pragma line as evidence. The generated release revision
builds offline with the ABI-15 runtime.

The fixture has 5 definitions, 2 calls, 5 imports and 1 inheritance edge. Fresh
parsing averages 472.68 microseconds/KiB; daemon size is 69,770,184 bytes
(+4,742,352). All language fixtures and graph regressions pass; contracts and
recall scoring are unchanged.

### Make

Make navigation extracts literal targets, `define` macros, prerequisites,
macro calls and includes. The fixture has 4 definitions, 4 calls (including
3 target dependencies) and 1 include. Fresh parsing averages 149.84
microseconds/KiB; daemon size is 69,968,848 bytes (+198,664).

Offline builds, extraction and query-edge assertions pass for every language
so far. Empty and truncated source tests exposed unsafe optional AST access;
null-safe traversal fixes it. Exclusive AST end positions now keep a call
following `endef` outside the macro. Contracts and recall scoring are unchanged.

### CMake

CMake navigation extracts functions, macros, build targets and command calls;
includes and subdirectories point to files, while explicit target dependencies
connect targets. The fixture has 5 definitions, 11 calls (one dependency),
2 imports and 10 identifier references. Fresh parsing averages 442.69
microseconds/KiB; daemon size is 70,056,592 bytes (+87,744).

Offline extraction, all fixture query edges and source-collection regressions
pass. Git collection now excludes untracked ignores in its initial listing,
then checks only tracked ignores, avoiding a redundant walk of build caches.
Both tracked and untracked `.gitignore`/`.chittaignore` cases are verified.
Contracts and recall scoring are unchanged.

### SQL

SQL navigation extracts tables, views, functions/procedures, calls and object
references. The generated grammar parses SQL function bodies as statements,
so their table references remain AST-backed. The fixture has 4 definitions,
1 call and 32 reference records; extraction and query assertions verify both
table references. Fresh parsing averages 323.00 microseconds/KiB.
Daemon size is 81,139,352 bytes (+11,082,760). Offline builds and all language
fixtures pass; contracts and recall scoring are unchanged.

### PHP and optional grammars

PHP navigation extracts functions/methods, classes/interfaces/traits/enums,
calls, literal include/require paths, namespace imports and inheritance.
The fixture has 4 definitions and 4 query edges (2 calls, 1 import, 1 inheritance).
Fresh parsing averages 287.26 microseconds/KiB; daemon size is 82,239,112 bytes
(+1,099,760). Offline extraction/query fixtures pass. Contracts and recall
scoring are unchanged.

`CHITTA_EXTRA_GRAMMARS` defaults ON and controls the Graphify-parity group;
OFF retains all priority bioinformatics/build grammars. The shared FetchContent
helper supports grammar repositories with generated parsers in subdirectories.

The source-filter performance fix qualifies on a fresh private daemon:
855/865 tracked supported files (98.84%) in 37.374 seconds, 80,953,087 index
bytes, including 208 Bash symbols. Ten deliberately ignored files remain out.
