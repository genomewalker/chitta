#!/usr/bin/env bash
# Fill or refresh the shared FetchContent source cache from a configured build
# dir, so a fresh worktree's cmake configure reuses the 32 dependency sources
# instead of cloning them (37 min from a compute node, 2026-09-19).
#
#   scripts/cmake-deps-cache.sh [BUILD_DIR] [CACHE_DIR]
#
# Defaults: BUILD_DIR = chitta/build of this checkout; CACHE_DIR = $CHITTA_DEPS_CACHE
# or /projects/caeg/scratch/kbd606/tmp/cmake-deps. Git metadata is left out: the
# sources are pinned by revision in CMakeLists.txt and built as plain trees.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${1:-$ROOT/chitta/build}"
cache="${2:-${CHITTA_DEPS_CACHE:-/projects/caeg/scratch/kbd606/tmp/cmake-deps}}"
[[ -d "$build/_deps" ]] || { echo "no _deps under $build; configure a build first" >&2; exit 1; }
mkdir -p "$cache"
for src in "$build"/_deps/*-src; do
    rsync -a --delete --exclude .git "$src/" "$cache/$(basename "$src")/"
done
echo "$(ls -d "$cache"/*-src | wc -l) sources in $cache ($(du -sh "$cache" | cut -f1))"
