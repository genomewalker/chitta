# Explicit relation-transfer completion — 2026-09-15

## Outcome
Keep recall_analogy under decision memo section 2. On selected family bbcaed33,
generation 38047, the independent exact baseline and RPC both scored 14/14
hit@1, 14/14 hit@3, 14/14 negative abstentions, zero unsupported answers and
zero missing grounding. RPC errors: zero. See docs/EVALS.md for the full table,
latency caveats, thresholds, provenance, and artifacts.

## Implementation
Indexed exact directed a→b predicate transfer to actual c neighbors replaces
the endpoint VSA cache, structural mode, and fact cap. Each returned neighbor
includes supporting graph edges. Weight and recency determine rank. Missing
relations have explicit reasons. Exact subject guards reject stale index IDs.
query_graph, graph persistence, and the HDC organ remain available.

## Validation and limits
Rust release: 264 passed, 2 ignored; all binary/doc targets passed.
ctest: 17 passed, 1 skipped, zero failures. Six benchmark tests, the corrected
14-proportional hook panel assertion, and touched-Python Ruff checks passed.
Previously completed isolated hook retries and SMRITI suite passed. The prior
MCP run has one unrelated HTTP SDK error in 130 tests: missing
BoundedSessionManager._session_owners. No MCP changes were made.

The earlier Rust test process survived the prior turn and completed successfully.
A redundant overlapping invocation failed linking and was stopped. Successful
Rust evidence is chitta/build/analogy-evidence/analogy-rust-test.log; both logs
are retained locally. No further native edits occurred after the tested build.

The scratch daemon opened a selector-verified fresh copy of the replica family,
with private runtime/socket/port, disabled queues/autonomous work, and quiescence.
It was stopped after the final benchmark. No installation, service restart,
publication, shared-store writes, or graph cleanup was performed.
