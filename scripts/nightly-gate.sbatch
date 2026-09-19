#!/usr/bin/env bash
#SBATCH --job-name=chitta-nightly
#SBATCH --partition=compregular
#SBATCH --exclude=dandycmpn17fl
#SBATCH --cpus-per-task=16
#SBATCH --mem=64G
#SBATCH --time=04:00:00
set -euo pipefail
# Submit from daemon node: sbatch --export=ALL,CHITTA_NIGHTLY_REPO=/path/to/repo scripts/nightly-gate.sbatch
repo="${CHITTA_NIGHTLY_REPO:?set CHITTA_NIGHTLY_REPO to the source checkout}"
base="${CHITTA_SCRATCH_TMP:-/projects/caeg/scratch/kbd606/tmp}/nightly-gate"
mkdir -p "$base"
exec >> "$base/$(date +%F).log" 2>&1
run=uncreated
verdict() {
    local rc=$?
    printf 'nightly verdict=%s node=%s retained=%s\n' "$rc" "$(hostname -s)" "$run"
}
trap verdict EXIT
printf 'nightly started=%s node=%s job=%s\n' "$(date -Is)" "$(hostname -s)" "${SLURM_JOB_ID:-local}"
# A unique path prevents overlapping submissions from sharing build outputs.
run=$(mktemp -d "$base/run.XXXXXX")
git -C "$repo" fetch --quiet origin main
git -C "$repo" worktree add --detach "$run/tree" origin/main
git -C "$run/tree" submodule update --init --quiet chitta-field
cd "$run/tree"
export CHITTA_ON_COMPUTE=0 GATE_TMP="$run/logs"
source scripts/build-env.sh
# This fresh run primes the node and reports its observed cache statistics.
rc=0
bash scripts/gate-full.sh || rc=$?
printf 'nightly commit=%s\n' "$(git rev-parse HEAD)"
exit "$rc"
