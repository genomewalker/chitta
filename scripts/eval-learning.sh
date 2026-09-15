#!/usr/bin/env bash
# Automatic-learning harness; freezing and running require explicit arguments.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
py="${CHITTA_PY:-/maps/projects/fernandezguerra/apps/opt/conda/envs/bioinfo/bin/python3}"
action="${1:-}"
[[ $# -gt 0 ]] && shift
case "$action" in
    freeze) exec "$py" "$root/benchmarks/learning/freeze.py" freeze "$@" ;;
    run) exec "$py" "$root/benchmarks/learning/runner.py" run "$@" ;;
    report) exec "$py" "$root/benchmarks/learning/runner.py" report "$@" ;;
    *) printf 'usage: %s freeze <options> | run --manifest FILE [--out DIR] [--isolation strict|home-audit] [--dry-run] [--trials 3] | report RUN\n' "$0" >&2; exit 2 ;;
esac
