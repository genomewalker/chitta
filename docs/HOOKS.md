# chitta Hooks System

Status as of 2026-09-13.

chitta integrates with Claude Code and Codex through the hooks system, enabling
automatic context injection and lifecycle management.

---

## Table of Contents

- [Overview](#overview)
- [Hook Files](#hook-files)
- [Hook Events](#hook-events)
- [PreToolUse — pre-tool-hook.sh](#pretooluse--pre-tool-hooksh)
- [Code-Intel Shadow Logging and Enforcement](#code-intel-shadow-logging-and-enforcement)
- [Environment Variables](#environment-variables)
- [Configuration](#configuration)
- [Transparent Memory](#transparent-memory)
- [Proactive Learning](#proactive-learning)
- [Subconscious Daemon](#subconscious-daemon)
- [Custom Hooks](#custom-hooks)
- [Troubleshooting](#troubleshooting)

---

## Overview

Hooks are shell commands that execute in response to Claude Code events. chitta uses hooks for:

1. **Context injection** — Soul state and relevant memories appear automatically
2. **Transparent memory** — Memories surface without explicit MCP calls
3. **Proactive learning** — Corrections, preferences, and milestones detected automatically
4. **Session continuity** — State saved and restored across sessions via ledger
5. **Tool safety** — Destructive commands blocked, expensive reads truncated
6. **Background processing** — Subconscious daemon management

### Two Hook Systems

chitta provides hooks in two forms:

| System           | Files                                                                               | Used When                        |
|------------------|-------------------------------------------------------------------------------------|----------------------------------|
| **Plugin hooks** | `hooks/hooks.json` + `hooks/*.sh`                                                   | Installed as Claude Code plugin  |
| **Settings hooks** | `hooks/*.sh` + `~/.claude/settings.json`                                          | Standalone installation          |

The `smart-install.sh` script configures the appropriate system automatically.

### Hook Output Format

All lifecycle hooks write JSON `hookSpecificOutput` schema on stdout. This is not the old "SSL format" (`[conf%:type]`) from earlier versions — that format is obsolete.

---

## Hook Files

| Script                  | Event            | Purpose                                                                              |
|-------------------------|------------------|--------------------------------------------------------------------------------------|
| `session-start-hook.sh` | SessionStart     | Realm detection, code re-indexing, soul context injection, ledger restore, git diff  |
| `prompt-hook.sh`        | UserPromptSubmit | Wraps `prompt-core.sh`: six-lane realm-scoped recall, admission filtering, code symbol injection, anticipation prediction, learning hints |
| `prompt-core.sh`        | UserPromptSubmit | The recall fan-out and admission policy itself. See the [recall pipeline page](https://genomewalker.github.io/chitta/recall.html) |
| `stop-hook.sh`          | Stop             | Extract `[SOLUTION]`, `[GOTCHA]`, `[PREFERENCE]`, `[DECISION]`, `[FAILURE]`, `[PATTERN]`, `[LEARN]` blocks; anticipation recording; ledger save |
| `pre-compact-hook.sh`   | PreCompact       | Save ledger checkpoint before context compaction                                     |
| `pre-tool-hook.sh`      | PreToolUse       | Safety checks, file dedup, large-file truncation, Write guards, Agent routing, ScheduleWakeup budgeting |
| `post-bash-hook.sh`     | PostToolUse:Bash | Check the command result, append a `bash_outcome` event to the outcome ledger        |
| `outcome-ledger.sh`     | Library          | Fail-open JSONL append of `injected` / `recall_empty` / `bash_outcome` / `session_end` events |
| `session-end-hook.sh`   | SessionEnd       | Session teardown and the closing ledger event                                        |
| `resume-inject-hook.sh` | SessionStart:resume | Inject the recap capsule when a session resumes                                   |
| `compact-restore-hook.sh` | SessionStart:compact | Restore context after a compaction                                             |
| `subagent-stop-hook.sh` | SubagentStop     | Capture what a subagent learned, asynchronously                                      |
| `file-changed-hook.sh`  | FileChanged      | Re-index a changed file, asynchronously                                              |
| `log-bash-history.sh`   | PostToolUse (async) | Append every Bash command to history for pattern learning                         |
| `memory-intercept.sh`   | PostToolUse:Write (async) | Capture Write operations to learn what you build                           |
| `distill.sh`            | Background       | Transcript distillation into compressed wisdom nodes                                 |
| `subconscious.sh`       | SessionStart     | Start/stop/restart/status for the chittad daemon                                     |

**Where these run.** Inside `hooks.json`, lifecycle handlers resolve against
`${CLAUDE_PLUGIN_ROOT}` — the plugin cache — while the `PreToolUse` matchers and
most `PostToolUse` matchers resolve against `${HOME}/.claude/hooks`. A source
checkout is neither, so editing one changes nothing until you install.
`scripts/dev-install.sh` symlinks both live paths back at a checkout; re-run it
after a plugin update, which can replace those symlinks with a fresh clone.

**Communication:** All scripts use a Unix domain socket to talk to the daemon (fast path). Falls back to the `chitta` thin client if the socket is unavailable. Socket path is derived from the mind path using a djb2 hash: `/tmp/chitta-{hash}.sock`.

---

## Hook Events

### SessionStart

**What chitta Does:**
1. Auto-install binaries if needed (`smart-install.sh`)
2. Start subconscious daemon (`subconscious.sh start`)
3. Detect current realm/project
4. Incremental code re-indexing (only changed files)
5. Auto-register transcript for distillation
6. Load soul context (nodes, triplets, confidence, status)
7. Surface user profile (expertise, preferences, style)
8. Load active goals
9. Surface behavioral corrections
10. Load ledger checkpoint (pending tasks, next steps, mood)
11. Detect git changes since last session

### Stop

**What chitta Does:**
1. Read Claude's response from stdin
2. Extract tagged blocks: `[SOLUTION]`, `[GOTCHA]`, `[PREFERENCE]`, `[DECISION]`, `[FAILURE]`, `[PATTERN]`, `[LEARN]`
3. Auto-detect missed learning opportunities (corrections, preferences not explicitly tagged)
4. Record anticipation patterns (context to action)
5. Verify predictions from earlier anticipation
6. Auto-checkpoint every 5 turns or on meaningful work
7. Save ledger with session state

### UserPromptSubmit

**What chitta Does:**
1. Extract user message from stdin JSON
2. Context recovery: if user says "continue"/"resume", re-inject full soul context
3. Detect learning opportunities (corrections, preferences, frustration, milestones)
4. Determine proactive surfacing tags
5. Run `full_resonate(query)` via daemon socket
6. Filter results: only ≥25% relevance, strip type prefixes, max 500 chars
7. Surface code symbols if query is code-related
8. Run `anticipation_predict` for context-based predictions
9. Output combined context

### PostToolUse

**What chitta Does:**
- **post-bash-hook.sh**: Records significant Edit/Write operations as signals for background learning
- Note: `capture-hook.sh` is currently disabled (exits immediately). Write safety checks are handled by `pre-tool-hook.sh`, not the post-bash hook.

### Recall scheduling telemetry

The prompt hook appends a compact timing field to its admission summary:

```text
[admit] C2:KNOWN(55%) sem:1 hyb:1 | drop conf:2 | t:sem=412,hyb=3105!,kw=88,total=3270
```

Times are wall milliseconds. `!` means that lane either reached `timeout` or
returned an empty output file. Only attempted lanes are shown; `total` is hook
wall time measured just before context emission.

The outcome ledger's `injected` event carries the same data as structured
`lane_ms` and `lane_timeout` objects plus `hook_ms`. If recall completes without
usable context, including after the cross-realm fallback, the hook appends a
`recall_empty` event with those timing fields even though it emits no context.
This keeps empty prompt-hook runs diagnosable without changing fail-open output.

Set `CHITTA_RECALL_LANES_RPC=1` to replace the prompt hook's six recall CLI
processes with one `recall_lanes` RPC. The switch defaults off while latency is
measured. Ablated lanes are omitted from the request, the cross-realm `xr` lane
remains a separate fallback, and an unavailable or invalid fan-in response
falls open to the original process fan-out for that prompt.

### PreToolUse

Handled by `pre-tool-hook.sh`. See the [full section below](#pretooluse--pre-tool-hooksh).

### PreCompact

**What chitta Does:**
1. Save current state to ledger checkpoint
2. Record that compaction is happening
3. Ensure work state is preserved for continuation

---

## PreToolUse — pre-tool-hook.sh

`pre-tool-hook.sh` is the main safety and intelligence orchestrator. It handles these tool matchers:

```
Read | Edit | Write | Bash | Agent | ScheduleWakeup
```

### Bash Matcher

**Stage 1 — Safety blocks (exit 2, command is cancelled):**

| Pattern                      | Block reason                    |
|------------------------------|---------------------------------|
| `rm -rf /` or `rm -rf ~`    | Destroys root or home           |
| `chmod -R 777 /`            | Exposes entire filesystem       |
| `dd` to a raw disk device   | Overwrites disk directly        |

**Stage 2 — Find/grep/ls fallback strategy:**

- Unbounded `find` on `/` or `~` gets scoped: `chitta recall` is called first to get a memory hint; `find` is limited to a specific directory with `-maxdepth 3`. Set `CHITTA_DEEP_SEARCH=1` to allow root-wide search.
- Grep content mode is capped at 50 matches via `head_limit` injection (not 200 — the 200 figure in older docs was wrong).
- Glob results are capped at 100 entries via `head_limit` injection.

**Stage 3 — Bash pattern recall (2s timeout, realm-scoped):**

Detects command patterns and injects relevant corrections and gotchas from memory:

| Pattern detected | Memory tags queried |
|-----------------|---------------------|
| R / Rscript     | r-lang, bioconductor |
| Python / conda  | python, conda, environment |
| git             | git, version-control |
| cmake / make    | cmake, build        |
| daemon / systemctl | daemon, service  |

**Task ledger pre-stage:**

Detects trackable commands (`sbatch`, `srun`, `nohup`, python scripts, bash scripts) and snapshots the current file state for provenance tracking before the command runs.

### Read Matcher

1. **Per-turn dedup**: A sentinel file `$MIND/.soul_injected_<session>_<turn>` ensures soul recall is injected only once per turn, not on every Read call.
2. **Realm detection**: Calls `chitta realm_detect` before injecting any recall. If realm detection fails, injection is skipped entirely to prevent cross-project memory bleed.
3. **Code-intel advisory**: If the file is indexed in chitta, suggests using `read_symbol` or `smart_context` instead of reading the whole file. Advisory only appears if chitta has indexed the file.
4. **Large-file truncation**: Reads of files >200 lines are truncated to the first 150 lines. (Older docs said ">500 lines → head -200"; the actual thresholds are 200 and 150.)
5. **Read dedup cache**: Tracks file reads by mtime hash at `$MIND/.read_cache_<session>`. Repeat reads of unchanged files return a 13-token `§ref:HASH§` reference via sqz instead of full content.
6. **Strict-mode enforcement for indexed files**: When enforcement is active, reading a fully-indexed file is blocked with a suggestion to use symbol-level tools instead. Bypass for the session with `CHITTA_ALLOW_READ=1` or by creating the flag file `$MIND/.allow_read_<session_id>`.
7. **System-path exception**: Enforcement is skipped for paths under `/site-packages`, `/usr/lib`, `/opt/*/lib`, and similar system locations — those files are never indexed.

### Write Matcher

1. **Blocks temp patch scripts**: Rejects Write calls to paths matching `/tmp`, `*/scratch`, `*patch*`, `*fix_*`, `*edit_*` and tells Claude to use the `Edit` tool directly instead.
2. **Delegates to file_patch for large files**: If the target file exists and is ≥50 lines, routes through the `file_patch` MCP tool rather than a full rewrite.

### Agent Matcher

1. **Haiku routing**: Agents whose prompt matches search/research patterns are rerouted to `claude-haiku-4-5` with a ≤200 word limit injected. Bypass with `CHITTA_AGENT_NO_FORCE=1`.
2. **Subagent count tracking**: Increments a per-session counter.
   - Warns at `CHITTA_AGENT_WARN` (default: 20).
   - Hard advisory at `CHITTA_AGENT_LIMIT` (default: 50).

### ScheduleWakeup Matcher

Guards autonomous loops (agents that reschedule themselves):

- Warns at `CHITTA_LOOP_WARN` iterations (default: 10).
- Blocks at `CHITTA_LOOP_LIMIT` iterations (default: 20).

This is separate from the Agent subagent count above.

---

## Code-Intel Shadow Logging and Enforcement

`pre-tool-hook.sh` logs every Read/Edit decision to `$MIND/.hook_shadow.jsonl`.

### Log Fields

| Field      | Description                                      |
|------------|--------------------------------------------------|
| `ts`       | Unix timestamp                                   |
| `tool`     | Tool name (`Read`, `Edit`, etc.)                 |
| `file`     | Absolute path to the file                        |
| `lines`    | Line count of the file                           |
| `indexed`  | Whether chitta has indexed this file (`true`/`false`) |
| `decision` | `allow` or `deny`                                |
| `reason`   | Short string explaining the decision             |
| `enforced` | Whether the hook was in enforce mode             |

### Auto-Activation of Enforcement

Enforcement auto-activates when **both** conditions are met:

1. Shadow log has **≥100 entries**
2. Shadow log is **≥3 days old**

No manual environment flip is needed. The log accumulates in shadow mode (decisions logged but not enforced), then silently switches to enforce mode once the threshold is met.

### Enforcement Controls

| Variable                 | Effect                                                            |
|--------------------------|-------------------------------------------------------------------|
| `CHITTA_HOOK_ENFORCE=1` | Force enforce mode on immediately (skip the 3-day wait)          |
| `CHITTA_HOOK_ENFORCE=0` | Force shadow-only mode (disable enforcement even after threshold) |
| `CHITTA_ALLOW_READ=1`   | Bypass Read deny for this session                                 |

The flag file `$MIND/.allow_read_<session_id>` is an equivalent per-session bypass for `CHITTA_ALLOW_READ=1`.

### Log Rotation

The shadow log auto-rotates when it reaches **10 MB**. The current log is renamed to `.hook_shadow.jsonl.1` and a fresh log starts.

### Review Data

```bash
./scripts/hook-stats.sh   # decisions, reasons, tool split, enforce status
```

---

## Environment Variables

Every `CHITTA_*` variable below also works under its pre-rename `CC_SOUL_*`
name (see [docs/RENAME.md](RENAME.md)).

| Variable                      | Default      | Description                                                              |
|-------------------------------|--------------|--------------------------------------------------------------------------|
| `CHITTA_HOOK_ENFORCE`        | (auto)       | `1` = force enforce; `0` = force shadow only                             |
| `CHITTA_ALLOW_READ`          | `0`          | `1` = bypass Read deny and indexed-large deny for this session           |
| `CHITTA_DEEP_SEARCH`         | `0`          | `1` = allow root-wide `find` (removes `-maxdepth 3` scoping)             |
| `CHITTA_STRICT_MODE`         | `0`          | `1` = enforce symbol-level flow for indexed files                        |
| `CHITTA_AGENT_NO_FORCE`      | `0`          | `1` = disable haiku routing for search/research agents                   |
| `CHITTA_AGENT_WARN`          | `20`         | Subagent count at which a warning is issued                              |
| `CHITTA_AGENT_LIMIT`         | `50`         | Subagent count at which a hard advisory fires                            |
| `CHITTA_LOOP_WARN`           | `10`         | ScheduleWakeup iterations before warning                                 |
| `CHITTA_LOOP_LIMIT`          | `20`         | ScheduleWakeup iterations before block                                   |
| `CHITTA_SUBAGENT_BASH_RECALL`| `0`          | `1` = run Bash recall for subagent calls (adds ~2s per call, default off)|
| `CHITTA_MAX_WAIT`            | `5`          | Max seconds to wait for daemon responses                                 |
| `CHITTA_MCP_LAG_INTERVAL_MS` | `250`        | MCP asyncio scheduling-delay sampling interval in milliseconds            |
| `CHITTA_MCP_LAG_WARN_MS`     | `200`        | Scheduling delay that increments `over_count` and logs a warning          |
| `CHITTA_LEAN`                | `false`      | Ultra-lean context mode (stats only)                                     |
| `DEBUG_SOUL`                  | `0`          | `1` = enable debug output to stderr                                      |
| `SUBCONSCIOUS_INTERVAL`       | `60`         | Daemon cycle interval in seconds                                         |

This table covers the enforcement and budget knobs only. The hook scripts read
far more than this — around 97 distinct `CHITTA_*` / `CC_SOUL_*` names, most of
them internal. Enumerate the full set with:

```bash
grep -ohE 'CHITTA_[A-Z_]+|CC_SOUL_[A-Z_]+' hooks/*.sh | sort -u
```

The user-facing subset, with the old-to-new name mapping, is tabulated in
[docs/RENAME.md](RENAME.md). Recall-specific knobs — pool depth, the pre-filter,
lane ablation, the confidence band — are documented on the
[recall pipeline page](https://genomewalker.github.io/chitta/recall.html).
The MCP `health_check` tool includes process-local `mcp_loop_lag.max_lag_ms`
and `mcp_loop_lag.over_count` counters; the two knobs above also honor their
`CC_SOUL_MCP_LAG_*` aliases.

**Testing manually:** `CHITTA_HOOK_ENFORCE=1 bash hook.sh Read` will NOT work because the prefix assignment is not exported to nested bash. Use `export CHITTA_HOOK_ENFORCE=1` first.

---

## Configuration

### Plugin Mode (hooks.json)

```json
{
  "hooks": {
    "SessionStart": [
      {
        "matcher": "*",
        "hooks": [
          {"type": "command", "command": "${CLAUDE_PLUGIN_ROOT}/hooks/subconscious.sh start"},
          {"type": "command", "command": "${CLAUDE_PLUGIN_ROOT}/hooks/session-start-hook.sh"}
        ]
      }
    ],
    "UserPromptSubmit": [
      {
        "matcher": "*",
        "hooks": [
          {"type": "command", "command": "${CLAUDE_PLUGIN_ROOT}/hooks/prompt-hook.sh", "timeout": 10}
        ]
      }
    ],
    "Stop": [
      {
        "matcher": "*",
        "hooks": [
          {"type": "command", "command": "${CLAUDE_PLUGIN_ROOT}/hooks/stop-hook.sh"}
        ]
      }
    ],
    "PreCompact": [
      {
        "matcher": "*",
        "hooks": [
          {"type": "command", "command": "${CLAUDE_PLUGIN_ROOT}/hooks/pre-compact-hook.sh"}
        ]
      }
    ],
    "PreToolUse": [
      {
        "matcher": "Bash",
        "hooks": [
          {"type": "command", "command": "${CLAUDE_PLUGIN_ROOT}/hooks/pre-tool-hook.sh Bash", "timeout": 10}
        ]
      }
    ],
    "PostToolUse": [
      {
        "matcher": "Bash",
        "hooks": [
          {"type": "command", "command": "${CLAUDE_PLUGIN_ROOT}/hooks/post-bash-hook.sh", "timeout": 5}
        ]
      }
    ]
  }
}
```

The PreToolUse section handles matchers `Read`, `Edit`, `Write`, `Bash`, `Agent`, and `ScheduleWakeup` — not only Bash.

### Settings Mode (~/.claude/settings.json)

```json
{
  "hooks": {
    "SessionStart": [{
      "matcher": "*",
      "hooks": [
        {"type": "command", "command": "~/.claude/hooks/subconscious.sh start"},
        {"type": "command", "command": "~/.claude/hooks/session-start-hook.sh"}
      ]
    }],
    "UserPromptSubmit": [{
      "matcher": "*",
      "hooks": [
        {"type": "command", "command": "~/.claude/hooks/prompt-hook.sh"}
      ]
    }],
    "Stop": [{
      "matcher": "*",
      "hooks": [
        {"type": "command", "command": "~/.claude/hooks/stop-hook.sh"}
      ]
    }],
    "PreCompact": [{
      "matcher": "*",
      "hooks": [
        {"type": "command", "command": "~/.claude/hooks/pre-compact-hook.sh"}
      ]
    }],
    "PreToolUse": [{
      "matcher": "Read|Edit|Write|Bash|Agent|ScheduleWakeup",
      "hooks": [
        {"type": "command", "command": "~/.claude/hooks/pre-tool-hook.sh", "timeout": 10}
      ]
    }]
  }
}
```

### Hook Configuration Fields

| Field     | Description                                              |
|-----------|----------------------------------------------------------|
| `matcher` | Regex pattern to filter events (`*` = match all)         |
| `type`    | Hook type: `command`                                     |
| `command` | Shell command to execute                                 |
| `timeout` | Timeout in seconds (optional)                            |

---

## Transparent Memory

The key innovation is **transparent memory** — memories that surface automatically without explicit tool calls.

### How It Works

1. **User sends message**: "How should I handle rate limiting?"

2. **Hook receives JSON on stdin**:
   ```json
   {"message": "How should I handle rate limiting?"}
   ```

3. **Hook runs resonance via daemon socket**:
   ```bash
   rpc_call "full_resonate" '{"query":"How should I handle rate limiting?","k":3}'
   ```

4. **Output injected into Claude's context**:
   ```
   In Project X, used exponential backoff for rate limiting...
   Rate limiting gotcha: always return 429, not 500...
   Redis INCR with EXPIRE for distributed rate limiting...
   ```

5. **Claude sees memories as part of the conversation** — no explicit recall needed.

### Filtering and Formatting

- **Relevance threshold**: Only ≥25% relevance results pass
- **Type stripping**: `[72%] [wisdom]` prefix removed — just content
- **Truncation**: Max 500 chars total injection
- **Tag exclusion**: Auto-captured signals (`auto:cmd`, `auto:file`, `auto:edit`) are excluded

---

## Proactive Learning

The hooks detect learning opportunities in user messages and inject hints for Claude to act on.

### Detection Patterns

| Pattern        | Detection                                          | Action                     |
|----------------|----------------------------------------------------|----------------------------|
| **Correction** | "no", "actually", "that's wrong", "not quite"      | Hint: use `learn_correction` |
| **Preference** | "I prefer", "always", "never", "more concise"      | Hint: use `learn_preference` |
| **Frustration**| "stuck", "confused", "frustrated", "tedious"       | Hint: use `learn_approach`   |
| **Milestone**  | "it works", "shipped", "released", "deployed"      | Hint: use `learn_milestone`  |

### Auto-Learning (Stop Hook)

If Claude didn't use a learning tool but the user clearly corrected or expressed a preference, the stop hook auto-stores the learning:

```bash
chitta remember --content "[correction] WRONG: ... CORRECT: ..." \
  --tags "correction,auto-learned" --type wisdom --visibility 2
```

### Anticipation

1. **UserPromptSubmit**: Calls `anticipation_predict` to suggest the likely next action
2. **Stop**: Records what Claude actually did via `anticipation_observe`
3. **Verification**: If prediction matched, calls `anticipation_success` to strengthen the pattern

---

## Subconscious Daemon

The subconscious daemon runs background processing without consuming main context tokens.

### Lifecycle

```
SessionStart
     │
     ▼
subconscious.sh start
     │
     ├─▶ Kill stale daemons
     ├─▶ Acquire atomic lock
     ├─▶ Start chittad daemon
     ├─▶ Wait for socket + heartbeat
     └─▶ Log: "[subconscious] Started (pid=12345, socket=..., heartbeat=ok)"

(Daemon runs independently)
     │
     ├─▶ Every 1 second:   Check for work
     ├─▶ Every 30 seconds: Process embedding queue (batch of 20)
     ├─▶ Every 30 minutes: Hygiene cycle (decay, prune, consolidate)
     ├─▶ Every 60 minutes: Theme maintenance (split, merge, reassign)
     └─▶ Pattern detection: Corrections, preferences, frustration, milestones
```

### Managing the Daemon

```bash
./hooks/subconscious.sh status    # Check status (PID, socket, managed/MCP-spawned)
./hooks/subconscious.sh health    # Health check with auto-recovery
./hooks/subconscious.sh stop      # Graceful stop then force kill
./hooks/subconscious.sh restart   # Stop then start

tail -f ~/.claude/mind/.subconscious.log   # View logs
```

**Socket path:** `/tmp/chitta-{djb2_hash(MIND_PATH)}.sock`
**PID file:** `/tmp/chitta-{djb2_hash(MIND_PATH)}.pid`

---

## Custom Hooks

### Adding a Custom Hook

1. Create your script in `scripts/`:

```bash
#!/bin/bash
input=$(cat)
# Output appears in Claude's context as <system-reminder>
```

2. Make it executable: `chmod +x scripts/my-custom-hook.sh`

3. Add to `hooks/hooks.json` (plugin mode):

```json
{
  "hooks": {
    "UserPromptSubmit": [
      {
        "hooks": [
          {
            "type": "command",
            "command": "${CLAUDE_PLUGIN_ROOT}/scripts/my-custom-hook.sh",
            "timeout": 5
          }
        ]
      }
    ]
  }
}
```

### Hook Input

**UserPromptSubmit:**
```json
{
  "message": "User's message text",
  "context_window": {"remaining_percent": 85}
}
```

**PostToolUse:**
```json
{
  "tool_name": "Bash",
  "tool_input": {"command": "ls -la"},
  "tool_response": "...",
  "cwd": "/path/to/project",
  "session_id": "abc123"
}
```

**SessionStart:**
```json
{"transcript_path": "/path/to/transcript.jsonl"}
```

### Hook Output

- **stdout** — Injected as `<system-reminder>` in Claude's context (JSON `hookSpecificOutput` schema)
- **stderr** — Logged but not shown to Claude
- **Exit code 2** — Blocks the tool call and shows the hook's stdout as an error explanation

### RPC from Custom Scripts

```bash
# Via Unix socket (fast path)
echo '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"recall","arguments":{"query":"test","limit":3}}}' \
  | nc -U /tmp/chitta-HASH.sock

# Via thin client (fallback)
echo '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"recall","arguments":{"query":"test","limit":3}}}' \
  | ~/.claude/bin/chitta
```

---

## Troubleshooting

### Hooks Not Running

```bash
# Plugin mode: check hooks.json
jq . hooks/hooks.json

# Settings mode: check settings.json
jq '.hooks' ~/.claude/settings.json

# Verify scripts are executable
ls -la hooks/*.sh

# Ensure daemon is running
./hooks/subconscious.sh status
```

### Slow Hook Execution

1. Increase timeout in configuration
2. Check daemon responsiveness: `./hooks/subconscious.sh health`
3. Set `CHITTA_MAX_WAIT=10` for longer daemon response timeout

### Resonance Not Working

```bash
# Check daemon socket exists
ls /tmp/chitta-*.sock

# Test daemon responds
echo "stats" | nc -U /tmp/chitta-*.sock

# Test resonance directly
echo '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"full_resonate","arguments":{"query":"test","k":3}}}' \
  | nc -U /tmp/chitta-*.sock
```

### Daemon Not Starting

```bash
pgrep -f "chittad daemon"                            # stale processes?
rm -f /tmp/chitta-*.sock /tmp/chitta-*.pid /tmp/chitta-*.lock   # clean up
ls -la ~/.claude/bin/chittad                          # binary exists?
cat ~/.claude/mind/.subconscious.log                  # daemon logs
~/.claude/bin/chittad daemon --path ~/.claude/mind --foreground  # manual start
```

### Debug Mode

```bash
export DEBUG_SOUL=1
claude
# or test a hook directly:
DEBUG_SOUL=1 ./hooks/prompt-hook.sh prompt "your test query"
```

Debug output (stderr) shows: query, realm, boost k, extra tags, raw full_resonate results with relevance scores, symbol search results, and the final injected context.

### Checking Shadow Log Enforcement Status

```bash
# Review decisions and enforce status
./scripts/hook-stats.sh

# Count entries in shadow log
wc -l ~/.claude/mind/.hook_shadow.jsonl

# Check log age
stat ~/.claude/mind/.hook_shadow.jsonl
```

Enforcement auto-activates when the shadow log reaches ≥100 entries AND is ≥3 days old. Use `CHITTA_HOOK_ENFORCE=1` to force it on early, or `CHITTA_HOOK_ENFORCE=0` to keep it in shadow mode.
