#!/usr/bin/env bash
# Extract the pure writer: never run smart-install or any service command.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
# shellcheck disable=SC1090
source <(sed -n '/^write_primary_node_config() {$/,/^}$/p' "$ROOT/scripts/smart-install.sh")
MIND_PATH="$T/custom mind"
write_primary_node_config "$T/units"
[[ "$(<"$MIND_PATH/.daemon-node")" == "$(hostname -s)" ]]
CHITTA_DAEMON_NODE=remote-primary write_primary_node_config "$T/units"
[[ "$(<"$MIND_PATH/.daemon-node")" == remote-primary ]]
python3 - "$T/units/chittad.service.d/primary-node.conf" "$MIND_PATH/.daemon-node" <<'PY'
import os
import shlex
import socket
import subprocess
import sys
from pathlib import Path

unit = Path(sys.argv[1]).read_text()
marker = Path(sys.argv[2])
assert 'RestartPreventExitStatus=75' in unit
line = next(v for v in unit.splitlines() if v.startswith('ExecStartPre='))
argv = shlex.split(line.split('=', 1)[1])
# systemd resolves $$ to a literal $, then /bin/sh expands the environment.
argv = [v.replace('$$', '$') for v in argv]
env = dict(os.environ, CHITTA_PRIMARY_MARKER=str(marker))
assert subprocess.run(argv, env=env, capture_output=True).returncode == 75
marker.write_text(socket.gethostname().split('.')[0] + '\n')
assert subprocess.run(argv, env=env, capture_output=True).returncode == 0
marker.write_text('')
assert subprocess.run(argv, env=env, capture_output=True).returncode == 0
marker.unlink()
assert subprocess.run(argv, env=env, capture_output=True).returncode == 0
print('ok: primary guard accepts primary/missing/empty marker, rejects other host with 75')
PY

# Default layout reproduces the hand-installed ExecStartPre exactly.
export -f write_primary_node_config
env HOME="$T/home" MIND_PATH="$T/home/.claude/mind" bash -c 'write_primary_node_config "$1"' _ "$T/default-units"
grep -qF "ExecStartPre=/bin/sh -c 'm=%h/.claude/mind/.daemon-node;" "$T/default-units/chittad.service.d/primary-node.conf"
