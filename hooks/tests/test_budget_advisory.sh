#!/usr/bin/env bash
# [budget] advisory: one stderr line, once per session, when mean context/turn
# (from the Stop snapshot's token_usage) exceeds CHITTA_CONTEXT_BUDGET.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
# Source only the production function definition; no hook side effects.
sed -n '/^_budget_advisory() {/,/^}/p' "$ROOT/hooks/stop-core.sh" > "$T/functions"
source "$T/functions"

HOOK_STATE_DIR="$T/state"
mkdir -p "$HOOK_STATE_DIR"
SESSION_ID=budget-test

# Over budget (default 150000): (300000+120000+10000)/3 = 143333 ... use values
# that clear the default comfortably: mean 200000 over 3 turns.
_SNAP_TOTAL_INPUT=500000 _SNAP_TOTAL_CACHE_READ=90000 _SNAP_TOTAL_CACHE_CREATION=10000 _SNAP_N_MESSAGES=3
out=$(_budget_advisory 2>&1)
[[ "$out" == "[budget] context 200k/turn over 3 turns; /compact now or start a fresh session" ]] || {
    echo "FAIL: unexpected line: $out"; exit 1;
}
echo "ok: single [budget] line printed when mean context/turn exceeds the budget"

# Once per session: a second call with the same session/marker prints nothing.
out=$(_budget_advisory 2>&1)
[[ -z "$out" ]] || { echo "FAIL: advisory fired twice for the same session: $out"; exit 1; }
echo "ok: marker suppresses a second advisory in the same session"

# A different session gets its own advisory.
SESSION_ID=budget-test-2
out=$(_budget_advisory 2>&1)
[[ -n "$out" ]] || { echo "FAIL: expected an advisory for a fresh session"; exit 1; }
echo "ok: a different session is not suppressed by another session's marker"

# Under budget: no line at all.
SESSION_ID=budget-test-3
_SNAP_TOTAL_INPUT=30000 _SNAP_TOTAL_CACHE_READ=5000 _SNAP_TOTAL_CACHE_CREATION=0 _SNAP_N_MESSAGES=3
out=$(_budget_advisory 2>&1)
[[ -z "$out" ]] || { echo "FAIL: advisory fired under budget: $out"; exit 1; }
echo "ok: no advisory when mean context/turn is under budget"

# CHITTA_CONTEXT_BUDGET=0 disables the advisory outright.
SESSION_ID=budget-test-4
_SNAP_TOTAL_INPUT=500000 _SNAP_TOTAL_CACHE_READ=90000 _SNAP_TOTAL_CACHE_CREATION=10000 _SNAP_N_MESSAGES=3
out=$(CHITTA_CONTEXT_BUDGET=0 _budget_advisory 2>&1)
[[ -z "$out" ]] || { echo "FAIL: advisory fired with CHITTA_CONTEXT_BUDGET=0: $out"; exit 1; }
echo "ok: CHITTA_CONTEXT_BUDGET=0 disables the advisory"
