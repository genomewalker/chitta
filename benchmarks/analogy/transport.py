"""Explicit Unix RPC only: no daemon discovery, startup, or live fallback."""
from __future__ import annotations

import json
import socket
import time


def rpc(path: str, name: str, arguments: dict, timeout: float = 2) -> dict:
    deadline = time.monotonic() + timeout
    request = dict(jsonrpc="2.0", id=1, method="tools/call",
                   params=dict(name=name, arguments=arguments))
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as conn:
        conn.settimeout(timeout)
        conn.connect(path)
        conn.sendall((json.dumps(request) + "\n").encode())
        data = bytearray()
        while b"\n" not in data:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("RPC deadline exceeded")
            conn.settimeout(remaining)
            chunk = conn.recv(65536)
            if not chunk:
                raise ValueError("replica closed before response")
            data.extend(chunk)
            if len(data) > 64 * 1024 * 1024:
                raise ValueError("oversized RPC response")
    reply = json.loads(data.split(b"\n", 1)[0])
    result = reply.get("result", {})
    if reply.get("error") or result.get("isError"):
        raise ValueError(str(reply.get("error") or result)[:500])
    structured = result.get("structured")
    if not isinstance(structured, dict):
        raise ValueError("missing structured RPC result")
    return structured
