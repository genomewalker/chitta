#!/usr/bin/env bash
# Run one command on a compute node instead of the shared login node.
#
#   scripts/on-compute.sh [-c CPUS] [-m MEM] [-t HH:MM:SS] -- <command...>
#
# The login nodes carry a load average of 70–140 all day; a full build, ctest,
# a replica start or a golden run takes three to seven times longer there than
# on an idle compute node. This wrapper runs the command synchronously through
# srun with the working directory, exit status and the CHITTA_*/thread env
# preserved, so gate scripts and Codex streams can prefix heavy commands with
# it and nothing else changes. Compute nodes see /maps, /projects and $HOME;
# node-local /tmp does not survive the job, so replica copies belong on
# /projects/caeg/scratch (eval-replica.sh's default) and TMPDIR is set there.
#
# dandycmpn17fl is excluded: its $HOME NFS client mount has wedged before
# (build-chitta3.sbatch). Set CHITTA_ON_COMPUTE=0 to run locally instead
# (CI, laptops), which makes the wrapper a no-op.
set -euo pipefail
cpus=16 mem=64G time=04:00:00
while [[ $# -gt 0 ]]; do
    case "$1" in
        -c) cpus=$2; shift 2 ;;
        -m) mem=$2; shift 2 ;;
        -t) time=$2; shift 2 ;;
        --) shift; break ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) break ;;
    esac
done
[[ $# -gt 0 ]] || { echo "usage: $0 [-c CPUS] [-m MEM] [-t TIME] -- <command...>" >&2; exit 2; }
if [[ "${CHITTA_ON_COMPUTE:-1}" != 1 ]] || ! command -v srun >/dev/null 2>&1; then
    exec "$@"
fi
scratch_tmp="${CHITTA_SCRATCH_TMP:-/projects/caeg/scratch/kbd606/tmp}"
mkdir -p "$scratch_tmp"
exec srun --quiet -N1 -n1 -c "$cpus" --mem="$mem" -t "$time" \
    --partition="${CHITTA_COMPUTE_PARTITION:-compregular}" \
    --exclude="${CHITTA_COMPUTE_EXCLUDE:-dandycmpn17fl}" \
    --chdir="$PWD" --export=ALL,TMPDIR="$scratch_tmp",OMP_NUM_THREADS="${OMP_NUM_THREADS:-$cpus}",OPENBLAS_NUM_THREADS="${OPENBLAS_NUM_THREADS:-$cpus}",RAYON_NUM_THREADS="${RAYON_NUM_THREADS:-$cpus}" \
    "$@"
