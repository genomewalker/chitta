#!/usr/bin/env bash
# Full gate: once per stream and before every merge. Native build and tests,
# every hook suite, and with --replica the store and recall gates on a private
# copy of the frozen family. Heavy steps run on a compute node through
# scripts/on-compute.sh (CHITTA_ON_COMPUTE=0 keeps them local).
#
#   scripts/gate-full.sh                # quick gate + Rust + C++ + ctest + hook suites
#   scripts/gate-full.sh --replica      # ... + chaos 9/9 + restart identity 20/20
#   scripts/gate-full.sh --replica --recall   # ... + golden and current-truth (recall changes)
#
# Rust and C++ builds are incremental per worktree (each keeps its own target
# and build dirs), so the second run is minutes, not tens of minutes.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 1
PY="$("$ROOT/scripts/python-with-mcp.sh")"; export PATH="$(dirname "$PY"):$PATH"
ON="$ROOT/scripts/on-compute.sh"
replica=0 recall=0
GATE_TMP="$(mktemp -d "${TMPDIR:-/projects/caeg/scratch/kbd606/tmp}/gate-full.XXXXXX")"
for a in "$@"; do case "$a" in --replica) replica=1 ;; --recall) replica=1; recall=1 ;; esac; done
fail=0
step() { printf '\n== %s\n' "$1"; }
export LIBRARY_PATH="/maps/projects/fernandezguerra/apps/opt/conda/envs/bioinfo/lib:${LIBRARY_PATH:-}"
export LD_LIBRARY_PATH="/maps/projects/fernandezguerra/apps/opt/conda/envs/bioinfo/lib:${LD_LIBRARY_PATH:-}"
CXX_BIN="${CXX:-$(ls /maps/projects/fernandezguerra/apps/opt/conda/envs/bioinfo/bin/*-g++ 2>/dev/null | head -1)}"
export CXX="${CXX_BIN:-c++}"

step "quick gate"
bash scripts/gate-quick.sh || fail=1

step "Rust build and tests (release)"
# Rust tests use TempDir: node-local /tmp, not the NFS scratch on-compute exports as
# TMPDIR (NFS server clocks make a just-touched file look younger than "now" and the
# janitor age-gate test fails there, 2026-09-17).
TMPDIR=/tmp "$ON" -c 16 -- bash -c 'cd chitta-field && ./build.sh build --release 2>&1 | grep -E "^error" ; ./build.sh test --release 2>&1 | grep -E "^test result|FAILED|panicked"' | tee -a /dev/stderr | grep -qE '^test result: ok' || { echo "FAIL: Rust"; fail=1; }

step "C++ build and ctest"
# A fresh worktree has no configured build dir: configure it like the main checkout
# (Release, 768-d embeddings, conda g++; llama.cpp off, the gate does not need chitta_hintd).
[[ -f chitta/build/CMakeCache.txt && ( -f chitta/build/Makefile || -f chitta/build/build.ninja ) ]] || "$ON" -c 4 -- cmake -S chitta -B chitta/build -DCMAKE_BUILD_TYPE=Release \
    -DCHITTA_EMBED_DIM="${CHITTA_EMBED_DIM:-768}" -DCMAKE_CXX_COMPILER="${CXX:-/maps/projects/fernandezguerra/apps/opt/conda/envs/bioinfo/bin/g++}" > /dev/null
"$ON" -c 16 -- bash -c 'cd chitta && cmake --build build --parallel 2>&1 | grep -E "error|Built target chittad"; cd build && ctest -j8 2>&1 | grep -E "tests passed|tests failed"' | tee -a /dev/stderr | grep -q '100% tests passed' || { echo "FAIL: C++/ctest"; fail=1; }

step "hook suites"
pass=0; failed=""
for t in hooks/tests/test_*.sh; do
    # Hook tests use mktemp -d; on compute nodes on-compute exports an NFS TMPDIR whose
    # mtimes skew, which fails handoff_capsule and noise_hook_metric (2026-09-18). Node-local /tmp.
    if TMPDIR=/tmp timeout 300 bash "$t" >/dev/null 2>&1; then pass=$((pass+1)); else failed="$failed $(basename "$t")"; fi
done
echo "hook suites passed=$pass failed=[${failed# }]"
[[ -z "$failed" ]] || fail=1

if [[ $replica == 1 ]]; then
    step "chaos harness (compute node)"
    "$ON" -c 16 -m 96G -- "$PY" scripts/chaos-replica.py > "$GATE_TMP/chaos.log" 2>&1; rc=$?; tail -3 "$GATE_TMP/chaos.log"
    [[ $rc -eq 0 ]] && echo ok || { echo "FAIL: chaos (exit $rc; $GATE_TMP/chaos.log)"; fail=1; }
    step "restart identity (compute node)"
    CHITTA_RECALL_EMBED_WAIT_MS=10000 "$ON" -c 16 -m 96G -- "$PY" scripts/restart-identity.py --restarts 3 > "$GATE_TMP/identity.log" 2>&1; rc=$?; grep -E '^(restart|control) [0-9]+:' "$GATE_TMP/identity.log"
    [[ $rc -eq 0 ]] && echo ok || { echo "FAIL: identity (exit $rc; $GATE_TMP/identity.log)"; fail=1; }
fi
if [[ $recall == 1 ]]; then
    step "golden and current-truth on the replica (compute node)"
    echo "run scripts/eval-replica.sh start on a private copy, then the golden panel three times and benchmarks/current_truth/run.py; compare against the stored bands (see docs/EVALS.md)"
fi

bash scripts/tmp-janitor.sh
printf '\n== full gate: %s\n' "$([[ $fail == 0 ]] && echo PASS || echo FAIL)"
exit $fail
