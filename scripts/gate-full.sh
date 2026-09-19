#!/usr/bin/env bash
# Full gate: all native and hook checks, one compute allocation, retained logs.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/scripts/build-env.sh"
cd "$ROOT" || exit 1
if [[ "${CHITTA_ON_COMPUTE:-1}" == 1 && -z "${SLURM_JOB_ID:-}" ]] && command -v srun >/dev/null; then
    exec bash "$ROOT/scripts/on-compute.sh" -c 16 -- bash "$0" "$@"
fi
chitta_build_init || exit 1
export CHITTA_ON_COMPUTE=0
replica=0 recall=0
for a in "$@"; do case "$a" in --replica) replica=1 ;; --recall) replica=1; recall=1 ;; *) echo "unknown option: $a"; exit 2 ;; esac; done
fail=0
run() { chitta_stage "$@" || fail=1; }
printf 'full gate node=%s logs=%s\n' "$(hostname -s)" "$GATE_TMP"
# Keep nested timings and diagnostics distinct.
run quick env GATE_TMP="$GATE_TMP/quick" bash scripts/gate-quick.sh
run native bash scripts/build.sh --tests
# Surface native step verdicts and cache counters in the outer gate log too.
grep -E '^(PASS:|FAIL:|cache statistics:|Cache hits|Cache misses|Cache location|[[:space:]]+Hits:|[[:space:]]+Misses:)' "$GATE_TMP/native.log" || true
chitta_cache_stats hooks-before
pass=0
for t in hooks/tests/test_*.sh; do
    if chitta_stage "$(basename "$t" .sh)" timeout 300 bash "$t"; then pass=$((pass+1)); else fail=1; fi
done
echo "hook suites passed=$pass"
chitta_cache_stats hooks-after
# An unavailable quick probe is never counted as end-to-end coverage.
run daemon-contracts bash scripts/contract-snapshot.sh check
if [[ $replica == 1 ]]; then
    run chaos "$CHITTA_PY" scripts/chaos-replica.py
    run identity env CHITTA_RECALL_EMBED_WAIT_MS=10000 "$CHITTA_PY" scripts/restart-identity.py --restarts 3
fi
if [[ $recall == 1 ]]; then
    echo 'FAIL: recall panels require the separate pinned replica procedure in docs/EVALS.md'; fail=1
fi
# Generic deletion remains orchestrator-owned; gates only report stale scratch.
run tmp-janitor bash scripts/tmp-janitor.sh --dry-run
printf '== full gate: %s\n' "$([[ $fail == 0 ]] && echo PASS || echo FAIL)"
exit "$fail"
