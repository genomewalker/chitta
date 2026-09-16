#!/usr/bin/env python3
"""Source-anchor integration check. Requires an explicitly declared private replica with queue enabled."""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import subprocess
import time
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main():
    spec = importlib.util.spec_from_file_location("truth", ROOT / "benchmarks/current_truth/run.py")
    truth = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(truth)
    socket = truth.endpoint()  # Verifies the listener PID's actual private mind.
    mind = Path(os.environ["CHITTA_EVAL_MIND"]).resolve()
    queue = Path(os.environ["CHITTA_QUEUE"]).resolve()
    if mind not in queue.parents:
        raise ValueError("test queue must be inside the private mind")
    binary = str(ROOT / "bin/chitta")
    token = "anchortest" + uuid.uuid4().hex
    realm = "project:" + token
    repo = mind / token
    (repo / ".git").mkdir(parents=True)
    path = repo / "guide.md"

    def rpc(tool, **args):
        command = [binary, tool, "--socket-path", socket, "--json"]
        for key, value in args.items():
            command += [
                "--" + key.replace("_", "-"),
                json.dumps(value) if not isinstance(value, str) else value,
            ]
        return json.loads(
            subprocess.run(command, check=True, capture_output=True, text=True, timeout=30).stdout
        )

    def recall():
        return rpc("recall", query=token, realm=realm, strategy="keyword", limit=20, no_learn=True)[
            "results"
        ]

    def wait(predicate):
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            rows = recall()
            if predicate(rows):
                return rows
            time.sleep(0.2)
        raise AssertionError("queue/anchor expectation not reached")

    def enqueue(body, scope="Runtime", captured=None):
        anchor = {
            "repo": str(repo),
            "path": path.name,
            "scope": scope,
            "content_hash": captured or hashlib.sha256(path.read_bytes()).hexdigest(),
        }
        args = {
            "source": "hook_regex",
            "category": "signal",
            "realm": realm,
            "content": f"[artifact] {token} {body} input:{path}",
            "anchor": anchor,
        }
        subprocess.run(
            [binary, "queue_write", "observe", json.dumps(args)],
            check=True,
            capture_output=True,
            timeout=10,
        )
        return anchor["content_hash"]

    path.write_text("version one")
    old_hash = enqueue("first version")
    rows = wait(lambda rows: len(rows) == 1 and rows[0].get("anchor_state") == "current")
    old_id = rows[0]["id"]
    path.write_text("version two")
    rows = wait(lambda rows: any(r.get("anchor_state") == "stale" for r in rows))
    enqueue("second version")
    rows = wait(
        lambda rows: len(rows) == 1
        and rows[0]["id"] != old_id
        and rows[0].get("anchor_state") == "current"
    )
    current_id = rows[0]["id"]
    enqueue("independent fact", scope="Other heading")
    wait(lambda rows: len(rows) == 2 and all(r.get("anchor_state") == "current" for r in rows))
    enqueue("delayed version one", captured=old_hash)
    wait(
        lambda rows: len(rows) == 3
        and any(r["id"] == current_id and r.get("anchor_state") == "current" for r in rows)
    )
    path.unlink()
    wait(lambda rows: len(rows) == 3 and all(r.get("anchor_state") == "missing" for r in rows))
    print(
        "PASS: capture/current, missed-edit stale, same-anchor supersession, independent scope, delayed event, deletion"
    )


if __name__ == "__main__":
    main()
