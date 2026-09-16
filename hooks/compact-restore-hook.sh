#!/bin/bash
# Client envelope and local transcript/git data; policy lives in the daemon.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/lib.sh"
ROOT=$(resolve_cc_soul_root) || {
    printf '[chitta] daemon unavailable; context not loaded.\n'
    exit 0
}
[[ -f "$ROOT/chitta-mcp/hook_client.py" ]] || ROOT="$(dirname "$(dirname "$(realpath "${BASH_SOURCE[0]}")")")"
exec python3 -S "$ROOT/chitta-mcp/hook_client.py" compact-restore
