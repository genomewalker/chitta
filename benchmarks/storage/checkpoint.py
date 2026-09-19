#!/usr/bin/env python3
"""Measure WAL-budget checkpoints and flush-only shutdown on a private frozen-cut replica."""
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
    parser.add_argument("--control", action="store_true", help="measure without acceptance assertions")
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
               RAYON_NUM_THREADS="1", CUDA_VISIBLE_DEVICES="",
               CHITTA_CHECKPOINT_WAL_MB="1", CHITTA_WAL_SNAPSHOT_RECORDS="100000000")
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
        return response["result"].get("structured", response["result"])

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

    def replica(action, label):
        with (out / (label + ".log")).open("w") as log:
            subprocess.run(["bash", "scripts/eval-replica.sh", action], cwd=ROOT,
                           env=env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=1900)

    def daemon_log():
        return (mind / "replica.log").read_text(errors="replace")

    def write(index):
        vector = [0.0] * 768
        vector[index % 768] = 1.0
        return rpc("observe", {"content": f"checkpoint probe {index:09d} " + "x" * 8192,
                              "category": "wisdom", "realm": "storage-checkpoint",
                              "_preembedding": vector})

    def terminate():
        pid = int((mind / "replica.pid").read_text())
        proc = Path(f"/proc/{pid}")
        assert str(mind).encode() in proc.joinpath("cmdline").read_bytes().split(bytes([0]))
        begin = time.monotonic()
        os.kill(pid, signal.SIGTERM)
        while proc.exists() and proc.joinpath("stat").read_text().split(") ", 1)[1][0] != "Z":
            if time.monotonic() - begin > 60:
                raise TimeoutError("owned replica exceeded shutdown deadline")
            time.sleep(0.05)
        return time.monotonic() - begin

    try:
        replica("start", "initial-start")
        report["initial_memory_count"] = rpc("health_check")["memory_count"]
        # Explicit commit provides a stable starting family for both binaries.
        begin = time.monotonic()
        rpc("compact_wal")
        report["explicit_snapshot_s"] = time.monotonic() - begin
        prior = daemon_log().count("reason=wal_budget")
        begin = time.monotonic()
        for index in range(160):
            last_observation = write(index)
        report["write_s"] = time.monotonic() - begin
        if not args.control:
            deadline = time.monotonic() + 180
            while daemon_log().count("reason=wal_budget") <= prior:
                if time.monotonic() > deadline:
                    raise TimeoutError("WAL byte budget did not commit a family")
                time.sleep(0.25)
        # A state-only tail exercises replay without invalidating event-tape caches.
        # This is not a mixed-write cache-validity test.
        rpc("strengthen", {"id": last_observation["id"], "amount": 0.01})
        report["tail_workload"] = "state-only strengthen; background writes remain enabled"
        report["before_memory_count"] = rpc("health_check")["memory_count"]
        report["shutdown_s"] = terminate()
        shutdown_log = daemon_log()
        (out / "shutdown-daemon.log").write_text(shutdown_log)
        report["checkpoints"] = re.findall(r"\[checkpoint\] reason=(\w+) duration_ms=(\d+) result=([^\n]+)", shutdown_log)
        report["watchdog_fired"] = "watchdog fired" in shutdown_log.lower() or "forcing exit" in shutdown_log.lower()
        tail_counts = re.findall(r"shutdown_wal_tail_records=(\d+)", shutdown_log)
        report["expected_tail_records"] = int(tail_counts[-1]) if tail_counts else None
        # eval-replica truncates its log on restart; keep the previous log above.
        replica("restart", "restart")
        report["after_memory_count"] = rpc("health_check")["memory_count"]
        reopened = daemon_log()
        (out / "restart-daemon.log").write_text(reopened)
        report["replay_records"] = sum(int(n) for n in re.findall(r"replay_apply kind=\w+ records=(\d+)", reopened))
        report["phases_ms"] = {k: int(v) for k, v in re.findall(r"load phase=(\w+) ms=(\d+)", reopened)}
        report["cache_hits"] = {
            label: bool(re.search(re.escape(label) + r" cache hit=true", reopened))
            for label in ("LSH", "Turbo", "event tape organs")
        }
        report["family_validation_failed"] = "failed validation" in reopened
        assert report["after_memory_count"] == report["before_memory_count"], report
        if not args.control:
            assert any(reason == "wal_budget" and result == "Ok(())" for reason, _, result in report["checkpoints"]), report
            assert not any(reason == "shutdown" for reason, _, _ in report["checkpoints"]), report
            assert not report["watchdog_fired"], report
            assert not report["family_validation_failed"], report
            assert report["shutdown_s"] < 2, report
            assert report["expected_tail_records"] is not None and report["expected_tail_records"] > 0, report
            assert report["replay_records"] == report["expected_tail_records"], report
            assert report["phases_ms"]["normalize"] < 1000, report
            assert report["phases_ms"]["field_store"] < 15000, report
            assert all(report["cache_hits"].values()), report
        report["status"] = "PASS"
    finally:
        crash_owned()
        (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report), flush=True)


if __name__ == "__main__":
    main()
