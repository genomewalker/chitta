#!/bin/bash
# Shared core already applies Codex's missing/null field defaults.
SCRIPT_DIR="$(cd -- "${BASH_SOURCE[0]%/*}" && pwd)"
exec "$SCRIPT_DIR/stop-core.sh"
