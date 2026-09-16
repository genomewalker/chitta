#!/bin/bash
# SessionEnd hook: deregister this session so it stops showing as "active" in
# session_list (msg_* targets, cross-session discovery) after the window closes.
# Without this, a dead session's registration lingers forever (no TTL on
# session_list) and looks like a live messaging target.

CHITTA_BIN="${CHITTA_BIN:-$HOME/.claude/bin/chitta}"
MAX_WAIT="${CHITTA_MAX_WAIT:-${CC_SOUL_MAX_WAIT:-2}}"

INPUT="$(cat 2>/dev/null || true)"
SESSION_ID="$(jq -r '.session_id // empty' <<<"$INPUT" 2>/dev/null || true)"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[[ -f "${SCRIPT_DIR}/lib.sh" ]] && source "${SCRIPT_DIR}/lib.sh"
if [[ -n "$SESSION_ID" && "$SESSION_ID" != "default" ]]; then
    printf '%s' "$INPUT" | registry_call "$MAX_WAIT" close ||
        printf '[chitta] daemon unavailable; context not loaded.\n'
fi

exit 0
