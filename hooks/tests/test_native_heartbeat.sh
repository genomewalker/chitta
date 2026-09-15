#!/bin/bash
# Native success, durable offline/failure fallback, and malformed queue input.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
export CHITTA_QUEUE="$T/queue" CHITTA_BIN="$T/chitta" CALLS="$T/calls"
source "$ROOT/hooks/lib.sh"
cat > "$CHITTA_BIN" <<'STUB'
#!/bin/bash
printf '%s\n' "$@" >> "$CALLS"
[[ "$1" == session_heartbeat && "${RPC_FAIL:-0}" == 0 ]]
STUB
chmod +x "$CHITTA_BIN"
daemon_available() { return 0; }
input='{"thread_id":"thread-α","client":"codex"}'
session_heartbeat 'session-"α' "$input"
[[ ! -f "$CHITTA_QUEUE" ]]
[[ $(sed -n '3p' "$CALLS") == 'session-"α' ]]
sed -n '5p' "$CALLS" | jq -e '. == {thread_id:"thread-α",client:"codex"}' >/dev/null
export RPC_FAIL=1
session_heartbeat 'rpc-failure' "$input"
daemon_available() { return 1; }
session_heartbeat 'offline' '{}'
jq -se 'length == 2 and .[0].tool == "session_heartbeat" and
    .[0].args == {session_id:"rpc-failure",metadata:{thread_id:"thread-α",client:"codex"}} and
    .[1].args == {session_id:"offline",metadata:{thread_id:"",client:""}} and
    all(.[]; (.ack_id | length > 0) and (.ts | type == "number"))' "$CHITTA_QUEUE" >/dev/null
if queue_write invalid '{bad json' 2>/dev/null; then
    echo 'FAIL: malformed queue args accepted' >&2
    exit 1
fi
[[ $(wc -l < "$CHITTA_QUEUE") == 2 ]]
queue_write wide-id $'{\n"id":18446744073709551614,"text":"line1\\nline2"\n}'
[[ $(wc -l < "$CHITTA_QUEUE") == 3 ]]
grep -q '"id":18446744073709551614' "$CHITTA_QUEUE"
printf 'ok: native heartbeat, failed RPC, missing socket, JSONL boundaries and invalid args\n'
