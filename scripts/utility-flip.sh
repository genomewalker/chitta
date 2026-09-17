#!/usr/bin/env bash
# The CHITTA_UTILITY_RECALL flip procedure from docs/EVALS.md ("Utility
# posteriors"), as one command. Earliest date 2026-09-21: one week of real
# failure signal since post-bash-hook.sh started receiving failure payloads on
# 2026-09-13. Steps 1-3 run here; step 4 (accept) stays a human decision and is
# printed, never applied: setting the flag in the chittad drop-in and restarting
# is the operator's move after reading the comparison.
#
#   scripts/utility-flip.sh            # report, credit --apply, on/off comparison on a private replica
#   scripts/utility-flip.sh --force    # ignore the date gate (rehearsal only; never accept from a rehearsal)
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 1
PY="${CHITTA_PY:-$(command -v python3)}"
OUT="${CHITTA_SCRATCH_TMP:-/projects/caeg/scratch/kbd606/tmp}/utility-flip-$(date +%Y%m%d)"
mkdir -p "$OUT"
if [[ "${1:-}" != "--force" && "$(date +%Y%m%d)" -lt 20260921 ]]; then
    echo "utility-flip: earliest date is 2026-09-21 (one week of failure signal); pass --force only to rehearse." >&2
    exit 2
fi
step() { printf '\n== %s\n' "$1"; }

step "1. failure signal in the outcome ledger"
"$PY" chitta-mcp/outcome_ledger.py report | head -20 | tee "$OUT/report.txt"

step "2. recompute posteriors and apply them (rows before 2026-09-13 carry no failures)"
"$PY" chitta-mcp/outcome_ledger.py credit --apply | tail -5 | tee "$OUT/credit.txt"

step "3. golden and SMRITI with the flag off, then on, on a private replica copy"
export CHITTA_EVAL_MIND="$OUT/replica" CHITTA_EVAL_PORT="${CHITTA_EVAL_PORT:-17490}"
export CHITTA_RECALL_EMBED_WAIT_MS=10000 OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1 RAYON_NUM_THREADS=1
bash scripts/eval-replica.sh start >/dev/null 2>&1 || { echo "replica start failed; see scripts/eval-replica.sh status" >&2; exit 1; }
# shellcheck disable=SC1091
source "$CHITTA_EVAL_MIND/replica.env" 2>/dev/null || true
for arm in off on; do
    if [[ $arm == on ]]; then export CHITTA_UTILITY_RECALL=1; else unset CHITTA_UTILITY_RECALL; fi
    bash scripts/eval-noise.sh --agent claude-code --tasks 3 --trials 3 --output "$OUT/noise-utility-$arm.json" 2>&1 | tail -3
done
bash scripts/eval-replica.sh stop >/dev/null 2>&1 || true

step "4. decision (not applied here)"
"$PY" - "$OUT" <<'PYEOF'
import json, sys
from pathlib import Path
out = Path(sys.argv[1])
bands = json.load(open("benchmarks/noise.json"))
def flat(d, prefix=""):
    for k, v in d.items():
        if isinstance(v, dict): yield from flat(v, prefix + k + ".")
        elif isinstance(v, (int, float)): yield prefix + k, v
arms = {arm: dict(flat(json.load(open(out / f"noise-utility-{arm}.json")))) for arm in ("off", "on")}
band = dict(flat(bands))
for key in ("smriti.on.sr", "golden.ndcg"):
    off = next((v for k, v in arms["off"].items() if k.endswith(key)), None)
    on = next((v for k, v in arms["on"].items() if k.endswith(key)), None)
    margin = next((v for k, v in band.items() if key in k and ("band" in k or "margin" in k or "sd" in k)), None)
    print(f"{key}: off={off} on={on} margin={margin}")
print("ACCEPT only if both improve beyond their band; then add CHITTA_UTILITY_RECALL=1 to the chittad drop-in and restart.")
print(f"raw outputs: {out}")
PYEOF
