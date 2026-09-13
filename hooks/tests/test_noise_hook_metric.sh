#!/bin/bash
# Exercise panel aggregation, empty/failure rejection, and isolated hook launch.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PYTHONDONTWRITEBYTECODE=1 python3 - "$ROOT" <<'PY'
import importlib.util
import json
import os
import socket
import subprocess
import sys
import tempfile
from pathlib import Path
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("noise", Path(sys.argv[1]) / "benchmarks/noise.py")
noise = importlib.util.module_from_spec(spec)
spec.loader.exec_module(noise)
with tempfile.TemporaryDirectory() as temp:
    live = str(Path(temp) / "daemon.sock")
    sock = socket.socket(socket.AF_UNIX)
    sock.bind(live)
    prompts = []
    values = iter([300, 100, 200, 600, 400, 500])
    def run(cmd, **kwargs):
        if cmd[-1] == "status":
            return subprocess.CompletedProcess(cmd, 0)
        env = kwargs["env"]
        assert "CHITTA_HEADLESS" not in env and "CC_SOUL_HEADLESS" not in env
        assert env["HOME"] != os.environ.get("HOME")
        assert env["CHITTA_DB_PATH"].startswith(str(Path(env["HOME"]).parent))
        if len(cmd) > 2 and "get_socket_path" in cmd[2]:
            return subprocess.CompletedProcess(cmd, 0, env["XDG_RUNTIME_DIR"] + "/stub.sock\n")
        assert Path(env["XDG_RUNTIME_DIR"], "stub.sock").resolve() == Path(live)
        assert '*) exit 0' in Path(env["CHITTA_BIN"]).read_text()
        prompts.append(json.loads(kwargs["input"])["prompt"])
        total = next(values)
        return subprocess.CompletedProcess(cmd, 0, f'[admit] | t:sem=1,total={total}\n')
    with patch.dict(os.environ, {"CHITTA_BENCH_SOCKET": live, "CHITTA_BENCH_BIN": "/bin/true",
                                 "CHITTA_HEADLESS": "1", "CC_SOUL_HEADLESS": "1"}):
        with patch.object(noise.subprocess, "run", side_effect=run):
            metric = noise.hook_runs(2)
        assert metric["samples"] == [200, 500] and metric["median"] == 350
        assert metric["n"] == 2 and metric["sd"] > 0
        assert prompts == list(noise.HOOK_QUERIES) * 2
        def empty(cmd, **kwargs):
            if cmd[-1].endswith("prompt-core.sh"):
                return subprocess.CompletedProcess(cmd, 0, "{}")
            return run(cmd, **kwargs)
        with patch.object(noise.subprocess, "run", side_effect=empty):
            try:
                noise.hook_runs(2)
            except ValueError as exc:
                assert "refusing empty timing" in str(exc)
            else:
                raise AssertionError("empty output accepted")
        def failed(cmd, **kwargs):
            if cmd[-1].endswith("prompt-core.sh"):
                raise subprocess.CalledProcessError(1, cmd)
            return run(cmd, **kwargs)
        with patch.object(noise.subprocess, "run", side_effect=failed):
            try:
                noise.hook_runs(2)
            except subprocess.CalledProcessError:
                pass
            else:
                raise AssertionError("failed hook accepted")
    sock.close()
try:
    noise.band({"acceptance_ready": False}, "hook_total_ms")
except ValueError:
    pass
else:
    raise AssertionError("live smoke accepted as calibration")
assert noise.band({"acceptance_ready": True, "metrics": {"hook_total_ms": metric}},
                  "hook_total_ms") == 2 * metric["sd"]
print("ok: hook panels, isolation, empty/failure rejection, and acceptance bands")
PY
