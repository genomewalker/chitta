#!/usr/bin/env bash
# No replica lifecycle changes; source its replica.env before real calibration.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
exec python3 "$root/benchmarks/noise.py" run "$@"
