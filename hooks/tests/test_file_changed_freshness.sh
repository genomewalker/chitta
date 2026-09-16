#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
export CHITTA_BIN="$T/cli" CHITTA_DB_PATH="$T/mind" CHITTA_QUEUE="$T/queue"
mkdir -p "$CHITTA_DB_PATH" "$T/repo"
"${CXX:-g++}" -std=c++17 -O2 -pthread -I"$ROOT/chitta/include" \
    "$ROOT/hooks/tests/event-response.cpp" -lcrypto -o "$CHITTA_BIN"
for event in change change unlink; do
    jq -nc --arg path "$T/repo/a quoted \"file\".md" --arg event "$event" \
        '{file_path:$path,event:$event}' | bash "$ROOT/hooks/file-changed-hook.sh" >/dev/null
done
jq -s -e '
    [.[] | select(.tool == "ledger_op" and .args.op == "hook_apply") | .args.args | select(.tool == "learn_codebase")] as $rows |
    ([$rows[] | select(.args.incremental == true)] | length) == 3 and
    ([$rows[] | select(.args.incremental != true)] | length) == 1
' "$CHITTA_QUEUE" >/dev/null
echo 'ok: index refresh includes edits and deletion; symbol extraction stays throttled'
