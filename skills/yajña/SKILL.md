---
name: yajña
aliases: [yajna, autonomous, loop, agentic-loop, coordinate, ritual]
description: Autonomous development ritual with role-based coordination. Loops until complete using specialized agents (hotṛ→research, adhvaryu→implement, udgātṛ→test).
execution: direct
---

# Yajña (यज्ञ) - Autonomous Development Ritual

Status as of 2026-09-13: rewritten as constraints. The iteration pseudo-code,
the validation snippets, and the per-role model table were removed — the model
plans the loop; this file says what must hold while it runs.

An autonomous development loop over three roles: **hotṛ** researches,
**adhvaryu** implements, **udgātṛ** validates. Use it for a task list long
enough that stopping for confirmation between items costs more than it saves.

## What must hold

- **Do not pause for routine decisions.** The point of the ritual is that it
  runs unattended. Stop only for a blocker, for completion, or on interrupt.
- **Every iteration starts with a live daemon.** `health_check` first; an
  unreachable daemon is a blocker, not something to work around, because the
  loop's state and memory both live behind it.
- **Implementation happens in a worktree, never on `main`.** Enter it before the
  first mutation, leave it on completion or failure, and surface patches as
  artifacts rather than applying them directly.
- **Validate that work landed, don't trust the report.** An agent that says it
  edited a file and a file that changed are different claims. Check the file.
- **Three strikes ends an iteration.** Same error three times, or a file still
  missing after three retries, is stagnation — stop and report rather than
  burning the budget on a loop that has stopped converging.
- **Address agents to explicit absolute paths.** Vague targets are the dominant
  cause of an agent editing the wrong file.

Delegation follows the repo-wide policy in `CLAUDE.md`: research cheap,
implementation to Codex `gpt-6-astra` in the worktree, review in the
orchestrator. `hooks/pre-tool-hook.sh` enforces the routing, so pass `model`
only where the default would be wrong.

## Blocker or not

A failing test, a partial result, or a slow task is **not** a blocker — log it and
keep going. A dead daemon, a permission denial, a critically broken build, or
three consecutive identical failures **is** — stop, state what is needed, and
leave the ritual resumable.

## Output

Status lines while looping, not commentary: which role ran, what changed, tasks
remaining. Full detail only on a blocker or at completion.

## MCP Tools

| Tool | Purpose |
|------|---------|
| `health_check` | Pre-flight daemon check |
| `long_task_start` | Initialize ritual |
| `long_task_active` | Check for existing |
| `long_task_snapshot` | Load context |
| `long_task_update` | Record progress |
| `long_task_event` | Log blockers/checkpoints |
| `long_task_complete` | Mark done |
