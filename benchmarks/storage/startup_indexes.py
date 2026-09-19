#!/usr/bin/env python3
"""Measure deferred index startup on a frozen replica with a fixed 65 MB sidecar."""
import argparse
import json
import os
import re
import socket
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRATCH = Path("/projects/caeg/scratch/kbd606/tmp")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--deferred", action="store_true")
    args = parser.parse_args()
    out = args.output.absolute()
    if not out.is_relative_to(SCRATCH) or out.exists():
        parser.error("output must be a new directory under project scratch")
    out.mkdir()
    # Keep the Unix socket pathname below 108 bytes.
    import tempfile
    mind = Path(tempfile.mkdtemp(prefix="ix", dir=SCRATCH)) / "m"
    with socket.socket() as port_socket:
        port_socket.bind(("127.0.0.1", 0))
        port = port_socket.getsockname()[1]
    env = dict(os.environ, CHITTA_LIVE_MIND=str(SCRATCH / "learning-cut-20260915-frozen"),
               CHITTA_EVAL_MIND=str(mind), CHITTA_EVAL_PORT=str(port),
               CHITTAD_BIN=str(ROOT / "bin/chittad"), CHITTA_BIN=str(ROOT / "bin/chitta"),
               CHITTA_EVAL_START_TIMEOUT="1800", CHITTA_RECALL_NOW="1789473600000",
               CHITTA_RECALL_EMBED_WAIT_MS="10000", OPENBLAS_NUM_THREADS="1",
               OMP_NUM_THREADS="1", MKL_NUM_THREADS="1", RAYON_NUM_THREADS="1")
    report = {"mind": str(mind), "host": socket.gethostname(), "loading_samples": 0}

    def rpc(name, arguments=None):
        address = next((mind / "run/chitta").glob("*.sock"))
        with socket.socket(socket.AF_UNIX) as client:
            client.settimeout(3)
            client.connect(str(address))
            client.sendall((json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                                       "params": {"name": name, "arguments": arguments or {}}}) + "\n").encode())
            line = client.makefile("rb").readline()
        result = json.loads(line)["result"]
        return result.get("structured", result)

    def replica(action, label):
        with (out / (label + ".log")).open("w") as log:
            subprocess.run(["bash", "scripts/eval-replica.sh", action], cwd=ROOT, env=env,
                           stdout=log, stderr=subprocess.STDOUT, timeout=1900, check=True)

    def recall():
        begin = time.monotonic()
        result = rpc("recall", {"query": "storage persistence", "limit": 5, "sources": False})
        return result, time.monotonic() - begin

    try:
        replica("start", "initial")
        report["memories_before"] = rpc("health_check")["memory_count"]
        before, report["recall_before_s"] = recall()
        replica("stop", "initial-stop")
        # Same parse and rebuild work on both binaries; no live index is read.
        body = "value = storage_persistence + checkpoint;\n" * 390
        symbols = [{"id": str(i), "name": "fixture_" + str(i), "kind": "function",
                    "signature": "fixture()", "line_start": i + 1, "line_end": i + 1,
                    "body": body} for i in range(4096)]
        fixture = {"version": 1, "files": {str(ROOT / "fixture.cpp"): {
            "root": str(ROOT), "project": "startup-fixture", "symbols": symbols, "edges": []}}}
        sidecar = mind / "code-navigation.json"
        sidecar.write_text(json.dumps(fixture))
        report["fixture_bytes"] = sidecar.stat().st_size
        del fixture, symbols
        with (out / "restart.log").open("w") as log:
            process = subprocess.Popen(["bash", "scripts/eval-replica.sh", "restart"],
                                       cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 1800
            loading_recall = None
            samples = []
            while time.monotonic() < deadline:
                try:
                    begin = time.monotonic()
                    health = rpc("health_check")
                    if not health.get("loading"):
                        code = rpc("code_query", {"question": "fixture_1", "path": str(ROOT)})
                        if code.get("phase") == "code_indexes" and code.get("error") == "loading":
                            report["loading_samples"] += 1
                            if loading_recall is None:
                                loading_recall, report["recall_during_s"] = recall()
                        elif report["loading_samples"] or process.poll() is not None:
                            report["code_normal"] = "error" not in code and bool(code.get("symbols"))
                            report["memories_after"] = health["memory_count"]
                            break
                    samples.append(time.monotonic() - begin)
                except (OSError, StopIteration, KeyError, ValueError):
                    pass
                time.sleep(0.025)
            else:
                raise TimeoutError("code index did not become ready")
            process.wait(timeout=30)
            if process.returncode:
                raise RuntimeError("replica restart failed")
        after, report["recall_after_s"] = recall()
        def ids(result):
            return sorted(item["id"] for item in result["results"])
        report["recall_ids_before"] = ids(before)
        report["recall_ids_after"] = ids(after)
        report["recall_ids_during"] = ids(loading_recall) if loading_recall else None
        report["recall_equal"] = before == after
        report["recall_ids_equal"] = bool(ids(before)) and ids(before) == ids(after) and (loading_recall is None or ids(before) == ids(loading_recall))
        report["max_probe_s"] = max(samples, default=0)
        daemon_log = (mind / "replica.log").read_text(errors="replace")
        (out / "daemon.log").write_text(daemon_log)
        report["startup"] = re.findall(r"\[startup\] phase=(\w+) ms=(\d+)(?: since_field_ready_ms=(\d+))?", daemon_log)
        listening = [row for row in report["startup"] if row[0] == "listening"]
        report["field_to_listening_ms"] = int(listening[-1][2])
        report["pass"] = report["code_normal"] and report["memories_before"] == report["memories_after"] and report["recall_ids_equal"]
        if args.deferred:
            report["pass"] &= report["field_to_listening_ms"] < 1000 and report["loading_samples"] > 0
        (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report))
        if not report["pass"]:
            raise SystemExit(1)
    finally:
        if (mind / "replica.pid").exists():
            replica("stop", "final-stop")


if __name__ == "__main__":
    main()
