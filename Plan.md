# Field performance — 2026-09-14

Scope: `fix/field-perf`, this worktree only. No deployment or live-store writes.
Baseline must use an independently validated copy of the eval replica family,
including migration markers, private path/socket/runtime/port, queue disabled.

## Decisions and sequence
1. Build unchanged Rust/C++ with production embedding identity 768:nomic-embed-text-v1.5.
   Preserve baseline artifacts before rebuilding. CMake uses production CHITTA_* values
   except FIELD_ROOT, which must name this worktree. Dependency sources may be read
   from the main build, but all generated output stays here.
2. Add reproducible runner and timing instrumentation; measure BEFORE before optimization.
3. Warm search structures before readiness, eliminate repeated triplet migration safely,
   profile hybrid stages, and account/trim memory based on measured components.
4. Run identical AFTER workloads, compare response key sets and ordered IDs, run gates,
   document measured targets honestly, commit submodule first then superproject.

## Environment
- Clean branch fix/field-perf; chitta-field initially detached and clean.
- Sandbox launcher fails with missing bubblewrap; commands use approval-reviewed execution.
- No existing worktree build. Production identity file reads 768:nomic-embed-text-v1.5.
- Main CMake CHITTA_BUILD_RPC=ON, CHITTA_BUILD_TESTS=ON, CHITTA_EMBED_DIM=768,
  CHITTA_WITH_LLAMA_CPP=ON.

## Evidence
Baseline and final benchmark artifacts are complete; see docs/FIELD_PERF.md.

## Baseline observations
- Pilot: ready 35.43 s, first recall 6.412 s, RSS 4321.6 MiB; warm fused p50 36.6 ms.
- First recall spends 3.07 + 3.15 s in synchronous competitive-weight refresh; semantic searches themselves 47/76 ms.
- Final baseline measures hybrid first, before the fused samples can consume its refresh backlog.
- Use health_check RPC (status is a CLI command). Allocation diagnostics must also pass details=true to bypass the daemon atomic fast path.
- Candidate memory fix: eager Turbo build before the existing startup malloc_trim, avoiding retained temporary quantization allocations after readiness.
- Plan a backward-compatible V23 optional triplets_clean section; legacy families still migrate once, dirty WAL replay must invalidate the marker.

## Implemented / measured progression
- BEFORE final: 35,928.8 ms ready; 6,440.6 ms first recall; hybrid 33.7/245.1 ms p50/p95; RSS 4322.4 MiB.
- warm: eager Turbo joins HDC/lite startup work, no age-only Turbo rebuild, borrowed dedup keys.
  First recall 345.0 ms; RSS 4266.8 MiB; host load rose to 79.4. This did not meet targets.
- memory: exact bit-sliced episode-HDC u16 counters + V23 triplets_clean marker.
  Episode counters shrink from a 54,211,801-byte legacy estimate to 7,659,609 bytes;
  total RSS 4233.4 MiB. Useful startup improvement, insufficient memory reduction.
- Found larger avoidable memory: keyword doc_terms duplicates (String,u32) for every posting.
  Replaced runtime-only reverse entries with u32 interned term IDs; postings wire layout and
  BM25 scoring untouched. Term IDs are reclaimed on last-posting removal. Measured in the `final` artifact.
- Competitive refresh now has an immutable, unchanged-Turbo view and bounded 16-thread pool.
  Queries retain single-query arithmetic/filtering. Dirty/scalar/HNSW routes use prior logic.
- Added cf_recall_profile_enabled FFI; C++ handler timings cover setup, semantic lanes,
  keyword, HDC, bridge/fusion, rescore, prefilter, formatting, spans.
- New tests cover Turbo age/no-op behavior, warming during open, serial/parallel exact scores,
  V23 marker absence/roundtrip, invalidation and winner choice, HDC growth/saturation,
  keyword legacy decode and remove/reindex/rebuild.

## Validation caveats to resolve/report
- BEFORE/warm ordered IDs match 20/20 for keyword/fused/hybrid. Smart and recall_lanes vary
  within BEFORE itself (4 variants each); optional affect key varies with returned rows.
  Do not claim deterministic parity for those production-adaptive routes from these runs.
- Ancillary hook tests: session-start-cards failed with an outer CHITTA_SOCKET_PATH override;
  rerun without conflicting overrides passed all six fixtures. SMRITI 46 tests pass.
- MCP 130 tests: one baseline environment error (mcp 1.27.2 StreamableHTTPSessionManager
  lacks _session_owners expected by existing test/server). No out-of-scope dependency install.
- Final Rust/C++ gates, clean checkpoint/restart measurement and report are complete; local submodule commit is bc709a3; superproject commit follows.

## Final verification decisions
- Optimized sample: RSS 3223.5 MiB (25.42% below BEFORE); hybrid p95 70.4 ms;
  first 110.0 ms (2.76x fused p50), hybrid p50 49.0 ms vs 33.7 ms BEFORE.
  Targets on first/warm ratio and unchanged hybrid median are NOT established.
- Clean-checkpoint restart: triplet migration phase 0 ms.
- Rejected the four-query batching experiment: it introduced floating-point rounding
  differences without a demonstrated latency improvement. Restored single-query
  arithmetic in parallel and retained the BITWISE score/ID regression assertion.
- Startup test now writes one real payload/WAL op before adding index fixture rows;
  a zero-sequence, payload-empty snapshot was not a valid embedding-load fixture.
- CTest: initial 17 pass + pool test skipped without model; explicit local GGUF run
  makes pool test pass too. No system dependency changes.

- Final first-call profile still shows a cold semantic scan and a lazy refresh pool.
  Prime all bounded workers with a read-only corpus-vector Turbo search before
  publishing each built index; no query-specific state or competitive weights change.
  Measure this independently against verified; retain only if useful.

## Final outcome (2026-09-14)
- Primary AFTER is `primed`: ready 28,712.3 ms; first recall 92.4 ms; RSS 3,220.2 MiB
  (25.5% below BEFORE); hybrid p50/p95 34.7/64.1 ms versus 33.7/245.1.
- Worker priming is retained: first recall improved from verified 118.5 to 92.4 ms.
- First/warm ratio is 2.71x and hybrid median is slightly higher; these targets remain unmet.
- Final-binary clean checkpoint restart: triplet migration 0 ms, ready 28,533.2 ms.
- All 277 active Rust tests pass (2 existing ignored); all 18 CTests pass with the model fixture.
  All hooks and 46 SMRITI tests pass. Additional MCP suite retains one unmodified SDK error.
- Five top-level recall contract key sets match; keyword/fused/hybrid IDs match 20/20.
  Adaptive smart/lanes IDs vary and selected rows expose optional affect-field differences.
  Strict comparator exits 1; no full adaptive-parity claim is made.
- No live service, installation, model identity, or live mind changes. Runtime code and
  benchmark evidence are ready for local submodule-first commits and orchestrator review.

- Rust submodule committed first as `bc709a3`. Superproject records this pointer and the report; no push or deployment.
