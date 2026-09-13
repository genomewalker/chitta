# MCP fixes and evidence — 2026-09-13

Scope: `chitta-mcp/`, branch `fix/mcp`, baseline `7d67ef2e`.
No deployment, service restarts, live mind file writes, or pushes.

## Recall latency

`recall_gateway.py:28` adds `CHITTA_MCP_PROFILE=1` monotonic timings.
`server.py:1164` measures daemon connection/RPC; `server.py:2427` measures the
recall gateway. Model load, executor wait, tokenization, inference, compression
and total tools/call duration are logged without queries or memory contents.
Existing loop-lag monitoring remains active.

The dominant cold cost was loading Transformers/Torch via AutoTokenizer.
`recall_gateway.py:103` now drives the exported Rust tokenizer directly with
ONNX, preserving normalization, pair templates, special tokens, padding and
truncation. Vocabulary-only exports retain the old loader. The socket already
persisted and inference was fast; limit 10 still fetches 40 candidates.

Real stdio initialize/initialized/tools-call sequence, query
`SMRITI benchmark result memory-on vs memory-off`, realm `project:cc-soul`,
default hybrid strategy, limit 10. One fresh MCP process per arm, one cold and
three warm recalls each. Cold means first model load, not a flushed OS cache.
Baseline is the original code plus instrumentation only. Both arms used the
same live socket/model, a read-only RPC allowlist, isolated HOME/cache/runtime,
offline model loading, and production sqz. The harness cannot start a daemon.

| Seconds | Before | After |
| --- | ---: | ---: |
| Initialize including process startup | 0.935 | 0.934 |
| Cold recall end to end | 10.848 | 1.082 |
| Cold model load | 10.465 | 0.348 |
| Cold daemon RPC | 0.123 | 0.453 |
| Cold inference including tokenization | 0.138 | 0.173 |
| Cold compression | 0.100 | 0.089 |
| Warm median (3 calls) | 0.373 | 0.404 |
| Warm p95 (nearest rank, 3 calls) | 0.374 | 0.651 |
| Warm range | 0.284–0.374 | 0.378–0.651 |

Targets pass: cold <8 s, warm <3 s. Cold recall improved about 90%; including
initialization, startup plus first recall fell from 11.783 to 2.016 s.
These are small live samples, not a quality evaluation; no warm speedup is
claimed. Zero MCP errors or empty content responses. The last response in each
arm was a 22-byte sqz dedup reference; RPC and inference still ran.

Raw evidence: `tests/evidence/before.{json,log}`, `tests/evidence/after.{json,log}`.
Reproduce candidate timings from `chitta-mcp/`:

```sh
/maps/projects/fernandezguerra/apps/opt/conda/envs/bioinfo/bin/python3 tests/profile_recall.py /tmp/mcp-recall-profile
```

The unit test compares all three input arrays with Transformers on mixed, long,
Unicode and empty pairs. A separate read-only check against the actual MiniLM
export also found exactly equal input arrays and ONNX scores for five such pairs.

## HTTP sessions

`server.py:124` implements the monotonic table, atomic admission and eviction;
`server.py:3061` wires it into the SDK. Defaults:
`CHITTA_MCP_SESSION_IDLE_S=1800`, `CHITTA_MCP_MAX_SESSIONS=64`. Invalid settings
fall back to defaults. Capacity evicts the least recently used session. Idle
expiry runs every min(30, idle/2) seconds and before requests, so an expired ID
cannot refresh itself. Eviction terminates transport and removes owner metadata.
Expired IDs return 404. `health_check` and HTTP `/health` expose active, created,
expired and capacity-evicted counts plus the configured limits.

The previous HTTP configuration was stateless. It now uses standard stateful
MCP initialization: clients retain the returned Mcp-Session-Id and reinitialize
after 404. Authentication remains in front of MCP requests. This uses the SDK's
session table/creation lock; real MCP SDK 1.27.2 integration tests cover that
coupling alongside isolated table tests. No running HTTP service was changed.

## Cycle cleanup

`evolve/cycle.py:46` closes stdin and polls within the remaining cycle deadline.
`evolve/cycle.py:212` adds `-o <artifacts>/implementer-final.txt`. Stale output is
removed first; a nonempty file stable for 250 ms completes the implementation
stage even if Codex hangs in MCP shutdown. Existing commit/branch/gates follow.

Every exit path cleans only the new private process group: SIGTERM, up to 200 ms
to drain, then SIGKILL for remaining descendants and a bounded final reap.
Cleanup also handles an exited leader whose child still holds stdout open.
Real nonzero exits are preserved; our cleanup signal after completed output is
treated as success. Budget expiry permits up to 1.2 s of cleanup grace.
Tests use local Codex stubs, covering final-output-then-sleep, ignored SIGTERM,
descendants, closed stdin, stale output, budget expiry and inherited stdout.

## Validation

See `tests/evidence/gates.txt`: 110 MCP tests, 46 SMRITI tests and all seven hook
shell suites pass. Hook state was isolated and both headless aliases unset.
MCP-wide ruff passes; its pre-existing unused loop variable in
`saddle_detector.py:90` was renamed `_j`. Shared modules import on PyPy 3.9.18;
ten hook files had no version failures, with one optional-dependency skip
(`hopfield-probe.py`: PyPy lacks numpy). No shell or native sources changed.

An initial discovery was mistakenly run outside chitta-mcp and found no tests;
the required command was rerun in the correct directory. Earlier suite failures
found a profiling callable-name bug and two test-fixture issues (ONNX output
shape and a /proc exit race), all fixed in the passing final run.
