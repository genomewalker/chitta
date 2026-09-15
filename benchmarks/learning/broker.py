"""Restricted socket bridge: recall hooks cannot turn the daemon into a host-file proxy."""

from __future__ import annotations

import json
import socket
import socketserver
import threading
from pathlib import Path

from common import canonical

# Memory/session operations only. In particular: no transcript/code-file readers,
# predicates, imports, exports, process control, queue consumers or maintenance.
ALLOWED = {
    "health_check",
    "version_check",
    "recall",
    "recall_lanes",
    "smart_recall",
    "get",
    "query_graph",
    "correction_check",
    "recall_spreading",
    "recall_failure_pattern",
    "realm_detect",
    "session_heartbeat",
    "session_register",
    "session_update",
    "log_event",
    "gate_init",
    "narrative_log",
    "narrative_status",
    "anticipation_filter",
    "anticipation_predict",
    "habit_match",
    "goal_list",
    "curiosity_gaps",
    "msg_inbox",
    "msg_ack",
    "predicate_list",
}


def allowed_request(request):
    return (
        isinstance(request, dict)
        and request.get("method") == "tools/call"
        and isinstance(request.get("params"), dict)
        and request["params"].get("name") in ALLOWED
        and isinstance(request["params"].get("arguments", {}), dict)
    )


class Broker:
    def __init__(self, path, upstream):
        self.path = Path(path)
        self.upstream = str(upstream)
        broker = self

        class Handler(socketserver.StreamRequestHandler):
            def handle(self):
                self.connection.settimeout(120)
                while True:
                    raw = self.rfile.readline(1024 * 1024)
                    if not raw:
                        return
                    try:
                        request = json.loads(raw)
                        if not allowed_request(request):
                            response = {
                                "jsonrpc": "2.0",
                                "id": request.get("id"),
                                "error": {
                                    "code": -32601,
                                    "message": "learning sandbox: RPC denied",
                                },
                            }
                            self.wfile.write(canonical(response) + b"\n")
                            continue
                        with socket.socket(socket.AF_UNIX) as target:
                            target.settimeout(120)
                            target.connect(broker.upstream)
                            target.sendall(canonical(request) + b"\n")
                            with target.makefile("rb") as reply:
                                self.wfile.write(reply.readline(64 * 1024 * 1024))
                    except (ValueError, OSError):
                        return

        class Server(socketserver.ThreadingUnixStreamServer):
            daemon_threads = True

        self.server = Server(str(self.path), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def start(self):
        self.thread.start()
        return self

    def close(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
        self.path.unlink(missing_ok=True)
