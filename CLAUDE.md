# chitta

Persistent memory for coding agents: a C++ daemon (`chittad`) over a Rust store,
an MCP server, and shell hooks that inject context into Claude Code and Codex.

> Status as of 2026-09-13: **this file is canonical for Claude Code.**
> `codex-plugin/AGENTS.md` is canonical for Codex; `CLAUDE.lean.md` and
> `.claude-plugin/CLAUDE-full.md` are the lean and shipped-plugin variants and
> defer here. Rewritten as constraints rather than procedure, and trimmed to
> non-obvious gotchas, per the sources at the bottom.
>
> Renamed cc-soul → chitta on 2026-09-02. `CC_SOUL_*` env names still work via
> the alias shim in `hooks/lib.sh` (`CHITTA_*` wins if both are set).

## Who does what

Claude Code here is **Claude Fable 5.1** (`claude-fable-5-1`). It orchestrates
and reviews; it should not grind through mechanical work a cheaper model does
correctly. Delegate by cost of being wrong, not by size of the task:

- **Inventory, search, "where is X"** → Haiku 4.5 (`model: "haiku"`).
- **Needs a judgement call** → Sonnet 5. **Architecture or an invariant** → Opus 5.
- **Implementation** → Codex `gpt-6-astra`, one git worktree per stream, driven by
  a written spec. Codex writes, Fable reviews the diff, Fable decides what merges.
- **Needs this session's context** → `subagent_type: "fork"` (inherits Fable's
  model, system prompt, and this file; ordinary subagents inherit none of it).

`hooks/pre-tool-hook.sh` already routes research-shaped `Agent` calls to Haiku
and caps their report; it exempts an explicit `model` and any `fork`. Don't
restate the policy in a skill — pass `model` when the default would be wrong.

Reasoning effort defaults to `high` (levels `low|medium|high|xhigh|max`). Prompts
tuned for an older model are worth re-sweeping; this generation needs fewer
instructions, not more. Don't leave the lead session blocked on a subagent.

**Two rules that hold everywhere, stated once:**

1. Prepend `@~/.claude/agent_safety_preamble.md` verbatim to every subagent
   prompt. Subagents are read-only unless the task says otherwise.
2. Never pass main-session context into a subagent prompt. Write a self-contained
   prompt and let the agent call `mcp__chitta__recall` for what it needs.

## Build & deploy

```bash
cd chitta && cmake --build build --parallel
install -m 0755 ../bin/chittad ~/.claude/bin/chittad
install -m 0755 ../bin/chitta  ~/.claude/bin/chitta
[ -f ../bin/chitta_hintd ] && install -m 0755 ../bin/chitta_hintd ~/.claude/bin/chitta_hintd
systemctl --user restart chittad
systemctl --user try-restart chitta-hintd 2>/dev/null || true
bash scripts/dev-install.sh
```

- `install`, never `cp`: `cp` over a running binary gives ETXTBSY. `install` is an
  atomic rename.
- `chitta_hintd` exists only in a `CHITTA_WITH_LLAMA_CPP=ON` build.
- **Never `pkill` the `--http` MCP process.** Codex connects to it on port 9481;
  SIGTERM reads as a clean exit, so `Restart=on-failure` won't revive it — that
  left port 9481 dead from 2026-09-02 to 09-08. `dev-install.sh` skips `--http`
  and restarts the unit through systemd.
- Embedding dimension is fixed at **compile time** (`CHITTA_EMBED_DIM`, multiple
  of 64, CMake default 1024 for bge-large-en-v1.5). Changing it needs a fresh
  build dir; the daemon rejects a model whose `n_embd` disagrees.

## Dev install: the repo IS the live plugin

The live plugin loads hooks and MCP python from `~/.claude/hooks/*` and the
marketplace checkout, never from this repo directly. `scripts/dev-install.sh`
symlinks those paths back here, so editing the repo is editing the live plugin.

- Run `dev-install.sh` after touching hooks or MCP python. It symlinks, then
  calls `sync-installed-hooks.sh` for the plugin and Codex caches in the same
  step. Hooks go live on the next invocation; MCP python needs the restart.
- **A plugin update re-clones the marketplace and replaces the symlinks with a
  stale copy.** This is what caused the `server.py` dual-copy drift. Re-run
  `dev-install.sh` after any plugin update.
- `sync-installed-hooks.sh --check` reports drift without writing, and is safe
  mid-edit. It skips destinations that are already dev-install symlinks.

## Release

`./scripts/release.sh patch|minor|major -y`

⚠️ **Rollback floor: chitta-field v2.1.0.** Snapshots use the V23 sectioned
format since v2.1.0 and older daemons can't read the magic. Rolling back below
v2.1.0 only works while a pre-V23 snapshot family is still on disk, and
`prune_old_snapshots` keeps two families — roughly two save cycles.

## Orientation

| | Path |
|---|---|
| Daemon | `chitta/src/simple_cli.cpp` |
| RPC | `chitta/include/chitta/rpc/field_handler.hpp` |
| Store | `chitta-field/src/store.rs` |
| MCP | `chitta-mcp/server.py` |
| Hooks | `hooks/*.sh` |

Hook behaviour, the full `CHITTA_*` table, and the Read/Edit enforcement rules
live in `docs/HOOKS.md` — the hooks enforce themselves, so this file doesn't
restate them. Review the decision log with `./scripts/hook-stats.sh`.

⚠️ Setting a `CHITTA_*` var as a command prefix (`CHITTA_HOOK_ENFORCE=1 bash
hook.sh`) does **not** reach a nested bash. `export` it first.

## Codex

Conventions, the `codex exec` invocation, sandbox flags, and MCP wiring live in
`codex-plugin/AGENTS.md`. One thing that bites from this side: `~/.codex/config.toml`
defaults to `gpt-5.6-sol` at `xhigh`, so pass `-m gpt-6-astra` explicitly.

Sources for this pass, with the primary-source URLs, are in `docs/HOOKS.md`.
