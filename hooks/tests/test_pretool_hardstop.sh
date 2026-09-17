#!/bin/bash
# Hard stop: PreToolUse denies every tool call once the transcript's last
# assistant usage (input + cache_read + cache_creation) exceeds
# CHITTA_CONTEXT_HARD_STOP, except the handoff allowlist; release once the
# last usage drops back down (compaction or fresh session).
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
export HOME="$T/home" CHITTA_DB_PATH="$T/mind" CHITTA_QUEUE="$T/queue" CHITTA_BIN="$T/cli" CHITTA_PLUGIN_DIR="$ROOT" HOOK_POLICY_FIXTURE="$T/responses"
export CHITTA_CONTEXT_HARD_STOP=900000
mkdir -p "$CHITTA_DB_PATH"
"${CXX:-g++}" -std=c++17 -O2 -pthread -I"$ROOT/chitta/include" "$ROOT/hooks/tests/event-response.cpp" -lcrypto -o "$CHITTA_BIN"
printf '%s\n' '{}' > "$HOOK_POLICY_FIXTURE"

usage_line() {
    # $1=input $2=cache_read $3=cache_creation
    printf '{"type":"assistant","message":{"usage":{"input_tokens":%s,"cache_read_input_tokens":%s,"cache_creation_input_tokens":%s}}}\n' "$1" "$2" "$3"
}

TRANSCRIPT="$T/transcript.jsonl"
# The hard stop is opt-in: CHITTA_CONTEXT_HARD_STOP=900000 for this test; 700000 + 250000 + 5000 = 955000 exceeds it.
{ usage_line 20000 0 0; usage_line 700000 250000 5000; } > "$TRANSCRIPT"

call() {
    local matcher="$1" input_json="$2"
    python3 - "$ROOT" <<PY
import json, os, subprocess, sys
from pathlib import Path
root = Path(sys.argv[1])
p = subprocess.run(['bash', str(root / 'hooks/pre-tool-hook.sh'), '$matcher'],
                    input='''$input_json''', text=True, capture_output=True, timeout=5)
sys.stdout.write(str(p.returncode) + "\n")
sys.stdout.write(p.stdout)
PY
}

payload() {
    local cmd="$1"
    printf '{"session_id":"hardstop-test","transcript_path":"%s","tool_input":{"command":"%s"}}' "$TRANSCRIPT" "$cmd"
}

# Over budget: an ordinary Bash command is denied.
out=$(call Bash "$(payload 'ls -la')")
rc=$(head -1 <<< "$out"); body=$(tail -n +2 <<< "$out")
[[ "$rc" == "0" ]] || { echo "FAIL: expected exit 0 with deny JSON, got rc=$rc"; exit 1; }
echo "$body" | python3 -c "
import json, sys
data = json.load(sys.stdin)
out = data['hookSpecificOutput']
assert out['permissionDecision'] == 'deny', out
assert '[hard-stop]' in out['permissionDecisionReason'], out
assert '955k' in out['permissionDecisionReason'], out
assert '900k' in out['permissionDecisionReason'], out
"

# Allowlisted Bash command (chitta remember/checkpoint/ledger_op) is not denied.
out=$(call Bash "$(payload 'chitta remember --content x')")
body=$(tail -n +2 <<< "$out")
[[ -z "$body" ]] || echo "$body" | python3 -c "
import json, sys
data = json.loads(sys.stdin.read() or '{}')
assert data.get('hookSpecificOutput', {}).get('permissionDecision') != 'deny', data
"

# Allowlisted MCP tool is not denied even though usage is still over budget.
out=$(call mcp__chitta__checkpoint "$(payload 'ignored')")
body=$(tail -n +2 <<< "$out")
[[ -z "$body" ]] || echo "$body" | python3 -c "
import json, sys
data = json.loads(sys.stdin.read() or '{}')
assert data.get('hookSpecificOutput', {}).get('permissionDecision') != 'deny', data
"

# Coordination remains available, but forked context and lookalike commands do not.
for cmd in 'chitta msg_send --to lead --body done' 'chitta msg_inbox' 'chitta msg_ack' 'chitta session_list' 'chitta ledger_op' 'chitta checkpoint'; do
    out=$(call Bash "$(payload "$cmd")")
    tail -n +2 <<< "$out" | python3 -c "import json,sys; d=json.loads(sys.stdin.read() or '{}'); assert d.get('hookSpecificOutput',{}).get('permissionDecision') != 'deny',d"
done
for kind in general-purpose fork; do
    input=$(printf '{"session_id":"hardstop-test","transcript_path":"%s","tool_input":{"subagent_type":"%s"}}' "$TRANSCRIPT" "$kind")
    out=$(call Agent "$input")
    tail -n +2 <<< "$out" | python3 -c "import json,sys; d=json.loads(sys.stdin.read() or '{}'); assert (d.get('hookSpecificOutput',{}).get('permissionDecision') == 'deny') == ('$kind' == 'fork'),d"
done
out=$(call Bash "$(payload 'chitta msg_send_extra')")
tail -n +2 <<< "$out" | python3 -c "import json,sys; assert json.load(sys.stdin)['hookSpecificOutput']['permissionDecision'] == 'deny'"

# Disabled via CHITTA_CONTEXT_HARD_STOP=0: no deny despite the same transcript.
out=$(CHITTA_CONTEXT_HARD_STOP=0 call Bash "$(payload 'ls -la')")
body=$(tail -n +2 <<< "$out")
[[ -z "$body" ]] || echo "$body" | python3 -c "
import json, sys
data = json.loads(sys.stdin.read() or '{}')
assert data.get('hookSpecificOutput', {}).get('permissionDecision') != 'deny', data
"

# Release: a synthetic compaction drops the last assistant usage back under
# the limit, so the next Bash call is no longer denied.
{ usage_line 20000 0 0; usage_line 8000 1000 500; } > "$TRANSCRIPT"
out=$(call Bash "$(payload 'ls -la')")
body=$(tail -n +2 <<< "$out")
[[ -z "$body" ]] || echo "$body" | python3 -c "
import json, sys
data = json.loads(sys.stdin.read() or '{}')
assert data.get('hookSpecificOutput', {}).get('permissionDecision') != 'deny', data
"

echo "ok: hard-stop deny, handoff allowlist (Bash + MCP), disable flag, release after synthetic compaction"
