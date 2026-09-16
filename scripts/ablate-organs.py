#!/usr/bin/env python3
"""Measure organ ablations on private replica copies; never infer missing margins.

Three fresh copies per arm. Results are descriptive unless every declared margin
and invariant passes. This tool never deletes source or changes contracts.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import importlib.util
import json
import math
import os
import shlex
import shutil
import socket
import statistics
import subprocess
import sys
import tempfile
import time
import uuid
from contextlib import contextmanager
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PYTHON = "/maps/projects/fernandezguerra/apps/opt/conda/envs/bioinfo/bin/python3"
SOURCE = Path("/projects/caeg/scratch/kbd606/tmp/chitta-eval-mind")
RECALL = (
    "cortical_idx",
    "hdc_idx",
    "lite_encoder",
    "sparse_encoder",
    "learners",
    "span_store",
    "predictor",
    "surprise_store",
    "epistemic_debt_store",
    "integration_kernel",
    "surprise_learning",
    "wisdom_promotion",
    "learned_scorer",
)
EVENT = (
    "session_registry",
    "transcript_registry",
    "task_registry",
    "user_model_registry",
    "theme_organ",
    "analytics_registry",
    "msg_registry",
    "skill_registry",
    "agent_registry",
    "constraint_store",
    "trigger_store",
    "intervention_store",
    "agent_protocol_store",
    "wisdom_lineage_store",
    "symbol_event_log",
    "repl_sessions",
    "event_tape",
    "cdawg",
    "episode_hdc",
    "refutation_ledger",
    "cec_policy_store",
    "decision_tape",
    "hypothesis_market",
    "turiya_monitor",
    "fep_prior",
    "observer",
    "observer_state",
    "interaction_ledger",
    "predicate_store",
    "archive",
)
ORGANS = EVENT + RECALL
GROUPS = {
    "event_api": EVENT,
    "recall_adjacent": RECALL,
    "event_prediction": ("event_tape", "cdawg", "episode_hdc", "fep_prior"),
    "all": ORGANS,
}
REQUIRED = ("golden.ndcg", "current_truth.p3", "current_truth.abstain", "hook_total_ms")
SNAPSHOT = {
    "event_tape",
    "decision_tape",
    "turiya_monitor",
    "observer_state",
    "interaction_ledger",
    "predicate_store",
    "msg_registry",
}
WRITE = {
    "archive",
    "observer",
    "observer_state",
    "event_tape",
    "cdawg",
    "fep_prior",
    "cortical_idx",
    "sparse_encoder",
    "hdc_idx",
    "learners",
    "span_store",
}


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    sys.modules[name] = result
    spec.loader.exec_module(result)
    return result


def run(argv, env, log, timeout=1200):
    with log.open("w") as output:
        subprocess.run(
            [str(a) for a in argv],
            env=env,
            cwd=ROOT,
            stdin=subprocess.DEVNULL,
            stdout=output,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=True,
        )


def metadata(mind):
    return {
        key: shlex.split(value)[0]
        for key, value in (
            line.split("=", 1) for line in (mind / "replica.env").read_text().splitlines()
        )
    }


@contextmanager
def private_directory(root):
    directory = Path(tempfile.mkdtemp(prefix="p2-", dir=root))
    try:
        yield str(directory)
    finally:
        pidfile = directory / "mind/replica.pid"
        if pidfile.exists():
            pid = int(pidfile.read_text())
            status = Path(f"/proc/{pid}/stat")
            if status.exists() and status.read_text().split()[2] != "Z":
                raise RuntimeError(f"scratch process {pid} still alive; retained {directory}")
        shutil.rmtree(directory)


def stop_copy(mind, env, log):
    # Restarts are children of this Python process. Reap them while the existing
    # stop script waits, otherwise kill -0 sees our zombie for its full deadline.
    pid = int((mind / "replica.pid").read_text())
    with log.open("w") as output:
        proc = subprocess.Popen(
            ["bash", str(ROOT / "scripts/eval-replica.sh"), "stop"],
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=output,
            stderr=subprocess.STDOUT,
        )
        deadline = time.monotonic() + 90
        while proc.poll() is None:
            try:
                os.waitpid(pid, os.WNOHANG)
            except ChildProcessError:
                pass
            if time.monotonic() > deadline:
                proc.terminate()
                proc.wait(timeout=10)
                raise TimeoutError("scratch stop timed out; retaining its directory")
            time.sleep(0.1)
        if proc.returncode:
            raise RuntimeError(f"scratch stop failed; see {log}")


def free_port():
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


def margins(noise, required=REQUIRED):
    result, missing = {}, []
    for key in required:
        row = noise.get("metrics", {}).get(key, {})
        value = row.get("accept_delta")
        if (
            not isinstance(value, (int, float))
            or not math.isfinite(value)
            or value < 0
            or row.get("n", 0) < 2
        ):
            missing.append(key)
        else:
            result[key] = value
    return result, missing


def compare(control, treatment, declared, missing):
    """Every repetition must fit; never turn a failed/missing panel into zero."""
    deltas, moved, unstable = {}, [], []
    for key in dict.fromkeys([*declared, *missing]):
        margin = declared.get(key)
        baseline = [r.get("metrics", {}).get(key) for r in control]
        values = [r.get("metrics", {}).get(key) for r in treatment]
        if (
            not baseline
            or not values
            or any(
                not isinstance(v, (int, float)) or not math.isfinite(v) for v in baseline + values
            )
        ):
            missing = [*missing, key]
            continue
        center = statistics.mean(baseline)
        if margin is not None and any(abs(value - center) > margin for value in baseline):
            unstable.append(key)
        deltas[key] = {
            "control": center,
            "samples": values,
            "delta": statistics.mean(values) - center,
            "margin": margin,
        }
        if margin is not None and any(abs(value - center) > margin for value in values):
            moved.append(key)
    failures = [r.get("error") for r in control + treatment if r.get("error")]
    if unstable:
        failures.append("control repetitions exceed declared margins: " + ", ".join(unstable))
    if len(control) != 3 or len(treatment) != 3:
        failures.append("requires three complete repetitions in each arm")
    if any(not r.get("invariants", {}).get("passed") for r in control + treatment):
        failures.append("write/keyed/restart invariants incomplete or failed")
    verdict = "unqualified" if missing or failures else "moved" if moved else "within margins"
    return {
        "verdict": verdict,
        "deltas": deltas,
        "moved": moved,
        "unstable_controls": unstable,
        "missing_margins": sorted(set(missing)),
        "failures": failures,
    }


def hook_metrics(path):
    rows = {}
    for line in path.read_text().splitlines():
        parts = line.split("\t")
        if len(parts) == 5 and parts[0] in ("on", "off"):
            arm, count, median, p95, empties = parts
            rows[arm] = {
                "n": int(count),
                "median_ms": float(median),
                "p95_ms": float(p95),
                "empties": int(empties),
            }
    if set(rows) != {"on", "off"} or any(r["n"] != 15 or r["empties"] for r in rows.values()):
        raise ValueError("hook panel missing samples or returned empty injections")
    return rows


def contains_text(value, expected):
    if isinstance(value, str):
        if expected in value:
            return True
        try:
            decoded = json.loads(value)
        except (ValueError, TypeError):
            return False
        return decoded != value and contains_text(decoded, expected)
    if isinstance(value, dict):
        return any(contains_text(v, expected) for v in value.values())
    if isinstance(value, list):
        return any(contains_text(v, expected) for v in value)
    return False


def invariants(stress, mind):
    sock, _ = stress.scratch(mind)
    nonce = uuid.uuid4().hex
    realm = "ablation:" + nonce
    contents = [f"ablation remember {nonce} {i}" for i in range(197)] + [
        f"[done] sha:{nonce} input:/tmp/ablation-{nonce} verified",
        f"[task] task:{nonce} status:running next:verify",
        f"[correction] USE: amber kettle\nNOT: zephyr marmalade orchard {nonce}",
    ]
    connection = stress.WriteConnection(sock)
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=32) as pool:
            replies = list(
                pool.map(
                    lambda i: connection.remember(
                        i + 1000,
                        {
                            "content": contents[i],
                            "type": "correction" if i == 199 else "signal",
                            "realm": realm,
                        },
                    )[1],
                    range(200),
                )
            )
    finally:
        connection.close()
    ids = [stress.ordered_ids(reply)[0] for reply in replies]
    if len(set(ids)) != 200:
        raise AssertionError("remember acknowledgements did not contain 200 unique IDs")
    keyed = [
        ("provenance_check", {"sha": nonce}, ids[197]),
        ("task_state", {"id": nonce}, ids[198]),
        ("correction_check", {"text": f"zephyr marmalade orchard {nonce}"}, ids[199]),
    ]

    def check():
        for identity, content in zip(ids, contents):
            reply = stress.rpc(sock, "get", {"id": identity})[1]
            if not contains_text(reply, content):
                raise AssertionError(f"remember content missing for {identity}")
        for tool, args, expected in keyed:
            if expected not in stress.ordered_ids(stress.rpc(sock, tool, args)[1]):
                raise AssertionError(f"keyed lane failed: {tool}")

    check()
    # SIGKILL the owned process, then reopen the SAME copied store: no recopy
    # and no shutdown snapshot can disguise a missing acknowledged WAL write.
    stress.restart_copy(mind, kill_after_ack=True)
    check()
    identity = stress.restart_measurement(mind)
    distinct = identity.get("ordered_recall_identity")
    # Fail closed until the existing restart report's identity count is present.
    return {
        "passed": distinct == "20/20",
        "remembers": 200,
        "wal_recovered": 200,
        "keyed_lanes": 3,
        "restart": identity,
    }


def trial(args, organs, index, output):
    result = {"organs": list(organs), "trial": index, "metrics": {}}
    env = {k: v for k, v in os.environ.items() if not k.startswith(("CHITTA_", "CC_SOUL_"))}
    env.update(
        PATH=str(args.cli.parent)
        + os.pathsep
        + str(Path(PYTHON).parent)
        + os.pathsep
        + env.get("PATH", ""),
        CHITTA_LIVE_MIND=str(args.source),
        CHITTAD_BIN=str(args.daemon),
        CHITTA_BIN=str(args.cli),
        CHITTA_PY=PYTHON,
        CHITTA_ABLATE_ORGANS=",".join(organs),
        CHITTA_STORE_LOCK_WAIT_S="45",
        CHITTA_CLI_AUTOSTART="0",
        CC_SOUL_CLI_AUTOSTART="0",
        CHITTA_EVAL_EMBED_MODEL=str(args.model),
        CHITTA_EVAL_PORT=str(free_port()),
    )
    with private_directory(args.scratch_root) as directory:
        mind = Path(directory) / "mind"
        home = Path(directory) / "home"
        (home / ".claude/mind").mkdir(parents=True)
        env.update(HOME=str(home), CHITTA_EVAL_MIND=str(mind))
        old_env = dict(os.environ)
        try:
            run(["bash", "scripts/eval-replica.sh", "start"], env, output / "start.log")
            env.update(metadata(mind))
            env.update(
                CHITTA_BENCH_BIN=str(args.cli),
                CHITTA_BENCH_MIND=str(mind),
                CHITTA_BENCH_SOCKET=env["CHITTA_EVAL_SOCKET"],
                CHITTA_BENCH_REALM="project:cc-soul",
            )
            os.environ.clear()
            os.environ.update(env)
            stress = module("ablation_stress", ROOT / "scripts/stress-embed-recall.py")
            stress.wait_ready(stress.scratch(mind)[0])
            noise = module("ablation_noise", ROOT / "benchmarks/noise.py")
            golden, snapshot = noise.golden_runs(1)
            result.update(snapshot_id=snapshot)
            result["metrics"]["golden.ndcg"] = golden[0]
            run(
                [PYTHON, "benchmarks/current_truth/run.py", "--output", output / "truth.json"],
                env,
                output / "truth.log",
            )
            truth = json.loads((output / "truth.json").read_text())
            result["truth"] = truth["overall"]
            for key in ("p3", "abstain"):
                result["metrics"][f"current_truth.{key}"] = truth["overall"][key]
            run(["bash", "scripts/bench-recall-lanes.sh", "5"], env, output / "hook.log")
            result["hooks"] = hook_metrics(output / "hook.log")
            # Match the noise file's statistic and fixed queries exactly; the
            # requested paired latency panel above remains separately reported.
            result["metrics"]["hook_total_ms"] = noise.hook_runs(2)["samples"][0]
            if args.smriti_agent:
                run(
                    [
                        PYTHON,
                        "benchmarks/smriti/runner.py",
                        "--split",
                        "visible",
                        "--agent",
                        args.smriti_agent,
                        "--trials",
                        "1",
                        "--output",
                        output / "smriti",
                    ],
                    env,
                    output / "smriti.log",
                    timeout=7200,
                )
                records = [
                    json.loads(line)
                    for p in (output / "smriti").glob("*.jsonl")
                    for line in p.read_text().splitlines()
                ]
                if not records or any(r.get("dry_run") for r in records):
                    raise ValueError("SMRITI needs real, non-dry-run records")
                for arm in ("off", "on"):
                    rows = [r for r in records if r["condition"] == arm]
                    if not rows:
                        raise ValueError(f"missing SMRITI arm: {arm}")
                    result["metrics"][f"smriti.{arm}.sr"] = statistics.mean(
                        r["passed"] for r in rows
                    )
                    result["metrics"][f"smriti.{arm}.tokens"] = statistics.mean(
                        r["tokens_used"] for r in rows
                    )
            else:
                result["smriti"] = "unavailable: no isolated replica agent configured"
            result["invariants"] = invariants(stress, mind)
        except (
            OSError,
            ValueError,
            RuntimeError,
            AssertionError,
            subprocess.SubprocessError,
        ) as exc:
            result["error"] = str(exc)
        finally:
            try:
                if (mind / "replica.pid").exists():
                    stop_copy(mind, env, output / "stop.log")
            finally:
                os.environ.clear()
                os.environ.update(old_env)
    (output / "trial.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def write_table(report, destination):
    lines = [
        "## Organ ablation 2026-09-16",
        "",
        "Three repetitions per arm; missing calibration or invariants block retirement.",
        "No organ or tool is deleted by the measurement runner.",
        "",
        "| Organ | Dependency class | Panels moved (Δ; margin) | Verdict |",
        "|---|---|---|---|",
    ]
    for organ in (*ORGANS, *GROUPS):
        row = report.get("comparisons", {}).get(organ, {})
        numbers = (
            "; ".join(
                f"{k}: {v['delta']:+.6g}; margin={v['margin']}"
                for k, v in row.get("deltas", {}).items()
            )
            or "not measured"
        )
        dependency = (
            "group (" + str(len(GROUPS[organ])) + " organs)"
            if organ in GROUPS
            else "recall-adjacent"
            if organ in RECALL
            else "event/API"
        )
        if organ in WRITE:
            dependency += "; write path (retain)"
        if organ in SNAPSHOT:
            dependency += "; snapshot section retained"
        verdict = row.get("verdict", "unqualified")
        if row.get("missing_margins"):
            verdict += ": missing " + ", ".join(row["missing_margins"])
        if row.get("unstable_controls"):
            verdict += "; unstable control: " + ", ".join(row["unstable_controls"])
        if row.get("moved"):
            verdict += "; observed beyond band: " + ", ".join(row["moved"])
        lines.append(f"| `{organ}` | {dependency} | {numbers} | {verdict} |")
    lines += [
        "",
        "### Paired prompt-hook latency",
        "",
        "Each trial runs `bench-recall-lanes.sh 5`: 15 samples per arm. Cells list",
        "each repetition's median/p95 in milliseconds; these are descriptive and",
        "separate from the calibrated `hook_total_ms` statistic above.",
        "",
        "| Ablation | Off median/p95 | On median/p95 | Empty outputs | Write/restart invariants |",
        "|---|---|---|---|---|",
    ]
    for name in ("control", *ORGANS, *GROUPS):
        runs = report.get("runs", {}).get(name, [])
        if not runs:
            continue
        arms = []
        for arm in ("off", "on"):
            arms.append(
                ", ".join(
                    f"{r['hooks'][arm]['median_ms']:g}/{r['hooks'][arm]['p95_ms']:g}"
                    if arm in r.get("hooks", {})
                    else "incomplete"
                    for r in runs
                )
            )
        empties = sum(h["empties"] for r in runs for h in r.get("hooks", {}).values())
        passed = sum(bool(r.get("invariants", {}).get("passed")) for r in runs)
        identities = ", ".join(
            r.get("invariants", {}).get("restart", {}).get("ordered_recall_identity", "incomplete")
            for r in runs
        )
        lines.append(
            f"| `{name}` | {arms[0]} | {arms[1]} | {empties} | "
            f"{passed}/{len(runs)} passed; recall identity {identities} |"
        )
    marker = "<!-- ORGAN-ABLATION-TABLE -->"
    text = destination.read_text() if destination.exists() else ""
    block = marker + "\n" + "\n".join(lines) + "\n" + marker
    if text.count(marker) == 2:
        before, _, after = text.split(marker)
        text = before + block + after
    else:
        text += "\n" + block + "\n"
    destination.write_text(text)


def self_test():
    assert contains_text({"text": "line1\nline2"}, "line1\nline2")
    assert contains_text({"text": json.dumps({"content": "line1\nline2"})}, "line1\nline2")
    assert not contains_text({"text": "other"}, "line1\nline2")
    declared, missing = margins({"metrics": {"golden.ndcg": {"accept_delta": 0.1, "n": 3}}})
    assert missing == ["current_truth.p3", "current_truth.abstain", "hook_total_ms"]
    row = {"metrics": {"golden.ndcg": 0.5}, "invariants": {"passed": True}}
    assert compare([row] * 3, [row] * 3, declared, missing)["verdict"] == "unqualified"
    assert compare([row] * 3, [row] * 3, declared, [])["verdict"] == "within margins"
    changed = {"metrics": {"golden.ndcg": 0.7}, "invariants": {"passed": True}}
    assert compare([row] * 3, [changed] * 3, declared, [])["verdict"] == "moved"
    assert compare([row] * 3, [row] * 2, declared, [])["verdict"] == "unqualified"
    unstable = compare([row, row, changed], [row] * 3, declared, [])
    assert unstable["verdict"] == "unqualified"
    assert unstable["unstable_controls"] == ["golden.ndcg"]
    assert compare([], [], declared, [])["verdict"] == "unqualified"
    print("ablation runner self-tests passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--preflight", action="store_true")
    parser.add_argument("--organ", action="append", choices=ORGANS + tuple(GROUPS))
    parser.add_argument("--source", type=Path, default=SOURCE)
    parser.add_argument("--scratch-root", type=Path, default=Path("/tmp"))
    parser.add_argument("--output", type=Path, default=ROOT / "results/ablation")
    parser.add_argument("--noise", type=Path, default=ROOT / "benchmarks/noise.json")
    parser.add_argument("--daemon", type=Path, default=ROOT / "bin/chittad")
    parser.add_argument("--cli", type=Path, default=ROOT / "bin/chitta")
    parser.add_argument(
        "--model",
        type=Path,
        default=Path("/maps/projects/caeg/people/kbd606/models/nomic-embed-text-v1.5.gguf"),
    )
    parser.add_argument("--smriti-agent", choices=("claude-code",))
    parser.add_argument("--write-table", action="store_true")
    parser.add_argument(
        "--resume",
        action="store_true",
        help="reuse completed repetitions only for the identical binary and noise declaration",
    )
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    args.source = args.source.resolve(strict=True)
    if args.source == (Path.home() / ".claude/mind").resolve():
        parser.error("source must be the evaluation replica, never the live mind")
    if not (args.source / "replica.env").is_file():
        parser.error("source must have evaluation replica metadata")
    required = list(REQUIRED)
    if args.smriti_agent:
        required += [
            f"smriti.{arm}.{metric}" for arm in ("off", "on") for metric in ("sr", "tokens")
        ]
    noise = json.loads(args.noise.read_text())
    declared, missing = margins(noise, required)
    if not noise.get("acceptance_ready") or noise.get("mode") != "replica":
        missing.append("acceptance-ready replica calibration")
    if noise.get("golden_config") != {"limit": 20, "strategy": "hybrid", "reranker": False}:
        missing.append("matching golden configuration")
    report = {
        "schema_version": 1,
        "noise_sha256": hashlib.sha256(args.noise.read_bytes()).hexdigest(),
        "binary_sha256": hashlib.sha256(args.daemon.read_bytes()).hexdigest()
        if args.daemon.is_file()
        else None,
        "margins": declared,
        "missing_margins": missing,
        "runs": {},
        "comparisons": {},
    }
    if args.preflight:
        print(json.dumps({**report, "organs": ORGANS}, indent=2))
        return
    for path in (args.daemon, args.cli, args.model):
        if not path.is_file():
            parser.error(f"required file missing: {path}")
    if args.smriti_agent and not shutil.which("claude"):
        parser.error("requested SMRITI agent is unavailable")
    args.output.mkdir(parents=True, exist_ok=True)
    previous = args.output / "report.json"
    if args.resume and previous.exists():
        saved = json.loads(previous.read_text())
        for key in ("binary_sha256", "noise_sha256", "margins", "missing_margins"):
            if saved.get(key) != report.get(key):
                parser.error(f"cannot resume with changed {key}")
        report = saved
    elif previous.exists():
        parser.error("output already contains a report; use --resume or a fresh directory")
    # Freeze the declaration before the first treatment result exists.
    (args.output / "declaration.json").write_text(json.dumps(report, indent=2) + "\n")
    selections = args.organ or [*ORGANS, *GROUPS]
    for name in ["control", *selections]:
        organs = () if name == "control" else GROUPS.get(name, (name,))
        existing = report["runs"].setdefault(name, [])
        for index in range(len(existing), 3):
            out = args.output / name / str(index + 1)
            out.mkdir(parents=True, exist_ok=True)
            print(f"{name} trial {index + 1}/3", flush=True)
            result = trial(args, organs, index + 1, out)
            report["runs"][name].append(result)
            if result.get("snapshot_id") != noise.get("snapshot_id"):
                result["error"] = "snapshot identity differs from the declared noise calibration"
            (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        if name != "control":
            report["comparisons"][name] = compare(
                report["runs"]["control"], report["runs"][name], declared, missing
            )
        (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        if args.write_table:
            write_table(report, ROOT / "docs/FIELD_PERF.md")
    print(f"report: {args.output / 'report.json'}", flush=True)


if __name__ == "__main__":
    main()
