# Shepherd — Configuration & Session Isolation

Read this when tuning shepherd flags or deciding whether a pipeline run needs an isolated zellij session.

## Configuration

| Flag | Default | Description |
|------|---------|-------------|
| `--interval` | 60 | Seconds between sense cycles |
| `--max-restarts` | 3 | Max automatic restarts before escalating |
| `--notify` | true | Send notifications on events |
| `--auto-fix` | true | Attempt automatic fixes from memory |
| `--session` | auto | Target session: "auto", "current", or session name |
| `--isolate` | auto | Session isolation: "auto", "yes", "no" |

### Session Isolation Strategy

When `--isolate=auto` (default), shepherd chooses based on task characteristics:

| Condition | Isolation | Reason |
|-----------|-----------|--------|
| HPC/SLURM job | yes | Long-running, survives disconnects |
| Remote SSH pipeline | yes | Needs persistent attachment |
| Quick local task (<1h) | no | Tab isolation sufficient |
| Multiple parallel pipelines | yes | Separate sessions per pipeline |
| Interactive monitoring | no | Keep in current session |

```javascript
function shouldIsolate(command, options) {
  if (options.isolate === "yes") return true;
  if (options.isolate === "no") return false;

  // Auto-detect based on command/context
  if (command.includes("sbatch") || command.includes("srun")) return true;
  if (options.ssh_session) return true;
  if (command.includes("--jobs") && parseInt(command.match(/--jobs\s*(\d+)/)?.[1]) > 4) return true;

  return false;  // Default: use tab isolation
}
```

