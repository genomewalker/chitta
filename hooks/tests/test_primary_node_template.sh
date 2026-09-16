#!/usr/bin/env bash
# Extract the pure writer: never run smart-install or any service command.
# The host check itself lives in chittad (exit 75 when <mind>/.daemon-node
# names another host); the drop-in only keeps systemd from restarting on 75.
# An ExecStartPre exit is not covered by RestartPreventExitStatus (probed
# 2026-09-16), so the template must not carry the check there.
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
unit="$T/units/chittad.service.d/primary-node.conf"
grep -qF 'RestartPreventExitStatus=75' "$unit"
! grep -q '^ExecStartPre=' "$unit"
! grep -q '^ExecStart=' "$unit"
echo "ok: template writes the marker and a restart-suppressing drop-in without ExecStartPre"

# The daemon-side check, when a built daemon is available (dev checkouts, the
# build job); the hooks CI job has no binary and skips this part.
BIN="${CHITTA_TEST_DAEMON:-$ROOT/bin/chittad}"
if [[ -x "$BIN" ]]; then
    M="$T/mind"
    mkdir -p "$M"
    echo other-host > "$M/.daemon-node"
    set +e
    timeout 30 "$BIN" daemon --path "$M" --foreground --no-autonomous --no-distill --no-enrich >"$T/daemon.log" 2>&1
    rc=$?
    set -e
    [[ "$rc" == 75 ]] || { echo "FAIL: daemon exited $rc, expected 75"; tail -3 "$T/daemon.log"; exit 1; }
    grep -q 'primary node for' "$T/daemon.log"
    echo "ok: chittad exits 75 on a non-primary host"
else
    echo "skip: bin/chittad not built (set CHITTA_TEST_DAEMON)"
fi
