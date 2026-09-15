# Finish hook interpreter removal

Branch: `fix/hook-nopython`. Prior state recovered from
`hooks/tests/hook_nopython_latency.md`; Plan.md was missing on resumption.

## Authorized scope

- Update Python-specific fixtures to CLI registration and heartbeat, preserving
  registration evidence, concurrency, and durable offline heartbeat queueing.
- Remove interpreter work performed on every prompt; preserve conditional features.
- Rerun unchanged recall-lanes benchmark (N=5), plus ten SessionStart runs.
- Record both tables and remaining latency attribution in docs/HOOKS.md.
- Run required checks and commit on this branch without attribution lines.
- No install, service changes, deployment, or push. The worktree MCP
  `_session_owners` failure is an accepted environmental exception.

## Progress

- Recovered prior implementation and isolated harness in
  `/tmp/chitta-nopython-evidence/measure.py`.
- Updated both CLI fixtures and gated classifier on required regex evidence.
- Fixed jq accepting empty RPC input; fallback tests now pass.
- Completed unchanged N=5 prompt benchmark and ten SessionStart runs; tables and
  separate phase measurements are in docs/HOOKS.md.
- Required shell lint/syntax and MCP/SMRITI checks pass. All hook scripts passed
  at least once; the unchanged saddle timing gate failed on subsequent runs
  under variable node load. Exact failures are recorded in Documentation.md.
- Completed stream, committed on its branch; no install/restart/push.
