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
    # 2 s + the 2 s fallback stays inside the 5 s hook budget in hooks.json.
    if ! printf '%s' "$INPUT" | registry_call 2 close; then
        timeout "$MAX_WAIT" "$CHITTA_BIN" session_deregister \
            --session_id "$SESSION_ID" >/dev/null 2>&1 || true
    fi
fi

exit 0
