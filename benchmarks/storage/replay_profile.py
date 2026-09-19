#!/usr/bin/env python3
"""Prepare an ingestion WAL or profile an unchanged copy using eval-replica."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
SCRATCH = Path("/projects/caeg/scratch/kbd606/tmp")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--source", type=Path, default=SCRATCH / "learning-cut-20260915-frozen")
    parser.add_argument("--prepare", action="store_true")
    args = parser.parse_args()
    out = args.output.resolve()
    if not out.is_relative_to(SCRATCH.resolve()) or out.exists():
        parser.error("output must be a new project-scratch directory")
    if not args.source.resolve().is_relative_to(SCRATCH.resolve()):
        parser.error("source must be a frozen or derived replica in project scratch")
    out.mkdir()
    mind = out / "m"
    with socket.socket() as port_socket:
        port_socket.bind(("127.0.0.1", 0))
        port = port_socket.getsockname()[1]
    env = dict(os.environ, CHITTA_LIVE_MIND=str(args.source), CHITTA_EVAL_MIND=str(mind),
               CHITTA_EVAL_PORT=str(port), CHITTAD_BIN=str(ROOT / "bin/chittad"),
               CHITTA_BIN=str(ROOT / "bin/chitta"), CHITTA_PROFILE_REPLAY="1",
               CHITTA_PROFILE_SNAPSHOT="1", CHITTA_EVAL_START_TIMEOUT="1800",
               CHITTA_RECALL_NOW="1789473600000", CHITTA_RECALL_EMBED_WAIT_MS="10000",
               OPENBLAS_NUM_THREADS="1", OMP_NUM_THREADS="1", MKL_NUM_THREADS="1",
               RAYON_NUM_THREADS="1", CUDA_VISIBLE_DEVICES="")
    report = {"status": "FAIL", "host": socket.gethostname(),
              "daemon_sha256": hashlib.sha256((ROOT / "bin/chittad").read_bytes()).hexdigest()}

    def rpc(tool, arguments=None):
        sock = next((mind / "run/chitta").glob("*.sock"))
        request = dict(jsonrpc="2.0", id=1, method="tools/call",
                       params=dict(name=tool, arguments=arguments or {}))
        with socket.socket(socket.AF_UNIX) as client:
            client.settimeout(1800)
            client.connect(str(sock))
            client.sendall((json.dumps(request) + "\n").encode())
            response = json.loads(client.makefile().readline())
        if "error" in response or response.get("result", {}).get("isError"):
            raise RuntimeError(response)
        return response["result"]["structured"]

    def crash_owned():
        if not (mind / "replica.pid").exists():
            return
        pid = int((mind / "replica.pid").read_text())
        proc = Path(f"/proc/{pid}")
        if not proc.exists() or proc.joinpath("stat").read_text().split(") ", 1)[1][0] == "Z":
            return
        if str(mind).encode() not in proc.joinpath("cmdline").read_bytes().split(bytes([0])):
            raise RuntimeError("refuse to signal a foreign process")
        os.kill(pid, signal.SIGKILL)
        deadline = time.monotonic() + 30
        while proc.exists() and proc.joinpath("stat").read_text().split(") ", 1)[1][0] != "Z":
            if time.monotonic() > deadline:
                raise TimeoutError("owned replica failed to stop")
            time.sleep(0.05)

    report["source_wal_sha256"] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                   for p in (args.source / "chitta-field/segments").glob("*.seg")}
    try:
        begin = time.monotonic()
        with (out / "start.log").open("w") as log:
            subprocess.run(["bash", "scripts/eval-replica.sh", "start"], cwd=ROOT,
                           env=env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=1900)
        report["copy_and_start_s"] = time.monotonic() - begin
        if args.prepare:
            begin = time.monotonic()
            report["ingest"] = rpc("learn_codebase", {"path": str(ROOT), "project": "p22-replay-profile"})
            report["ingest_s"] = time.monotonic() - begin
            # Wait for the existing periodic WAL fsync, without taking a snapshot.
            time.sleep(3)
        report["memory_count"] = rpc("health_check")["memory_count"]
        report["code_context"] = rpc("code_context")
        crash_owned()
        log = (mind / "replica.log").read_text(errors="replace")
        (out / "daemon.log").write_text(log)
        report["phases_ms"] = {k: int(v) for k, v in re.findall(r"load phase=(\w+) ms=(\d+)", log)}
        report["normalize_ms"] = {k: int(v) for k, v in re.findall(r"snapshot section=(normalize_\w+) decode_ms=(\d+)", log)}
        report["apply"] = {k: {"records": int(n), "ns": int(t)} for k, n, t in
                           re.findall(r"replay_apply kind=(\w+) records=(\d+) apply_ns=(\d+)", log)}
        report["loop"] = {}
        for line in log.splitlines():
            if "replay_loop kind=" in line:
                row = dict(re.findall(r"(\w+)=(\w+)", line))
                kind = row.pop("kind")
                report["loop"][kind] = {k: int(v) for k, v in row.items()}
        report["wal_sha256"] = {str(p.relative_to(mind)): hashlib.sha256(p.read_bytes()).hexdigest()
                                for p in (mind / "chitta-field/segments").glob("*.seg")}
        report["status"] = "PASS"
    finally:
        crash_owned()
        (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: report[k] for k in ("status", "memory_count", "phases_ms", "normalize_ms")}))


if __name__ == "__main__":
    main()
