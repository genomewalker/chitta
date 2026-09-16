#!/bin/bash
# Local envelope; daemon owns policy through ledger_op.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"
ROOT=$(resolve_cc_soul_root) || { printf '[chitta] daemon unavailable; context not loaded.\n'; exit 0; }
[[ -f "$ROOT/chitta-mcp/hook_client.py" ]] || ROOT="$(dirname "$(dirname "$(realpath "${BASH_SOURCE[0]}")")")"
exec python3 -S "$ROOT/chitta-mcp/hook_client.py" persona "$@"
