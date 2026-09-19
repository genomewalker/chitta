#!/usr/bin/env python3
"""Measure legacy July replay, forced-family commit, and certified reopens.

Run on compute with a new short --output path under project scratch. All
mutable data belongs to eval-replica; the immutable source is only copied.
"""
import argparse
import json
import os
import pathlib
import re
import signal
import socket
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--source", required=True, type=pathlib.Path)
    parser.add_argument("--port", type=int, default=18439)
    args = parser.parse_args()
    root = args.output.resolve()
    scratch = pathlib.Path("/projects/caeg/scratch/kbd606/tmp").resolve()
    if not root.is_relative_to(scratch) or root.exists():
        parser.error("output must be a new directory under project scratch")
    root.mkdir()
    repo = pathlib.Path(__file__).resolve().parents[2]
    mind = root / "mind"
    env = dict(os.environ, CHITTA_EVAL_MIND=str(mind),
               CHITTA_LIVE_MIND=str(args.source.resolve()),
               CHITTA_EVAL_PORT=str(args.port), CHITTA_PROFILE_REPLAY="1",
               CHITTAD_BIN=str(repo / "bin/chittad"), CHITTA_BIN=str(repo / "bin/chitta"),
               CHITTA_RECALL_NOW="1789473600000", CHITTA_RECALL_EMBED_WAIT_MS="10000",
               CHITTA_EVAL_START_TIMEOUT="1800", OPENBLAS_NUM_THREADS="1",
               OMP_NUM_THREADS="1", MKL_NUM_THREADS="1", RAYON_NUM_THREADS="1",
               CUDA_VISIBLE_DEVICES="")
    report = {"status": "FAIL", "trials": []}

    def replica(action, label):
        with (root / (label + ".log")).open("w") as log:
            subprocess.run(["bash", "scripts/eval-replica.sh", action], cwd=repo,
                           env=env, stdout=log, stderr=subprocess.STDOUT,
                           timeout=2100, check=True)

    def rpc(tool):
        sock = next((mind / "run/chitta").glob("*.sock"))
        request = dict(jsonrpc="2.0", id=1, method="tools/call",
                       params=dict(name=tool, arguments={}))
        with socket.socket(socket.AF_UNIX) as connection:
            connection.settimeout(1800)
            connection.connect(str(sock))
            connection.sendall((json.dumps(request) + "\n").encode())
            response = json.loads(connection.makefile().readline())
        assert "error" not in response, response
        result = response["result"]
        assert not result.get("isError"), result
        return result["structured"]

    def measure(label):
        health = rpc("health_check")
        text = (mind / "replica.log").read_text(errors="replace")
        (root / (label + "-daemon.log")).write_text(text)
        phases = {k: int(v) for k, v in re.findall(r"load phase=(\w+) ms=(\d+)", text)}
        counts = {k: int(v) for k, v in re.findall(r"replay_apply kind=(\w+) records=(\d+)", text)}
        trial = dict(label=label, phases_ms=phases, replay_counts=counts,
                     total_applied=sum(counts.values()), memory_count=health["memory_count"])
        report["trials"].append(trial)
        print(json.dumps(trial), flush=True)
        return trial

    def crash_owned_replica():
        pid = int((mind / "replica.pid").read_text())
        cmdline = pathlib.Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0")
        assert str(mind).encode() in cmdline, "refuse to kill an unrelated process"
        os.kill(pid, signal.SIGKILL)

    try:
        replica("start", "start")
        legacy = measure("legacy")
        expected = dict(AddAssocEdge=296, AddTriplet=35, AnalyticsEvent=13,
                        MsgEvent=11, PutPayload=12, RecordRecallBatch=56,
                        SessionEvent=19, TranscriptEvent=41, UpdateMemoryContent=12,
                        UpdateSparseCode=12, UpdateState=11727)
        assert legacy["replay_counts"] == expected, legacy
        assert legacy["memory_count"] == 120590, legacy
        report["compaction"] = rpc("compact_wal")
        assert report["compaction"].get("segments_deleted", -1) > 0, report["compaction"]
        text = (mind / "replica.log").read_text(errors="replace")
        (root / "commit-daemon.log").write_text(text)
        report["pruning_audit_lines"] = sum("unlink" in line and "reason=" in line
                                             for line in text.splitlines())
        report["certified_pruning_lines"] = sum(
            "reason=wal-family-certified-full-and-cortical-covered" in line
            and "result=Ok(())" in line for line in text.splitlines())
        assert report["certified_pruning_lines"] > 0, report
        # No shutdown snapshot: both reopens must use the forced family.
        for label in ("certified_cold", "certified_warm"):
            crash_owned_replica()
            replica("restart", label + "-start")
            trial = measure(label)
            assert trial["memory_count"] == legacy["memory_count"], trial
            assert trial["total_applied"] == 0, trial
            assert trial["phases_ms"]["wal_replay"] < 2000, trial
        report["status"] = "PASS"
    finally:
        (root / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        replica("stop", "stop")
    print(json.dumps(report), flush=True)


if __name__ == "__main__":
    main()
