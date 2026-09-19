#!/usr/bin/env bash
#SBATCH --job-name=chitta-baseline
#SBATCH --partition=compregular
#SBATCH --cpus-per-task=16
#SBATCH --mem=64G
#SBATCH --time=04:00:00
set -euo pipefail
# sbatch --chdir=/path/to/worktree scripts/baseline-build-env.sbatch <baseline args>
# p22 owns baseline.py; the wrapper supplies this stream's common environment.
source "$PWD/scripts/build-env.sh"
chitta_build_init
exec "$CHITTA_PY" benchmarks/storage/baseline.py "$@"
