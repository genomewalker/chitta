#!/usr/bin/env python3
"""Score graph-only navigation; source reads are solely a separate byte proxy."""

from __future__ import annotations

import argparse
import json
import socket
import statistics
import time
from pathlib import Path


def rpc(socket_path, name, arguments):
    request = {
        "jsonrpc": "2.0",
        "id": 1,
        "method": "tools/call",
        "params": {"name": name, "arguments": arguments},
    }
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(90)
        client.connect(str(socket_path))
        client.sendall(json.dumps(request).encode() + b"\n")
        with client.makefile("rb") as stream:
            response = json.loads(stream.readline())
    if "error" in response:
        raise RuntimeError(response["error"])
    result = response["result"]
    if result.get("isError"):
        raise RuntimeError(result)
    return result.get("structured", result)


def evaluate(socket_path, root, questions, repeats=5):
    rows, latencies, responses = [], [], []
    for case in questions:
        args = {"question": case["question"], "path": str(root), "limit": 8}
        response = None
        for _ in range(repeats):
            start = time.perf_counter()
            answer = rpc(socket_path, "code_query", args)
            latencies.append((time.perf_counter() - start) * 1000)
            if response is not None and response != answer:
                raise AssertionError("nondeterministic repeated query: " + case["id"])
            response = answer
        assert response.get("indexed"), "benchmark requires a populated repository graph"
        responses.append(response)
        expected_file = root / case["file"]
        hits = [
            s
            for s in response["symbols"]
            if Path(s["file"]) == expected_file and s["name"].split("::")[-1] == case["symbol"]
        ]
        # Neither source contents nor read_symbol responses select/rank answers.
        # These calls account only for bytes after the graph answer is frozen.
        injected = rpc(socket_path, "code_query", {"path": str(expected_file), "limit": 20})
        body_args = (
            hits[0]["read_symbol"] if hits else {"name": case["symbol"], "path": str(expected_file)}
        )
        body = rpc(socket_path, "read_symbol", body_args)
        body_bytes = len(body["code"].encode())
        without = expected_file.stat().st_size
        injected_text = injected.get("text", "")
        if injected.get("truncated"):
            injected_text += (
                "\n[code-nav] Symbol list truncated; use code_query with a question to narrow it."
            )
        # Charge the full block even if the hook's 12,000-byte cap removes lines.
        with_graph = (
            len(response.get("text", "").encode()) + len(injected_text.encode()) + body_bytes
        )
        rows.append(
            {
                "id": case["id"],
                "hit": bool(hits),
                "without_bytes": without,
                "with_bytes": with_graph,
                "body_bytes": body_bytes,
                "candidates": [s["file"] + ":" + s["name"] for s in response["symbols"]],
            }
        )
    total_without = sum(row["without_bytes"] for row in rows)
    total_with = sum(row["with_bytes"] for row in rows)
    ordered = sorted(latencies)
    return {
        "score": sum(row["hit"] for row in rows),
        "total": len(rows),
        "p95_ms": round(ordered[max(0, (95 * len(ordered) + 99) // 100 - 1)], 3),
        "median_ms": round(statistics.median(latencies), 3),
        "without_bytes": total_without,
        "with_bytes": total_with,
        "byte_reduction": 1 - total_with / total_without,
        "rows": rows,
        "query_responses": responses,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument(
        "--questions", type=Path, default=Path(__file__).with_name("questions.json")
    )
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = evaluate(
        args.socket,
        args.root.resolve(),
        json.loads(args.questions.read_text())["questions"],
        args.repeats,
    )
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(
        json.dumps(
            {k: v for k, v in result.items() if k not in {"rows", "query_responses"}},
            sort_keys=True,
        )
    )
    if result["score"] < 18 or result["p95_ms"] > 300 or result["byte_reduction"] < 0.5:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
