# chitta (legacy full-plugin entrypoint)

[CLAUDE.md](../CLAUDE.md) is the canonical Claude Code instruction source.
Read it before repository work; this derived pointer summarizes its constraints
and does not define a separate policy. Codex follows
[codex-plugin/AGENTS.md](../codex-plugin/AGENTS.md). Maintain these summaries when the canonical
constraints change; keep model routing, build commands and deployment details
in the canonical files.

- Work in one git worktree per implementation stream. The orchestrator reviews
  and deploys; streams never install, restart services or modify live state.
- Never `pkill` the `--http` MCP process: it is Codex's transport on port 9481
  and does not self-restart. Recovery belongs to the orchestrator.
- Authorized deployment uses `install`, never `cp`, over a running binary.
  Preserve the compiled embedding dimension and model identity; changing the
  dimension requires a fresh build directory.
- Store long-running work state in worktree files and keep tool output short.
- Every delegated prompt includes the safety preamble verbatim. Delegates are
  read-only unless their task says otherwise; do not pass main-session context.
- Hooks enforce their own policy; see [docs/HOOKS.md](../docs/HOOKS.md). `CHITTA_*`
  names take precedence over the compatible `CC_SOUL_*` aliases.
- Use `mcp__chitta__*` for memory tools. `grow` and `connect` are behind the
  `advanced` gateway. Verify retrieved context before relying on it.
