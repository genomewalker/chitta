# Shepherd — Resume Protocol

Read this when resuming a shepherd task after a disconnect or session restart.

## Resume Protocol

When resuming from checkpoint:

```javascript
// 1. Load task snapshot
snapshot = mcp__chitta__long_task_snapshot({ task_id: task_id, mode: "debug" });

// 2. Extract session info from work_items
const sessionItem = snapshot.work_items?.find(w => w.startsWith("session:"));
const targetSession = sessionItem?.split(":")[1];
const session = targetSession === "current" ? null : targetSession;

// 3. If isolated session, ensure it's still alive
if (session) {
  const sessions = mcp__zellij_mcp__list_sessions();
  const alive = sessions.sessions?.some(s => s.name === session);
  if (!alive) {
    output(`[SHEPHERD] Session ${session} no longer exists - recreating`);
    mcp__zellij_mcp__agent_session({ action: "create", session: session });
  }
}

// 4. Restore pane state
panes = mcp__zellij_mcp__list_named_panes({ session: session });
if (!panes.panes?.some(p => p.name === "pipeline-main")) {
  // Recreate pane
  mcp__zellij_mcp__create_named_pane({
    name: "pipeline-main",
    tab: "shepherd",
    session: session
  });
}

// 5. Check pipeline state
state = mcp__zellij_mcp__read_pane({
  pane_name: "pipeline-main",
  tail: 50,
  session: session
});

// 6. Decide: resume or restart
if (state.content?.match(/waiting|running|submitted/i)) {
  // Pipeline still running, just monitor
  continue_loop(session);
} else {
  // Pipeline stopped, restart from checkpoint
  restart_pipeline(session);
}
```

