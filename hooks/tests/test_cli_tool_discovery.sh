#!/usr/bin/env bash
# Exercise the shipped CLI against an isolated, restartable tools/list fixture.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CHITTA_TEST_BIN="${CHITTA_TEST_BIN:-$ROOT/bin/chitta}"
[[ -x "$CHITTA_TEST_BIN" ]] || { echo 'FAIL: build bin/chitta first' >&2; exit 1; }
export CHITTA_TEST_BIN
python3 - <<'PY'
import json
import multiprocessing
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time


def serve(root):
    path = root / "daemon.sock"
    path.unlink(missing_ok=True)
    with socket.socket(socket.AF_UNIX) as listener:
        listener.bind(str(path))
        listener.listen()
        (root / "ready").touch()
        while True:
            conn, _ = listener.accept()
            with conn, conn.makefile("rb") as stream:
                for line in stream:
                    request = json.loads(line)
                    method = request["method"]
                    with (root / "requests").open("a") as log:
                        log.write(method + "\n")
                    if method == "tools/list":
                        result = {"tools": json.loads((root / "tools.json").read_text())}
                    elif request.get("params", {}).get("name") == "health_check":
                        result = {"structured": {"protocol_major": 1, "protocol_minor": 0,
                                                 "status": "ready", "pid": os.getpid()}}
                    else:
                        result = {"structured": request["params"]}
                    conn.sendall((json.dumps({"jsonrpc": "2.0", "id": request["id"],
                                              "result": result}) + "\n").encode())


with tempfile.TemporaryDirectory(prefix="chitta-cli-discovery-") as temporary:
    root = Path(temporary)
    (root / "run").mkdir()
    env = dict(os.environ, XDG_RUNTIME_DIR=str(root / "run"),
               CHITTA_SOCKET_PATH=str(root / "daemon.sock"), HOME=str(root),
               CHITTA_DB_PATH=str(root / "mind"))
    tool = {"name": "schema_fixture", "description": "Discovered description",
            "inputSchema": {"type": "object", "required": ["id"], "properties": {
                "id": {"type": "string", "description": "Identifier"},
                "enabled": {"type": "boolean", "description": "Enable flag", "default": False},
                "count": {"type": "integer", "description": "Count", "default": 7},
                "labels": {"type": "array", "items": {"type": "string"}},
                "payload": {"type": "object"}}}}
    tools = [tool]

    def publish():
        (root / "tools.json").write_text(json.dumps(tools))

    def start():
        (root / "ready").unlink(missing_ok=True)
        process = multiprocessing.Process(target=serve, args=(root,), daemon=True)
        process.start()
        for _ in range(200):
            if (root / "ready").exists():
                return process
            time.sleep(0.01)
        process.terminate()
        process.join()
        raise AssertionError("fixture did not start")

    def run(*args, ok=True):
        result = subprocess.run([env["CHITTA_TEST_BIN"], *args], env=env,
                                text=True, capture_output=True, timeout=10)
        assert (result.returncode == 0) == ok, (args, result.returncode, result.stderr)
        return result.stdout + result.stderr

    def lists():
        return (root / "requests").read_text().splitlines().count("tools/list")

    publish()
    process = start()
    try:
        help_text = run("schema_fixture", "--help")
        assert "Discovered description" in help_text
        assert "--id (required)" in help_text
        assert "--count [default: 7]" in help_text
        assert "--enabled [default: false]" in help_text
        initial_lists = lists()
        assert run("schema_fixture", "--help") == help_text
        assert lists() == initial_lists, "known tool should use cache"
        missing = run("schema_fixture", ok=False)
        assert missing.startswith("Error: Missing required parameter(s): --id\n")
        for value in ("18446744073709551615", "001", "true", '{"a":1}'):
            args = json.loads(run("--json", "schema_fixture", "--id", value,
                                  "--enabled", "true", "--labels", "one,two",
                                  "--payload", '{"a":1}'))["arguments"]
            assert args == {"id": value, "enabled": True, "count": 7,
                            "labels": ["one", "two"], "payload": {"a": 1}}, args
        assert json.loads(run("--json", "schema_fixture", "--id", "x",
                              "--labels", "one"))["arguments"]["labels"] == ["one"]
        args = json.loads(run("schema_fixture", "--json", "--id", "x", "--count", "001",
                              "--enabled"))["arguments"]
        assert args == {"id": "x", "count": 1, "enabled": True}, args
        legacy_args = json.loads(run("--json", "version_check", "--id",
                                     "18446744073709551615"))["arguments"]
        assert legacy_args["id"] == 18446744073709551615
        before_unknown = lists()
        unknown = run("not_a_tool", ok=False)
        assert unknown.startswith("Unknown option: not_a_tool\nUsage:\n")
        assert lists() == before_unknown + 1, "unknown tool should refresh"
        tools.append({"name": "new_fixture", "description": "Added after cache",
                      "inputSchema": {"properties": None}})
        publish()
        assert "Added after cache" in run("new_fixture", "--help")
        legacy = json.loads(run("--json", "version_check"))
        assert legacy["name"] == "version_check", "legacy handler remains callable"
        assert run("background_status", "--help", ok=False).startswith("Unknown option:")
        cache = next((root / "run" / "chitta").glob("cli-tools-*.json"))
        contents = json.loads(cache.read_text())
        assert contents["identity"]["pid"] == process.pid
        assert contents["identity"]["start_time"] and contents["identity"]["boot_id"]
        assert cache.stat().st_mode & 0o777 == 0o600
        cache.write_text("{broken")
        assert run("schema_fixture", "--help") == help_text
        process.terminate()
        process.join()
        assert run("schema_fixture", "--help") == help_text, "offline cached help"
        tool["description"] = "Changed after restart"
        publish()
        process = start()
        assert "Changed after restart" in run("schema_fixture", "--help")
        assert json.loads(cache.read_text())["identity"]["pid"] == process.pid
    finally:
        process.terminate()
        process.join()
print("ok: CLI discovery, schema parsing/help, missing/unknown errors, cache refresh and offline/restart help")
PY
