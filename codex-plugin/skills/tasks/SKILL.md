---
name: tasks
description: Browse and resume tasks, threads, and background jobs across sessions
execution: direct
---

Interactive task browser. Shows active threads, pending inbox items, and running jobs across all sessions. Also supports registering new tasks.

## Behavior

1. Query the task ledger for active threads and pending inbox items for the current realm.
2. Present them using AskUserQuestion so the user can select what to focus on.
3. For the selected thread/task, load the resume capsule and surface the context.
4. If "Add new task" is selected, collect title + description and create a new thread.

## Implementation

Run this Python snippet to gather data:

```python
import subprocess, json, os

mcp_dir = os.path.expanduser("~/.claude/plugins/cache/genomewalker-chitta/chitta/$(cat ~/.claude/plugins/cache/genomewalker-chitta/chitta/.version 2>/dev/null || echo 'latest')/chitta-mcp")
# Fallback: find it relative to the skills dir
import pathlib
skill_file = pathlib.Path(__file__) if '__file__' in dir() else pathlib.Path('.')
```

Actually, use the Bash tool to gather the data, then use AskUserQuestion.

### Step 1 — gather data

```bash
_BASE="$HOME/.claude/plugins/cache/genomewalker-chitta/chitta"
[ -d "$_BASE" ] || _BASE="$HOME/.claude/plugins/cache/genomewalker-cc-soul/cc-soul"
_VER="$(cat "$(dirname "$_BASE")/.version" 2>/dev/null || ls -v "$_BASE" | tail -1)"
_MCP_DIR="$_BASE/$_VER/chitta-mcp"

# Active threads
python3 "$_MCP_DIR/task_ledger.py" thread_list --status active --limit 10

# Pending inbox
python3 "$_MCP_DIR/task_ledger.py" inbox_list --state pending --limit 20
```

### Step 2 — present with AskUserQuestion

Build options from threads + inbox items. Each option label should be:
- For threads: `[thread] <title> (last active: <relative time>)`
- For inbox completed: `✓ <digest[:80]>`
- For inbox failed: `✗ <digest[:80]>`

Always include these fixed options at the bottom:
- "Add new task" — register a new persistent thread
- "Dismiss all inbox" — ack all pending inbox items
- "Nothing / close" — no action

### Step 3 — act on selection

**Thread selected**: Run resume capsule and display it:
```bash
python3 "$_MCP_DIR/resume_capsule.py" build --thread-id <thread_id>
```
Parse the `summary` field and present it as context. Then ask the user if they want to continue that thread or just view it.

**Inbox item selected**: Acknowledge it:
```bash
python3 "$_MCP_DIR/task_ledger.py" inbox_ack --item-id <item_id> --state acked
```
Then summarize what happened.

**"Dismiss all inbox"**: Ack all pending items for the realm.

**"Add new task"**:
1. Ask the user for a title (AskUserQuestion, free text via "Other").
2. Ask for a short description of what needs to be done (AskUserQuestion, free text via "Other").
3. Detect the current realm:
```bash
~/.claude/bin/chitta realm_detect 2>/dev/null || echo "global"
```
4. Create the thread:
```bash
python3 "$_MCP_DIR/task_ledger.py" thread_create --title "<title>" --realm "<realm>"
```
5. Store the description as the first inbox item so it survives across sessions:
```bash
python3 "$_MCP_DIR/task_ledger.py" inbox_push \
  --thread-id <thread_id> \
  --event-type note \
  --digest "<description[:120]>" \
  --target-realm "<realm>"
```
6. Confirm: print the thread ID and title, tell the user it will appear in `/tasks` next session.

## Notes

- The MCP dir can be found reliably as: the directory containing `task_ledger.py` under `$HOME/.claude/plugins/cache/genomewalker-chitta/` (or the pre-rename `genomewalker-cc-soul/`, if that's what's installed).
- The ledger lives in the chitta daemon (`ledger_op` RPC); if the daemon is down every call returns empty — say "No tracked tasks yet (daemon unreachable)."
- Use `realm_detect` via chitta if REALM is not set: `chitta realm_detect`
