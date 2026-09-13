#!/bin/bash
# Commands are JSON data sent to PreToolUse; none is executed.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
unset CHITTA_HEADLESS CC_SOUL_HEADLESS CHITTA_ALLOW_MCP_KILL
export HOME="$T/home" CHITTA_DB_PATH="$T/mind" CHITTA_BIN=/bin/true
export XDG_RUNTIME_DIR="$T/runtime" CHITTA_QUEUE="$T/queue" CHITTA_TASK_LEDGER="$T/tasks"
mkdir -p "$HOME/.claude/mind" "$CHITTA_DB_PATH"
check() {
    local expected="$1" command="$2" output
    output=$(jq -nc --arg c "$command" '{tool_name:"Bash",tool_input:{command:$c}}' |
        bash "$ROOT/hooks/pre-tool-hook.sh" Bash)
    if [[ "$expected" == deny ]]; then
        jq -e '.hookSpecificOutput.permissionDecision == "deny" and (.hookSpecificOutput.permissionDecisionReason | contains("scripts/dev-install.sh"))' <<< "$output" >/dev/null
    else
        ! grep -q '"permissionDecision":"deny"' <<< "$output"
    fi
    echo "ok: $expected $command"
}
check deny "pkill -f 'chitta-mcp.*--http'"
CHITTA_HEADLESS=1 check deny "pkill -f chitta-mcp"
CC_SOUL_HEADLESS=1 check deny "pkill -f chitta-m[c]p"
check deny "sudo /usr/bin/pkill -9 -f 'chitta-m[c]p'"
check deny 'kill $(pgrep -f "chitta-mcp.*--http")'
check deny "pgrep -f chitta-mcp | xargs kill -TERM"
check deny "echo safe; pkill -f chitta-mcp"
check deny $'echo safe\npkill -f chitta-mcp'
check deny "grep -v -- --http elsewhere; pkill -f chitta-mcp"
check deny "pkill -f chitta-mcp | grep -v -- --http"
check deny 'kill $(pgrep -f chitta-mcp) | grep -v -- --http'
check allow "pgrep -af 'chitta-m[c]p'"
check allow "pkill -f unrelated-worker"
check allow 'kill $(pgrep -af "chitta-m[c]p" | grep -v -- --http | cut -d " " -f1)'
check allow 'pgrep -af "chitta-mcp" | grep -v -- --http | cut -d " " -f1 | xargs kill'
CHITTA_ALLOW_MCP_KILL=1 check allow "pkill -f 'chitta-mcp.*--http'"
