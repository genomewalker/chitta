#!/usr/bin/env bash
# post-bash-hook.sh must record the real exit code for BOTH payload shapes:
#   PostToolUse (success; result under .tool_response) and
#   PostToolUseFailure (.error = "Exit code N\n<stderr>", no tool_response).
# Regression guard for the 10-day window where the ledger saw only exit 0.
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
hook="$here/../post-bash-hook.sh"
T=$(mktemp -d "${TMPDIR:-/tmp}/postbash.XXXXXX")
trap 'rm -rf "$T"' EXIT
export CHITTA_DB_PATH="$T" MIND_PATH="$T" CHITTA_BIN="${CHITTA_BIN:-$HOME/.claude/bin/chitta}"
fail=0
check() { if [[ "$2" == *"$3"* ]]; then echo "ok: $1"; else echo "FAIL: $1 -> $2"; fail=1; fi; }

printf '%s' '{"hook_event_name":"PostToolUseFailure","session_id":"t-fail","tool_name":"Bash","tool_input":{"command":"ls /nope"},"error":"Exit code 2\nls: cannot access /nope"}' \
  | bash "$hook" >/dev/null 2>&1 || true
check "failure event records exit 2" "$(grep t-fail "$T/outcome_ledger.jsonl")" '"exit_code":2'

printf '%s' '{"hook_event_name":"PostToolUseFailure","session_id":"t-fail-noexit","tool_name":"Bash","tool_input":{"command":"true"},"error":"Command was killed"}' \
  | bash "$hook" >/dev/null 2>&1 || true
check "failure without exit code defaults to 1" "$(grep t-fail-noexit "$T/outcome_ledger.jsonl")" '"exit_code":1'

printf '%s' '{"hook_event_name":"PostToolUse","session_id":"t-ok","tool_name":"Bash","tool_input":{"command":"echo hi"},"tool_response":{"stdout":"hi","stderr":"","interrupted":false}}' \
  | bash "$hook" >/dev/null 2>&1 || true
check "success event (tool_response) records exit 0" "$(grep '"t-ok"' "$T/outcome_ledger.jsonl")" '"exit_code":0'

printf '%s' '{"session_id":"t-legacy","tool_name":"Bash","tool_input":{"command":"echo hi"},"tool_result":{"exit_code":0,"stdout":"hi"}}' \
  | bash "$hook" >/dev/null 2>&1 || true
check "legacy tool_result shape still records" "$(grep t-legacy "$T/outcome_ledger.jsonl")" '"exit_code":0'

exit $fail
