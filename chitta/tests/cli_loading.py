#!/usr/bin/env python3
"""CLI loading replies must preserve health availability and retry other calls within their budget."""

import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path


def main():
    with tempfile.TemporaryDirectory(prefix="cli-loading-") as directory:
        root = Path(directory)
        sock = root / "daemon.sock"
        stopped = threading.Event()
        loading_left = None
        loading_count = 0
        with socket.socket(socket.AF_UNIX) as listener:
            listener.bind(str(sock))
            listener.listen()
            listener.settimeout(0.1)

            def serve():
                nonlocal loading_left, loading_count
                while not stopped.is_set():
                    try:
                        conn, _ = listener.accept()
                    except socket.timeout:
                        continue
                    with conn, conn.makefile("rb") as stream:
                        for line in stream:
                            request = json.loads(line)
                            name = request.get("params", {}).get("name", request["method"])
                            state = {
                                "loading": True,
                                "status": "warming_up",
                                "phase": "initializing",
                            }
                            result = {"structured": state}
                            if name not in ("health_check", "status"):
                                state.update(error="loading", retry_after_s=1)
                                result["isError"] = True
                            if loading_left is not None:
                                if name == "health_check":
                                    result = {"structured": {"status": "warming_up"}}
                                elif loading_left:
                                    loading_left -= 1
                                    loading_count += 1
                                    state["retry_after_s"] = 0.01
                                elif request["method"] == "tools/list":
                                    result = {"tools": [{"name": "retry_fixture", "description": "fixture", "inputSchema": {"type": "object", "properties": {}}}]}
                                elif request["method"] == "initialize":
                                    result = {"serverInfo": {"version": "5.72.0"}, "protocolVersion": "2024-11-05"}
                                elif name == "error_fixture":
                                    result = {"structured": None, "isError": True, "content": [{"type": "text", "text": "compaction failed"}]}
                                else:
                                    result = {"structured": {"ready": True}}
                            response = {"jsonrpc": "2.0", "id": request["id"], "result": result}
                            conn.sendall(json.dumps(response).encode() + b"\n")

            worker = threading.Thread(target=serve)
            worker.start()
            try:
                env = dict(os.environ, XDG_RUNTIME_DIR=str(root), CHITTA_DB_PATH=str(root / "mind"), CHITTA_CLI_TIMEOUT="0.35")
                for name in ("health_check", "status", "observe", "unknown_loading_tool"):
                    for thin in (False, True):
                        command = [sys.argv[1], "--socket-path", str(sock)]
                        request = {
                            "jsonrpc": "2.0",
                            "id": 42,
                            "method": "tools/call",
                            "params": {"name": name, "arguments": {}},
                        }
                        if not thin:
                            command.append(name)
                        begin = time.monotonic()
                        result = subprocess.run(
                            command,
                            input=json.dumps(request) + "\n" if thin else "",
                            capture_output=True,
                            text=True,
                            env=env,
                            timeout=2,
                        )
                        expected = 0 if name in ("health_check", "status") else 75
                        elapsed = time.monotonic() - begin
                        assert (elapsed < 0.3 if expected == 0 else 0.3 <= elapsed < 1.5), elapsed
                        assert result.returncode == expected, (name, thin, result)
                        reply = json.loads(result.stdout)
                        state = reply["result"]["structured"] if thin else reply
                        assert state["loading"] and state["phase"] == "initializing", reply
                        if expected == 75:
                            assert state["error"] == "loading" and state["retry_after_s"] == 1, (
                                reply
                            )
                        if thin:
                            assert reply["id"] == 42, reply
                for thin in (False, True):
                    loading_left, loading_count = 2, 0
                    command = [sys.argv[1], "--socket-path", str(sock), "--timeout", "3"]
                    if not thin:
                        command.extend(["--json", "retry_fixture"])
                    request["params"]["name"] = "retry_fixture"
                    begin = time.monotonic()
                    result = subprocess.run(command, input=json.dumps(request) + "\n" if thin else "", capture_output=True, text=True, env=env, timeout=4)
                    assert result.returncode == 0, result
                    reply = json.loads(result.stdout)
                    state = reply["result"]["structured"] if thin else reply
                    assert state == {"ready": True}, reply
                    assert loading_count == 2 and time.monotonic() - begin >= 0.5
                request["params"]["name"] = "error_fixture"
                result = subprocess.run([sys.argv[1], "--socket-path", str(sock)], input=json.dumps(request) + "\n", capture_output=True, text=True, env=env, timeout=2)
                assert result.returncode == 0, result
                assert json.loads(result.stdout)["result"]["isError"], result
            finally:
                stopped.set()
                worker.join()
    print("CLI loading: 11/11 health, deadline, loading-twice-then-ready, and null error payload cases passed")


if __name__ == "__main__":
    main()
