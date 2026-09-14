#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
unset CHITTA_HEADLESS CC_SOUL_HEADLESS
exec /maps/projects/fernandezguerra/apps/opt/conda/envs/bioinfo/bin/python3 benchmarks/field-perf/run.py "$@"
