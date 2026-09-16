#!/bin/bash
# Git envelope; cleanup selection and crystallization are daemon policy.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export CHITTA_BIN="${CHITTA_BIN:-$SCRIPT_DIR/../bin/chitta}"
exec python3 -S "$SCRIPT_DIR/../chitta-mcp/hook_client.py" post-commit "$@"
