#!/usr/bin/env bash
# One native recipe. --tests includes release Rust tests and ctest.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
source "$ROOT/scripts/build-env.sh"
chitta_build_init || exit 1
cd "$ROOT" || exit 1
if [[ -n "${CARGO_TARGET_DIR:-}" ]]; then
    case "$(realpath -m "$CARGO_TARGET_DIR")" in "$ROOT"/*) ;; *) echo 'FAIL: shared CARGO_TARGET_DIR is forbidden'; exit 2 ;; esac
fi
chitta_cache_stats before
native() { chitta_stage "$@"; }
# A failed cached stage gets one uncached retry; retain both verdicts and logs.
rust_stage() {
    local name="$1"; shift
    native "$name" bash chitta-field/build.sh "$@" && return 0
    [[ "${RUSTC_WRAPPER:-}" == */sccache ]] || return 1
    echo "cache fallback: $name; performance acceptance invalid"
    chitta_stage "$name-uncached" env -u RUSTC_WRAPPER CHITTA_CACHE=0 bash chitta-field/build.sh "$@"
}
rust_stage rust-build build --release || exit 1
if [[ "${1:-}" == --tests ]]; then native rust-tests bash chitta-field/build.sh test --release || exit 1; fi
native configure cmake -S chitta -B chitta/build -DCMAKE_BUILD_TYPE=Release \
    -DCHITTA_EMBED_DIM="${CHITTA_EMBED_DIM:-768}" -DCMAKE_CXX_COMPILER="$CXX" -DCMAKE_C_COMPILER="$CC" \
    -DCMAKE_CXX_COMPILER_LAUNCHER="${CMAKE_CXX_COMPILER_LAUNCHER:-}" -DCMAKE_C_COMPILER_LAUNCHER="${CMAKE_C_COMPILER_LAUNCHER:-}" || exit 1
native cpp-build cmake --build chitta/build --parallel "$CMAKE_BUILD_PARALLEL_LEVEL" || {
    [[ "${CMAKE_CXX_COMPILER_LAUNCHER:-}" == */ccache ]] || exit 1
    echo 'cache fallback: cpp-build; performance acceptance invalid'
    chitta_stage cpp-build-uncached env CCACHE_DISABLE=1 cmake --build chitta/build --parallel "$CMAKE_BUILD_PARALLEL_LEVEL" || exit 1
}
if [[ "${1:-}" == --tests ]]; then native ctest ctest --test-dir chitta/build -j8 --output-on-failure || exit 1; fi
chitta_cache_stats after
