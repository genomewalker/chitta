"""Serialize the control behind the existing p13 completion watcher (login node)."""
import fcntl
import json
import subprocess
import sys
import time
from pathlib import Path

from think_ablation import save

root = Path("/projects/caeg/scratch/kbd606/tmp/p13-data")
with (root / "control-worker.lock").open("a") as own_lock:
    fcntl.flock(own_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    with (root / "finish-ablation.lock").open("a") as primary_lock:
        # The primary completion watcher holds this throughout all its retries.
        fcntl.flock(primary_lock, fcntl.LOCK_EX)
        markers = list(root.glob("ablation-finished-*.json"))
        if not markers:
            raise RuntimeError("primary watcher exited without a completion marker; verify workers before retry")
        args = ["--pairs", "/projects/caeg/scratch/kbd606/tmp/p11b-data/pairs-20260917",
                "--output", str(root)]
        base = [sys.executable, "benchmarks/distill/think_ablation.py"]
        expected = {f'{i["id"]}-1-8192.json' for i in json.loads((root / "dev.json").read_text())}
        def missing():
            return expected - {p.name for p in (root / "control-responses").glob("*.json")}
        for attempt in range(3):
            if not missing():
                break
            subprocess.run(base + ["run"] + args + ["--control", "--workers", "2"], check=True)
        subprocess.run(base + ["analyze"] + args + ["--parser", str(root.parent / "p13-parser-tests/parser_cli")], check=True)
        result = {"expected_responses": 100, "complete_responses": 100 - len(missing()),
                  "missing": sorted(missing()), "finished_at": time.time()}
        save(root / f"control-finished-{time.time_ns()}.json", result)
        print(json.dumps(result), flush=True)
        sys.exit(bool(missing()))
