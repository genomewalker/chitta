#!/usr/bin/env bash
# Invoke through scripts/on-compute.sh; requires a fresh replica path.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
export CHITTA_LIVE_MIND=/projects/caeg/scratch/kbd606/tmp/learning-cut-20260915-frozen
export CHITTA_EVAL_MIND="${CHITTA_EVAL_MIND:-/projects/caeg/scratch/kbd606/tmp/p13-replay-eval}"
export CHITTA_EVAL_PORT="${CHITTA_EVAL_PORT:-7494}"
export CHITTAD_BIN="$ROOT/bin/chittad" CHITTA_BIN="$ROOT/bin/chitta"
export CHITTA_RECALL_NOW="${CHITTA_RECALL_NOW:-1789423200}"
export CHITTA_RECALL_EMBED_WAIT_MS=10000
export OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1 MKL_NUM_THREADS=1
export PATH="/maps/projects/fernandezguerra/apps/opt/conda/envs/bioinfo/bin:$ROOT/bin:$PATH"
unset CHITTA_SOCKET_PATH CHITTA_HEADLESS CC_SOUL_HEADLESS
[[ ! -e "$CHITTA_EVAL_MIND" ]] || { echo 'requires a fresh replica path'; exit 1; }
trap 'bash scripts/eval-replica.sh stop' EXIT
bash scripts/eval-replica.sh start
set -a
# shellcheck disable=SC1090
source "$CHITTA_EVAL_MIND/replica.env"
set +a
python3 benchmarks/distill/evaluate_replay.py \
    --replay /projects/caeg/scratch/kbd606/tmp/p13-data/replay.json \
    --output "${1:-/projects/caeg/scratch/kbd606/tmp/p13-data/retrieval}"
