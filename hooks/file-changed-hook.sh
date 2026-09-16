#!/bin/bash
# FileChanged hook: Re-index project files when watched files change
#
# Triggered by Claude Code's file watcher when files registered via
# hookSpecificOutput.watchPaths are modified. Rate-limited to avoid
# re-indexing on every save.

CHITTA_BIN="${CHITTA_BIN:-$HOME/.claude/bin/chitta}"
MIND_PATH="${CHITTA_DB_PATH:-${HOME}/.claude/mind}"
MAX_WAIT="${CHITTA_MAX_WAIT:-${CC_SOUL_MAX_WAIT:-2}}"
RATE_LIMIT_SECS="${CHITTA_REINDEX_RATE_LIMIT:-${CC_SOUL_REINDEX_RATE_LIMIT:-300}}"  # 5 minutes

[[ ! -x "$CHITTA_BIN" ]] && exit 0

# Parse JSON input: { file_path, event (change|add|unlink) }
INPUT=$(cat)
FILE_PATH=$(echo "$INPUT" | jq -r '.file_path // empty' 2>/dev/null)
EVENT=$(echo "$INPUT" | jq -r '.event // "change"' 2>/dev/null)

[[ -z "$FILE_PATH" ]] && exit 0
[[ "$EVENT" == "unlink" ]] && exit 0  # Don't re-index on deletion

# Event tape: the settings-mode post-edit-hook.sh (removed 2026-09-16) logged
# an "edit" event per changed source file; keep that signal here so the
# episode organs still see edits in plugin mode. Bounded, background, fail-open.
timeout 0.5 "$CHITTA_BIN" log_event --tool "edit" --entity "$FILE_PATH" \
    --outcome 0 --ts_ms "$(date +%s%3N)" >/dev/null 2>&1 </dev/null &

# Rate limit: one re-index per directory per RATE_LIMIT_SECS
DIR_PATH=$(dirname "$FILE_PATH")
RATE_FILE="$MIND_PATH/.reindex_$(echo "$DIR_PATH" | md5sum | cut -c1-16)"
if [[ -f "$RATE_FILE" ]]; then
    LAST=$(cat "$RATE_FILE" 2>/dev/null || echo 0)
    NOW=$(date +%s)
    ELAPSED=$((NOW - LAST))
    [[ $ELAPSED -lt $RATE_LIMIT_SECS ]] && exit 0
fi
date +%s > "$RATE_FILE"

# Fire-and-forget: queue learn_codebase for the changed file's directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib.sh"

REALM=$(timeout "$MAX_WAIT" "$CHITTA_BIN" realm_detect 2>/dev/null || echo "brahman")
FILENAME=$(basename "$FILE_PATH")

queue_write "learn_codebase" "{\"path\":\"$DIR_PATH\",\"project\":\"$REALM\"}"

echo "[reindex] $FILENAME changed → queued re-index for $(basename "$DIR_PATH")"

exit 0
