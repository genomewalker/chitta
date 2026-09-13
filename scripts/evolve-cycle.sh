#!/usr/bin/env bash
# Run from the checkout containing this script, never the installed live plugin.
set -euo pipefail
EVOLVE_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
export PYTHONPATH="$EVOLVE_ROOT/chitta-mcp${PYTHONPATH:+:$PYTHONPATH}"
exec "${EVOLVE_PYTHON:-python3}" -m evolve.cycle --repo "$EVOLVE_ROOT" "$@"
