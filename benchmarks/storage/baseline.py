#!/usr/bin/env python3
"""Run on compute: frozen-copy storage baseline, synthetic time at 1 write/s."""

import argparse
import concurrent.futures as futures
import hashlib
import json
import math
import os
import re
import signal
import socket
import subprocess
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRATCH = Path("/projects/caeg/scratch/kbd606/tmp").resolve()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--daemon", type=Path, default=ROOT / "bin/chittad")
    parser.add_argument("--cli", type=Path, default=ROOT / "bin/chitta")
    parser.add_argument("--capsule-rows", type=int, nargs="+", default=[100, 364, 1000, 5000])
    args = parser.parse_args()
    out = args.output.resolve()
    if not out.is_relative_to(SCRATCH) or out.exists():
        parser.error("output must be a new directory under project scratch")
    out.mkdir()
    mind = out / "mind"
    with socket.socket() as port_socket:
        port_socket.bind(("127.0.0.1", 0))
        port = port_socket.getsockname()[1]
    env = dict(
        os.environ,
        CHITTA_LIVE_MIND=str(SCRATCH / "learning-cut-20260915-frozen"),
        CHITTA_EVAL_MIND=str(mind),
        CHITTA_EVAL_PORT=str(port),
        CHITTAD_BIN=str(args.daemon.resolve()),
        CHITTA_BIN=str(args.cli.resolve()),
        CHITTA_PROFILE_REPLAY="1",
        CHITTA_RECALL_NOW="1789473600000",
        CHITTA_RECALL_EMBED_WAIT_MS="10000",
        CHITTA_EVAL_START_TIMEOUT="1800",
        OPENBLAS_NUM_THREADS="1",
        OMP_NUM_THREADS="1",
        MKL_NUM_THREADS="1",
        RAYON_NUM_THREADS="1",
    )
    h = 5381
    for byte in str(mind).encode():
        h = (h * 33 + byte) & 0xFFFFFFFF
    sock = mind / f"run/chitta/chitta-{h}.sock"
    report = {
        "host": socket.gethostname(),
        "synthetic_writes_per_second": 1,
        "daemon_sha256": hashlib.sha256(args.daemon.read_bytes()).hexdigest(),
        "git_head": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True
        ).strip(),
        "trials": [],
    }
    pid = None
    heartbeat_done = threading.Event()

    def keep_quiesced():
        while not heartbeat_done.is_set():
            flag = mind / ".quiesce"
            if flag.exists():
                flag.touch()
            heartbeat_done.wait(30)

    heartbeat = threading.Thread(target=keep_quiesced, daemon=True)
    heartbeat.start()

    def save():
        (out / "report.json").write_text(json.dumps(report, indent=2))

    def rpc(method, arguments=None, timeout=900):
        begin = time.monotonic()
        request = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {"name": method, "arguments": arguments or {}},
        }
        result = subprocess.run(
            [str(args.cli), "--socket-path", str(sock)],
            input=json.dumps(request) + chr(10),
            env=env,
            text=True,
            capture_output=True,
            timeout=timeout,
            check=True,
        )
        response = json.loads(result.stdout)
        if "error" in response or response.get("result", {}).get("isError"):
            raise RuntimeError(str(response)[:1000])
        return time.monotonic() - begin, response["result"]

    def start(label, initial=False):
        nonlocal pid
        observed = {}
        daemon_log = mind / "replica.log"
        done = threading.Event()

        def watch(raw):
            series = observed.setdefault("raw" if raw else "cli", {})
            while not done.is_set():
                try:
                    process = int((mind / "replica.pid").read_text())
                    fields = Path(f"/proc/{process}/stat").read_text().split(") ", 1)[1].split()
                    born = int(fields[19]) / os.sysconf("SC_CLK_TCK")
                    if raw:
                        with socket.socket(socket.AF_UNIX) as client:
                            client.settimeout(1)
                            client.connect(str(sock))
                            observed.setdefault("socket_s", time.monotonic() - born)
                            client.sendall(
                                json.dumps(
                                    {
                                        "jsonrpc": "2.0",
                                        "id": 1,
                                        "method": "tools/call",
                                        "params": {"name": "health_check", "arguments": {}},
                                    }
                                ).encode()
                                + b"\n"
                            )
                            with client.makefile("rb") as stream:
                                response = json.loads(stream.readline())
                            health = response["result"]
                            if health.get("isError"):
                                raise RuntimeError(str(response))
                    else:
                        _, health = rpc("health_check", timeout=1)
                    state = health.get("structured", {})
                    if state.get("loading"):
                        phases = series.setdefault("loading_phases", [])
                        if state.get("phase") not in phases:
                            phases.append(state.get("phase"))
                    now = time.monotonic() - born
                    series.setdefault("health_s", now)
                    series.setdefault("health_samples_s", []).append(now)
                except (OSError, ValueError, KeyError, RuntimeError, subprocess.SubprocessError):
                    series["probe_failures"] = series.get("probe_failures", 0) + 1
                done.wait(0.05)

        watchers = [threading.Thread(target=watch, args=(raw,)) for raw in (True, False)]
        for watcher in watchers:
            watcher.start()
        try:
            with (out / f"{label}-harness.log").open("w") as log:
                subprocess.run(
                    [
                        "bash",
                        str(ROOT / "scripts/eval-replica.sh"),
                        "start" if initial else "restart",
                    ],
                    env=env,
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    check=True,
                    timeout=1900,
                )
            pid = int((mind / "replica.pid").read_text())
        finally:
            done.set()
            for watcher in watchers:
                watcher.join()
        with daemon_log.open("rb") as source:
            # eval-replica truncates this file on every start, including restart.
            log = source.read().decode(errors="replace")
        (out / f"{label}-daemon.log").write_text(log)
        observed["phase_ms"] = dict(re.findall(r"load phase=(\w+) ms=(\d+)", log))
        observed["replay_apply"] = [
            {"kind": kind, "records": int(records), "apply_ns": int(ns)}
            for kind, records, ns in re.findall(
                r"replay_apply kind=(\w+) records=(\d+) apply_ns=(\d+)", log
            )
        ]
        observed["health"] = rpc("health_check")[1]
        fields = Path(f"/proc/{pid}/stat").read_text().split(") ", 1)[1].split()
        ready_s = time.monotonic() - int(fields[19]) / os.sysconf("SC_CLK_TCK")
        observed["ready_s"] = ready_s
        for name in ("raw", "cli"):
            series = observed[name]
            samples = series.get("health_samples_s", [])
            series["max_health_gap_s"] = (
                max(
                    [samples[0], ready_s - samples[-1]]
                    + [b - a for a, b in zip(samples, samples[1:])]
                )
                if samples
                else ready_s
            )
        # The availability gate uses raw socket probes; CLI remains user-visible evidence.
        observed["max_health_gap_s"] = observed["raw"]["max_health_gap_s"]
        return observed

    def stop(sig=signal.SIGKILL):
        nonlocal pid
        if pid is None:
            return None
        if str(mind).encode() not in Path(f"/proc/{pid}/cmdline").read_bytes().split(bytes([0])):
            raise RuntimeError("foreign process; refusing signal")
        begin = time.monotonic()
        os.kill(pid, sig)
        while time.monotonic() - begin < 360:
            try:
                if Path(f"/proc/{pid}/stat").read_text().split(") ", 1)[1][0] == "Z":
                    break
            except FileNotFoundError:
                break
            time.sleep(0.05)
        else:
            raise TimeoutError("replica shutdown exceeded 360 s")
        pid = None
        (mind / "replica.pid").unlink(missing_ok=True)
        return time.monotonic() - begin

    def write(index):
        vector = [0.0] * 768
        vector[index % 768] = 1.0
        return rpc(
            "observe",
            {
                "content": f"storage baseline {index:09d} " + "x" * 228,
                "category": "wisdom",
                "realm": "storage-baseline",
                "_preembedding": vector,
            },
        )

    def manifests():
        return {
            p.name: hashlib.sha256(p.read_bytes()).hexdigest()
            for p in (mind / "chitta-field").glob("MANIFEST.[12]")
        }

    try:
        report["initial"] = start("initial", True)
        report["snapshot_s"] = rpc("compact_wal")[0]
        family = manifests()
        stop()
        report["trials"].append({"minutes": 0, "startup": start("wal-0")})
        previous = 0
        save()
        for minutes in (10, 60, 240):
            target = minutes * 60
            begin = time.monotonic()
            for index in range(previous, target):
                write(index)
            elapsed = time.monotonic() - begin
            trial = {
                "minutes": minutes,
                "cumulative_writes": target,
                "write_s": elapsed,
                "writes_per_s": (target - previous) / elapsed,
                "same_snapshot_family": family == manifests(),
                "field_files": {
                    p.name: p.stat().st_size
                    for p in (mind / "chitta-field").iterdir()
                    if p.is_file()
                },
            }
            previous = target
            stop()
            trial["startup"] = start(f"wal-{minutes}")
            report["trials"].append(trial)
            save()
        barrier = threading.Barrier(16)

        def client(index):
            barrier.wait()
            samples = []
            for turn in range(20):
                method = ["health_check", "status", "recall", "observe"][(index + turn) % 4]
                begin = time.monotonic()
                error = None
                try:
                    if method == "observe":
                        write(14400 + index * 20 + turn)
                    else:
                        rpc(
                            method,
                            {"query": "storage recovery", "limit": 5} if method == "recall" else {},
                        )
                except (RuntimeError, ValueError, subprocess.SubprocessError) as exc:
                    error = str(exc)[:300]
                samples.append({"method": method, "s": time.monotonic() - begin, "error": error})
            return samples

        with futures.ThreadPoolExecutor(max_workers=16) as pool:
            samples = sum(pool.map(client, range(16)), [])
        times = sorted(sample["s"] for sample in samples)
        report["mixed"] = {
            "clients": 16,
            "requests": len(times),
            "p50_s": times[math.ceil(len(times) * 0.5) - 1],
            "p95_s": times[math.ceil(len(times) * 0.95) - 1],
            "max_s": max(times),
            "errors": sum(s["error"] is not None for s in samples),
        }
        (out / "mixed.json").write_text(json.dumps(samples))
        save()

        # Curve uses unrelated sessions: capsule_get must find a fixed missing key.
        # Always record the actual table count, including rows in the frozen cut.
        def ledger(op, arguments):
            return rpc("ledger_op", {"op": op, "args": arguments})

        def row_count():
            _, result = ledger("counts", {})
            return result["structured"]["value"]["thread_sessions"]

        report["capsule_curve"] = []
        initial_rows = row_count()
        for target in sorted(set([initial_rows] + args.capsule_rows)):
            for index in range(row_count(), target):
                ledger(
                    "session_bind",
                    {"session_id": f"storage-curve-{index}", "project_dir": str(mind)},
                )
            samples = [
                ledger(
                    "capsule_get",
                    {
                        "repository": "storage-baseline-missing",
                        "stream_id": "curve",
                        "project_dir": str(mind),
                        "code_head": "baseline",
                    },
                )[0]
                for _ in range(30)
            ]
            samples.sort()
            report["capsule_curve"].append(
                {
                    "rows": row_count(),
                    "samples": len(samples),
                    "p95_ms": samples[math.ceil(len(samples) * 0.95) - 1] * 1000,
                    "max_ms": max(samples) * 1000,
                }
            )
            save()
        with futures.ThreadPoolExecutor(max_workers=1) as pool:
            compact = pool.submit(rpc, "compact_wal")
            time.sleep(0.1)
            report["shutdown_compact_pending"] = not compact.done()
            report["shutdown_s"] = stop(signal.SIGTERM)
            try:
                compact.result()
            except (RuntimeError, ValueError, subprocess.SubprocessError):
                pass
        save()
        print(json.dumps({k: report[k] for k in ("mixed", "shutdown_s", "snapshot_s")}))
    finally:
        heartbeat_done.set()
        heartbeat.join()
        save()
        if pid:
            stop()


if __name__ == "__main__":
    main()
