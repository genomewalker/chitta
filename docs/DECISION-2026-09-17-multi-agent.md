# Decision: multi-agent coordination and context economy

Date: 2026-09-17. Stream: p12-multiagent.

Chitta keeps work moving by economising context. Capsules, output caps, code maps,
short sessions and explicit handoffs reduce repeated input. There is no default
context ceiling: `CHITTA_CONTEXT_HARD_STOP=0`. If an operator opts into a limit,
the allowlist still admits messages, session discovery, ledger and handoff writes,
and fresh non-fork Agent launches. A fork inherits expensive context and is not
an escape route. Coordination remains available for finishing or transferring work.

## Launcher lifecycle

Both launchers create one worktree per stream, include the safety preamble and
recalled context, claim before launching, and use a shared supervisor. A worktree
lock prevents concurrent local launches; a durable claim prevents competing
sessions from owning the same named stream. The PID file records the actual
worker, not a shell supervisor. Completion is established by a handoff and inbox
message, not by PID disappearance.

The supervisor records failures when a worker exits unsuccessfully or omits its
handoff. It persists the final handoff, sends it once to `CHITTA_LEAD_SESSION`
(or `--lead`), releases ownership and removes the PID file. TERM/INT forwards to
the worker process group. Abrupt supervisor death is recovered through lease
expiry; this is not an exactly-once delivery protocol across machine failures.
Claude workers use a private configuration directory and explicit settings.
Deployment and modifications to the user's live configuration remain outside a
stream's authority.

## Ledger operation contracts

These operations extend the existing `ledger_op` op/args interface; the public
JSON schema stays unchanged. Contract snapshots are regenerated and checked.

| Operation | Inputs | Result and guarantees |
| --- | --- | --- |
| `stream_claim` | stream, session_id, worktree, branch, title; optional ttl | claimed and lease metadata; one live holder per stream; TTL defaults to 21600 seconds, range 30–604800 |
| `stream_list` | optional stream | claims containing live lease and session metadata |
| `stream_release` | stream, session_id | released; another holder cannot release ownership |
| `stream_handoff` | stream, session_id, content | saved memory and renewed lease; requires a live owned claim and matching `[handoff] stream=<name> ` prefix |

A session heartbeat preserves its stream TTL instead of replacing it with the
ordinary fifteen-minute lease. Intermediate launcher handoffs use
`stream_handoff`; a plain `remember` call does not renew a stream lease. Final
handoffs go through the supervisor. Release clears advertised claim metadata;
reclaim restores it. The handler serialises compound operations in-process and
uses the durable task ledger's lease ownership checks. No store format changes.

## Validation

Launcher fixtures cover both clients, actual PID identity, competing launches,
missing handoffs, interruption, release and completion messages. Replica tests
cover exclusive ownership, wrong-holder release, six-hour heartbeat/handoff
renewal and reclaim. Native build and per-step quick gates are required.
The real worker round trip and full compute gate are recorded in the stream
handoff after execution. Costs are measured from the worker transcript using
`token-ledger.py`, with explicit token-price assumptions rather than billing data.
