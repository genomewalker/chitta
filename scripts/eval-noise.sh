#!/usr/bin/env bash
# No replica lifecycle changes; source its replica.env before real calibration.
# Hook-only smoke: --hook-only --hook-runs 5 --output /tmp/hook-noise.json
# Set CHITTA_BENCH_BIN to an existing CLI. Live smoke never supplies acceptance bands.
# Truth-only replica calibration: --current-truth-only --current-truth-runs 3
# Use --output benchmarks/current_truth/baseline-2026-09-16.json to preserve noise.json.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
exec "${CHITTA_PY:-python3}" "$root/benchmarks/noise.py" run "$@"
