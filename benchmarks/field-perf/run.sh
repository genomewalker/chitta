#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
unset CHITTA_HEADLESS CC_SOUL_HEADLESS
# Restart an existing private eval-replica copy, preserving its WAL for recovery.
if [[ "${1:-}" == --restart ]]; then
    shift
    exec /maps/projects/fernandezguerra/apps/opt/conda/envs/bioinfo/bin/python3 \
        scripts/stress-embed-recall.py --restart-only "$@"
fi
exec /maps/projects/fernandezguerra/apps/opt/conda/envs/bioinfo/bin/python3 benchmarks/field-perf/run.py "$@"
