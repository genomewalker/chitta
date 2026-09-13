#!/usr/bin/env bash
# Editable dev install: make the live plugin load code directly from THIS repo,
# so `edit repo -> restart service -> live` with one source of truth and no
# copy/drift. Idempotent and reversible (backups printed).
#
# What it does / does NOT touch, and why:
#   hooks     -> already symlinked to repo by smart-install; verified/repaired here.
#   MCP *.py  -> symlinked repo -> marketplace so the plugin runtime (which imports
#                server.py from its own dir) runs repo code. This is the one gap
#                that caused the dual-copy drift (marketplace pinned at an old tag).
#   binaries  -> deliberately NOT symlinked. chitta/chittad use `build -> install`
#                (atomic rename) to avoid ETXTBSY on the running daemon (see CLAUDE.md).
#                Rebuild + install is the correct flow for those.
#
# Ownership (single sync owner, see repo CLAUDE.md "Dev install"): this script
# owns ~/.claude/hooks/*.sh and the marketplace chitta-mcp/*.py via symlink —
# scripts/sync-installed-hooks.sh must never overwrite either with a copy, and
# skips them for that reason. This script does NOT touch Claude's versioned
# plugin cache or the Codex cache (no symlink target makes sense there — the
# cache path is version-pinned, and Codex reads from its own cache dir); it
# calls sync-installed-hooks.sh at the end so those stay owned by, and
# refreshed via, that script in the same step.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Prefer the renamed marketplace dir; fall back to the pre-rename one for
# installs that haven't re-added the marketplace under its new name yet.
MKT="$HOME/.claude/plugins/marketplaces/genomewalker-chitta"
[ -d "$MKT" ] || MKT="$HOME/.claude/plugins/marketplaces/genomewalker-cc-soul"
HOOKS_DST="$HOME/.claude/hooks"
BK="$HOME/.claude/.dev-install-backup/$(date +%Y%m%d_%H%M%S)"
mkdir -p "$BK"

link_editable() { # src (repo) dst (live) — back up a real file, then symlink
  local src="$1" dst="$2"
  [ -e "$src" ] || return 0
  if [ -L "$dst" ] && [ "$(readlink -f "$dst")" = "$(readlink -f "$src")" ]; then
    return 0  # already the right symlink
  fi
  if [ -e "$dst" ] && [ ! -L "$dst" ]; then
    mkdir -p "$BK/$(dirname "${dst#"$HOME"/}")"
    cp -a "$dst" "$BK/${dst#"$HOME"/}"
  fi
  ln -sfn "$src" "$dst"
  echo "  linked $dst -> $src"
}

echo "[dev-install] repo=$REPO"
echo "[dev-install] backups (only for replaced real files) -> $BK"

echo "[dev-install] MCP python (repo -> marketplace):"
if [ -d "$MKT/chitta-mcp" ]; then
  for py in "$REPO"/chitta-mcp/*.py; do
    link_editable "$py" "$MKT/chitta-mcp/$(basename "$py")"
  done
  # soul_repl/ is a subpackage the top-level *.py glob misses; without this the
  # marketplace copy drifts stale on a plugin reclone (caused the sandbox.py 100% bug).
  if [ -d "$REPO/chitta-mcp/soul_repl" ]; then
    mkdir -p "$MKT/chitta-mcp/soul_repl"
    for py in "$REPO"/chitta-mcp/soul_repl/*.py; do
      link_editable "$py" "$MKT/chitta-mcp/soul_repl/$(basename "$py")"
    done
  fi
else
  echo "  WARN: marketplace chitta-mcp dir not found ($MKT/chitta-mcp) — is the plugin installed?"
fi

echo "[dev-install] owner: ~/.claude/hooks/*.sh + marketplace chitta-mcp/*.py (this script, via symlink)"
echo "[dev-install] hooks (repo -> ~/.claude/hooks), repairing any drifted copies:"
mkdir -p "$HOOKS_DST"
for h in "$REPO"/hooks/*.sh; do
  link_editable "$h" "$HOOKS_DST/$(basename "$h")"
done

echo "[dev-install] restarting MCP (stdio instances respawn on next tool call; HTTP unit restarted):"
# Process is `chitta-mcp` (hyphen), not the old `chitta mcp` subcommand — the
# space pattern silently matched nothing, so MCP python edits never went live.
# [c] bracket keeps this pkill from matching its own shell. `--http` instances
# are the systemd unit Codex connects to: killing it with SIGTERM counts as a
# clean exit, Restart=on-failure does not bring it back, and the port stayed
# dead for six days (2026-09-02 → 09-08). Restart that one via systemd instead.
# Match only real MCP server processes (the console script or server.py), never
# an arbitrary shell whose argv merely mentions "chitta-mcp" — pgrep -f on the
# bare name once killed the calling deploy shell (`systemctl … chitta-mcp-http`
# in its command line). Also skip our own process tree.
_self_tree="$$ $PPID"
pgrep -f 'chitta-mcp$|chitta-mcp/server\.py|bin/chitta-mcp( |$)' 2>/dev/null | while read -r pid; do
  case " $_self_tree " in *" $pid "*) continue ;; esac
  cmd=$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null)
  case "$cmd" in *--http*) continue ;; esac
  kill "$pid" 2>/dev/null || true
done
systemctl --user try-restart chitta-mcp-http 2>/dev/null || true

echo "[dev-install] refreshing plugin cache + Codex cache (owner: sync-installed-hooks.sh):"
"$REPO/scripts/sync-installed-hooks.sh"

echo "[dev-install] DONE. Live plugin now loads from $REPO."
echo "[dev-install] Binaries unchanged — rebuild+install them via the CLAUDE.md flow."
echo "[dev-install] Revert: restore files from $BK and re-run smart-install.sh."
