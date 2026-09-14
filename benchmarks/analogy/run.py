#!/usr/bin/env python3
"""Evaluate fixed analogy labels on CHITTA_EVAL_SOCKET; never fall back to live."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import socket as unix_socket
import statistics
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path


def matches(result: dict, expected: dict) -> bool:
    if "id" in expected:
        return str(result.get("id", "")) == str(expected["id"])
    return str(result.get("answer", "")).casefold().strip() == expected["answer"].casefold().strip()


def rpc_probe(task: dict, socket: str) -> dict:
    """Diagnostic for CLIs whose static tool allowlist predates recall_analogy.

    Explicit Unix socket only; this transport has no daemon startup path.
    """
    deadline = time.monotonic() + 2
    request = dict(jsonrpc="2.0", id=1, method="tools/call",
                   params=dict(name="recall_analogy", arguments=dict(task["params"], limit=3)))
    with unix_socket.socket(unix_socket.AF_UNIX, unix_socket.SOCK_STREAM) as conn:
        conn.settimeout(2)
        conn.connect(socket)
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("timeout (2 s)")
        conn.settimeout(remaining)
        conn.sendall((json.dumps(request) + "\n").encode())
        data = b""
        while b"\n" not in data:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("timeout (2 s)")
            conn.settimeout(remaining)
            chunk = conn.recv(65536)
            if not chunk:
                raise ValueError("replica closed before response")
            data += chunk
            if len(data) > 4 * 1024 * 1024:
                raise ValueError("oversized replica response")
    reply = json.loads(data.split(b"\n", 1)[0])
    if not isinstance(reply, dict):
        raise ValueError("non-object RPC reply")
    result = reply.get("result", {})
    if not isinstance(result, dict):
        raise ValueError("non-object RPC result")
    if reply.get("error") or result.get("isError"):
        raise ValueError(str(reply.get("error") or result)[:300])
    return result.get("structured", {})


def evaluate(task: dict, cli: str, socket: str, transport: str = "cli") -> dict:
    argv = [cli, "--socket-path", socket, "recall_analogy", "--json", "--limit", "3"]
    for key, value in task["params"].items():
        argv.extend(["--" + key, str(value).lower() if isinstance(value, bool) else str(value)])
    started = time.perf_counter()
    results, error = [], None
    try:
        if transport == "rpc":
            payload = rpc_probe(task, socket)
        else:
            proc = subprocess.run(argv, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                                  timeout=2, env=dict(os.environ, CHITTA_SOCKET_PATH=socket))
            if proc.returncode:
                raise ValueError(f"exit {proc.returncode}: {(proc.stderr or proc.stdout)[:300]}")
            payload = json.loads(proc.stdout)
        if not isinstance(payload, dict) or not isinstance(payload.get("results"), list):
            raise ValueError("missing results array")
        results = payload["results"][:3]
        if not all(isinstance(row, dict) for row in results):
            raise ValueError("non-object result")
    except subprocess.TimeoutExpired:
        error = "timeout (2 s)"
    except (OSError, ValueError) as exc:
        error = str(exc)[:300]
    elapsed = (time.perf_counter() - started) * 1000
    if error:
        results = []
    hits = [any(matches(row, label) for label in task["expected"]) for row in results]
    return dict(id=task["id"], style=task["style"], hit_at_1=bool(hits and hits[0]),
                hit_at_3=any(hits), latency_ms=round(elapsed, 3), error=error, results=results)


def summarize(rows: list[dict]) -> dict:
    return dict(tasks=len(rows), hit_at_1=sum(r["hit_at_1"] for r in rows) / len(rows),
                hit_at_3=sum(r["hit_at_3"] for r in rows) / len(rows),
                median_latency_ms=round(statistics.median(r["latency_ms"] for r in rows), 3),
                errors=sum(r["error"] is not None for r in rows),
                empty=sum(not r["results"] and r["error"] is None for r in rows))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tasks", type=Path, default=Path(__file__).with_name("tasks.json"))
    parser.add_argument("--output", type=Path, default=Path(__file__).with_name("results.json"))
    parser.add_argument("--cli", default=os.environ.get("CHITTA_BIN", os.path.expanduser("~/.claude/bin/chitta")))
    parser.add_argument("--transport", choices=("cli", "rpc"), default="cli",
                        help="rpc is a separate diagnostic when the CLI rejects the tool")
    parser.add_argument("--snapshot-id", default=os.environ.get("CHITTA_EVAL_SNAPSHOT", "unspecified"))
    args = parser.parse_args()
    socket = os.environ.get("CHITTA_EVAL_SOCKET", "")
    if not socket or not Path(socket).is_socket():
        parser.error("CHITTA_EVAL_SOCKET must name the running frozen replica socket")
    raw = args.tasks.read_bytes()
    tasks = json.loads(raw)["tasks"]
    if not tasks or len({t["id"] for t in tasks}) != len(tasks):
        parser.error("task IDs must be nonempty and unique")
    rows = [evaluate(task, args.cli, socket, args.transport) for task in tasks]
    summary = summarize(rows)
    output = dict(timestamp=datetime.now(timezone.utc).isoformat(), socket=socket,
                  snapshot_id=args.snapshot_id, transport=args.transport, cli=str(Path(args.cli).resolve()),
                  tasks_sha256=hashlib.sha256(raw).hexdigest(), timeout_seconds=2,
                  scoring="Exact answer (case-insensitive) or exact memory ID; errors/timeouts are misses. "
                          "Median includes every attempted query and CLI startup.",
                  summary=summary,
                  by_style={style: summarize([r for r in rows if r["style"] == style])
                            for style in sorted({r["style"] for r in rows})},
                  ready_for_hook=summary["hit_at_3"] >= 0.3, results=rows)
    args.output.write_text(json.dumps(output, indent=2, ensure_ascii=False) + "\n")
    print(json.dumps(summary))
    if not output["ready_for_hook"]:
        print("hit@3 < 0.3: the analogy lane is not ready for a hook.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
