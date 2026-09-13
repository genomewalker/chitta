#!/usr/bin/env bash
# No replica lifecycle changes; source its replica.env before real calibration.
# Hook-only smoke: --hook-only --hook-runs 5 --output /tmp/hook-noise.json
# Set CHITTA_BENCH_BIN to an existing CLI. Live smoke never supplies acceptance bands.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
exec python3 "$root/benchmarks/noise.py" run "$@"
