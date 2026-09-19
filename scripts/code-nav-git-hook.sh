#!/usr/bin/env bash
# Called by post-commit/post-checkout; structural refresh only, never embeddings.
set -euo pipefail
# Only the main checkout is indexed by default: git hooks fire in every worktree
# (dozens of Codex streams) and each learn_codebase is a full walk inside the daemon's
# request queue; on 2026-09-19 that held the RPC queue for minutes at a time.
if [[ "${CHITTA_CODE_NAV_ALL_WORKTREES:-0}" != 1 ]]; then
    common=$(git rev-parse --git-common-dir 2>/dev/null); gitdir=$(git rev-parse --git-dir 2>/dev/null)
    [[ -n "$common" && "$common" == "$gitdir" ]] || exit 0
fi
root=$(git rev-parse --show-toplevel 2>/dev/null) || exit 0
bin=${CHITTA_BIN:-$HOME/.claude/bin/chitta}
[[ -x "$bin" ]] || exit 0
# The daemon canonicalizes the shared-worktree identity and ignores ignored files.
timeout 60 "$bin" learn_codebase --path "$root" >/dev/null 2>&1 </dev/null &
