#!/usr/bin/env bash
# Login-node gate. Every check retains its exit status and complete diagnostic.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/scripts/build-env.sh"
chitta_log_init || exit 1
cd "$ROOT" || exit 1
PY="$CHITTA_PY"
RUFF="${RUFF:-$(dirname "$PY")/ruff}"; command -v "$RUFF" >/dev/null 2>&1 || RUFF=ruff
export RAYON_NUM_THREADS=1
CI_PY=(chitta-mcp hooks benchmarks/smriti benchmarks/noise.py benchmarks/check_eval_immutable.py benchmarks/current_truth benchmarks/continuation scripts/provenance-coverage.py)
fail=0
run() { chitta_stage "$@" || fail=1; }
run ruff-check "$RUFF" check "${CI_PY[@]}"
run ruff-format "$RUFF" format --check "${CI_PY[@]}"
if [[ "${1:-}" == --changed ]]; then
    mapfile -t shells < <(git diff --name-only origin/main...HEAD -- '*.sh'; git diff --name-only -- '*.sh')
else
    mapfile -t shells < <(git ls-files 'hooks/*.sh' 'scripts/*.sh' 'hooks/tests/*.sh')
fi
shell_syntax() { local f; for f in "${shells[@]}"; do bash -n "$f" || return; done; }
run shell-syntax shell_syntax
if command -v shellcheck >/dev/null 2>&1; then run shellcheck shellcheck -S warning "${shells[@]}"; else echo 'shellcheck not installed (skipped)'; fi
mcp_tests() { (cd chitta-mcp && "$PY" -m unittest discover tests); }
run mcp-tests mcp_tests
run hook-python "$PY" -m unittest discover -s hooks/tests -p 'test_*.py'
run mcp-table "$PY" scripts/gen-tools-static.py --check
run mcp-budget "$PY" scripts/check-mcp-surface.py
# Exit 3 is unavailable, distinct from drift (1) and capture failures (other).
if chitta_stage contracts bash scripts/contract-snapshot.sh check; then :; else
    rc=$?
    if [[ $rc == 3 ]]; then echo "contract service unavailable: deferred to full gate ($GATE_TMP/contracts.log)"; else fail=1; fi
fi
run docs-links bash scripts/check-docs-links.sh
run docs-citations bash scripts/check-citations.sh
run docs-site "$PY" scripts/check-site.py
printf '== quick gate: %s\n' "$([[ $fail == 0 ]] && echo PASS || echo FAIL)"
exit "$fail"
