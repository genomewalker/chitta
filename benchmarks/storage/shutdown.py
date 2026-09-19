#!/usr/bin/env python3
"""Measure SIGTERM-to-exit on a private frozen replica, including worker joins."""

import argparse
import hashlib
import json
import os
import signal
import socket
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRATCH = Path("/projects/caeg/scratch/kbd606/tmp")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--delays", type=float, nargs="+", default=[0, 65, 0])
    args = parser.parse_args()
    out = args.output.absolute()
    if not out.is_relative_to(SCRATCH) or out.exists():
        parser.error("output must be a new directory under project scratch")
    out.mkdir()
    mind = out / "m"
    with socket.socket() as port_socket:
        port_socket.bind(("127.0.0.1", 0))
        port = port_socket.getsockname()[1]
    env = dict(
        os.environ,
        CHITTA_LIVE_MIND=str(SCRATCH / "learning-cut-20260915-frozen"),
        CHITTA_EVAL_MIND=str(mind),
        CHITTA_EVAL_PORT=str(port),
        CHITTAD_BIN=str(ROOT / "bin/chittad"),
        CHITTA_BIN=str(ROOT / "bin/chitta"),
        CHITTA_EVAL_START_TIMEOUT="1800",
        CHITTA_RECALL_NOW="1789473600000",
        CHITTA_RECALL_EMBED_WAIT_MS="10000",
        OPENBLAS_NUM_THREADS="1",
        OMP_NUM_THREADS="1",
        MKL_NUM_THREADS="1",
        RAYON_NUM_THREADS="1",
    )
    report = {
        "host": socket.gethostname(),
        "daemon_sha256": hashlib.sha256((ROOT / "bin/chittad").read_bytes()).hexdigest(),
        "trials": [],
    }
    pid = None

    def alive():
        try:
            return Path(f"/proc/{pid}/stat").read_text().split(") ", 1)[1][0] != "Z"
        except FileNotFoundError:
            return False

    def stop():
        if str(mind).encode() not in Path(f"/proc/{pid}/cmdline").read_bytes().split(bytes([0])):
            raise RuntimeError("refusing to signal a foreign process")
        begin = time.monotonic()
        os.kill(pid, signal.SIGTERM)
        while alive():
            if time.monotonic() - begin > 120:
                raise TimeoutError("replica shutdown exceeded 120 s")
            time.sleep(0.01)
        return time.monotonic() - begin

    try:
        for index, delay in enumerate(args.delays):
            with (out / f"start-{index}.log").open("w") as log:
                result = subprocess.run(
                    ["bash", "scripts/eval-replica.sh", "start"],
                    cwd=ROOT,
                    env=env,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    timeout=1900,
                )
            if (mind / "replica.pid").exists():
                pid = int((mind / "replica.pid").read_text())
            result.check_returncode()
            time.sleep(delay)
            elapsed = stop()
            pid = None
            daemon_log = (mind / "replica.log").read_text(errors="replace")
            (out / f"daemon-{index}.log").write_text(daemon_log)
            trial = {
                "delay_after_ready_s": delay,
                "sigterm_to_exit_s": elapsed,
                "watchdog": "Shutdown timeout" in daemon_log,
                "joins": [line for line in daemon_log.splitlines() if "[shutdown]" in line],
                "normal_stop": "[daemon] Stopped" in daemon_log,
            }
            report["trials"].append(trial)
            (out / "report.json").write_text(json.dumps(report, indent=2))
            print(json.dumps(trial), flush=True)
    finally:
        if pid and alive():
            stop()


if __name__ == "__main__":
    main()
