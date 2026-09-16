#!/usr/bin/env bash
# Real hook lifecycle with a stub CLI: both placements share each session's state.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PYTHONDONTWRITEBYTECODE=1 python3 - "$ROOT" <<'PY'
import json
import os
import socket
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(sys.argv[1])
for placement in ("0", "1"):
    with tempfile.TemporaryDirectory(prefix="p6-markers-") as tmp:
        base = Path(tmp)
        mind = base / "home/.claude/mind"
        mind.mkdir(parents=True)
        runtime = base / "run"
        runtime.mkdir()
        cli = base / "cli"
        cli.write_text('#!/bin/bash\ncase "$1" in queue_write) exit 1;; *) echo "{}";; esac\n')
        cli.chmod(0o755)
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(("CHITTA_", "CC_SOUL_", "MIND"))}
        env.update(HOME=str(base / "home"), CHITTA_DB_PATH=str(mind),
                   CHITTA_BIN=str(cli), CHITTA_RUNTIME_LOCAL=placement,
                   XDG_RUNTIME_DIR=str(runtime), CHITTA_SOCKET_PATH=str(base / "socket"),
                   CHITTA_DISABLE_CONSOLIDATION="1", CHITTA_STRICT_MODE="0",
                   CHITTA_HOOK_BUDGET_MS="3000", CHITTA_HOOK_RPC_BUDGET_S="0")
        state = Path(subprocess.check_output(
            ["bash", "-c", 'source "$1/hooks/lib.sh"; runtime_state_dir', "bash", str(root)],
            env=env, text=True).strip())
        state.mkdir(parents=True, exist_ok=True)
        assert (state == mind) == (placement == "0")

        def hook(name, payload, *args):
            return subprocess.run(["bash", str(root / "hooks" / name), *args],
                                  input=json.dumps(payload), text=True, env=env,
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  timeout=15, check=True)

        # No local daemon: SessionStart still resets state and reads the turn.
        (state / ".turn_index_placement").write_text("7\n")
        (state / ".size_warned_placement").touch()
        hook("session-start-hook.sh", {"session_id": "placement", "source": "startup"})
        assert (state / ".last_store_turn_placement").read_text().strip() == "7"
        assert not (state / ".size_warned_placement").exists()
        assert (mind / ".strict_claude_style").exists()  # persistent policy

        # A socket inode satisfies shell availability; stub CLI serves no real RPC.
        with socket.socket(socket.AF_UNIX) as sock:
            sock.bind(env["CHITTA_SOCKET_PATH"])
            hook("prompt-core.sh", {"session_id": "placement", "prompt": "placement regression fixture"})
            assert (state / ".turn_index_placement").read_text().strip() == "8"
            assert (state / ".last_user_message").read_text().strip() == "placement regression fixture"
            assert (state / ".hb_placement").exists()
            hook("pre-tool-hook.sh", {"session_id": "placement", "tool_input": {"command": "true"}}, "Bash")
            assert (state / ".soul_injected_placement_8").exists()
            # Read dedup and its escape hatch must select the same placement.
            source = base / "large.py"
            source.write_text("pass\n" * 250)
            read_input = {"session_id": "placement", "tool_input": {"file_path": str(source)}}
            hook("pre-tool-hook.sh", read_input, "Read")
            assert (state / ".trace_cache_placement").exists()
            assert (state / ".read_cache_placement").exists()
            repeat = hook("pre-tool-hook.sh", read_input, "Read")
            assert "read-dedup" in repeat.stdout
            (state / ".allow_read_placement").touch()
            allowed = hook("pre-tool-hook.sh", read_input, "Read")
            assert "read-dedup" not in allowed.stdout
            # Stop's heartbeat runs even without a transcript.
            (state / ".hb_placement").unlink()
            hook("stop-core.sh", {"session_id": "placement"})
            assert (state / ".hb_placement").exists()
        if placement == "1":
            for marker in (".hb_placement", ".turn_index_placement", ".last_user_message",
                           ".soul_injected_placement_8", ".last_store_turn_placement"):
                assert not (mind / marker).exists(), marker
        print("ok: real hook marker placement", placement)
PY
