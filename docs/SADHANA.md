# Sadhana: Autonomous Agents

Status as of 2026-09-16.

Sadhana (disciplined practice) is a persistent background agent managed by
`chittad`. Its cycles pursue a goal, observe results and record checkpoints.
The implementation is `chitta/include/chitta/sadhana/sadhana_manager.hpp`,
`chitta/src/sadhana/sadhana_manager.cpp` and
`chitta/src/handlers/field_misc_sadhana.cpp`. The [generated API](API.md)
documents the discovered daemon tools and composed MCP gateway separately.

## Lifecycle

States are `pending`, `running`, `paused`, `done` and `failed`. A running agent
is eligible for another cycle after its interval elapses. Its provider supplies
the agent/model execution; status retains iteration and brain-call counts,
last action/result, goal, realm and timing. A failed provider initialization
on reload pauses the agent instead of scheduling a null brain.

Sadhana state is stored as task payloads and transitions in chitta-field through
the store's task API. It is not an independently managed SQL table. On daemon
startup, `--no-autonomous` pauses restored running agents, which is important
for evaluation replicas. See [CLI.md](CLI.md).

## Start and Manage

Use the daemon tool `sadhana_start` through MCP/RPC. It is not in the native
CLI's direct-command help table on this revision, so `chitta sadhana_start`
is not a supported invocation. The client can still forward its JSON-RPC:

```bash
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"sadhana_start","arguments":{"goal":"Monitor pipeline until complete","brain_provider":"claude","brain_model":"haiku","interval_seconds":120,"max_turns":10}}}' | chitta mcp
```

Starting an agent authorizes ongoing work toward that goal; choose its provider,
scope and limits deliberately. This example is a request shape, not a daemon
configuration change. Inspect status and pause/stop through the same RPC path.

| Tool | Arguments | Effect |
|---|---|---|
| `sadhana_start` | `goal` required; `brain_provider`, `brain_model`, `interval_seconds`, `realm`, `goal_dsl`, `max_turns` optional | Create and start |
| `sadhana_list` | See discovery schema | List states |
| `sadhana_status` | `id` | Status and history |
| `sadhana_pause` / `sadhana_resume` | `id` | Suspend / resume |
| `sadhana_stop` | `id` | Stop and mark done |
| `sadhana_set_goal` | `id`, `goal` | Replace goal |
| `sadhana_set_model` | `id`, `model` | Change model |
| `sadhana_set_interval` | `id`, `interval` | Change cycle interval |
| `sadhana_set_max_turns` | `id`, `max_turns` | Per-cycle bound; 0 uses manager default |
| `sadhana_checkpoint` | `id`, `summary`; optional `status` | Record progress |

The MCP `sadhana` gateway dispatches by `action` to these daemon tools. Its
advertised `model`/`interval` fields describe setters; the start handler reads
`brain_model`/`interval_seconds`. The gateway forwards arguments without renaming
them. For explicit provider/model selection at creation, use the direct daemon
schema through RPC or the `advanced` gateway, rather than assuming aliases.

## Manager Defaults

These are the C++ `SadhanaConfig` defaults, not environment-variable promises:

| Setting | Default |
|---|---:|
| Provider / model | `local` / `gemma4:26b` |
| Cycle interval | 300 s |
| Concurrent agents | 10 |
| Agent timeout | 600,000 ms |
| Turns per cycle | 20 |
| Consecutive failure limit | 5 |
| Stored output cap | 4,000 characters |
| Total iteration cap | 10,000 |
| Runtime cap | 168 hours |
| Iterations per hour | 500 |

Provider behavior and permissions are implemented in
`chitta/src/sadhana/brain_provider.cpp`; local and Claude providers have different
execution paths. Stored goals and previous checkpoints support continuity, but
do not guarantee that every action succeeds or every inferred learning is useful.

## Shepherd, Dreams and Evolution

`/chitta:shepherd` is the pipeline-monitoring skill. Dreams are exploratory
agents reached through `dream_start`, `dream_wander`, `dream_list` and related
tools. Their findings and task checkpoints can become memory; current admission
and evidence boundaries are in [HOOKS.md](HOOKS.md) and [EVALS.md](EVALS.md).

The [evolve loop](EVOLVE.md) is a separate code-change workflow: isolated
worktree, preregistered bet, implementation, gates, replica measurement and human
merge. Its opt-in `--candidates N` fan-out defaults to one stream, selecting
passing candidates by measured bet delta and then patch size. Sadhana itself is
not a substitute for the repository's worktree and deployment rules.

The [site illustration](sadhana.html) explains the lifecycle and shows historical
agent examples. Those examples and schedules are not current configuration or
a live agent dashboard. For the automatic-learning causal experiment, the
official cohort cut is recorded but the prospective task panel remains pending.
