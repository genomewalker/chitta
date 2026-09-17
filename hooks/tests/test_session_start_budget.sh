#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/bin" "$T/chitta-mcp"
cp "$ROOT/hooks/session-start-hook.sh" "$T/start.sh"
printf 'resolve_cc_soul_root() { printf "%%s\n" "$FIXTURE"; }\n' > "$T/lib.sh"
touch "$T/chitta-mcp/hook_client.py"
printf '#!/bin/bash\ncat "$FIXTURE/daemon"\n' > "$T/bin/python3"
printf '#!/bin/bash\ncat "$FIXTURE/nav"\n' > "$T/code-nav.sh"
chmod +x "$T/bin/python3"
export FIXTURE="$T" PATH="$T/bin:$PATH"
run() {
    printf '{"cwd":"/repo","session_id":"s"}' | bash "$T/start.sh" > "$T/output"
    (( $(wc -c < "$T/output") <= 1500 ))
}
printf '[handoff] exact\n' > "$T/daemon"
printf '[code-nav] small\n' > "$T/nav"
run
grep -q 'small' "$T/output"
for ((i=0; i<1000; i++)); do printf 'é😀'; done > "$T/nav"
run
grep -q 'exact' "$T/output"
grep -q 'use code_query' "$T/output"
cp "$T/nav" "$T/daemon"
run
grep -q 'Oversized response' "$T/output"
printf '[chitta] daemon unavailable; context not loaded.\n' > "$T/daemon"
run
cmp "$T/daemon" "$T/output"
echo 'SessionStart envelope including code map <=1500 bytes: PASS'
