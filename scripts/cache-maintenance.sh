#!/usr/bin/env bash
# Orchestrator-only cache hygiene. Dry-run by default. Never delete an NFS
# shard based on this node's process table; a remote server may still own it.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "$ROOT/scripts/build-env.sh"
mode="${1:---dry-run}"
[[ "$mode" == --dry-run || "$mode" == --apply ]] || exit 2
[[ -d "$CHITTA_BUILD_CACHE" ]] || { echo 'cache maintenance: no cache'; exit 0; }
du -sm "$CHITTA_BUILD_CACHE"
if [[ "$mode" == --apply ]] && command -v ccache >/dev/null; then
    CCACHE_DIR="$CHITTA_BUILD_CACHE/ccache" CCACHE_MAXSIZE=10G ccache --cleanup
else
    echo 'dry-run: tool-native ccache cleanup (10G cap)'
fi
[[ ! -d "$CHITTA_BUILD_CACHE/sccache" ]] || find "$CHITTA_BUILD_CACHE/sccache" -mindepth 1 -maxdepth 1 -type d -mtime +14 -print 2>/dev/null | while IFS= read -r shard; do
    echo "skip $shard: remote server/build inactivity not proven; orchestrator must quiesce it"
done
bytes=$(du -sm "$CHITTA_BUILD_CACHE/sccache" 2>/dev/null | cut -f1 || true)
[[ ${bytes:-0} -le 81920 ]] || echo "cache budget exceeded: $bytes MiB (80G target); active/unknown shards retained"
