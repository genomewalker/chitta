#!/usr/bin/env bash
# Print the first python3 that imports the MCP SDK: $CHITTA_PY, the conda
# bioinfo env, then PATH. Gates and the contract check use it so a login,
# compute or Codex shell whose python3 lacks `mcp` does not fail the MCP suite.
for c in "${CHITTA_PY:-}" /maps/projects/fernandezguerra/apps/opt/conda/envs/bioinfo/bin/python3 "$(command -v python3 2>/dev/null)"; do
    [[ -n "$c" && -x "$c" ]] && "$c" -c 'import mcp' 2>/dev/null && { echo "$c"; exit 0; }
done
command -v python3
