#!/usr/bin/env python3
"""Calibrate repeated-run noise; echo/live smoke data never supplies acceptance bands."""

from __future__ import annotations

import argparse
import importlib.util
import json
import math
import os
import re
import statistics
import subprocess
import sys
import tempfile
import uuid
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "benchmarks/noise.json"


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def summary(values: list) -> dict:
    if len(values) < 2 or any(not math.isfinite(v) for v in values):
        raise ValueError("noise needs at least two finite observations")
    mean, sd = statistics.mean(values), statistics.stdev(values)
    return {
        "n": len(values),
        "mean": mean,
        "sd": sd,
        "band_95": [mean - 1.96 * sd, mean + 1.96 * sd],
        "accept_delta": 2 * sd,
        "samples": values,
    }


def smriti_metrics(records: list) -> dict:
    # One observation is an entire fixed task panel in one trial. Variation
    # between different task difficulties is not repeated-run noise.
    grouped = defaultdict(list)
    for row in records:
        grouped[(row["condition"], row["trial"])].append(row)
    values = defaultdict(list)
    for (condition, _), rows in sorted(grouped.items()):
        values[f"smriti.{condition}.sr"].append(statistics.mean(r["passed"] for r in rows))
        values[f"smriti.{condition}.tokens"].append(statistics.mean(r["tokens_used"] for r in rows))
    return {metric: summary(samples) for metric, samples in values.items()}


def strict_recall(grader, query: str, limit: int, strategy: str = "") -> dict:
    cmd = [
        "chitta",
        "recall",
        "--query",
        query,
        "--limit",
        str(limit),
        "--realm",
        grader.GRADE_REALM,
        "--no-learn",
        "--json",
    ]
    if strategy:
        cmd += ["--strategy", strategy]
    if grader.CHITTA_EVAL_SOCKET:
        cmd += ["--socket-path", grader.CHITTA_EVAL_SOCKET]
    result = subprocess.run(cmd, check=True, capture_output=True, text=True, timeout=60)
    data = json.loads(result.stdout)
    if not isinstance(data, dict) or not isinstance(data.get("results"), list) or data.get("error"):
        raise ValueError(
            "recall failed or returned an invalid envelope; refusing a zero-noise score"
        )
    return data


def golden_runs(k: int) -> tuple:
    grader = load_module("noise_golden_grader", ROOT / "hooks/grade-recall.py")
    # Fixed production hybrid ranking at depth 20, no optional reranker or
    # entropy dependencies. grade_one is the grader's actual scoring function;
    # main() also writes gap memories, so deliberately do not invoke it.
    grader._reranker = False
    grader._HAS_NUMPY = False
    grader._recall_full = lambda query, limit, strategy="": strict_recall(
        grader, query, limit, strategy
    )
    stored = json.loads(grader.GOLD_IDS_PATH.read_text())
    if stored.get("version") != grader.GOLDEN_VERSION or not stored.get("ids"):
        raise ValueError("missing or stale golden IDs")
    samples = []
    for trial in range(k):
        rows = [grader.grade_one(item, stored["ids"], 20, "hybrid") for item in grader.GOLDEN_SET]
        samples.append(statistics.mean(row["ndcg"] for row in rows))
        print(f"golden {trial + 1}/{k}: nDCG@20={samples[-1]:.6f}", flush=True)
    return samples, grader._replica_snapshot_id()


HOOK_QUERIES = (
    "how does chitta semantic recall use the field store",
    "what is the prompt hook admission policy",
    "how are durable corrections matched",
)


def hook_runs(n: int) -> dict:
    """One noise observation is the median of the fixed three-query panel."""
    if n < 2:
        raise ValueError("hook noise needs >=2 runs")
    env = os.environ.copy()
    for alias in ("CHITTA_HEADLESS", "CC_SOUL_HEADLESS"):
        env.pop(alias, None)
    binary = Path(env.get("CHITTA_BENCH_BIN", str(ROOT / "bin/chitta"))).resolve()
    if not os.access(binary, os.X_OK):
        raise ValueError(f"not executable: {binary}; set CHITTA_BENCH_BIN")
    socket = env.get("CHITTA_EVAL_SOCKET") or env.get("CHITTA_BENCH_SOCKET")
    if not socket:
        probe_env = dict(env)
        probe_env["CHITTA_DB_PATH"] = env.get(
            "CHITTA_BENCH_MIND", env.get("CHITTA_DB_PATH", str(Path.home() / ".claude/mind"))
        )
        socket = subprocess.run(
            ["bash", "-c", 'source "$1/hooks/lib.sh"; get_socket_path', "bash", str(ROOT)],
            env=probe_env,
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        ).stdout.strip()
    if not Path(socket).is_socket():
        raise ValueError(f"daemon socket not found: {socket}")
    subprocess.run(
        [str(binary), "--socket-path", socket, "status"],
        check=True,
        capture_output=True,
        timeout=10,
    )
    with tempfile.TemporaryDirectory(prefix="noise-hook-") as scratch:
        scratch = Path(scratch)
        mind = scratch / "mind"
        home = scratch / "home"
        (home / ".claude/mind").mkdir(parents=True)
        mind.mkdir()
        env.update(
            {
                "HOME": str(home),
                "XDG_RUNTIME_DIR": str(scratch / "runtime"),
                "CHITTA_DB_PATH": str(mind),
                "CHITTA_QUEUE": str(scratch / "queue"),
                "CHITTA_TASK_LEDGER": str(scratch / "tasks.sqlite"),
                "CHITTA_PLUGIN_DIR": str(ROOT),
                "CC_SOUL_PLUGIN_DIR": str(ROOT),
                "CHITTA_REALM": env.get("CHITTA_BENCH_REALM", "brahman"),
                "CHITTA_LEAN": "1",
                "CHITTA_HOOK_BUDGET_MS": env.get("CHITTA_BENCH_HOOK_BUDGET_MS", "6000"),
                "BENCH_REAL_CHITTA": str(binary),
                "BENCH_LIVE_SOCKET": socket,
            }
        )
        isolated_socket = subprocess.run(
            ["bash", "-c", 'source "$1/hooks/lib.sh"; get_socket_path', "bash", str(ROOT)],
            env=env,
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        ).stdout.strip()
        Path(isolated_socket).parent.mkdir(parents=True, exist_ok=True)
        Path(isolated_socket).symlink_to(socket)
        wrapper = scratch / "read-only"
        wrapper.write_text("""#!/bin/bash
case "${1:-}" in
    recall|smart_recall|recall_lanes|correction_check|predicate_list|narrative_status|\\
    anticipation_filter|anticipation_predict|habit_match|goal_list|curiosity_gaps|\\
    msg_inbox|recall_failure_pattern)
        exec "$BENCH_REAL_CHITTA" --socket-path "$BENCH_LIVE_SOCKET" "$@" ;;
    *) exit 0 ;;
esac
""")
        wrapper.chmod(0o700)
        env["CHITTA_BIN"] = str(wrapper)
        panels = []
        for trial in range(n):
            totals = []
            for query in HOOK_QUERIES:
                payload = {
                    "session_id": f"noise-hook-{uuid.uuid4().hex}",
                    "prompt": query,
                    "cwd": "/tmp",
                }
                result = subprocess.run(
                    ["bash", str(ROOT / "hooks/prompt-core.sh")],
                    input=json.dumps(payload),
                    env=env,
                    check=True,
                    capture_output=True,
                    text=True,
                    timeout=30,
                )
                matches = re.findall(r"\| t:[^\n]*?\btotal=(\d+)\b", result.stdout)
                if len(matches) != 1:
                    raise ValueError(
                        "prompt hook missing/ambiguous t: total; refusing empty timing"
                    )
                totals.append(int(matches[0]))
            panels.append(totals)
            print(f"hook {trial + 1}/{n}: median={statistics.median(totals)} ms", flush=True)
    samples = [statistics.median(panel) for panel in panels]
    return {
        **summary(samples),
        "median": statistics.median(samples),
        "query_totals_ms": panels,
        "queries": list(HOOK_QUERIES),
        "statistic": "median of three fixed queries per run",
        "recall_lanes_rpc": env.get(
            "CHITTA_RECALL_LANES_RPC", env.get("CC_SOUL_RECALL_LANES_RPC", "1")
        ),
    }


def hook_calibration(args) -> dict:
    socket = os.environ.get("CHITTA_EVAL_SOCKET")
    snapshot = os.environ.get("CHITTA_EVAL_SNAPSHOT_ID")
    mind = os.environ.get("CHITTA_EVAL_MIND")
    if socket and (not snapshot or not mind):
        raise ValueError(
            "hook replica calibration needs CHITTA_EVAL_SNAPSHOT_ID and CHITTA_EVAL_MIND"
        )
    if socket and Path(mind).resolve() == (Path.home() / ".claude/mind").resolve():
        raise ValueError("CHITTA_EVAL_MIND must be a replica")
    return {
        "schema_version": 1,
        "created_at": datetime.now(timezone.utc).isoformat(),
        "snapshot_id": snapshot,
        "socket": socket,
        "agent": None,
        "acceptance_ready": bool(socket and snapshot and mind),
        "mode": "replica" if socket else "live-smoke",
        "errors": [],
        "accept_rule": "hook_total_ms improvement strictly below -2 * sd",
        "metrics": {"hook_total_ms": hook_runs(args.hook_runs)},
    }


def band(data: dict, metric: str) -> float:
    if not data.get("acceptance_ready"):
        raise ValueError("not acceptance-ready on a frozen replica; band unavailable")
    item = data["metrics"][metric]
    if item["n"] < 2 or not math.isfinite(item["sd"]) or item["sd"] < 0:
        raise ValueError("invalid calibration")
    return 2 * item["sd"]


def calibrate(args) -> dict:
    socket = os.environ.get("CHITTA_EVAL_SOCKET")
    if not socket and (args.agent != "echo" or args.golden_runs != 2):
        raise ValueError("without CHITTA_EVAL_SOCKET only --agent echo --golden-runs 2 is allowed")
    if args.golden_runs < 2 or args.trials < 2 or args.tasks < 1:
        raise ValueError("need >=2 golden runs, >=2 trials, and >=1 task")
    if not socket:
        print(
            "CHITTA_EVAL_SOCKET unset: two read-only golden runs on LIVE daemon; echo is simulated.",
            flush=True,
        )
    sys.path.insert(0, str(ROOT / "benchmarks/smriti"))
    runner = load_module("noise_smriti_runner", ROOT / "benchmarks/smriti/runner.py")
    assignments = runner.task_split.load()
    selected = runner.task_split.select(assignments, args.task or [], args.split)
    selected = selected if args.task else selected[: args.tasks]
    if not selected or (not args.task and len(selected) != args.tasks):
        raise ValueError("not enough tasks in the requested split")
    # Require replica provenance before starting expensive real agent calls.
    snapshot_id = os.environ.get("CHITTA_EVAL_SNAPSHOT_ID")
    mind = os.environ.get("CHITTA_EVAL_MIND")
    if args.agent == "claude-code" and (not snapshot_id or not mind):
        raise ValueError("real calibration requires CHITTA_EVAL_SNAPSHOT_ID and CHITTA_EVAL_MIND")
    if args.agent == "claude-code":
        if Path(mind).resolve() == (Path.home() / ".claude/mind").resolve():
            raise ValueError("CHITTA_EVAL_MIND must be a replica")
        runner.MIND_PATH = Path(mind)
        os.environ["CHITTA_DB_PATH"] = mind
    errors = []
    try:
        golden, observed_snapshot = golden_runs(args.golden_runs)
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        if args.agent != "echo":
            raise
        # Keep an honest smoke artifact and exercise the offline panel even
        # when live recall is unavailable. Failed queries are never zeros.
        golden, observed_snapshot = [], None
        errors.append(f"golden calibration incomplete: {exc}")
        print(errors[-1], file=sys.stderr, flush=True)
    snapshot_id = snapshot_id or observed_snapshot
    if socket and not snapshot_id:
        raise ValueError("replica snapshot ID is required (source replica.env first)")
    rows = []
    run_id = uuid.uuid4().hex[:8]
    agent = runner.EchoAdapter() if args.agent == "echo" else runner.ClaudeCodeAdapter()
    with tempfile.TemporaryDirectory(prefix="eval-noise-") as scratch:
        runner.SCRATCH_ROOT = scratch
        for trial in range(args.trials):
            for task_id in selected:
                task_dir = ROOT / "benchmarks/smriti/tasks" / task_id
                task = runner.load_task(task_dir)
                for condition in ("off", "on"):
                    row = runner.run_one(
                        task,
                        task_dir,
                        condition,
                        agent,
                        run_id,
                        trial,
                        dry_run=args.agent == "echo",
                    )
                    row["split"] = assignments[task_id]
                    rows.append(row)
            print(f"SMRITI trial {trial + 1}/{args.trials} complete", flush=True)
    confirmed = all(r["injected_confirmed"] is True for r in rows if r["condition"] == "on")
    unavailable = {
        "n": 0,
        "mean": None,
        "sd": None,
        "band_95": None,
        "accept_delta": None,
        "samples": [],
        "status": "unavailable",
    }
    metrics = {
        "golden.ndcg": summary(golden) if golden else unavailable,
        **smriti_metrics(rows),
        "hook_total_ms": unavailable,
    }
    # Older programmatic smoke callers have no hook panel argument.
    if getattr(args, "hook_runs", 0):
        try:
            metrics["hook_total_ms"] = hook_runs(args.hook_runs)
        except (OSError, ValueError, subprocess.SubprocessError) as exc:
            if args.agent != "echo":
                raise
            errors.append(f"hook calibration incomplete: {exc}")
            print(errors[-1], file=sys.stderr, flush=True)
    return {
        "schema_version": 1,
        "created_at": datetime.now(timezone.utc).isoformat(),
        "snapshot_id": snapshot_id,
        "socket": socket,
        "agent": args.agent,
        "acceptance_ready": bool(
            socket and snapshot_id and args.agent == "claude-code" and confirmed and not errors
        ),
        "mode": "replica" if socket else "live-smoke",
        "errors": errors,
        "golden_runs_requested": args.golden_runs,
        "injection_confirmed": confirmed,
        "golden_config": {"limit": 20, "strategy": "hybrid", "reranker": False},
        "tasks": selected,
        "trials": args.trials,
        "split_hash": runner.task_split.fingerprint(assignments),
        "band_definition": "mean +/- 1.96 sample SD; descriptive normal band, not CI of mean",
        "accept_rule": "improvement strictly beyond 2 * sd; positive SR/nDCG, negative tokens/hook_total_ms",
        "metrics": metrics,
        "smriti_records": rows,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    run = commands.add_parser("run")
    run.add_argument("--hook-only", action="store_true", help="skip golden and SMRITI calibration")
    run.add_argument("--hook-runs", type=int, default=5, help="repetitions of the fixed hook panel")
    run.add_argument("--golden-runs", "-k", type=int, default=5)
    run.add_argument("--tasks", type=int, default=3)
    run.add_argument("--task", action="append")
    run.add_argument("--trials", type=int, default=3)
    run.add_argument("--split", choices=["visible", "holdout", "all"], default="visible")
    run.add_argument("--agent", choices=["echo", "claude-code"], default="echo")
    run.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    read = commands.add_parser("band")
    read.add_argument("metric")
    read.add_argument("--file", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    try:
        if args.command == "band":
            print(band(json.loads(args.file.read_text()), args.metric))
        else:
            result = hook_calibration(args) if args.hook_only else calibrate(args)
            temporary = args.output.with_suffix(".json.tmp")
            temporary.write_text(json.dumps(result, indent=2) + "\n")
            temporary.replace(args.output)
            print(f"noise report: {args.output}; acceptance_ready={result['acceptance_ready']}")
            if result["errors"] or (
                not args.hook_only
                and args.agent == "claude-code"
                and not result["acceptance_ready"]
            ):
                parser.exit(2, "eval-noise: incomplete or unconfirmed calibration; see report\n")
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
        parser.exit(2, f"eval-noise: {exc}\n")


if __name__ == "__main__":
    main()
