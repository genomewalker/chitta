# chitta

Persistent memory for coding agents: a C++ daemon (`chittad`) over a Rust store,
an MCP server, and shell hooks that inject context into Claude Code and Codex.

> Status as of 2026-09-13: **this file is canonical for Claude Code.**
> `codex-plugin/AGENTS.md` is canonical for Codex; `CLAUDE.lean.md` and
> `.claude-plugin/CLAUDE-full.md` are the lean and shipped-plugin variants and
> defer here. This file contains constraints, model routing, build commands,
> deployment details, and source references.
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

- Streams: `scripts/codex-stream.sh <name> <branch> <spec> [effort]`; cost: `scripts/token-ledger.py`
  (turns × context is the bill; see `docs/DECISION-2026-09-17-token-efficiency.md`).
- Gates: `scripts/gate-quick.sh` per commit (about a minute), `scripts/gate-full.sh
  [--replica] [--recall]` before a merge; heavy steps run on a compute node via
  `scripts/on-compute.sh -- <cmd>` (the login nodes sit at load 70–140).
  Python for the gates and the contract check is whichever interpreter imports
  `mcp` (`scripts/python-with-mcp.sh`: `CHITTA_PY`, then the conda bioinfo env);
  on a compute node the contract check re-runs itself over ssh on the daemon
  node named in `<mind>/.daemon-node`, since the socket is not reachable there.
- The pre-tool hard stop (`CHITTA_CONTEXT_HARD_STOP`) is opt-in and off by
  default since 2026-09-17: chitta economises context, it never refuses work.
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
- **Restart loop with `Failed to open chitta-field store`** and no other
  `chittad` on the mind dir: a stale NFS lock on `<mind>/chitta-field/.instance.lock`,
  left when the previous daemon was killed mid-snapshot. Since 2026-09-15 the
  store replaces a lock whose recorded holder (pid host) is dead on this host
  and logs `[chitta-field] open failed: …` with the reason; the unit's
  `TimeoutStopSec` is 300 s so a clean shutdown finishes its snapshot. If it
  still loops, `mv` the lock file aside; never delete a lock whose holder is
  another host.
- **The lock's recorded holder is another login node**: the chittad user unit
  lives in the shared home, so every node you log into starts it, and a unit on
  another node retries the lock every 10 s until a restart here releases it
  (2026-09-16: 85,272 retries on dandycomp03fl, then dandycomp08fl took over).
  `<mind>/.daemon-node` names the primary host and the `primary-node.conf`
  drop-in makes other nodes' units exit 75 without restarting; a node honours
  it only after `systemctl --user daemon-reload` there. Stop the foreign unit
  (`ssh <node> systemctl --user stop chittad`) rather than touching the lock.
- **WAL disappearance / NFS ESTALE** (2026-09-16–18): the old compactor
  inferred the active tail from directory filenames; a later empty or resurrected
  name could make the actual open writer a deletion candidate. It also read
  coverage after saving, allowing newer writes to be treated as committed.
  Pruning now pins the descriptor's actual path under the writer lock, scans
  candidates against validated committed-family coverage, and keeps segments
  not older than the writer. Rotation creates and syncs the replacement before
  closing the old descriptor; unlink and recovery rename sync the directory.
  Audit lines identify each unlink path, reason and result. Historical logs
  cannot prove the incident's unlink caller: the untimestamped c1cf5221 warning
  follows a CW sweep; the timestamped 18:02:26Z warning names 1564dc7c.
  The repeated-failure cause is certain: recovery tried reading the same stale
  descriptor on every attempt. Recovery now retains the active segment's
  accepted bytes (rotation threshold 256 MiB) and installs a fresh inode without
  reading that descriptor, preserving acknowledged records and their hash chain.
  ESTALE therefore does not require restart when replacement I/O succeeds.
  The cache is process-local; it cannot recover bytes lost before this binary
  started or survive a crash before successful recovery. Monitor recovery logs
  and `memory_count`; do not assume an old deployment has this protection.
  Frozen-copy compute proof (2026-09-18): 36,000 writes in 1,800 s at 20 Hz,
  30 forced compactions, memory count 134,805 → 170,805 with no regressions,
  zero vanished-segment warnings and zero ESTALE errors.
- **Do not restart while a snapshot is in flight.** SIGTERM during a save
  abandons that family (`manifest family … failed validation` on the next
  start), so the daemon falls back to the previous family and replays the
  WAL since then (2026-09-16: 2 h of WAL → 32 s replay + 41 s normalize,
  105 s to ready). Before `systemctl --user restart chittad`, check that
  `chittad.log` shows no `Turbo rebuild start` / `LSH cache` / `event tape
  organs` lines in the last minute, or wait for the family to commit.
- Startup on the live store is ~9.5 s when the derived-state sidecars hit
  (`.lsh`, `.turbo`, `.organs` next to the snapshot family; since 2026-09-16,
  parallel snapshot decode included) and ~20 s on the first start after a
  deploy or snapshot-format change, which rebuilds them. The quantized index
  finishes rebuilding on the maintenance thread a few seconds after `ready`. Phase timers are in `chittad.log` (`load phase=<name> ms=`,
  `cache hit=`). Hooks time out against a warming daemon, so do not measure
  recall in the first 30 s after a restart.

## Dev install: the repo IS the live plugin

The live plugin loads hooks and MCP python from `~/.claude/hooks/*` and the
marketplace checkout, never from this repo directly. `scripts/dev-install.sh`
symlinks those paths back here, so editing the repo is editing the live plugin.

- **Never merge a branch that touches `hooks/` in this checkout.** The live
  hooks are symlinks here, so conflict markers in `pre-tool-hook.sh` turn the
  PreToolUse hook into a syntax error that blocks every Read, Write and Bash
  call (2026-09-17). Merge in a separate worktree, resolve, run
  `scripts/gate-quick.sh`, then `git merge --ff-only` here.
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
| Code navigation | Navigate code with code_query before reading files; codebase_overview shows the repo map across supported languages |
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
