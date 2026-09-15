# Hook interpreter removal continuation

## Result

Native CLI registration, heartbeat/lease renewal, and task cards replace Python
on the normal lifecycle path. bash/sed/jq preserve prompt cleanup, correction IDs,
queue framing, and telemetry. The two old fixtures now verify CLI registration,
concurrency, and durable heartbeat queueing when RPCs fail or sockets disappear.

An installed fastText model is now invoked only when the existing regex evidence
can affect a decision. Ordinary prompt turns do not start Python. Explicit RLM,
regex-matching classifier prompts, periodic hints, and lifecycle compatibility
adapters retain Python. Empty RPC responses now reliably trigger fan-out fallback.

## Measurements

Unchanged `scripts/bench-recall-lanes.sh 5`: 15 runs/arm, off median/p95 693/843 ms,
on 608/717 ms, zero empties. The script does not expose individual exit statuses.
Ten SessionStart runs: median/p95 599/677 ms, zero failures/empties. All hook writes
were isolated, and live APIs were read-only. No services or binaries were changed.

The prompt <500 ms target remains unmet. In the median of five separately profiled
RPC runs: preparation 112 ms; RPC/validation 255 ms; collection/merge 37 ms;
filtering 174 ms; admission 20 ms; exposure bookkeeping 28 ms; intent 19 ms;
enrichment 92 ms; output assembly 8 ms. Phase sum 745 ms, reported total 747 ms;
final rendering interval another 47 ms from the phase checkpoint. This diagnostic
is separate from the 608 ms uninstrumented benchmark median. Load is uncontrolled.
Tables and limitations: `docs/HOOKS.md`, native hook paths subsection.

## Validation

- All changed shell files pass `bash -n` and ShellCheck 0.10.0's repository gate
  (`--severity=warning`); no new suppressions.
- Prompt fixture suite passes after fixing empty JSON response acceptance.
- Native registration, card parity, cleanup parity, durable heartbeat/queue,
  SessionStart concurrency and deadline tests pass.
- MCP: 130 tests pass. The user-exempted `_session_owners` error did not reproduce.
- SMRITI: all 46 unit tests pass.
- Full hook suite: 17/18 passed on the final sweep; the unchanged saddle timing
  test had incremental p95 355.58 ms against 150 ms, after passing the earlier
  sweep. The isolated rerun also exceeded the limit (466.73 ms incremental p95;
  baseline p95 569.68 ms, enabled p95 1032.26 ms). This unchanged timing gate is
  load-sensitive on this node; no test thresholds or unrelated hooks were changed.

Evidence: `/tmp/chitta-nopython-finish/`, plus the ten-run harness and outputs
`/tmp/chitta-nopython-evidence/measure.py`, `session-finish.json`, and
`session-finish-*.out`. Earlier before/after evidence remains in
`hooks/tests/hook_nopython_latency.md`.

No installation, service changes, deployment, or push. Commit on this branch only.
