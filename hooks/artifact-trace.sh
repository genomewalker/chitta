#!/bin/bash
# artifact-trace.sh — PostToolUse hook for Write.
# Registers a newly written script as an [artifact] signal memory so later
# sessions find it when they touch the same path (pre-tool-hook [traces]) or
# ask for it, and fork it instead of rewriting it. SwarmWorld (arXiv:2608.26081)
# measured ~95% of first reuse through observing artifacts; this is the trace.
# Scripts only, outside temp/scratch/mind dirs; one CLI call; never blocks.
set -u
INPUT=$(cat)
exec </dev/null
FILE_PATH=$(printf '%s' "$INPUT" | jq -r '.tool_input.file_path // ""')
[[ -z "$FILE_PATH" || ! -f "$FILE_PATH" ]] && exit 0
case "$FILE_PATH" in
    *.sh|*.py|*.R|*.sbatch|*.pl|*.jl|*.nf|*.smk) ;;
    *) exit 0 ;;
esac
case "$FILE_PATH" in
    /tmp/*|*/scratch/*|*/.claude/mind/*|*/node_modules/*|*/build/*) exit 0 ;;
esac
[[ "$(wc -l < "$FILE_PATH")" -lt 5 ]] && exit 0

CHITTA_BIN="${CHITTA_BIN:-$HOME/.claude/bin/chitta}"
[[ -x "$CHITTA_BIN" ]] || exit 0
SHA=$(sha256sum "$FILE_PATH" 2>/dev/null | cut -c1-8)
# Purpose: first comment or docstring line after the shebang, if any.
PURPOSE=$(sed -n '2,6p' "$FILE_PATH" | grep -m1 -E '^\s*(#|//|"""|'"'''"')' | sed -E 's/^\s*(#+|\/\/|"""|'"'''"')\s*//' | cut -c1-120)
REALM=$(cd "$(dirname "$FILE_PATH")" && timeout 1 "$CHITTA_BIN" realm_detect 2>/dev/null || echo "")
[[ -z "$REALM" ]] && exit 0
timeout 3 "$CHITTA_BIN" remember \
    --content "[artifact] $FILE_PATH sha:$SHA purpose:${PURPOSE:-unknown} — reuse or fork this script before writing a new one" \
    --type signal --realm "$REALM" --tags '["artifact","provenance"]' --visibility 1 >/dev/null 2>&1 || true
exit 0
