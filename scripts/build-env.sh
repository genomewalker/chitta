#!/usr/bin/env bash
# Source only: no cache server, installation, or filesystem mutation here.
CHITTA_BUILD_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
CHITTA_REAL_HOME="${CHITTA_REAL_HOME:-$(getent passwd "$(id -u)" | cut -d: -f6)}"
export CHITTA_REAL_HOME
_chitta_prepend() {
    local key="$1" entry="$2" value="${!1:-}"
    [[ -d "$entry" ]] || return 0
    case ":$value:" in *":$entry:"*) ;; *) printf -v "$key" '%s' "$entry${value:+:$value}"; export "$key" ;; esac
}
_chitta_conda=/maps/projects/fernandezguerra/apps/opt/conda/envs/bioinfo
_chitta_prepend PATH "$_chitta_conda/bin"
_chitta_prepend PATH "$CHITTA_REAL_HOME/.local/bin"
_chitta_prepend PATH "$CHITTA_REAL_HOME/.cargo/bin"
_chitta_prepend PATH "${RUSTUP_HOME:-$CHITTA_REAL_HOME/.rustup}/toolchains/1.93.0-x86_64-unknown-linux-gnu/bin"
_chitta_prepend LIBRARY_PATH "$_chitta_conda/lib"
_chitta_prepend LD_LIBRARY_PATH "$_chitta_conda/lib"
if [[ -x "$_chitta_conda/bin/x86_64-conda-linux-gnu-g++" ]]; then
    export CXX="${CXX:-$_chitta_conda/bin/x86_64-conda-linux-gnu-g++}" CC="${CC:-$_chitta_conda/bin/x86_64-conda-linux-gnu-gcc}"
else
    export CXX="${CXX:-c++}" CC="${CC:-cc}"
fi
export CARGO_HOME="${CARGO_HOME:-$CHITTA_REAL_HOME/.cargo}" RUSTUP_HOME="${RUSTUP_HOME:-$CHITTA_REAL_HOME/.rustup}"
export CHITTA_PY="${CHITTA_PY:-$("$CHITTA_BUILD_ROOT/scripts/python-with-mcp.sh")}"
export PYO3_PYTHON="${PYO3_PYTHON:-$CHITTA_PY}"
export CHITTA_SCRATCH_TMP="${CHITTA_SCRATCH_TMP:-/projects/caeg/scratch/kbd606/tmp}"
[[ -d "$CHITTA_SCRATCH_TMP" ]] || export CHITTA_SCRATCH_TMP="${TMPDIR:-/tmp}"
export CHITTA_BUILD_CACHE="${CHITTA_BUILD_CACHE:-$CHITTA_SCRATCH_TMP/build-cache}"
[[ ! -d "${CHITTA_DEPS_CACHE:-$CHITTA_SCRATCH_TMP/cmake-deps}" ]] || export CHITTA_DEPS_CACHE="${CHITTA_DEPS_CACHE:-$CHITTA_SCRATCH_TMP/cmake-deps}"
export CARGO_BUILD_JOBS="${CARGO_BUILD_JOBS:-${SLURM_CPUS_PER_TASK:-16}}" CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-${SLURM_CPUS_PER_TASK:-16}}"
export OPENBLAS_NUM_THREADS=1 OMP_NUM_THREADS=1
# CMake and Rust must receive the same gate dimension.
export CHITTA_EMBED_DIM="${CHITTA_EMBED_DIM:-768}"
unset CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER
# Deliberately do not set SCCACHE_BASEDIRS: it is server-scoped, not per checkout.
# Relative Rust sources and common registry paths retain existing compilation flags.
chitta_log_init() {
    mkdir -p "$CHITTA_SCRATCH_TMP" || return
    # Never inherit the shared /tmp root or an NFS/long runtime path. Reuse
    # only our own private directory on this node (including nested gates).
    if [[ "${CHITTA_RUNTIME_NODE:-}" != "$(hostname -s)" || "${TMPDIR:-}" != /tmp/cb.* || ! -O "${TMPDIR:-/nonexistent}" || -L "${TMPDIR:-/nonexistent}" ]]; then
        TMPDIR=$(mktemp -d /tmp/cb.XXXXXX) || return
        export TMPDIR CHITTA_RUNTIME_NODE="$(hostname -s)"
    fi
    [[ -d "$TMPDIR" && -w "$TMPDIR" ]] || { echo "FAIL: TMPDIR is not writable: $TMPDIR" >&2; return 1; }
    GATE_TMP="${GATE_TMP:-$(mktemp -d "$CHITTA_SCRATCH_TMP/gate.XXXXXX")}"
    export GATE_TMP
    mkdir -p "$GATE_TMP" || return
}
chitta_build_init() {
    chitta_log_init || return
    export CCACHE_DIR="${CCACHE_DIR:-$CHITTA_BUILD_CACHE/ccache}" CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-10G}"
    export CCACHE_TEMPDIR="$TMPDIR" CCACHE_COMPILERCHECK=content CCACHE_NOINODECACHE=true
    export CCACHE_BASEDIR="$CHITTA_BUILD_ROOT"
    if [[ "${CHITTA_CACHE:-1}" == 1 ]] && command -v ccache >/dev/null && mkdir -p "$CCACHE_DIR" && [[ -w "$CCACHE_DIR" ]]; then
        export CMAKE_C_COMPILER_LAUNCHER="${CMAKE_C_COMPILER_LAUNCHER-$(command -v ccache)}"
        export CMAKE_CXX_COMPILER_LAUNCHER="${CMAKE_CXX_COMPILER_LAUNCHER-$(command -v ccache)}"
    else
        echo 'cache: C/C++ cold (disabled, absent, or inaccessible)'
    fi
    if [[ -z "${RUSTC_WRAPPER:-}" && "${CHITTA_CACHE:-1}" == 1 ]] && command -v sccache >/dev/null; then
        export SCCACHE_DIR="$CHITTA_BUILD_CACHE/sccache/$(hostname -s)/v1" SCCACHE_CACHE_SIZE="${SCCACHE_CACHE_SIZE:-20G}"
        export SCCACHE_IDLE_TIMEOUT=600 SCCACHE_CONF=/dev/null
        local identity socket_root
        identity=$(printf '%s' "$SCCACHE_DIR" | sha256sum | cut -c1-16)
        socket_root="/tmp/chitta-sccache-$(id -u)-$identity"
        export SCCACHE_SERVER_UDS="$socket_root/server.sock"
        if (umask 077; mkdir -p "$socket_root" "$SCCACHE_DIR") && [[ ! -L "$socket_root" && -O "$socket_root" && -w "$SCCACHE_DIR" ]]; then
            (
                flock -w 30 9 || exit 1
                sccache --show-stats --stats-format json > "$GATE_TMP/sccache-probe.json" 2> "$GATE_TMP/sccache-probe.log" || {
                    sccache --start-server >> "$GATE_TMP/sccache-probe.log" 2>&1 || exit
                    sccache --show-stats --stats-format json > "$GATE_TMP/sccache-probe.json" || exit
                }
                "$CHITTA_PY" - "$GATE_TMP/sccache-probe.json" "$SCCACHE_DIR" <<'CHECK'
import json,sys
v=json.load(open(sys.argv[1]))
assert sys.argv[2] in v['cache_location'], v
assert not v.get('basedirs'), 'incompatible server basedirs'
CHECK
            ) 9> "$socket_root/start.lock" && export RUSTC_WRAPPER="$(command -v sccache)" CARGO_INCREMENTAL=0
        fi
    fi
    [[ -n "${RUSTC_WRAPPER:-}" ]] || echo 'cache: Rust cold (disabled, absent, or unavailable; see sccache-probe.log)'
    printf 'build node=%s CXX=%s rust_wrapper=%s cpp_launcher=%s logs=%s\n' "$(hostname -s)" "$CXX" "${RUSTC_WRAPPER:-none}" "${CMAKE_CXX_COMPILER_LAUNCHER:-none}" "$GATE_TMP"
}
chitta_stage() {
    local name="$1"; shift
    local log="$GATE_TMP/$name.log" start="$SECONDS" rc
    printf '== %s\n' "$name"
    printf 'command:' > "$log"; printf ' %q' "$@" >> "$log"; printf '\n' >> "$log"
    "$@" >> "$log" 2>&1; rc=$?
    printf '%s\t%s\t%s\n' "$name" "$((SECONDS-start))" "$rc" >> "$GATE_TMP/timings.tsv"
    if ((rc)); then printf 'FAIL: %s (exit %s; %s)\n' "$name" "$rc" "$log"; else printf 'PASS: %s (%ss; %s)\n' "$name" "$((SECONDS-start))" "$log"; fi
    return "$rc"
}
chitta_cache_stats() {
    local label="$1"
    printf "cache statistics: %s node=%s\n" "$label" "$(hostname -s)"
    if [[ "${RUSTC_WRAPPER:-}" == */sccache ]]; then
        sccache --show-stats > "$GATE_TMP/sccache-$label.log" 2>&1 || true
        grep -E '^Cache (hits|misses|size)|^Non-cacheable|^Cache location' "$GATE_TMP/sccache-$label.log" || true
    fi
    if command -v ccache >/dev/null; then
        ccache --show-stats > "$GATE_TMP/ccache-$label.log" 2>&1 || true
        head -n 9 "$GATE_TMP/ccache-$label.log"
    fi
}
