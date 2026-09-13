# Shepherd — Dashboard (`/shepherd dashboard`)

Read this when implementing or using the interactive control center across all active shepherd tasks.

## Dashboard: `/shepherd dashboard`

Interactive control center for monitoring all shepherd tasks.

### Initialize Dashboard

```javascript
// Load required tools
ToolSearch({ query: "chitta long_task habit_list recall" });
ToolSearch({ query: "zellij tail_pane read_pane list_named_panes" });
```

### Step 1: Gather State

```javascript
// Get all active shepherd tasks
tasks = mcp__chitta__advanced({
  tool: "long_task_active",
  arguments: {}
});

// Get recent shepherd task events
events = mcp__chitta__recall({
  query: "shepherd pipeline task event",
  tag: "shepherd",
  limit: 10
});

// Get pipeline-related habits
habits = mcp__chitta__habit_list({
  filter: "pipeline",
  min_strength: 0.2
});

// Get pane status for each task
pane_status = {};
for (task of active_tasks) {
  pane_name = extract_pane_name(task.work_items);  // "pane:NAME"
  if (pane_name) {
    output = mcp__zellij_mcp__tail_pane({ pane_name: pane_name, lines: 5 });
    pane_status[task.task_id] = {
      pane: pane_name,
      last_output: output.content,
      idle: output.idle
    };
  }
}
```

### Step 2: Present Dashboard

```
Questions:
  - question: |
      SHEPHERD DASHBOARD
      ==================

      Active Tasks: {active_count}
      Recent Errors: {error_count}
      Learned Habits: {habit_count}

      PIPELINES:
      {for task in tasks}
        [{task.status}] {task.task_id}
          Goal: {task.goal}
          Iterations: {task.iterations}
          Last update: {task.updated_at}
          Pane: {pane_status[task.task_id].pane}
          Output: {pane_status[task.task_id].last_output | truncate(80)}
      {/for}

      RECENT EVENTS:
      {for event in events | limit(5)}
        [{event.kind}] {event.task_id}: {event.payload | truncate(60)}
      {/for}

      Select an action:
    header: "Dashboard"
    options:
      - label: "View Task Details"
        description: "Show full snapshot of a shepherd task"
      - label: "View Pane Output"
        description: "Read recent output from a pipeline pane"
      - label: "Restart Pipeline"
        description: "Send restart command to a stalled pipeline"
      - label: "Stop Pipeline"
        description: "Send ctrl+c and mark task paused"
      - label: "Escalate"
        description: "Mark task as needing manual intervention"
      - label: "View Habits"
        description: "Show learned pipeline patterns"
      - label: "Refresh"
        description: "Update dashboard data"
```

### Step 3: Handle Actions

**View Task Details:**
```javascript
// Show full task snapshot
snapshot = mcp__chitta__long_task_snapshot({
  task_id: selected_task,
  mode: "debug"
});
output(snapshot);
// Offer to return to dashboard
```

**View Pane Output:**
```javascript
// Read last 100 lines from pane
output = mcp__zellij_mcp__read_pane({
  pane_name: pane_name,
  tail: 100
});
output(output.content);
```

**Restart Pipeline:**
```javascript
// Confirm restart
confirm = AskUserQuestion({
  question: `Restart ${task_id}? This will send ctrl+c and rerun.`,
  options: ["Yes, restart", "No, cancel"]
});

if (confirm === "Yes, restart") {
  // Stop current run
  mcp__zellij_mcp__send_keys({ pane_name: pane_name, keys: "ctrl+c" });
  await sleep(2000);

  // Rerun with --rerun-incomplete
  mcp__zellij_mcp__write_to_pane({
    pane_name: pane_name,
    chars: "snakemake --rerun-incomplete",
    press_enter: true
  });

  // Log event
  mcp__chitta__long_task_event({
    task_id: task_id,
    kind: "checkpoint",
    payload: JSON.stringify({ action: "manual_restart", source: "dashboard" }),
    tags: ["shepherd", "dashboard", "restart"]
  });

  output("Restart initiated for " + task_id);
}
```

**Stop Pipeline:**
```javascript
// Send ctrl+c
mcp__zellij_mcp__send_keys({ pane_name: pane_name, keys: "ctrl+c" });

// Update task status
mcp__chitta__long_task_event({
  task_id: task_id,
  kind: "checkpoint",
  payload: JSON.stringify({ action: "manual_stop", source: "dashboard" }),
  tags: ["shepherd", "dashboard", "stop"]
});

mcp__chitta__long_task_update({
  task_id: task_id,
  blockers: ["Manually stopped via dashboard"]
});

output("Pipeline stopped: " + task_id);
```

**Escalate:**
```javascript
// Mark as needing intervention
mcp__chitta__long_task_event({
  task_id: task_id,
  kind: "error",
  payload: JSON.stringify({ action: "escalated", source: "dashboard", reason: user_reason }),
  tags: ["shepherd", "dashboard", "escalate"]
});

mcp__chitta__long_task_update({
  task_id: task_id,
  blockers: ["ESCALATED: " + user_reason]
});

// Send alert
mcp__chitta__msg_send({
  recipient: "user",
  message: "[SHEPHERD ESCALATION] " + task_id + ": " + user_reason
});

output("Escalated: " + task_id);
```

**View Habits:**
```javascript
habits = mcp__chitta__habit_list({
  filter: "pipeline",
  min_strength: 0.1
});

output("LEARNED PIPELINE PATTERNS:");
output("==========================");
for (habit of habits.habits) {
  output(`[${habit.strength.toFixed(2)}] ${habit.trigger}`);
  output(`  -> ${habit.response}`);
  output(`  Success rate: ${habit.success_count}/${habit.attempt_count}`);
  output("");
}
```

### Dashboard Output Format

```
SHEPHERD DASHBOARD
==================

Active Tasks: 2
Recent Errors: 1
Learned Habits: 5

PIPELINES:
  [active] shepherd-1707912345
    Goal: Monitor metagenome assembly pipeline
    Iterations: 3
    Last update: 2024-02-14T10:30:00Z
    Pane: pipeline-assembly
    Output: [89/120 rules] Running rule megahit_assembly...

  [active] shepherd-1707915678
    Goal: Monitor annotation pipeline
    Iterations: 1
    Last update: 2024-02-14T10:28:00Z
    Pane: pipeline-annot
    Output: Submitted batch job 12345678

RECENT EVENTS:
  [checkpoint] shepherd-1707912345: {"state":"running","rules":"89/120"}
  [error] shepherd-1707912345: {"pattern":"slurm_timeout","severity":"warning"}
  [checkpoint] shepherd-1707915678: {"state":"started","rules":"0/45"}

[1] View Task Details  [2] View Pane Output  [3] Restart Pipeline
[4] Stop Pipeline      [5] Escalate          [6] View Habits
[7] Refresh

Select action: _
```

### Quick Actions Summary

| Action | Command | Effect |
|--------|---------|--------|
| View details | Select task | Show `long_task_snapshot` |
| View output | Select task | Show `read_pane` last 100 lines |
| Restart | Confirm | ctrl+c + rerun + log event |
| Stop | Confirm | ctrl+c + add blocker |
| Escalate | Add reason | Log error + send alert |
| Habits | - | Show `habit_list` for pipelines |
| Refresh | - | Re-query all state |
