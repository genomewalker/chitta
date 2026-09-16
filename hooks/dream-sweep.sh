#!/bin/bash
# Nightly local transcript scan; daemon selects sessions and owns distillation.
export CHITTA_MAX_WAIT="${CHITTA_MAX_WAIT:-90}"
export CHITTA_HOOK_BUDGET_MS="${CHITTA_HOOK_BUDGET_MS:-1020000}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec python3 -S "$SCRIPT_DIR/../chitta-mcp/hook_client.py" dream-sweep "$@"
