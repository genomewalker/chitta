# chitta

> Status as of 2026-09-02: project renamed cc-soul → chitta (repo
> `genomewalker/chitta`, old URL redirects); `CC_SOUL_*` env vars still
> honored via the alias shim in `hooks/lib.sh` — see `docs/RENAME.md`.

## Build & deploy
```bash
cd chitta && cmake --build build --parallel
install -m 0755 ../bin/chittad ~/.claude/bin/chittad
install -m 0755 ../bin/chitta  ~/.claude/bin/chitta
# chitta_hintd exists only when built with CHITTA_WITH_LLAMA_CPP=ON
[ -f ../bin/chitta_hintd ] && install -m 0755 ../bin/chitta_hintd ~/.claude/bin/chitta_hintd
systemctl --user restart chittad
systemctl --user try-restart chitta-hintd 2>/dev/null || true
bash scripts/dev-install.sh   # restarts stdio MCP instances + the chitta-mcp-http unit (never pkill the --http one: Codex uses it)
```
`install` = atomic rename. Never `cp` over running binary → ETXTBSY.

## Dev install (editable — repo IS the live plugin)
> Status as of 2026-09-02: single sync owner — `dev-install.sh` symlinks own
> `~/.claude/hooks/*.sh` and the marketplace `chitta-mcp/*.py`;
> `sync-installed-hooks.sh` owns the versioned plugin cache and the Codex
> cache. `sync-installed-hooks.sh` never overwrites a destination that is
> already a symlink resolving into this repo — it detects and skips those
> (`[owner:symlink] ... not copying`), so it can no longer silently turn a
> dev symlink back into a stale copy. Always run `dev-install.sh` after
> editing hooks or MCP python: it symlinks, then calls
> `sync-installed-hooks.sh` itself so the plugin cache is refreshed in the
> same step — a bare `sync-installed-hooks.sh` alone only backstops
> non-symlinked (plugin-disabled) installs and the cache/Codex destinations.

The live plugin loads **hooks + MCP python from `~/.claude/hooks/*` and
`~/.claude/plugins/marketplaces/genomewalker-chitta/chitta-mcp/*`** (or the
pre-rename `genomewalker-cc-soul` marketplace dir, if that's what's still
installed) — NOT from this repo directly. `scripts/dev-install.sh` symlinks
those live paths back at
this repo so `edit repo → restart service → live`, one source of truth, no drift.
```bash
bash scripts/dev-install.sh   # symlinks hooks + MCP *.py to this repo; refreshes
                               # plugin cache + Codex cache; restarts MCP
```
- Hooks are live immediately (bash re-reads per invocation). MCP python needs the
  MCP restart (the script does `pkill -f "chitta-m[c]p"`).
- Binaries are the exception: keep the `build → install` (atomic) flow above —
  symlinking a running binary risks ETXTBSY.
- ⚠️ The marketplace is a **git checkout** that a plugin update can re-clone,
  replacing the symlinks with a stale copy (this is what caused the
  `server.py` dual-copy drift). Re-run `dev-install.sh` after any plugin update.
- `scripts/sync-installed-hooks.sh --check` reports drift without writing
  anything; it still catches real plugin-cache/Codex drift (it only skips
  destinations that are dev-install symlinks, where drift is structurally
  impossible) — safe to run any time, including mid-edit.

## Release
`./scripts/release.sh patch|minor|major -y`

⚠️ **Rollback floor: chitta-field v2.1.0.** Snapshots are written in the V23
sectioned format since v2.1.0 — older daemons can't read the magic. Rollback
below v2.1.0 only works while a pre-V23 snapshot family still exists on disk
(`prune_old_snapshots` keeps 2 families, so ~2 save cycles after upgrade).

## Key files
| | Path |
|---|---|
| Daemon | `chitta/src/simple_cli.cpp` |
| RPC | `chitta/include/chitta/rpc/field_handler.hpp` |
| Store | `chitta-field/src/store.rs` |
| MCP | `chitta-mcp/server.py` |
| Hooks | `hooks/*.sh` |

## Code-intel hook enforcement
`hooks/pre-tool-hook.sh` logs every Read/Edit decision to
`$MIND/.hook_shadow.jsonl` (shadow mode, default). Fields:
`tool,file,lines,indexed,decision,reason,enforced`.

Enforce mode auto-activates once shadow log has ≥100 entries AND is
≥3 days old — no manual env flip needed.

| Env (`CC_SOUL_*` still honored) | Effect |
|---|---|
| `CHITTA_HOOK_ENFORCE=1` | Force enforce on early (skip wait) |
| `CHITTA_HOOK_ENFORCE=0` | Force shadow only (disable enforcement) |
| `CHITTA_ALLOW_READ=1`   | Bypass Read deny for this session |
| `CHITTA_AGENT_NO_FORCE=1` | Disable haiku-force on research subagents (advisory only) |
| `CHITTA_AGENT_WARN=N` | Subagent count to warn at (default 20) |
| `CHITTA_AGENT_LIMIT=N` | Subagent count for hard advisory (default 50) |
| `CHITTA_SUBAGENT_BASH_RECALL=1` | Run Bash recall for subagent calls too (adds 2s/call) |
| `CHITTA_ALLOW_READ=1` | Bypass Read dedup deny for this session (also bypasses indexed-large deny) |

Review data: `./scripts/hook-stats.sh` (decisions, reasons, tool split, enforce-status).

⚠️ **Testing manually**: `CHITTA_HOOK_ENFORCE=1 bash hook.sh Read` won't work — the prefix-assignment isn't exported to nested bash. Use `export CHITTA_HOOK_ENFORCE=1` first.
