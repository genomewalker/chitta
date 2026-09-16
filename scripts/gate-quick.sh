#!/usr/bin/env bash
# Quick gate: what every commit must pass. Seconds to a couple of minutes on
# the login node; no native build, no replica. The full gate (gate-full.sh)
# runs once per stream and before every merge.
#
#   scripts/gate-quick.sh            # all checks
#   scripts/gate-quick.sh --changed  # shell syntax/lint only on files changed vs origin/main
#
# Checks: ruff lint and format on the CI paths, bash -n and shellcheck on
# shell, MCP unit tests, hook Python tests, generated MCP table, MCP surface
# budget, contract snapshot (needs the CLI; skipped without it), docs gates.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 1
PY="${CHITTA_PY:-$(command -v python3)}"
RUFF="${RUFF:-$(dirname "$PY")/ruff}"; command -v "$RUFF" >/dev/null 2>&1 || RUFF=ruff
export OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1 RAYON_NUM_THREADS=1
CI_PY="chitta-mcp hooks benchmarks/smriti benchmarks/noise.py benchmarks/check_eval_immutable.py benchmarks/current_truth benchmarks/continuation scripts/provenance-coverage.py"
fail=0
step() { printf '\n== %s\n' "$1"; }
run() { if "$@"; then echo "ok"; else echo "FAIL: $*"; fail=1; fi; }

step "ruff"
# shellcheck disable=SC2086
run "$RUFF" check $CI_PY
# shellcheck disable=SC2086
run "$RUFF" format --check $CI_PY

step "shell syntax and shellcheck"
if [[ "${1:-}" == "--changed" ]]; then
    mapfile -t shells < <(git diff --name-only origin/main...HEAD -- '*.sh' 2>/dev/null; git diff --name-only -- '*.sh')
else
    mapfile -t shells < <(git ls-files 'hooks/*.sh' 'scripts/*.sh' 'hooks/tests/*.sh')
fi
if [[ ${#shells[@]} -gt 0 ]]; then
    run bash -n "${shells[@]}"
    if command -v shellcheck >/dev/null 2>&1; then run shellcheck -S warning "${shells[@]}"; else echo "shellcheck not installed (skipped)"; fi
else
    echo "no shell files"
fi

step "MCP unit tests"
(cd chitta-mcp && "$PY" -m unittest discover tests 2>&1 | tail -1) | grep -qE '^OK' && echo ok || { echo "FAIL: MCP tests"; fail=1; }

step "hook Python tests"
"$PY" -m unittest discover -s hooks/tests -p 'test_*.py' 2>&1 | tail -1 | grep -qE '^OK' && echo ok || { echo "FAIL: hook python tests"; fail=1; }

step "generated MCP table and surface budget"
run "$PY" scripts/gen-tools-static.py --check
run "$PY" scripts/check-mcp-surface.py

step "contracts"
if [[ -x "${CHITTA_BIN:-$HOME/.claude/bin/chitta}" ]]; then
    bash scripts/contract-snapshot.sh check 2>&1 | tail -1 | grep -q 'contracts unchanged' && echo ok || { echo "FAIL: contract drift (run scripts/contract-snapshot.sh check)"; fail=1; }
else
    echo "no CLI (skipped)"
fi

step "docs"
run bash scripts/check-docs-links.sh
bash scripts/check-citations.sh 2>&1 | grep -qE '^ERROR' && { echo "FAIL: citations"; fail=1; } || echo ok
run "$PY" scripts/check-site.py

printf '\n== quick gate: %s\n' "$([[ $fail == 0 ]] && echo PASS || echo FAIL)"
exit $fail
