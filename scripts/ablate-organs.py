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
LARGEST = (
    "hdc_idx",
    "cortical_idx",
    "cdawg",
    "span_store",
    "episode_hdc",
    "event_tape",
    "lite_encoder",
    "sparse_encoder",
)
TRUTH_METRICS = tuple(
    f"current_truth.{prefix}{metric}"
    for prefix in ("", "visible.", "holdout.")
    for metric in ("p3", "abstain")
)
SMRITI_METRICS = tuple(
    f"smriti.{arm}.{metric}" for arm in ("off", "on") for metric in ("sr", "tokens")
)
REQUIRED = ("golden.ndcg", *TRUTH_METRICS, *SMRITI_METRICS, "hook_total_ms")
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
    "analytics_registry",
    "task_registry",
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


def run(argv, env, log, timeout=1200, accepted_codes=(0,)):
    with log.open("w") as output:
        result = subprocess.run(
            [str(a) for a in argv],
            env=env,
            cwd=ROOT,
            stdin=subprocess.DEVNULL,
            stdout=output,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
        if result.returncode not in accepted_codes:
            raise subprocess.CalledProcessError(result.returncode, argv)


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
        for pidfile in directory.glob("*/replica.pid"):
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


def direction(key):
    """Positive utility is better; token cost and latency run in reverse."""
    return -1 if key.endswith(".tokens") or key == "hook_total_ms" else 1


def compare(control, treatment, declared, missing):
    """Only degradation blocks; every repetition needs complete evidence."""
    deltas, moved, improved, unstable = {}, [], [], []
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
        if margin is not None and max(baseline) - min(baseline) > margin:
            unstable.append(key)
        utility = [direction(key) * (value - center) for value in values]
        degraded = margin is not None and min(utility) < -margin
        better = margin is not None and max(utility) > margin
        deltas[key] = {
            "control": center,
            "samples": values,
            "delta": statistics.mean(values) - center,
            "margin": margin,
            "direction": "lower is better" if direction(key) < 0 else "higher is better",
            "utility_delta": direction(key) * (statistics.mean(values) - center),
            "worst_utility_delta": min(utility),
            "panel_verdict": "degraded" if degraded else "improved" if better else "within margin",
        }
        if degraded:
            moved.append(key)
        elif better:
            improved.append(key)
    failures = [r.get("error") for r in control + treatment if r.get("error")]
    if unstable:
        failures.append("control repetitions exceed declared margins: " + ", ".join(unstable))
    if len(control) != 3 or len(treatment) != 3:
        failures.append("requires three complete repetitions in each arm")
    if any(r.get("invariants", {}).get("passed") is not True for r in control):
        failures.append("control invariants incomplete or failed")
    if any(not isinstance(r.get("invariants", {}).get("passed"), bool) for r in treatment):
        failures.append("treatment invariants incomplete")
    invariant_failed = any(r.get("invariants", {}).get("passed") is False for r in treatment)
    # One valid counterexample disproves equivalence; unavailable other panels
    # cannot erase it. They still prevent a positive equivalence claim.
    verdict = (
        "unqualified"
        if failures
        else "not equivalent"
        if moved or invariant_failed
        else "unqualified"
        if missing
        else "equivalent"
    )
    if invariant_failed:
        failures.append("treatment write/keyed/restart invariant failed")
    return {
        "verdict": verdict,
        "deltas": deltas,
        "moved": moved,
        "improved": improved,
        "unstable_controls": unstable,
        "missing_margins": sorted(set(missing)),
        "failures": failures,
    }


def control_gate(runs, declared, required=REQUIRED):
    """A spread outside any frozen band stops the matrix before treatments."""
    reasons, spreads = [], {}
    if len(runs) != 3 or any(r.get("error") for r in runs):
        reasons.append("requires three error-free controls")
    if any(not r.get("invariants", {}).get("passed") for r in runs):
        reasons.append("control write/keyed/identity gate failed")
    for key in required:
        margin = declared.get(key)
        if margin is None:
            reasons.append(f"{key} frozen margin unavailable")
            continue
        values = [r.get("metrics", {}).get(key) for r in runs]
        if not values or any(
            not isinstance(v, (int, float)) or not math.isfinite(v) for v in values
        ):
            reasons.append(f"{key} control evidence unavailable")
            continue
        spread = max(values) - min(values)
        spreads[key] = {"samples": values, "spread": spread, "margin": margin}
        if spread > margin:
            reasons.append(f"{key} spread {spread:.12g} exceeds {margin:.12g}")
    if "golden.ndcg" not in spreads:
        reasons.append("golden control spread unavailable")
    return {"passed": not reasons, "spreads": spreads, "reasons": reasons}


def truth_metrics(report):
    """Retain both frozen splits; overall success must not hide holdout losses."""
    metrics = {}
    for split, count in (("overall", 50), ("visible", 30), ("holdout", 20)):
        row = report["overall"] if split == "overall" else report["splits"][split]
        if row["n"] != count:
            raise ValueError(f"incomplete current-truth {split}: expected {count} questions")
        prefix = "" if split == "overall" else split + "."
        for metric in ("p3", "abstain"):
            value = row[metric]
            if not isinstance(value, (int, float)) or not math.isfinite(value):
                raise ValueError(f"missing current-truth {split}.{metric}")
            metrics[f"current_truth.{prefix}{metric}"] = value
    return metrics


def smriti_definition():
    split_path = ROOT / "benchmarks/smriti/split.json"
    assignments = json.loads(split_path.read_text())
    split = module("ablation_split", ROOT / "benchmarks/smriti/split.py")
    return {
        "agent": "claude-code",
        "tasks": sorted(k for k, v in assignments.items() if v == "visible"),
        "split_hash": split.fingerprint(assignments),
    }


def calibration_errors(noise):
    """Bands are usable only for the panel that actually produced them."""
    errors = []
    if not noise.get("acceptance_ready") or noise.get("mode") != "replica":
        errors.append("acceptance-ready replica calibration required")
    if noise.get("golden_config") != {"limit": 20, "strategy": "hybrid", "reranker": False}:
        errors.append("golden calibration configuration mismatch")
    for key, expected in smriti_definition().items():
        if noise.get(key) != expected:
            errors.append(f"SMRITI calibration {key} mismatch")
    truth = noise.get("current_truth") or {}
    panel = ROOT / "benchmarks/current_truth/questions.json"
    if truth.get("panel_sha256") != hashlib.sha256(panel.read_bytes()).hexdigest():
        errors.append("current-truth calibration panel hash missing or mismatched")
    if truth.get("config") != {
        "realm": "project:cc-soul",
        "limit": 3,
        "strategy": "fused",
        "no_learn": True,
    }:
        errors.append("current-truth calibration configuration missing or mismatched")
    if truth.get("snapshot_id") != noise.get("snapshot_id") or not noise.get("snapshot_id"):
        errors.append("current-truth calibration snapshot missing or mismatched")
    if noise.get("errors") or noise.get("injection_confirmed") is not True:
        errors.append("SMRITI calibration must have no errors and confirmed injection")
    return errors


def smriti_metrics(records, definition):
    expected = {(task, arm) for task in definition["tasks"] for arm in ("off", "on")}
    observed = [(row.get("task_id"), row.get("condition")) for row in records]
    if len(observed) != len(expected) or set(observed) != expected:
        raise ValueError("SMRITI requires exactly one record per visible task and off/on arm")
    for row in records:
        if row.get("dry_run") is not False or row.get("tokens_used", 0) <= 0:
            raise ValueError("SMRITI requires real agent output with nonzero usage")
        if not isinstance(row.get("passed"), bool):
            raise ValueError("SMRITI task verdict missing")
        if row.get("split_hash") != definition["split_hash"] or row.get("split") != "visible":
            raise ValueError("SMRITI split identity mismatch")
        if row["condition"] == "on" and row.get("injected_confirmed") is not True:
            raise ValueError("SMRITI on-arm memory injection not confirmed")
    return {
        f"smriti.{arm}.{metric}": statistics.mean(
            row[field] for row in records if row["condition"] == arm
        )
        for arm in ("off", "on")
        for metric, field in (("sr", "passed"), ("tokens", "tokens_used"))
    }


def run_smriti(env, output, home, mind, cli, model):
    """Run the actual benchmark with this checkout's hooks and private state."""
    sys.path.insert(0, str(ROOT / "benchmarks/smriti"))
    runner = module("ablation_smriti", ROOT / "benchmarks/smriti/runner.py")
    runner.ChittaAdapter.CHITTA_BIN = str(cli)
    runner.MIND_PATH = mind
    runner.SCRATCH_ROOT = str(home.parent / "smriti-work")
    settings = home / ".claude/settings.json"
    settings.write_text(
        json.dumps(
            {
                "disableAllHooks": False,
                "hooks": {
                    "UserPromptSubmit": [
                        {
                            "hooks": [
                                {
                                    "type": "command",
                                    "command": shlex.join(
                                        ["bash", str(ROOT / "hooks/prompt-hook.sh")]
                                    ),
                                    "timeout": 60,
                                }
                            ]
                        }
                    ]
                },
            }
        )
    )
    mcp = home / "mcp.json"
    mcp.write_text('{"mcpServers": {}}\n')

    class PrivateAgent(runner.ClaudeCodeAdapter):
        def build_cmd(self, prompt):
            return [
                *super().build_cmd(prompt),
                "--model",
                model,
                "--setting-sources",
                "",
                "--settings",
                str(settings),
                "--strict-mcp-config",
                "--mcp-config",
                str(mcp),
                "--permission-mode",
                "acceptEdits",
                "--no-session-persistence",
                "--tools",
                "Bash,Read,Edit,Write,Glob,Grep",
                "--allowedTools",
                "Bash,Read,Edit,Write,Glob,Grep",
            ]

    definition = smriti_definition()
    # These variables are private to the worker process, never shared threads.
    os.environ.update(
        env,
        CHITTA_DB_PATH=str(mind),
        CHITTA_SOCKET_PATH=env["CHITTA_EVAL_SOCKET"],
        CLAUDE_CONFIG_DIR=str(home / ".claude"),
        CC_SOUL_ROOT=str(ROOT),
    )
    path = runner.run_all(
        ROOT / "benchmarks/smriti/tasks",
        definition["tasks"],
        ["off", "on"],
        PrivateAgent(),
        output / "smriti",
        trials=1,
        dry_run=False,
    )
    if path is None:
        raise ValueError("SMRITI emitted no real records")
    records = [json.loads(line) for line in Path(path).read_text().splitlines() if line.strip()]
    return smriti_metrics(records, definition)


def golden_with_traces(noise, output, cli):
    """Save diagnostics from the exact requests scored by the frozen grader."""
    traces = []

    def recall(grader, query, limit, strategy=""):
        command = [
            str(cli),
            "recall",
            "--query",
            query,
            "--limit",
            str(limit),
            "--realm",
            grader.GRADE_REALM,
            "--no-learn",
            "--explain",
            "--json",
            "--socket-path",
            grader.CHITTA_EVAL_SOCKET,
        ]
        if strategy:
            command += ["--strategy", strategy]
        response = subprocess.run(
            command,
            check=True,
            capture_output=True,
            text=True,
            stdin=subprocess.DEVNULL,
            timeout=60,
        )
        data = json.loads(response.stdout)
        traces.append({"query": query, "response": data})
        (output / "golden-traces.json").write_text(json.dumps(traces, indent=2) + "\n")
        if (
            not isinstance(data, dict)
            or not isinstance(data.get("results"), list)
            or data.get("error")
        ):
            raise ValueError("invalid golden recall response")
        embeddings = data.get("diagnostics", {}).get("embeddings", [])
        if not embeddings or any(item.get("dimension") != 768 for item in embeddings):
            raise ValueError(f"golden query embedding missing: {query!r}; see golden-traces.json")
        return data

    noise.strict_recall = recall
    return noise.golden_runs(1)


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
        if expected == value:
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
    return {
        "passed": True,
        "remembers": 200,
        "wal_recovered": 200,
        "keyed_lanes": 3,
    }


def trial(args, organs, index, output):
    started = time.monotonic()
    result = {
        "organs": list(organs),
        "trial": index,
        "metrics": {},
        "host": socket.gethostname(),
        "allocation": os.environ.get("SLURM_JOB_ID"),
    }
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
        CHITTA_RECALL_EMBED_WAIT_MS="10000",
        CHITTA_RECALL_NOW=str(args.now_ms),
        OPENBLAS_NUM_THREADS="1",
        OMP_NUM_THREADS="1",
        RAYON_NUM_THREADS="1",
    )
    with private_directory(args.scratch_root) as directory:
        mind = Path(directory) / "mind"
        identity_mind = Path(directory) / "identity"
        home = Path(directory) / "home"
        (home / ".claude/mind").mkdir(parents=True)
        env.update(HOME=str(home), CHITTA_EVAL_MIND=str(mind))
        old_env = dict(os.environ)
        try:
            run(["bash", "scripts/eval-replica.sh", "start"], env, output / "start.log")
            env.update(metadata(mind))
            result["snapshot_id"] = env["CHITTA_EVAL_SNAPSHOT_ID"]
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
            golden, snapshot = golden_with_traces(noise, output, args.cli)
            result.update(snapshot_id=snapshot)
            result["metrics"]["golden.ndcg"] = golden[0]
            run(
                [PYTHON, "benchmarks/current_truth/run.py", "--output", output / "truth.json"],
                env,
                output / "truth.log",
            )
            truth = json.loads((output / "truth.json").read_text())
            result["truth"] = truth
            result["metrics"].update(truth_metrics(truth))
            run(["bash", "scripts/bench-recall-lanes.sh", "5"], env, output / "hook.log")
            result["hooks"] = hook_metrics(output / "hook.log")
            # The frozen metric is one fixed three-query median per trial.
            # The helper requires two probes; preselect the FIRST (never the
            # faster) and retain both for audit. The paired panel above is
            # separately reported and does not replace this statistic.
            result["hook_noise"] = noise.hook_runs(2)
            result["metrics"]["hook_total_ms"] = result["hook_noise"]["samples"][0]
            result["metrics"].update(
                run_smriti(env, output, home, mind, args.cli, args.smriti_model)
            )
            result["invariants"] = invariants(stress, mind)
            result["invariants"]["passed"] = False  # Identity must complete successfully too.
            result["consumer_tests"] = {}
            for organ in organs:
                command = args.consumer_commands.get(organ)
                if command:
                    log = output / f"consumer-{organ}.log"
                    consumer_env = {
                        **env,
                        "CHITTA_SOCKET_PATH": env["CHITTA_EVAL_SOCKET"],
                        "CHITTA_DB_PATH": str(mind),
                    }
                    run(command, consumer_env, log)
                    result["consumer_tests"][organ] = {
                        "passed": True,
                        "command": command,
                        "log_sha256": hashlib.sha256(log.read_bytes()).hexdigest(),
                    }
            stop_copy(mind, env, output / "stop.log")
            run(
                [
                    PYTHON,
                    "scripts/restart-identity.py",
                    "--source",
                    args.source,
                    "--mind",
                    identity_mind,
                    "--port",
                    str(free_port()),
                    "--daemon",
                    args.daemon,
                    "--cli",
                    args.cli,
                    "--now-ms",
                    str(args.now_ms),
                    "--embed-wait-ms",
                    "10000",
                    "--report",
                    output / "identity.json",
                ],
                env,
                output / "identity.log",
                accepted_codes=(0, 1),  # A completed ID mismatch is measured, not missing data.
            )
            identity = json.loads((output / "identity.json").read_text())
            comparisons = identity.get("comparisons", [])
            if (
                identity.get("error")
                or identity.get("stop_error")
                or len(comparisons) != 1
                or len(comparisons[0]) != 20
            ):
                raise ValueError("canonical identity gate incomplete; see identity.json")
            count = sum(row["identical"] for row in comparisons[0]) if comparisons else 0
            result["invariants"]["restart"] = {
                "ordered_recall_identity": f"{count}/20",
                "report": str(output / "identity.json"),
            }
            result["invariants"]["passed"] = (
                count == 20
                and len(comparisons) == 1
                and not identity.get("error")
                and not identity.get("stop_error")
            )
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
                if (identity_mind / "replica.pid").exists():
                    identity_env = {**env, **metadata(identity_mind)}
                    stop_copy(identity_mind, identity_env, output / "identity-stop.log")
            finally:
                for source, name in ((mind, "daemon.log"), (identity_mind, "identity-daemon.log")):
                    if (source / "replica.log").is_file():
                        shutil.copyfile(source / "replica.log", output / name)
                os.environ.clear()
                os.environ.update(old_env)
    result["wall_seconds"] = time.monotonic() - started
    (output / "trial.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def write_table(report, destination):
    lines = [
        "## Organ ablation 2026-09-16",
        "",
        "Three repetitions per arm; missing calibration or invariants block retirement.",
        "When controls exceed a declared band, differences are descriptive and cannot",
        "be attributed to the ablation. No organ or tool is deleted by this runner.",
        "Panel acceptance is one-sided: improvements pass; degradation beyond a band blocks.",
        "Positive quality deltas improve; negative token/latency deltas improve.",
        "Complete panel, identity and consumer-test evidence is required for acceptance.",
        "Unqualified raw observations remain in the report and are not equivalence estimates.",
        "",
        "| Organ | Dependency class | Panels moved (Δ; margin) | Verdict |",
        "|---|---|---|---|",
    ]
    golden_control = report.get("control_gate", {}).get("spreads", {}).get("golden.ndcg")
    if golden_control:
        lines[7:7] = [
            "",
            "Golden controls: "
            + ", ".join(f"{v:.12g}" for v in golden_control["samples"])
            + f"; spread {golden_control['spread']:.12g}; frozen margin {golden_control['margin']:.12g}.",
            "",
        ]
    for organ in (*ORGANS, *GROUPS):
        row = report.get("comparisons", {}).get(organ, {})
        numbers = (
            "; ".join(
                f"{k}: {v['delta']:+.6g}; margin={v['margin']}; {v['panel_verdict']}"
                for k, v in row.get("deltas", {}).items()
                if v["margin"] is not None
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
        if organ in {"cortical_idx", "hdc_idx"}:
            dependency += "; index sidecar retained"
        verdict = row.get("verdict", "unqualified")
        if verdict == "unqualified":
            numbers = "not qualified; raw observations retained"
        if not row:
            numbers = "not measured"
            verdict += ": not run"
            gate = report.get("control_gate", {})
            if not gate.get("passed", False):
                verdict += "; " + "; ".join(gate.get("reasons", ["controls pending"]))
            elif report.get("deferred"):
                verdict += "; " + report["deferred"]["reason"]
        if row.get("failures"):
            verdict += "; " + "; ".join(row["failures"])
        if row.get("missing_margins"):
            verdict += ": missing " + ", ".join(row["missing_margins"])
        if row.get("unstable_controls"):
            verdict += "; unstable control: " + ", ".join(row["unstable_controls"])
        if row.get("moved"):
            verdict += "; degraded beyond band: " + ", ".join(row["moved"])
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
        if name != "control" and report.get("comparisons", {}).get(name, {}).get("verdict") not in (
            "equivalent",
            "not equivalent",
        ):
            continue
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
    from unittest.mock import patch

    row = {"metrics": {key: 0.5 for key in REQUIRED}, "invariants": {"passed": True}}
    declared = {key: 0.01 for key in REQUIRED}
    changed = {**row, "metrics": {**row["metrics"], "golden.ndcg": 0.7}}
    assert compare([row] * 3, [row] * 3, declared, [])["verdict"] == "equivalent"
    assert compare([row] * 3, [changed] * 3, declared, [])["verdict"] == "equivalent"
    for key in REQUIRED:
        better = {**row, "metrics": {**row["metrics"], key: 0.5 + direction(key) * 0.2}}
        worse = {**row, "metrics": {**row["metrics"], key: 0.5 - direction(key) * 0.2}}
        accepted = compare([row] * 3, [better] * 3, declared, [])
        assert accepted["verdict"] == "equivalent", key
        assert accepted["improved"] == [key]
        rejected = compare([row] * 3, [better, better, worse], declared, [])
        assert rejected["verdict"] == "not equivalent", key
        assert rejected["moved"] == [key]
    missing = compare([row] * 3, [changed] * 3, declared, ["absent"])
    assert missing["verdict"] == "unqualified"
    broken = {**changed, "invariants": {"passed": False}}
    assert compare([row] * 3, [broken] * 3, declared, [])["verdict"] == "not equivalent"
    assert control_gate([row] * 3, declared)["passed"]
    for key in REQUIRED:
        incomplete = {**row, "metrics": {k: v for k, v in row["metrics"].items() if k != key}}
        assert not control_gate([incomplete] * 3, declared)["passed"], key
        assert not control_gate([row] * 3, {k: v for k, v in declared.items() if k != key})[
            "passed"
        ]
    assert not control_gate([row, row, changed], declared)["passed"]
    assert not control_gate([{**row, "error": "missing embedding"}] * 3, declared)["passed"]
    truth = {
        "overall": {"n": 50, "p3": 0.5, "abstain": 1.0},
        "splits": {
            "visible": {"n": 30, "p3": 0.7, "abstain": 1.0},
            "holdout": {"n": 20, "p3": 0.2, "abstain": 0.5},
        },
    }
    assert truth_metrics(truth)["current_truth.holdout.p3"] == 0.2
    assert truth_metrics(truth)["current_truth.visible.abstain"] == 1.0
    definition = {"tasks": ["example"], "split_hash": "frozen"}
    records = [
        {
            "task_id": "example",
            "condition": arm,
            "dry_run": False,
            "tokens_used": 10,
            "passed": True,
            "injected_confirmed": True,
            "split": "visible",
            "split_hash": "frozen",
        }
        for arm in ("off", "on")
    ]
    assert smriti_metrics(records, definition)["smriti.on.sr"] == 1
    for broken in (
        records[:1],
        records + records[:1],
        [records[0], {**records[1], "dry_run": True}],
        [records[0], {**records[1], "tokens_used": 0}],
        [records[0], {**records[1], "injected_confirmed": False}],
    ):
        try:
            smriti_metrics(broken, definition)
        except ValueError:
            pass
        else:
            raise AssertionError("invalid SMRITI evidence must not qualify")
    assert calibration_errors({})
    assert not contains_text({"text": "remember 10"}, "remember 1")
    assert contains_text({"text": '{"content": "remember 1"}'}, "remember 1")
    # No treatment may start after incomplete or unstable controls.
    with tempfile.TemporaryDirectory(prefix="p2-control-test-") as directory:
        base = Path(directory)
        source = base / "source"
        source.mkdir()
        (source / "replica.env").touch()
        fixture = base / "fixture"
        fixture.touch()
        noise = base / "noise.json"
        noise.write_text(
            json.dumps(
                {
                    "snapshot_id": "test",
                    "smriti_model": "test",
                    "metrics": {
                        key: {"accept_delta": value, "n": 3} for key, value in declared.items()
                    },
                }
            )
        )
        argv = [
            "ablate-organs.py",
            "--source",
            str(source),
            "--output",
            str(base / "out"),
            "--noise",
            str(noise),
            "--organ",
            "all",
            "--smriti-model",
            "test",
        ]
        for flag in ("--daemon", "--cli", "--model"):
            argv += [flag, str(fixture)]
        rows = [{**r, "snapshot_id": "test"} for r in (row, row, changed)]
        with (
            patch.object(sys, "argv", argv),
            patch(__name__ + ".trial", side_effect=rows) as fake,
            patch(__name__ + ".calibration_errors", return_value=[]),
            patch.dict(os.environ, {"ANTHROPIC_API_KEY": "unit-test-placeholder"}),
            patch("shutil.which", return_value="/fixture/claude"),
        ):
            try:
                main()
            except SystemExit as exc:
                assert "controls unqualified" in str(exc)
            else:
                raise AssertionError("unstable controls must stop the runner")
            assert fake.call_count == 3
            assert all(call.args[1] == () for call in fake.call_args_list)
    print("ablation runner self-tests passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--preflight", action="store_true")
    parser.add_argument("--organ", action="append", choices=ORGANS + tuple(GROUPS))
    parser.add_argument("--controls-only", action="store_true")
    parser.add_argument("--block", choices=("groups", "largest", "remaining"))
    parser.add_argument("--now-ms", type=int, help="one fixed Unix-ms clock for the entire matrix")
    parser.add_argument(
        "--consumer-tests",
        type=Path,
        help="JSON organ -> argv for API tests run on each ablated replica",
    )
    parser.add_argument(
        "--max-load",
        type=float,
        default=float(len(os.sched_getaffinity(0))),
        help="defer remaining individuals above this one-minute node load",
    )
    parser.add_argument("--source", type=Path, default=SOURCE)
    parser.add_argument(
        "--scratch-root", type=Path, default=Path("/projects/caeg/scratch/kbd606/tmp")
    )
    parser.add_argument("--output", type=Path, default=ROOT / "results/ablation")
    parser.add_argument("--noise", type=Path, default=ROOT / "benchmarks/noise.json")
    parser.add_argument("--daemon", type=Path, default=ROOT / "bin/chittad")
    parser.add_argument("--cli", type=Path, default=ROOT / "bin/chitta")
    parser.add_argument(
        "--model",
        type=Path,
        default=Path("/maps/projects/caeg/people/kbd606/models/nomic-embed-text-v1.5.gguf"),
    )
    parser.add_argument("--smriti-model", help="pinned model from the frozen SMRITI calibration")
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
    noise = json.loads(args.noise.read_text())
    declared, missing = margins(noise)
    problems = calibration_errors(noise)
    if not args.smriti_model or noise.get("smriti_model") != args.smriti_model:
        problems.append("explicit matching frozen SMRITI model required")
    previous = args.output / "report.json"
    saved = json.loads(previous.read_text()) if args.resume and previous.exists() else None
    args.now_ms = (
        args.now_ms
        or (saved or {}).get("evaluation", {}).get("recall_now_ms")
        or time.time_ns() // 1_000_000
    )
    if args.now_ms <= 0 or args.max_load <= 0:
        parser.error("now-ms and max-load must be positive")
    consumer_tests = json.loads(args.consumer_tests.read_text()) if args.consumer_tests else {}
    if not isinstance(consumer_tests, dict) or any(
        organ not in ORGANS
        or not isinstance(command, list)
        or not command
        or any(not isinstance(value, str) or not value for value in command)
        for organ, command in consumer_tests.items()
    ):
        parser.error("consumer-tests must map known organs to nonempty command argv lists")
    args.consumer_commands = consumer_tests
    report = {
        "schema_version": 3,
        "evaluation": {
            "recall_now_ms": args.now_ms,
            "embedding_wait_ms": 10000,
            "threads": {"OPENBLAS_NUM_THREADS": 1, "OMP_NUM_THREADS": 1, "RAYON_NUM_THREADS": 1},
            "identity_gate": "scripts/restart-identity.py",
            "max_load": args.max_load,
            "smriti": {**smriti_definition(), "model": args.smriti_model},
        },
        "consumer_tests": consumer_tests,
        "noise_sha256": hashlib.sha256(args.noise.read_bytes()).hexdigest(),
        "binary_sha256": hashlib.sha256(args.daemon.read_bytes()).hexdigest()
        if args.daemon.is_file()
        else None,
        "margins": declared,
        "missing_margins": missing,
        "calibration_errors": problems,
        "runs": {},
        "comparisons": {},
    }
    if args.preflight:
        print(json.dumps({**report, "organs": ORGANS}, indent=2))
        return
    if missing or problems:
        parser.error("incomplete frozen calibration: " + "; ".join([*missing, *problems]))
    if not any(
        os.environ.get(key)
        for key in ("ANTHROPIC_API_KEY", "ANTHROPIC_AUTH_TOKEN", "CLAUDE_CODE_OAUTH_TOKEN")
    ):
        parser.error(
            "real SMRITI requires env-only agent credentials; protected home config is never read"
        )
    for path in (args.daemon, args.cli, args.model):
        if not path.is_file():
            parser.error(f"required file missing: {path}")
    if not shutil.which("claude"):
        parser.error("requested SMRITI agent is unavailable")
    args.output.mkdir(parents=True, exist_ok=True)
    if saved is not None:
        for key in (
            "schema_version",
            "evaluation",
            "binary_sha256",
            "noise_sha256",
            "margins",
            "missing_margins",
            "calibration_errors",
            "consumer_tests",
        ):
            if saved.get(key) != report.get(key):
                parser.error(f"cannot resume with changed {key}")
        report = saved
    elif previous.exists():
        parser.error("output already contains a report; use --resume or a fresh directory")
    # Freeze the declaration before the first treatment result exists.
    (args.output / "declaration.json").write_text(json.dumps(report, indent=2) + "\n")
    remaining = [organ for organ in ORGANS if organ not in LARGEST]
    blocks = {"groups": list(GROUPS), "largest": list(LARGEST), "remaining": remaining}
    selections = (
        []
        if args.controls_only
        else args.organ or blocks.get(args.block, [*GROUPS, *LARGEST, *remaining])
    )
    for name in ["control", *selections]:
        if name in remaining and os.getloadavg()[0] > args.max_load:
            report["deferred"] = {
                "reason": "node load exceeds predeclared remaining-organ limit",
                "load": os.getloadavg()[0],
                "limit": args.max_load,
                "from": name,
            }
            break
        organs = () if name == "control" else GROUPS.get(name, (name,))
        existing = report["runs"].setdefault(name, [])
        for index in range(len(existing), 3):
            out = args.output / name / str(index + 1)
            out.mkdir(parents=True, exist_ok=True)
            print(f"{name} trial {index + 1}/3", flush=True)
            result = trial(args, organs, index + 1, out)
            report["runs"][name].append(result)
            if result.get("snapshot_id") != noise.get("snapshot_id"):
                result["error"] = "; ".join(
                    filter(
                        None,
                        [
                            result.get("error"),
                            "snapshot identity differs from the declared noise calibration",
                        ],
                    )
                )
            (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        if name == "control":
            report["control_gate"] = control_gate(existing, declared)
            (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
            print("control gate: " + json.dumps(report["control_gate"], sort_keys=True), flush=True)
            if not report["control_gate"]["passed"]:
                if args.write_table:
                    write_table(report, ROOT / "docs/FIELD_PERF.md")
                raise SystemExit(
                    "controls unqualified; no ablation trials started; see golden-traces.json and identity.json"
                )
        else:
            report["comparisons"][name] = compare(
                report["runs"]["control"], report["runs"][name], declared, missing
            )
            row = report["comparisons"][name]
            absent = [
                organ
                for organ in organs
                if not all(
                    r.get("consumer_tests", {}).get(organ, {}).get("passed") is True
                    for r in report["runs"][name]
                )
            ]
            if absent:
                row["failures"].append("consumer API test evidence missing: " + ", ".join(absent))
                if row["verdict"] == "equivalent":
                    row["verdict"] = "unqualified"
        (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        if args.write_table:
            write_table(report, ROOT / "docs/FIELD_PERF.md")
    (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    if args.write_table:
        write_table(report, ROOT / "docs/FIELD_PERF.md")
    print(f"report: {args.output / 'report.json'}", flush=True)


if __name__ == "__main__":
    main()
