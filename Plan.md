# Prompt hook evolution plan

Scope: proposal 69a8f047e822354f; branch evolve/auto-20260913-155357-075302-69a8f047e822354f; base 54bc7e6be5e4695bf6df3a57611f34ab1c7fa949.

Constraints: worktree only; no install, restart, live writes, push or merge. Frozen evaluation files stay unchanged; caller owns evaluation and verdict.

1. Measure unchanged bench-recall-lanes.sh with read-only live CLI and isolated state; profile prompt-core.sh with timestamps.
2. Fix only the evidenced delay and empty last-session rendering; add end-to-end regression tests.
3. Document root cause and improve codex-plugin/AGENTS.md based on actual missing guidance, combined size below 13 KB.
4. Run shell syntax, hook tests, MCP and SMRITI unit gates, immutable check; benchmark again. Commit after gates pass.

Progress: clean isolated branch confirmed. Default exec sandbox fails because bubblewrap is missing; automatically reviewed execution works. Initial benchmark had all-empty output because inherited CHITTA_HEADLESS and CC_SOUL_HEADLESS are both 1; rerunning with both unset. No benchmark file was changed.

## Final implementation state

- [x] Baseline benchmark and timestamped core inspection complete.
- [x] Evidence does not reproduce a fixed ~2 s floor; no speculative latency change.
- [x] Continuity parser fix with failing-before/passing-after regression coverage.
- [x] docs/HOOKS.md and AGENTS.md guidance updated; implementer ratings in Documentation.md.
- [x] After benchmark complete; <900 ms target unmet, no gain claimed.
- [x] Seven shell suites, 99 MCP, 46 SMRITI, four Python hook tests passed.
- [x] Final shell syntax (both changed scripts), whitespace, and protected-path checks passed.
- [x] Reviewed and staged the bounded diff for the branch commit after all applicable checks passed.

After committing, run `bash scripts/check-eval-immutable.sh 54bc7e6be5e4695bf6df3a57611f34ab1c7fa949 HEAD`; report its result with the commit ID.

Diagnostic details and all failures are recorded in Documentation.md. Native
builds do not apply. Frozen-replica evaluation and human review/merge remain
caller-owned, outside this stream.
