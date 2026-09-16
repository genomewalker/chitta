---
name: recap
description: "Rebuild working context from transcripts and chitta memories in approximately 1500 tokens. Use when starting a new session to continue previous work."
---

# Recap: multimodel-safe session continuation

Reconstruct a bounded working context from an exact Claude or Codex transcript.
Claude and Codex share one Chitta registry and one thread ledger. Never choose a
transcript from global file recency and never resume a live session.

## 1. Resolve the shared implementation

Locate `chitta-mcp/resume_selector.py` in this plugin root. Prefer
`$CHITTA_PLUGIN_DIR` (falling back to `$CC_SOUL_PLUGIN_DIR`), then the source
root containing this skill, then the newest installed
`~/.claude/plugins/cache/genomewalker-chitta/chitta/*` (or the pre-rename
`genomewalker-cc-soul/cc-soul/*`), then the Python environment from the
`chitta-mcp` executable (read its shebang and import `resume_selector` to
locate the module directory).
If the selector is unavailable, stop and report that chitta must be updated;
do not fall back to "newest transcript" heuristics.

Set the invoking client to `codex` when a `CODEX_*` session variable is present,
otherwise `claude` when a `CLAUDE_*` session variable is present. The selector
also identifies the current session from its registered ancestor PID.

## 2. Inventory before selecting

Run:

```bash
python3 "$_MCP_DIR/resume_selector.py" --project-dir "$PWD" --client "$CLIENT"
```

Arguments map as follows:

- `/recap <session_id>`: add `--session-id <session_id>`.
- `/recap <thread id or title>`: add `--thread <value>`.
- `/recap last N`: do not select; show the first N candidates.

Always surface the `live_sessions` inventory before proceeding. Show client,
model, short session ID, project, thread, and PID. This inventory includes both
Claude and Codex, so parallel work in the same project is visible.

Handle `status` strictly:

- `registry_unavailable`: Chitta did not answer the liveness query. Stop and
  retry later; never interpret a timeout as an empty live-session list.
- `locked`: report the owning live session/client/thread. Do not read, recap, or
  resume it.
- `ambiguous`: show the candidate session/client/project/thread choices and ask
  the user to choose. Never break a tie using mtime alone.
- `none`: report that no registered or project-exact historical transcript was
  found.
- `selected`: continue with that exact session and transcript only.

## 3. Atomically claim the thread

Before reading or presenting continuation context, rerun the same command with
`--claim`. This closes the race between selection and takeover. Continue only
when `claim.claimed` is true. If the claim reports `live_owner`, show that owner
and stop. Never use `--force` unless the user explicitly asks to take over a
known live thread.

The selector may create a durable thread for a historical transcript that
predates thread tracking. It records the current session as the new owner; do
not write a global `.current_thread_id` file.

## 4. Read bounded exact context

Use the selected `session_id` with Chitta's exact transcript reader, or parse
the selected `transcript_path` if MCP is unavailable. Read at most:

- First 20 and last 30 user turns, 180 characters each.
- Last 20 assistant turns, 240 characters each.

Support both formats:

- Claude: top-level `type=user|assistant`, content in `message.content`.
- Codex: top-level `type=response_item`, with
  `payload.type=message`, `payload.role`, and `payload.content`.

Ignore environment blocks, system/developer reminders, command markup, task
notifications, interruption markers, and duplicate messages. User corrections
and explicit scope changes have highest priority.

Query Chitta smart context using the recovered task as the query, then inspect
`git status --short` and `git log --oneline -5` in the selected project.

## 5. Produce the recap

Keep the result near 1500 tokens:

```markdown
## Session Recap: <short session id> (<client/model>)

### What was being done
<primary task and scope>

### Key decisions
<important choices and user corrections>

### Current state
- Project/branch
- Uncommitted files
- Other live Claude/Codex sessions in this project

### Pending work
<unfinished work and next action>

### Key context
<critical errors, constraints, and architectural facts>
```

Do not replay full tool output. Prefer decisions and pending work over a diary of
actions. Mention files only when they are still active.
