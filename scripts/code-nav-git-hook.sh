#!/usr/bin/env bash
# Called by post-commit/post-checkout; structural refresh only, never embeddings.
set -euo pipefail
root=$(git rev-parse --show-toplevel 2>/dev/null) || exit 0
bin=${CHITTA_BIN:-$HOME/.claude/bin/chitta}
[[ -x "$bin" ]] || exit 0
# The daemon canonicalizes the shared-worktree identity and ignores ignored files.
timeout 60 "$bin" learn_codebase --path "$root" >/dev/null 2>&1 </dev/null &
