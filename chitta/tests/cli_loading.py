#!/usr/bin/env python3
"""CLI loading replies must not wait for daemon readiness or hide retry hints."""

import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
from pathlib import Path


def main():
    with tempfile.TemporaryDirectory(prefix="cli-loading-") as directory:
        root = Path(directory)
        sock = root / "daemon.sock"
        stopped = threading.Event()
        with socket.socket(socket.AF_UNIX) as listener:
            listener.bind(str(sock))
            listener.listen()
            listener.settimeout(0.1)

            def serve():
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
                            response = {"jsonrpc": "2.0", "id": request["id"], "result": result}
                            conn.sendall(json.dumps(response).encode() + b"\n")

            worker = threading.Thread(target=serve)
            worker.start()
            try:
                env = dict(os.environ, XDG_RUNTIME_DIR=str(root), CHITTA_DB_PATH=str(root / "mind"))
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
                        result = subprocess.run(
                            command,
                            input=json.dumps(request) + "\n" if thin else "",
                            capture_output=True,
                            text=True,
                            env=env,
                            timeout=2,
                        )
                        expected = 0 if name in ("health_check", "status") else 75
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
            finally:
                stopped.set()
                worker.join()
    print("CLI loading: 8/8 immediate replies and exit codes passed")


if __name__ == "__main__":
    main()
