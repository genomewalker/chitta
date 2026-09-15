#!/usr/bin/env bash
# Run from the checkout containing this script, never the installed live plugin.
set -euo pipefail
EVOLVE_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
export PYTHONPATH="$EVOLVE_ROOT/chitta-mcp${PYTHONPATH:+:$PYTHONPATH}"
# The nightly timer has no conda env on PATH: plain python3 there is the system 3.6,
# which cannot even import the package (2026-09-15 02:44). Honour CHITTA_PY.
exec "${EVOLVE_PYTHON:-${CHITTA_PY:-python3}}" -m evolve.cycle --repo "$EVOLVE_ROOT" "$@"
