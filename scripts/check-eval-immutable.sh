#!/usr/bin/env bash
set -euo pipefail
if [[ $# != 2 ]]; then
    printf 'usage: %s BASE HEAD\n' "$0" >&2
    exit 2
fi
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
exec "${CHITTA_PY:-python3}" "$root/benchmarks/check_eval_immutable.py" "$1" "$2"
