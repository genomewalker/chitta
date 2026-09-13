---
name: shepherd
aliases: [watch, monitor, tend, guard, pipeline]
description: Autonomous pipeline monitor using sense-think-act loop. Watches snakemake/nextflow jobs, detects errors, applies fixes from memory, restarts on failure.
execution: direct
hooks:
  Stop:
    - matcher: ""
      hooks:
        - type: command
          command: "bash $CLAUDE_PLUGIN_ROOT/hooks/shepherd-stop-hook.sh"
  UserPromptSubmit:
    - matcher: ""
      hooks:
        - type: command
          command: "bash $CLAUDE_PLUGIN_ROOT/hooks/shepherd-prompt-hook.sh"
  PostToolUse:
    - matcher: "Bash"
      hooks:
        - type: prompt
          prompt: |
            You are a pipeline safety monitor. The following is the output from a Bash command run during pipeline monitoring.

            $ARGUMENTS

            Evaluate ONLY for CATASTROPHIC issues — data corruption, filesystem full, permission denied on critical output, segfault in main process, or unrecoverable SLURM/HPC failures (CANCELLED, NODE_FAIL).

            Return {"ok": true} if the output is normal, shows expected errors (retryable), or is just progress output.
            Return {"ok": false, "reason": "CRITICAL: <one-line description>"} ONLY for genuinely catastrophic failures that require immediate human intervention.

            Be conservative — most errors are retryable and should return ok: true.
          timeout: 10
---

# Shepherd - Autonomous Pipeline Monitor

Tends long-running pipelines (snakemake, nextflow) using a sense-think-act loop. Detects errors, recalls fixes from memory, restarts automatically, checkpoints progress.

## Quick Commands

```bash
/shepherd <command> [--interval=60] [--max-restarts=3]  # Start monitoring
/shepherd status                                         # Check shepherd status
/shepherd stop                                          # Stop monitoring
```

## Configuration

| Flag | Default | Description |
|------|---------|-------------|
| `--interval` | 60 | Seconds between sense cycles |
| `--max-restarts` | 3 | Max automatic restarts before escalating |
| `--notify` | true | Send notifications on events |
| `--auto-fix` | true | Attempt automatic fixes from memory |
| `--session` | auto | Target session: "auto", "current", or session name |
| `--isolate` | auto | Session isolation: "auto", "yes", "no" |

`--isolate=auto` picks isolation by job type (SLURM/remote-SSH/multiple parallel
pipelines get an isolated session; quick local or interactive jobs don't). Read
`reference/configuration.md` before implementing this decision — it has the full
condition table and the `shouldIsolate` logic.

## Core Workflow

1. **Initialize** — load the `zellij_mcp`/`chitta` tools via `ToolSearch`, check for
   an existing `long_task` to resume, decide session isolation, create the
   monitoring pane, and launch the pipeline.
2. **Sense** — tail the pane, check for idle/stall, search for error patterns.
3. **Think** — match sensed output against known habits and pattern libraries;
   decide to apply a habit, restart, fix from memory, checkpoint, or escalate.
4. **Act** — execute the decision: restart the command, apply a fix, log an
   event, notify, or mark the task complete/paused.
5. **Main loop** — repeat sense/think/act on `--interval`, with a pre-flight
   `health_check` and session-liveness check each cycle, until done or paused.

Read `reference/initialize-and-loop.md` before implementing any of the steps
above — it has the full pseudocode, including every `mcp__zellij_mcp__*` and
`mcp__chitta__*` call.

Read `reference/patterns-and-tools.md` for known snakemake/nextflow error
signatures, the expected status-line output format, and the full MCP tool list.

Read `reference/resume-protocol.md` when resuming a shepherd task after a
disconnect or session restart.

## Anti-Patterns

- Never poll faster than 30 seconds (wastes resources)
- Never restart without checkpointing first
- Never exceed max_restarts without escalating
- Never ignore repeated errors (they compound)
- Never fix without logging (lose learnings)

## Dashboard

`/shepherd dashboard` opens an interactive control center across all active
shepherd tasks — view task details or pane output, restart/stop/escalate a
pipeline, or view learned habits. Read `reference/dashboard.md` for the full
implementation before building or running it.
