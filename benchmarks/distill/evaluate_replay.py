"""Evaluate frozen, legacy-parser, and densified graphs on a private replica.

Run on compute after eval-replica.sh start, with its environment exported.
The source snapshot and live daemon are never mutated. Results are immutable.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import statistics
import socket as unix_socket
import sys
from pathlib import Path

from think_ablation import save, sha

ROOT = Path(__file__).resolve().parents[2]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    loaded = importlib.util.module_from_spec(spec)
    sys.modules[name] = loaded
    spec.loader.exec_module(loaded)
    return loaded


def triples(items, arm):
    result = set()
    for item in items:
        for row in item[arm]:
            if len(row) != 3 or not all(isinstance(v, str) and v for v in row):
                raise ValueError("invalid replay relation")
            result.add(tuple(row))
    return result


def connect_relation(socket, subject, predicate, obj):
    # main validates private listener ownership before this JSON-only write.
    request = {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
               "params": {"name": "connect", "arguments": {
                   "subject": subject, "predicate": predicate, "object": obj}}}
    with unix_socket.socket(unix_socket.AF_UNIX, unix_socket.SOCK_STREAM) as conn:
        conn.settimeout(60)
        conn.connect(socket)
        conn.sendall((json.dumps(request) + "\n").encode())
        with conn.makefile("rb") as stream:
            response = json.loads(stream.readline())
    if response.get("error") or "result" not in response:
        raise ValueError(f"connect failed: {response}")
    payload = response["result"]
    if payload.get("error") or payload.get("isError"):
        raise ValueError(f"connect failed: {payload}")
    return payload


def main():
    cli = argparse.ArgumentParser(__doc__)
    cli.add_argument("--replay", type=Path, required=True)
    cli.add_argument("--output", type=Path, required=True)
    args = cli.parse_args()
    mind = Path(os.environ["CHITTA_EVAL_MIND"]).resolve()
    scratch = Path("/projects/caeg/scratch/kbd606/tmp").resolve()
    if mind.parent != scratch or not mind.name.startswith("p13-"):
        raise ValueError("requires a p13 private replica under project scratch")
    for key, expected in (
        ("CHITTA_RECALL_EMBED_WAIT_MS", "10000"),
        ("OPENBLAS_NUM_THREADS", "1"),
        ("OMP_NUM_THREADS", "1"),
    ):
        if os.environ.get(key) != expected:
            raise ValueError(f"requires {key}={expected}")
    if not os.environ.get("CHITTA_RECALL_NOW"):
        raise ValueError("requires a pinned CHITTA_RECALL_NOW")
    truth = module("p13_truth", ROOT / "benchmarks/current_truth/run.py")
    socket = truth.endpoint()  # Validates socket ownership and daemon --path.
    noise = module("p13_noise", ROOT / "benchmarks/noise.py")
    replay = json.loads(args.replay.read_text())
    manifest = args.replay.parent / "manifest.json"
    if sha(manifest) != replay["manifest_sha256"]:
        raise ValueError("replay manifest changed")
    args.output.mkdir(exist_ok=False)
    before, after = (triples(replay["items"], arm) for arm in ("before", "after"))
    report = {
        "replay_sha256": sha(args.replay),
        "snapshot_id": os.environ["CHITTA_EVAL_SNAPSHOT_ID"],
        "recall_now": os.environ["CHITTA_RECALL_NOW"],
        "daemon_sha256": sha(Path(os.environ["CHITTAD_BIN"])),
        "legacy_unique_relations": len(before),
        "added_unique_relations": len(after - before),
        "preserved_legacy_relations": len(before - after),
        "stages": {},
    }
    panel = truth.load_panel()
    for stage, additions in (("frozen", set()), ("legacy", before), ("densified", after - before)):
        for subject, predicate, obj in sorted(additions):
            connect_relation(socket, subject, predicate, obj)
        golden, snapshot = noise.golden_runs(3)
        current = truth.evaluate(panel, socket)
        save(args.output / f"{stage}-truth.json", current)
        report["stages"][stage] = {
            "golden_ndcg_samples": golden,
            "golden_ndcg_mean": statistics.mean(golden),
            "snapshot_id": snapshot,
            "truth": current["overall"],
        }
        save(args.output / f"{stage}.json", report["stages"][stage])
        print(stage, json.dumps(report["stages"][stage]), flush=True)
    old, new = (report["stages"][arm] for arm in ("legacy", "densified"))
    report["delta"] = {
        "golden_ndcg_mean": new["golden_ndcg_mean"] - old["golden_ndcg_mean"],
        "truth": {
            k: new["truth"][k] - v for k, v in old["truth"].items() if isinstance(v, (int, float))
        },
    }
    save(args.output / "report.json", report)
    print("delta", json.dumps(report["delta"]))


if __name__ == "__main__":
    main()
