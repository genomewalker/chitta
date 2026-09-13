# Shepherd — Error Patterns, Output Format, MCP Tools

Read this for known snakemake/nextflow error signatures, the expected status-line output format, and the full list of MCP tools this skill calls.

## Pattern Libraries

### Snakemake Patterns

| Pattern | Detection | Action |
|---------|-----------|--------|
| `MissingInputException` | `MissingInputException.+file:` | Check upstream rule, restart |
| `WorkflowError` | `WorkflowError:` | Parse message, recall fix |
| `CalledProcessError` | `CalledProcessError.+returned` | Extract return code, check logs |
| `ProtectedOutputException` | `ProtectedOutputException` | `--forceall` or unlock |
| `IncompleteFilesException` | `IncompleteFilesException` | `--rerun-incomplete` |
| `LockException` | `locked.+unlock` | `snakemake --unlock` |
| `Complete` | `\d+ of \d+ steps \(100%\)` | Mark complete |

### Nextflow Patterns

| Pattern | Detection | Action |
|---------|-----------|--------|
| `Process failed` | `Error executing process` | Check `.command.err` |
| `Completion` | `Completed at:` | Mark complete |
| `Cached` | `Cached process` | Progress checkpoint |
| `Submission` | `Submitted process` | Log progress |
| `Memory error` | `OutOfMemoryError` | Increase memory, restart |


## Output Format

```
[SHEPHERD] 2024-01-15T10:30:00Z - Pipeline running normally (12/50 rules)
[SHEPHERD] 2024-01-15T10:31:00Z - Pipeline running normally (15/50 rules)
[SHEPHERD] 2024-01-15T10:32:00Z - Error detected: MissingInputException
[SHEPHERD] 2024-01-15T10:32:05Z - Applied fix: check upstream dependency
[SHEPHERD] 2024-01-15T10:32:10Z - Restart #1 initiated
[SHEPHERD] 2024-01-15T10:33:00Z - Pipeline resumed (15/50 rules)
...
[COMPLETE] Pipeline finished successfully (50/50 rules)
```

## MCP Tools Reference

### Zellij Tools

| Tool | Purpose |
|------|---------|
| `create_named_pane` | Create isolated monitoring pane |
| `write_to_pane` | Send commands to pipeline |
| `tail_pane` | Incremental output reading |
| `wait_for_idle` | Detect stalls |
| `search_pane` | Pattern matching in output |
| `send_keys` | Control sequences (ctrl+c) |
| `read_pane` | Full pane content |

### Chitta Tools

| Tool | Purpose |
|------|---------|
| `long_task_start` | Initialize shepherd session |
| `long_task_active` | Check for existing session |
| `long_task_snapshot` | Resume context |
| `long_task_event` | Log checkpoints/errors |
| `long_task_complete` | Mark finished |
| `habit_match` | Pattern recognition |
| `habit_strengthen` | Reinforce successful fixes |
| `recall` | Find fixes from memory |
| `health_check` | Pre-flight validation |

