#!/bin/bash
# Local envelope; daemon owns policy through ledger_op.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"
ROOT=$(resolve_cc_soul_root) || { printf '[chitta] daemon unavailable; context not loaded.\n'; exit 0; }
[[ -f "$ROOT/chitta-mcp/hook_client.py" ]] || ROOT="$(dirname "$(dirname "$(realpath "${BASH_SOURCE[0]}")")")"
INPUT=$(cat)
out=$(mktemp "${TMPDIR:-/tmp}/chitta-session-start.XXXXXX") || exit 0
printf '%s' "$INPUT" | python3 -S "$ROOT/chitta-mcp/hook_client.py" session-start "$@" > "$out"
rc=$?
cat "$out"
unavailable=$(grep -c "daemon unavailable" "$out" 2>/dev/null)
rm -f "$out"
# Phase 9: repo map once per session for an indexed project (hooks/code-nav.sh),
# only when the daemon answered; the timeout line stays the only output otherwise.
if [[ $rc -eq 0 && "${unavailable:-0}" == 0 ]]; then
    project_dir=$(jq -r '.cwd // empty' <<< "$INPUT" 2>/dev/null)
    session_id=$(jq -r '.session_id // empty' <<< "$INPUT" 2>/dev/null)
    [[ -n "$project_dir" ]] && bash "$SCRIPT_DIR/code-nav.sh" session "$project_dir" "$session_id" || true
fi
exit $rc
