# Hook interpreter removal — 2026-09-15

Working tree: `fix/hook-nopython`. Source-only change; no installation, service
changes, deployment, or push. CLI fixtures replace obsolete Python expectations.
Final timing tables and measured phase costs are in [docs/HOOKS.md](../../docs/HOOKS.md#native-hook-paths-final-worktree-measurements-2026-09-15).
The measurements below preserve the earlier before/after evidence; final reruns
supersede the after columns: prompt off 693/843 ms, on 608/717 ms, SessionStart
599/677 ms (median/p95). Final prompt empties and SessionStart failures: zero.

## Measurements

Warm live daemon, read-only CLI allowlist, all hook state under temporary HOME,
mind, runtime, and queue paths. Both `CHITTA_HEADLESS` and `CC_SOUL_HEADLESS`
were unset. Node load was uncontrolled and reached 105 during verification.
These measurements do not establish a controlled speedup.

Unchanged `scripts/bench-recall-lanes.sh 5`: three queries, five repetitions per
query and arm. CLI: `/home/kbd606/.claude/bin/chitta`, no binary installation.
Values below come from the hook's reported `total`, in milliseconds.

| Prompt arm | Runs per version | Before median | After median | Before p95 | After p95 | Before/after empties |
|---|---:|---:|---:|---:|---:|---:|
| RPC off | 15 | 842 | 698 | 1134 | 975 | 0 / 0 |
| RPC on | 15 | 822 | 635 | 1063 | 717 | 0 / 0 |

Ten separate SessionStart invocations, wall-clock milliseconds, payload
`{"session_id":"probe-N","cwd":"<worktree>","hook_event_name":"SessionStart","source":"startup"}`,
N = 0..9. No transcript supplied, matching the requested probe. Maintenance
scripts were excluded using a temporary plugin root; read APIs reached the live
daemon, while write APIs were no-ops. The old ledger renderer's direct socket
calls were reads. Median and nearest-rank p95 are rounded to the nearest ms.

| SessionStart | Runs per version | Before median | After median | Before p95 | After p95 | Before/after failures |
|---|---:|---:|---:|---:|---:|---:|
| Startup | 10 | 693 | 642 | 834 | 869 | 0 / 0 |

Before samples: 834, 751, 690, 652, 696, 670, 636, 704, 594, 714 ms.
After samples: 869, 771, 588, 698, 589, 648, 632, 631, 640, 644 ms.

The prompt <500 ms median target is unmet. SessionStart meets <700 ms;
its p95 increased. Earlier development measurements included one empty prompt
sample and an aborted empty SessionStart sample; neither is represented as a
successful final measurement.

### Per-lane `t:` samples

Separate representative invocations, same semantic-recall query as the first
benchmark query. These are individual samples, not the median rows above.

```text
before off: t:sem=224,ctx=127,hyb=726,kw=168,corr=699,corrk=130,total=2318
before on:  t:sem=43,ctx=87,hyb=337,kw=67,corr=389,corrk=0,total=1979
after off:  t:sem=86,ctx=25,hyb=279,kw=68,corr=252,corrk=15,total=761
after on:   t:sem=36,ctx=2,hyb=231,kw=44,corr=206,corrk=0,total=646
```

Raw logs and the isolated SessionStart harness are in
`/tmp/chitta-nopython-evidence` on this node.

## Implementation and parity

- `clean_query` uses bash to select the first matching close, then `sed -E`
  to strip the bounded markup. This retains the original non-greedy behavior
  for nested reminders. Twenty fixtures compare production output with the
  exact original Python regex, including multiline and Unicode prompts.
- Prompt telemetry and RPC validation use jq. Requested lanes validate before
  any lane file is replaced; invalid responses retain the existing fan-out.
- Prompt and Stop use an asynchronous native heartbeat. Missing sockets and
  failed RPCs fall back to durable `queue_write`; successful RPC/queueing alone
  advances the existing 120-second heartbeat marker.
- SessionStart supplies payload transcript/cwd, computed realm, frontend model,
  and parent PID to native registration. The daemon already performs
  `session_bind`; `ledger_op session_get` restores the existing thread and
  `lease_claim` retains its lease. Transcript registration preserves its queue
  fallback. The Python registration adapter remains only for missing binaries.
- Native inbox/thread formatting is byte-identical to the Python renderers for
  populated, empty, Unicode, multiline, and independent-failure fixtures.
  Correction parsing preserves u64 IDs and 120 Unicode characters on jq 1.6.
  This verifies fixture parity, not identity of changing live recall responses
  or the intentionally changing timing fields.
- The daemon exposes `inbox_list` and `thread_list` separately, with no batch or
  combined read operation. The implementation therefore uses two concurrent
  `ledger_op` calls; a single combined call cannot be implemented in this scope.
- Explicit RLM and periodic hint extraction still use Python. fastText now runs
  only after the regex evidence already required by every model consumer. A
  fixture with an installed model verifies no Python start on ordinary prompts
  and preserved correction hints on a matching prompt. The shared registry
  helper is absent from the native prompt path.
- jq RPC validation requires exactly one complete response. The updated fixture
  exposed an empty-input acceptance bug; empty responses now use fan-out fallback.

## Earlier gates (superseded by final validation)

Final validation is recorded in `Documentation.md`. The following failures were
observed before this continuation and explain the fixture updates; they are not
current blockers.

- `bash -n`: passes for every changed shell script.
- ShellCheck 0.10.0: passes using the repository CI gate
  `--severity=warning` and existing `.shellcheckrc`. No new suppressions.
  Informational/style diagnostics remain in the large existing hook scripts.
- New cleaner, heartbeat, and native SessionStart tests pass.
- Full hook suite: 15 of 18 scripts pass. Two unchanged scripts require the
  removed Python stubs: `test_prompt_core_lanes.sh` requires heartbeat overlap
  markers from `session_registry.py`; `test_session_start_concurrency.sh`
  requires Python registration and renderer process IDs. Their CLI stubs do
  not implement the new operations. Updating those test contracts requires
  relaxing the instruction to leave existing tests unchanged.
- `test_saddle_hook.sh` also fails on this node: first its incremental p95 was
  176.71 ms against a 150 ms limit, later its first bounded PreToolUse call
  returned no parseable JSON. That path was not changed by this work.
- MCP unit tests: 130 run, one error in
  `SDKSessionTests.test_initialize_cap_expire_and_expired_id_is_404`:
  `BoundedSessionManager` lacks `_session_owners`. No `chitta-mcp/` files changed.
- SMRITI unit tests: all 46 pass.

The continuation updates both CLI fixtures, fixes empty-response fallback, and
reruns the full suite. The user explicitly exempted the worktree `_session_owners`
MCP failure; it did not reproduce in the final 130-test run.
