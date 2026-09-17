#!/usr/bin/env bash
# claude-stream.sh NAME BRANCH TASK [--opus] [--lead ID] [--base REF]
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
S=${CHITTA_CLAUDE_SPECS:-/projects/caeg/scratch/kbd606/tmp/claude-specs}
CLI=${CHITTA_BIN:-$HOME/.claude/bin/chitta}
source "$ROOT/scripts/stream-lib.sh"
mkdir -p "$S"
if [[ ${1:-} == --check ]]; then
    name=${2:?name}
    stream_check
    exit
fi
name=${1:?name}; branch=${2:?branch}; task=$(realpath "${3:?task.md}"); shift 3
model=sonnet; base=origin/main
while (($#)); do
    case "$1" in
        --opus) model=opus; shift ;;
        --lead) export CHITTA_LEAD_SESSION=${2:?session}; shift 2 ;;
        --base) base=${2:?ref}; shift 2 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
[[ "$name" =~ ^[a-zA-Z0-9][a-zA-Z0-9._-]*$ ]] || exit 2
W=${CHITTA_CLAUDE_WORKTREES:-/projects/caeg/scratch/kbd606/tmp}/claude-wt-$name
if [[ ! -d "$W" ]]; then
    git -C "$ROOT" worktree add -q -b "$branch" "$W" "$base"
    git -C "$W" submodule update --init -q chitta-field
fi
title=$(grep -m1 -vE '^\s*$' "$task" | cut -c1-200)
context="$S/$name.context.md"
stream_context > "$context"
stream_launch claude -p --model "$model" --output-format json --permission-mode acceptEdits \
    --setting-sources '' --strict-mcp-config --mcp-config '{"mcpServers":{}}' \
    --settings '{"disableAllHooks":true}' --add-dir "$S"
