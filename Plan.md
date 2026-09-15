# Analogy relation-transfer stream — 2026-09-15

## Scope and decision
Execute decision memo section 2 on fix/analogy-rescope. No deployment or live-state writes.
Use exact case-sensitive triplet symbols and directed a→b predicates; multiple explicit predicates are a union, per the task's plural contract. No inferred reverse edges or ambiguous fuzzy matching. Rank distinct neighbours by strongest edge weight, then valid_from_ms descending, with deterministic ties. Return source and target supporting edges and explicit abstention reasons. Keep legacy Fact solely for out-of-scope snapshot API compatibility; remove the endpoint's VSA cache and structural implementation, leaving hdc.rs untouched.

## Plan
1. Replace analogy implementation and FFI/C++ endpoint; add indexed regression tests.
2. Copy selected eval family bbcaed33/generation 38047 using selector and replica copy rules into this worktree; private quiesced daemon, port/runtime/queue isolation.
3. Exact independent relation join for all 14 existing proportional queries; 14 negative targets selected before RPC evaluation; preserve missing grounding as failures.
4. Keep only if hit@3 >=12/14, negative abstentions 14/14, unsupported answers zero; otherwise remove per task.
5. Release Rust build/test with identity 768:nomic-embed-text-v1.5 and library exports; local CMake with main CHITTA values except local FIELD_ROOT; ctest and applicable repository gates.
6. Commit submodule first, then superproject, with problem/design/evidence messages.

## Constraints / findings
- Sandbox shell launch intermittently fails because bwrap is missing; escalated commands remain worktree scoped.
- Existing hook regression asserted 20 tasks including structural; updated to the authorized 14-proportional panel while completing the final gates.
- Worktree has no preexisting release identity file. Compare built FFI identity to main release marker (768:nomic-embed-text-v1.5); do not invent a before marker.

## Gate evidence (before benchmark)
- Rust release build passed; test --release: 263 passed, 2 ignored, zero failures (265 discovered). Removed 17 old VSA analogy tests and added 3 relation-transfer tests.
- CMake local build passed. ctest: 17 passed, 1 skipped (embed_pool_test), zero failures, 18 registered.
- Identity unchanged: both main and worktree chittad format-id = 9230643459983636874; generated config 768, nomic-embed-text-v1.5, text format 1.
- Six new Python benchmark tests and ruff passed. SMRITI suite passed.
- Hook suites initially had three isolation failures due to inherited long socket paths; all three passed with short runtime aliases and socket override removed. Remaining failure is the obsolete 20-task analogy assertion (scope extension pending).
- MCP: 129/130 pass, one preexisting/dependency-area error: BoundedSessionManager lacks _session_owners (HTTP SDK test). No MCP files changed.
- Copied only selected bbcaed33 family/manifests/WALs plus migration markers; source/destination selectors agreed before daemon start. Private daemon metadata is chitta/build/analogy-daemon.json; no shared store opened, no live writes.

## Replica discovery
Independent baseline initially refused chitta-bridge indexed output: 30 entries, only 7 exact subject matches. Legacy duplicate triplet IDs can resolve stale by_subject IDs to another subject. Apply exact e.subject checks after indexed lookup in endpoint and baseline (matching original benchmark grounding audit); add a duplicate-ID replay regression. No triplet-organ cleanup or write. Rerun native gates after this correction.

Complete exact baseline: all 14 positives grounded, 1–3 valid answers per query. Before endpoint evaluation, require 14 distinct negative argument triples and rotate target choices (the first draft repeated two source pairs). First scratch startup created additional private WAL state despite preserving the selected snapshot ID; stop the owned PID and refresh the private store from the original verified family for the final run. Never reuse that warmed store as a new frozen input.


## Final benchmark completed
- Reused the verified fresh-family scratch daemon PID 3378841 and socket from analogy-daemon.json.
- baseline.py then run.py: both hit@1 and hit@3 14/14; negative abstentions 14/14; unsupported answers 0; missing grounding 0; RPC errors 0.
- Median latency: baseline positive two-RPC joins 1.526 ms; endpoint all 28 one-RPC calls 0.190 ms. Different workloads; no speedup claim.
- Decision: KEEP explicit relation transfer. docs/EVALS.md records table, thresholds and provenance.
- Final Rust gate from the surviving earlier job completed: 264 passed, 2 ignored, zero failures; includes duplicate-ID regression and all binary/doc targets. A redundant new invocation failed linking during overlap and was stopped; use analogy-rust-test.log as successful evidence.
- Final ctest: 17 passed, embed_pool_test skipped, zero failures. Six benchmark tests, updated hook regression and Ruff passed. Prior MCP error remains unrelated and documented.
- Owned scratch daemon PID 3378841 stopped after evaluation. Scratch logs/runtime archived under chitta/build/analogy-evidence.
- Commit order: chitta-field implementation first, superproject pointer/handler/benchmark/docs second.
