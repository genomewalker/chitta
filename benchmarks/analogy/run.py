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


def evaluate(task: dict, cli: str, socket: str, transport: str = "cli", grounding: dict | None = None) -> dict:
    argv = [cli, "--socket-path", socket, "recall_analogy", "--json", "--limit", "3"]
    for key, value in task["params"].items():
        argv.extend(["--" + key, str(value).lower() if isinstance(value, bool) else str(value)])
    started = time.perf_counter()
    results, error, payload = [], None, {}
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
        results = payload["results"]
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
    unsupported = []
    missing = False
    if grounding is not None:
        negative = task["style"] == "negative"
        valid_edges = grounding["edges"] if not negative else []
        edge_by_id = {e["id"]: e for e in valid_edges}
        missing = bool(grounding["missing_grounding"]) or not grounding["relations"] or not grounding["answers"]
        hits = [row.get("answer") in grounding["answers"] and not missing and not negative for row in results]
        for row in results:
            support = row.get("edges", [])
            valid = bool(support) and row.get("answer") in {e["object"] for e in valid_edges}
            for edge in support:
                expected = edge_by_id.get(edge.get("triplet_id"))
                valid = valid and expected is not None and all(
                    edge.get(k) == expected[k] for k in ("subject", "predicate", "object"))
                valid = valid and expected is not None and edge.get("memory_id") == expected.get("source_memory_id")
                valid = valid and edge.get("object") == row.get("answer")
            valid = valid and any(e.get("memory_id", 0) == (row.get("id") or None)
                                  and e.get("predicate") == row.get("predicate") for e in support)
            if not valid:
                unsupported.append(row)
    abstention = (task["style"] == "negative" and not error and not results
                  and payload.get("reason") == "no_target_relation" and not missing)
    return dict(id=task["id"], style=task["style"], hit_at_1=bool(hits and hits[0]),
                hit_at_3=any(hits[:3]), latency_ms=round(elapsed, 3), error=error, results=results,
                reason=payload.get("reason"), abstention_correct=abstention,
                unsupported_answers=len(unsupported), missing_grounding=missing)


def summarize(rows: list[dict]) -> dict:
    return dict(tasks=len(rows), hit_at_1=sum(r["hit_at_1"] for r in rows) / len(rows),
                hit_at_3=sum(r["hit_at_3"] for r in rows) / len(rows),
                median_latency_ms=round(statistics.median(r["latency_ms"] for r in rows), 3),
                errors=sum(r["error"] is not None for r in rows),
                empty=sum(not r["results"] and r["error"] is None for r in rows))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tasks", type=Path, default=Path(__file__).with_name("tasks.json"))
    parser.add_argument("--baseline", type=Path, default=Path(__file__).with_name("baseline.json"))
    parser.add_argument("--output", type=Path, default=Path(__file__).with_name("results.json"))
    parser.add_argument("--cli", default=os.environ.get("CHITTA_BIN", "chitta"))
    parser.add_argument("--transport", choices=("cli", "rpc"), default="rpc")
    parser.add_argument("--snapshot-id", required=True)
    args = parser.parse_args()
    socket = os.environ.get("CHITTA_EVAL_SOCKET", "")
    if not socket or not Path(socket).is_socket():
        parser.error("CHITTA_EVAL_SOCKET must name the running frozen replica socket")
    raw = args.tasks.read_bytes()
    tasks = json.loads(raw)["tasks"]
    baseline_raw = args.baseline.read_bytes()
    baseline = json.loads(baseline_raw)
    if (baseline["tasks_sha256"] != hashlib.sha256(raw).hexdigest()
            or baseline["snapshot_id"] != args.snapshot_id or baseline["socket"] != socket):
        parser.error("baseline does not match tasks, snapshot and private socket")
    if len(tasks) != 14 or any(t["style"] != "proportional" for t in tasks):
        parser.error("exactly 14 proportional queries required; structural mode is retired")
    grounding = {p["id"]: p for p in baseline["positives"]}
    negatives = baseline["negatives"]
    if len(negatives) != 14 or len({t["id"] for t in tasks + negatives}) != 28:
        parser.error("need 14 unique negative queries")
    if len({tuple(t["params"][k] for k in ("a", "b", "c")) for t in negatives}) != 14:
        parser.error("negative argument triples must be distinct")
    rows = [evaluate(task, args.cli, socket, args.transport,
                     grounding[task.get("source_id", task["id"])]) for task in tasks + negatives]
    positives, negative_rows = rows[:14], rows[14:]
    summary = dict(positive_queries=14, negative_queries=14,
                   hit_at_1=sum(r["hit_at_1"] for r in positives),
                   hit_at_3=sum(r["hit_at_3"] for r in positives),
                   negative_abstentions=sum(r["abstention_correct"] for r in negative_rows),
                   unsupported_answers=sum(r["unsupported_answers"] for r in rows),
                   missing_grounding=sum(r["missing_grounding"] for r in positives),
                   errors=sum(r["error"] is not None for r in rows),
                   median_latency_ms=round(statistics.median(r["latency_ms"] for r in rows), 3))
    keep = (summary["hit_at_3"] >= 12 and summary["negative_abstentions"] == 14
            and summary["unsupported_answers"] == 0)
    output = dict(timestamp=datetime.now(timezone.utc).isoformat(), socket=socket,
                  snapshot_id=args.snapshot_id, transport=args.transport,
                  tasks_sha256=hashlib.sha256(raw).hexdigest(),
                  baseline_sha256=hashlib.sha256(baseline_raw).hexdigest(), timeout_seconds=2,
                  scoring="Complete exact directed-join answer sets. Missing grounding and errors are misses. "
                          "Every returned answer and citation is checked; negative abstention requires a reason.",
                  summary=summary, decision="keep" if keep else "remove", results=rows)
    args.output.write_text(json.dumps(output, indent=2, ensure_ascii=False) + "\n")
    print(json.dumps(summary))
    print("Decision: " + output["decision"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
