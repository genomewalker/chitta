#!/usr/bin/env bash
# Snapshot the public contracts of the chitta ecosystem so refactors can prove
# they changed nothing a caller can see. Modelled on the Hermes refactor
# (nousresearch.com/refactoring-hermes-with-1393-agents): tool JSON schemas and
# CLI --help output are compared byte for byte.
#
#   scripts/contract-snapshot.sh write [DIR]   # capture into DIR (default contracts/)
#   scripts/contract-snapshot.sh check [DIR]   # capture to a temp dir and diff against DIR
#
# Sources: the running daemon's tools/list (or CHITTA_SOCKET_PATH), the MCP
# static tool table, `chitta --help` and tool help (daemon names union LEGACY_HANDLERS), hooks.json,
# the hook install manifest, and the Codex plugin manifest.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CHITTA_BIN="${CHITTA_BIN:-$HOME/.claude/bin/chitta}"
PY="${CHITTA_PY:-python3}"
mode="${1:-check}"
dir="${2:-$ROOT/contracts}"

capture() {
    local out="$1"
    mkdir -p "$out"
    printf '{"jsonrpc":"2.0","id":1,"method":"tools/list"}\n' | timeout 30 "$CHITTA_BIN" 2>/dev/null \
        | jq -S '.result.tools | sort_by(.name)' > "$out/daemon-tools.json"
    jq -r '.[].name' "$out/daemon-tools.json" | LC_ALL=C sort -u > "$out/daemon-tool-names.txt"
    (cd "$ROOT/chitta-mcp" && "$PY" - <<'PY' > "$out/mcp-tools.json"
import json, sys
sys.path.insert(0, ".")
import tools_static
tools = list(getattr(tools_static, "TOOLS", [])) + list(getattr(tools_static, "COMPOSITE_TOOLS", []))
rows = []
for t in tools or []:
    d = t.model_dump() if hasattr(t, "model_dump") else (t if isinstance(t, dict) else vars(t))
    rows.append({k: d[k] for k in sorted(d) if k in ("name", "description", "inputSchema")})
print(json.dumps(sorted(rows, key=lambda r: r["name"]), indent=1, sort_keys=True))
PY
    )
    "$CHITTA_BIN" --help > "$out/cli-help.txt" 2>&1 || true
    mkdir -p "$out/cli-tool-help"
    { cat "$out/daemon-tool-names.txt"; sed -n '/^static const .* LEGACY_HANDLERS = {/,/^};/s/^    "\([a-z_0-9]*\)",$/\1/p' "$ROOT/chitta/src/rpc_server.cpp"; } | LC_ALL=C sort -u > "$out/cli-tool-names.txt"
    while read -r t; do
        timeout 5 "$CHITTA_BIN" "$t" --help > "$out/cli-tool-help/$t.txt" 2>&1 </dev/null || true
    done < "$out/cli-tool-names.txt"
    jq -S . "$ROOT/hooks/hooks.json" > "$out/hooks.json"
    cp "$ROOT/hooks/install-manifest.txt" "$out/install-manifest.txt"
    jq -S . "$ROOT/.claude-plugin/plugin.json" > "$out/plugin.json" 2>/dev/null || true
    grep -hoE 'handlers_\["[a-z_0-9]+"\]' "$ROOT"/chitta/src/handlers/register_*.cpp | grep -oE '"[a-z_0-9]+"' | tr -d '"' | LC_ALL=C sort -u > "$out/daemon-handler-names.txt"
}

case "$mode" in
    write) capture "$dir"; echo "contracts written to $dir ($(wc -l < "$dir/daemon-tool-names.txt") daemon tools, $(wc -l < "$dir/cli-tool-names.txt") CLI tools)";;
    check)
        tmp="$(mktemp -d "${TMPDIR:-/projects/caeg/scratch/kbd606/tmp}/contracts.XXXXXX")"
        trap 'rm -rf "$tmp"' EXIT
        capture "$tmp"
        if diff -ru "$dir" "$tmp" > "$tmp.diff"; then echo "contracts unchanged"; else echo "CONTRACT DRIFT:"; head -80 "$tmp.diff"; exit 1; fi;;
    *) echo "usage: $0 write|check [DIR]" >&2; exit 2;;
esac
