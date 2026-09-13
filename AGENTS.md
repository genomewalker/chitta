# AGENTS.md

chitta: persistent memory for coding agents — a C++ daemon over a Rust store, an
MCP server, and shell hooks that inject context into Claude Code and Codex.

## Hard constraints

First on the page on purpose: Codex truncates the combined `AGENTS.md` bytes
silently once past `project_doc_max_bytes`, dropping the end of the file
(#13386, #37956).

1. **Never `pkill` the `--http` MCP process** — it is Codex's own transport on port
   9481 and does not self-restart. Use `bash scripts/dev-install.sh`.
2. **Implementation happens in its own git worktree, never on `main`.** One
   worktree per stream; never two streams in one checkout.
3. **`install`, never `cp`, over a running binary** — `cp` gives ETXTBSY.
4. **Write state to files, not to context.** The spec plus `Plan.md` and
   `Documentation.md` in the worktree are the source of truth for a long run.
5. **Keep tool output short** — `head`/`tail`/`--quiet`, never `cat` a large log.
6. **`codex exec` needs `</dev/null`**, or it blocks waiting on stdin.
7. **Prepend `@~/.claude/agent_safety_preamble.md`** verbatim to any agent you
   spawn. Read-only unless the task says otherwise.

> Status as of 2026-09-13: **`codex-plugin/AGENTS.md` is canonical for Codex** —
> the `codex exec` invocation, effort and approval flags, MCP wiring, and known
> Codex defects. Open it before working here; Codex auto-loads this git-root file
> but not the plugin copy. `CLAUDE.md` is canonical for Claude Code and holds the
> model split and build gotchas. Sources: `docs/HOOKS.md`, plus Codex GitHub
> issues #13386, #37956, #34289, #26602, #41378, #41600, #43194, #43335, #42449,
> #44305 (Sept 2026), cited in `codex-plugin/AGENTS.md`.

chitta's MCP tools are namespaced `mcp__chitta__*`, not `mcp__chitta-mcp__*`.
`grow` and `connect` sit behind the `advanced` gateway.

Codex `PostToolUse` carries no exit code (#34289), so chitta records Codex bash
outcomes as `exit_code: null` plus a `likely_fail` text heuristic; consumers never
read null as success. Report your own failures explicitly.

<!-- BEGIN sqz-agents-guidance (auto-installed by sqz init; remove this block to disable) -->

## sqz — token-optimized CLI output

Pipe long command output through `sqz compress` (`cmd 2>&1 | sqz compress`) —
it typically saves 60-90% of tokens while preserving paths and identifiers. Skip
it for interactive commands, compound commands with shell operators, and output
already only a few lines. If `sqz` is not on PATH, run commands normally.

The `sqz-mcp` server exposes `compress`, `passthrough`, and `expand`. A
`§ref:HASH§` token is a dedup reference: resolve it with `sqz expand <prefix>`, or
set `SQZ_NO_DEDUP=1` for one command to opt out.

<!-- END sqz-agents-guidance -->
