#!/usr/bin/env bash
# Launch one Codex (Astra) stream whose opening context comes from chitta.
#
#   scripts/codex-stream.sh <name> <branch> <task.md> [effort] [base]
#   scripts/codex-stream.sh --check <name>       # did the stream leave handoffs?
#
# The prompt is: the stream contract (codex-plugin/stream-contract.md), what
# chitta knows about the task (decisions and handoffs recalled for the task
# text, realm chitta), the code map for the task (code_query on the worktree),
# then the task itself. The task file's first line is a one-line title (it is
# the query for recall and the code map); the rest states the goal, the write
# scope and the task-specific gates, never the rules. Codex auto-compacts its
# thread at CHITTA_CODEX_COMPACT_TOKENS (default 60000) so context per request
# stays bounded (Astra's round-2 budget). Effort: high
# for implementation (default), medium for docs, tables and measurement runs.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
S="${CHITTA_CODEX_SPECS:-/projects/caeg/scratch/kbd606/tmp/codex-specs}"
CLI="${CHITTA_BIN:-$HOME/.claude/bin/chitta}"
source "$ROOT/scripts/stream-lib.sh"
mkdir -p "$S"

if [[ "${1:-}" == "--check" ]]; then
    name=${2:?name}
    stream_check
    W="${CHITTA_CODEX_WORKTREES:-/projects/caeg/scratch/kbd606/tmp}/codex-wt-$name"
    head=$(git -C "$W" rev-parse --short HEAD 2>/dev/null || echo none)
    found=$(timeout 20 "$CLI" recall --query "stream=$name handoff" --realm chitta --tag handoff --limit 5 --sources false 2>/dev/null | grep -c "stream=$name" || true)
    printf 'stream %s: HEAD %s, handoff memories found: %s\n' "$name" "$head" "$found"
    timeout 20 "$CLI" recall --query "stream=$name handoff" --realm chitta --tag handoff --limit 3 --sources false 2>/dev/null | grep "stream=$name" | cut -c1-200 || true
    exit 0
fi

name=${1:?name}; [[ "$name" =~ ^[a-zA-Z0-9][a-zA-Z0-9._-]*$ ]] || exit 2; branch=${2:?branch}; task=${3:?task.md}; effort=${4:-high}; base=${5:-origin/main}
case "$effort" in high|medium|low) ;; *) echo "effort must be high|medium|low (never ultra unattended)" >&2; exit 2 ;; esac
[[ -f "$task" ]] || { echo "task file not found: $task" >&2; exit 2; }
W="${CHITTA_CODEX_WORKTREES:-/projects/caeg/scratch/kbd606/tmp}/codex-wt-$name"
if [[ -f "$S/$name.pid" ]] && kill -0 "$(cat "$S/$name.pid")" 2>/dev/null; then
    echo "stream $name is already running (pid $(cat "$S/$name.pid")); refusing a second thread in the same worktree" >&2
    exit 1
fi
git -C "$ROOT" fetch -q origin
if [[ ! -d "$W" ]]; then
    git -C "$ROOT" worktree add -q -b "$branch" "$W" "$base"
    git -C "$W" submodule update --init -q chitta-field
fi

# Query terms: the task's first line (its title) plus the stream name.
title=$(grep -m1 -vE '^\s*$' "$task" | cut -c1-200)
context="$S/$name.context.md"
{
    cat "$ROOT/codex-plugin/stream-contract.md"
    printf '\n## What chitta knows about this task (realm chitta)\n\n'
    timeout 25 "$CLI" recall --query "$title" --realm chitta --tag decision --limit 4 --sources false 2>/dev/null | cut -c1-600 || echo "(recall unavailable)"
    printf '\n'
    timeout 25 "$CLI" recall --query "stream=$name handoff $title" --realm chitta --tag handoff --limit 3 --sources false 2>/dev/null | cut -c1-600 || true
    printf '\n## Code map for this task (code_query on the worktree)\n\n'
    timeout 30 "$CLI" code_query --question "$title" --path "$W" --limit 12 2>/dev/null | head -n 60 || echo "(code_query unavailable; index the worktree with chitta learn_codebase --path $W)"
    printf '\n## Task\n\n'
    cat "$task"
} > "$context"
printf 'prompt: %s bytes (%s lines) in %s\n' "$(wc -c < "$context")" "$(wc -l < "$context")" "$context"
stream_launch codex exec -C "$W" --approve-for-me --skip-git-repo-check \
    -m "${CHITTA_CODEX_MODEL:-gpt-6-astra}" -c "model_reasoning_effort=$effort" \
    -c "model_auto_compact_token_limit=${CHITTA_CODEX_COMPACT_TOKENS:-60000}" \
    -o "$S/$name.last.md"
