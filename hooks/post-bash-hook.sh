#!/bin/bash
# Python owns aliases and local I/O; the daemon owns PostToolUse policy.
ROOT=${CHITTA_PLUGIN_DIR:-${CC_SOUL_PLUGIN_DIR:-}}
if [[ ! -f "$ROOT/chitta-mcp/hook_client.py" ]]; then
    HOOK_PATH=$(realpath "${BASH_SOURCE[0]}")
    ROOT=${HOOK_PATH%/hooks/*}
fi
exec python3 -S "$ROOT/chitta-mcp/hook_client.py" post-tool
