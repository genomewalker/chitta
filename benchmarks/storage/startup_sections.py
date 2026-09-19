#!/usr/bin/env python3
"""Measure process-to-store and first correct recall on a frozen-cut replica.

The acceptance flag is opt-in so the same probe can record a failing baseline.
Process age comes from /proc, excluding replica copying and shell setup time.
"""
import argparse
import json
import os
import re
import socket
import subprocess
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRATCH = Path("/projects/caeg/scratch/kbd606/tmp")
QUERIES = ("storage persistence", "session handoff", "memory recall")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--require-early-ready", action="store_true")
    args = parser.parse_args()
    out = args.output.absolute()
    if not out.is_relative_to(SCRATCH) or out.exists():
        parser.error("output must be a new directory under project scratch")
    out.mkdir()
    mind = Path(tempfile.mkdtemp(prefix="sc", dir=SCRATCH))
    with socket.socket() as port_socket:
        port_socket.bind(("127.0.0.1", 0))
        port = port_socket.getsockname()[1]
    env = dict(os.environ, CHITTA_LIVE_MIND=str(SCRATCH / "learning-cut-20260915-frozen"),
               CHITTA_EVAL_MIND=str(mind), CHITTA_EVAL_PORT=str(port),
               CHITTAD_BIN=str(ROOT / "bin/chittad"), CHITTA_BIN=str(ROOT / "bin/chitta"),
               CHITTA_EVAL_START_TIMEOUT="1800", CHITTA_RECALL_NOW="1789473600000",
               CHITTA_RECALL_EMBED_WAIT_MS="10000", CHITTA_PROFILE_SNAPSHOT="1",
               OPENBLAS_NUM_THREADS="1", OMP_NUM_THREADS="1", MKL_NUM_THREADS="1",
               RAYON_NUM_THREADS="1")
    report = {"mind": str(mind), "host": socket.gethostname(), "loading_samples": 0}

    def rpc(name, arguments=None):
        address = next((mind / "run/chitta").glob("*.sock"))
        with socket.socket(socket.AF_UNIX) as client:
            client.settimeout(12)
            client.connect(str(address))
            client.sendall((json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                                       "params": {"name": name, "arguments": arguments or {}}}) + "\n").encode())
            with client.makefile("rb") as stream:
                response = json.loads(stream.readline())
        result = response.get("result", response)
        return result.get("structured", result)

    def replica(action, label):
        with (out / (label + ".log")).open("w") as log:
            subprocess.run(["bash", "scripts/eval-replica.sh", action], cwd=ROOT, env=env,
                           stdout=log, stderr=subprocess.STDOUT, timeout=1900, check=True)

    def recall_ids(query):
        result = rpc("recall", {"query": query, "limit": 5, "sources": False})
        if "results" not in result:
            raise RuntimeError("recall did not return results: " + json.dumps(result))
        return sorted(item["id"] for item in result["results"])

    def process_age():
        pid = int((mind / "replica.pid").read_text())
        # comm may contain spaces or parentheses; field 22 follows its last ')'.
        fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
        started = int(fields[19]) / os.sysconf("SC_CLK_TCK")
        return float(Path("/proc/uptime").read_text().split()[0]) - started

    process = None
    try:
        replica("start", "initial")
        report["memory_count_before"] = rpc("health_check")["memory_count"]
        report["recall_ids_before"] = {q: recall_ids(q) for q in QUERIES}
        replica("stop", "initial-stop")
        with (out / "restart.log").open("w") as log:
            process = subprocess.Popen(["bash", "scripts/eval-replica.sh", "restart"],
                                       cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 1800
            while time.monotonic() < deadline:
                try:
                    health = rpc("health_check")
                    age = process_age()
                    report.setdefault("first_health_s", age)
                    if health.get("loading"):
                        report["loading_samples"] += 1
                    elif "memory_count" in health:
                        report["store_ready_s"] = age
                        report["memory_count_after"] = health["memory_count"]
                        report["first_recall_ids"] = recall_ids(QUERIES[0])
                        report["first_recall_s"] = process_age()
                        break
                except (OSError, StopIteration, ValueError):
                    pass
                if process.poll() is not None and process.returncode:
                    raise RuntimeError("replica restart failed")
                time.sleep(0.02)
            else:
                raise TimeoutError("store did not become ready")
            process.wait(timeout=1900)
            if process.returncode:
                raise RuntimeError("replica restart failed")
        report["recall_ids_after"] = {q: recall_ids(q) for q in QUERIES}
        report["content_equal"] = (
            report["memory_count_before"] == report["memory_count_after"]
            and bool(report["recall_ids_before"][QUERIES[0]])
            and report["recall_ids_before"] == report["recall_ids_after"]
            and report["first_recall_ids"] == report["recall_ids_before"][QUERIES[0]])
        daemon_log = (mind / "replica.log").read_text(errors="replace")
        (out / "daemon.log").write_text(daemon_log)
        report["load_phases"] = re.findall(r"load phase=(\w+) ms=(\d+)", daemon_log)
        report["early_ready_pass"] = report["store_ready_s"] < 3 and report["first_recall_s"] < 5
        report["pass"] = report["content_equal"] and (report["early_ready_pass"] or not args.require_early_ready)
        (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps({k: report[k] for k in ("store_ready_s", "first_recall_s", "memory_count_after", "content_equal", "early_ready_pass", "pass")}))
        if not report["pass"]:
            raise SystemExit(1)
    finally:
        # A failed probe must not race cleanup with a still-launching shell.
        if process is not None and process.poll() is None:
            process.wait(timeout=1900)
        if (mind / "replica.pid").exists():
            replica("stop", "final-stop")


if __name__ == "__main__":
    main()
