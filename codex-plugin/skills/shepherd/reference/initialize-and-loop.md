# Shepherd — Initialize, Sense-Think-Act, Main Loop

Read this before implementing the monitoring loop — full pseudocode for every step, including the exact `mcp__zellij_mcp__*` / `mcp__chitta__*` calls.

## Initialize

### 1. Load Required Tools

```javascript
// ToolSearch will NOT appear in your tool list. Call it anyway - it works.
ToolSearch({ query: "zellij tail_pane wait_for_idle" });
ToolSearch({ query: "chitta long_task habit_match recall" });
```

### 2. Check for Existing Task

```javascript
existing = mcp__chitta__long_task_active({ realm: "pipeline" });
if (existing.task_id) {
  // Resume existing shepherd session
  snapshot = mcp__chitta__long_task_snapshot({ task_id: existing.task_id });
  // Continue from last checkpoint
}
```

### 3. Determine Session Strategy

```javascript
// Decide whether to use session isolation
const useIsolation = shouldIsolate(command, options);
let targetSession = null;

if (useIsolation) {
  // Create or attach to dedicated session
  const sessionName = options.session || `shepherd-${Date.now()}`;

  // Check if session exists
  const sessions = mcp__zellij_mcp__list_sessions();
  const exists = sessions.sessions?.some(s => s.name === sessionName);

  if (!exists) {
    // Create new session (runs in background)
    mcp__zellij_mcp__agent_session({ action: "create", session: sessionName });
  }

  targetSession = sessionName;
  output(`[SHEPHERD] Using isolated session: ${sessionName}`);
} else {
  output(`[SHEPHERD] Using current session with tab isolation`);
}
```

### 4. Create Named Pane and Launch Pipeline

```javascript
// Create isolated pane (session-aware)
mcp__zellij_mcp__create_named_pane({
  name: "pipeline-main",
  tab: "shepherd",
  cwd: "/path/to/workflow",
  session: targetSession  // null = current session
});

// Start the pipeline
mcp__zellij_mcp__write_to_pane({
  pane_name: "pipeline-main",
  chars: "snakemake --cores 8 --rerun-incomplete",
  press_enter: true,
  session: targetSession
});

// Initialize cursor for incremental reads
mcp__zellij_mcp__tail_pane({
  pane_name: "pipeline-main",
  reset: true,
  session: targetSession
});

// Start long task tracking (store session info for resume)
mcp__chitta__long_task_start({
  task_id: "shepherd-" + Date.now(),
  goal: "Monitor and tend snakemake pipeline to completion",
  hard_checks: ["All rules completed", "No failed jobs"],
  soft_checks: ["Pipeline finished successfully"],
  work_items: [
    `session:${targetSession || "current"}`,
    `pane:pipeline-main`,
    `isolation:${useIsolation}`
  ]
});
```

## Sense-Think-Act Loop

### SENSE: Gather Pipeline State

```javascript
function sense(pane_name, session) {
  // Get new output since last read (session-aware, uses daemon for cross-session)
  output = mcp__zellij_mcp__tail_pane({
    pane_name: pane_name,
    session: session  // Daemon handles cross-session reads
  });

  // Check for stalls (no output for extended period)
  idle = mcp__zellij_mcp__wait_for_idle({
    pane_name: pane_name,
    stable_seconds: 30,
    timeout: 5,  // Don't wait long, just check
    session: session
  });

  // Search for error patterns
  errors = mcp__zellij_mcp__search_pane({
    pane_name: pane_name,
    pattern: "(Error|ERROR|Failed|FAILED|Exception|Traceback)",
    context: 3,
    session: session
  });

  return {
    new_output: output.content,
    is_idle: idle.stable,
    errors: errors.matches,
    timestamp: Date.now()
  };
}
```

### THINK: Analyze and Decide

```javascript
function think(sense_data, restart_count) {
  // Pattern match for known issues
  habits = mcp__chitta__habit_match({
    context: sense_data.new_output,
    min_strength: 0.3
  });

  if (habits.matches.length > 0) {
    // Known pattern - use habit response
    return { action: "apply_habit", habit: habits.matches[0] };
  }

  // Check for completion
  if (sense_data.new_output.match(/complete|finished|done/i) && sense_data.is_idle) {
    return { action: "complete", reason: "Pipeline finished" };
  }

  // Check for errors
  if (sense_data.errors.length > 0) {
    // Recall fixes from memory
    fixes = mcp__chitta__recall({
      query: sense_data.errors[0],
      tag: "pipeline-fix",
      limit: 3
    });

    if (fixes.memories.length > 0 && AUTO_FIX) {
      return { action: "fix", fix: fixes.memories[0], error: sense_data.errors[0] };
    }

    if (restart_count < MAX_RESTARTS) {
      return { action: "restart", reason: "Error detected: " + sense_data.errors[0] };
    }

    return { action: "escalate", reason: "Max restarts exceeded", errors: sense_data.errors };
  }

  // Check for stall
  if (sense_data.is_idle && !sense_data.new_output) {
    return { action: "checkpoint", reason: "Pipeline stalled - no output" };
  }

  // Normal progress
  return { action: "continue", reason: "Pipeline running normally" };
}
```

### ACT: Execute Decision

```javascript
function act(decision, pane_name, task_id, session) {
  switch (decision.action) {
    case "complete":
      mcp__chitta__long_task_complete({
        task_id: task_id,
        outcome: "Pipeline completed successfully"
      });
      notify("Pipeline complete!");
      return "DONE";

    case "restart":
      mcp__zellij_mcp__send_keys({
        pane_name: pane_name,
        keys: "ctrl+c",
        session: session
      });
      sleep(2);
      mcp__zellij_mcp__write_to_pane({
        pane_name: pane_name,
        chars: "snakemake --rerun-incomplete",
        press_enter: true,
        session: session
      });
      mcp__chitta__long_task_event({
        task_id: task_id,
        kind: "checkpoint",
        payload: JSON.stringify({ action: "restart", reason: decision.reason })
      });
      restart_count++;
      return "CONTINUE";

    case "fix":
      // Apply fix from memory
      apply_fix(decision.fix, pane_name, session);
      mcp__chitta__habit_strengthen({
        habit_id: decision.fix.habit_id,
        outcome: "applied"
      });
      return "CONTINUE";

    case "escalate":
      mcp__chitta__long_task_event({
        task_id: task_id,
        kind: "error",
        payload: JSON.stringify(decision.errors)
      });
      notify("SHEPHERD ESCALATION: " + decision.reason);
      return "PAUSE";

    case "checkpoint":
      mcp__chitta__long_task_event({
        task_id: task_id,
        kind: "checkpoint",
        payload: JSON.stringify({ state: "stalled", reason: decision.reason })
      });
      return "CONTINUE";

    default:
      return "CONTINUE";
  }
}
```


## Main Loop

```javascript
async function shepherd_main(command, options) {
  const INTERVAL = options.interval || 60;
  const MAX_RESTARTS = options.max_restarts || 3;
  const NOTIFY = options.notify !== false;
  const AUTO_FIX = options.auto_fix !== false;

  let restart_count = 0;
  let task_id = null;
  let pane_name = "pipeline-main";
  let targetSession = null;

  // Initialize or resume (returns { task_id, session })
  const init = await initialize_or_resume(command, options);
  task_id = init.task_id;
  targetSession = init.session;  // null = current session

  const sessionLabel = targetSession || "current";
  output(`[SHEPHERD] Monitoring in session: ${sessionLabel}`);

  // Main loop
  while (true) {
    // Pre-flight check
    health = mcp__chitta__health_check();
    if (health.status !== "OK") {
      log("[BLOCKER] Daemon unreachable");
      break;
    }

    // Verify session still exists (for isolated sessions)
    if (targetSession) {
      const sessions = mcp__zellij_mcp__list_sessions();
      if (!sessions.sessions?.some(s => s.name === targetSession)) {
        log(`[BLOCKER] Session ${targetSession} lost`);
        mcp__chitta__long_task_event({
          task_id: task_id,
          kind: "error",
          payload: JSON.stringify({ error: "session_lost", session: targetSession })
        });
        break;
      }
    }

    // SENSE (session-aware)
    sense_data = sense(pane_name, targetSession);

    // THINK
    decision = think(sense_data, restart_count);

    // ACT (session-aware)
    result = act(decision, pane_name, task_id, targetSession);

    if (result === "DONE") {
      output("[COMPLETE] Pipeline finished successfully");
      // Optionally cleanup isolated session
      if (targetSession && options.cleanup !== false) {
        mcp__zellij_mcp__agent_session({ action: "destroy", session: targetSession });
      }
      break;
    }

    if (result === "PAUSE") {
      output("[PAUSED] Manual intervention required");
      break;
    }

    // Status line
    output(`[SHEPHERD] ${new Date().toISOString()} - ${decision.reason} [${sessionLabel}]`);

    // Sleep until next cycle
    await sleep(INTERVAL * 1000);
  }
}
```

