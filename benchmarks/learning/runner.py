#!/usr/bin/env python3
"""Paired trials from one frozen source. Dry runs never produce an experiment verdict."""

from __future__ import annotations

import argparse
import ctypes
import json
import math
import os
import random
import re
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from audit import CAVEAT, audit_trial, deny_rules, read_history
from broker import Broker
from common import (
    REALM,
    ROOT,
    RPC,
    command,
    digest,
    family,
    live_paths,
    now_ms,
    read_json,
    require,
    socket_for,
    task_worktree,
    tree_identity,
    validate_runtime_roots,
    write_json,
)
from freeze import install_grader, task_manifest, validate_freeze
from isolation import probe, sandbox_command


def guard_environment(env, live, private):
    private = Path(private).resolve()
    for key in (
        "HOME",
        "CHITTA_SOCKET_PATH",
        "CHITTA_DB_PATH",
        "XDG_RUNTIME_DIR",
        "CHITTA_QUEUE",
        "XDG_CONFIG_HOME",
        "XDG_DATA_HOME",
        "XDG_CACHE_HOME",
        "XDG_STATE_HOME",
        "CLAUDE_CONFIG_DIR",
        "TMPDIR",
    ):
        require(key in env, f"missing private {key}")
        path = Path(env[key]).resolve()
        require(path.is_relative_to(private), f"{key} escapes private trial")
        for forbidden in live.values():
            f = Path(forbidden).resolve()
            require(path != f and not path.is_relative_to(f), f"{key} resolves to live state")
    require(env.get("CHITTA_REALM") == REALM, "wrong realm")
    require(
        not env.get("CHITTA_HEADLESS") and not env.get("CC_SOUL_HEADLESS"),
        "headless alias disables hooks",
    )
    require(env.get("CHITTA_UTILITY_RECALL") == "0", "utility recall must be OFF")


def private_environment(trial, live, config):
    """Allowlist inheritance: no live config, sockets, shell startup, queues or plugins."""
    trial = Path(trial).resolve()
    visible = trial / "visible"
    home, state = visible / "home", visible / "state"
    replica = trial / "replica"
    env = {
        "PATH": str(Path(sys.executable).parent) + ":/usr/local/bin:/usr/bin:/bin",
        "LANG": "C.UTF-8",
        "HOME": str(home),
        "XDG_CONFIG_HOME": str(home / ".config"),
        "XDG_DATA_HOME": str(home / ".local/share"),
        "XDG_CACHE_HOME": str(home / ".cache"),
        "XDG_STATE_HOME": str(home / ".local/state"),
        "CLAUDE_CONFIG_DIR": str(home / ".claude"),
        "HISTFILE": str(state / "bash-history.jsonl"),
        "LEARNING_ISOLATION": config.get("isolation", "strict"),
        "LEARNING_AUDIT_POLICY": str(state / "audit-policy.json"),
        "CHITTA_DB_PATH": str(state),
        "MIND_PATH": str(state),
        "CHITTA_SOCKET_PATH": socket_for(replica, replica / "run"),
        "XDG_RUNTIME_DIR": str(replica / "run"),
        "TMPDIR": str(visible / "tmp"),
        "CHITTA_QUEUE": str(state / "queue.jsonl"),
        "CHITTA_QUEUE_PATH": str(state / "queue.jsonl"),
        "CHITTA_REALM": REALM,
        "CHITTA_NO_QUEUE": "1",
        "CHITTA_UTILITY_RECALL": "0",
        "CHITTA_BIN": config["chitta_bin"],
        "CHITTAD_BIN": config["chittad_bin"],
        "CHITTA_EVAL_MIND": str(replica),
        "CHITTA_EVAL_EMBED_MODEL": config["embed_model"],
        "CHITTA_HINT_ENRICHER": "/bin/true",
        "LEARNING_HOOK_LOG": str(state / "hook-output.jsonl"),
        "GIT_CONFIG_NOSYSTEM": "1",
        "GIT_CONFIG_GLOBAL": "/dev/null",
        "PYTHONDONTWRITEBYTECODE": "1",
    }
    # Credentials are explicit env-only inputs. Never mount or copy user Claude config.
    for name in ("ANTHROPIC_API_KEY", "CLAUDE_CODE_OAUTH_TOKEN"):
        if os.environ.get(name):
            env[name] = os.environ[name]
    if config.get("ld_library_path"):
        env["LD_LIBRARY_PATH"] = config["ld_library_path"]
    guard_environment(env, live, trial)
    for path in (home / ".claude/mind", state, replica / "run/chitta", visible / "tmp"):
        path.mkdir(parents=True, exist_ok=True)
    for key in (
        "XDG_CONFIG_HOME",
        "XDG_DATA_HOME",
        "XDG_CACHE_HOME",
        "XDG_STATE_HOME",
        "CLAUDE_CONFIG_DIR",
    ):
        Path(env[key]).mkdir(parents=True, exist_ok=True)
    Path(env["HISTFILE"]).touch()
    return env


def exposed_ids(value):
    found = set()
    if isinstance(value, dict):
        for key, item in value.items():
            if key in {"id", "memory_id", "source_id", "target_id"} and str(item).isdecimal():
                found.add(str(item))
            elif key in {"ids", "memory_ids", "source_ids"} and isinstance(item, list):
                found.update(str(i) for i in item if str(i).isdecimal())
            found.update(exposed_ids(item))
    elif isinstance(value, list):
        for item in value:
            found.update(exposed_ids(item))
    elif isinstance(value, str):
        found.update(re.findall(r"#(\d+)", value))
    return found


def verify_exclusion(ids, lookups, lanes):
    ids = set(map(str, ids))
    require(set(lookups) == ids, "incomplete ID exclusion verification")
    require(all(value is None for value in lookups.values()), "removed cohort ID still resolves")
    require(
        {"hybrid", "graph", "correction", "correction_check"} <= set(lanes),
        "missing exclusion lanes",
    )
    for lane, value in lanes.items():
        require(not (exposed_ids(value) & ids), f"removed cohort surfaces through {lane}")


def remove_and_verify(rpc, ids, prompt):
    for mid in ids:
        require(rpc.call("get", id=mid) is not None, f"cohort ID absent before ablation: {mid}")
        rpc.call("forget", id=mid)
    lookups = {mid: rpc.call("get", id=mid) for mid in ids}
    lanes = {
        "hybrid": rpc.call(
            "recall", query=prompt, realm=REALM, strategy="hybrid", limit=100, no_learn=True
        ),
        "graph": rpc.call("recall_spreading", query=prompt, realm=REALM, limit=100),
    }
    lanes["correction"] = rpc.call(
        "recall",
        query=prompt,
        realm=REALM,
        tag="correction",
        include_global=True,
        limit=100,
        no_learn=True,
    )
    lanes["correction_check"] = rpc.call("correction_check", text=prompt)
    verify_exclusion(ids, lookups, lanes)
    return {"lookups": lookups, "lanes": lanes, "verified": True}


def prepare_task_arm(rpc, task, arm, trial):
    record = {"exclusion_verified": False}
    future = task["future_ids"]
    write_json(trial / "future-exclusion.json", remove_and_verify(rpc, future, task["prompt"]))
    record["future_exclusion_verified"] = True
    if arm == "B":
        exclusion = remove_and_verify(rpc, task["eligible_cohort_ids"], task["prompt"])
        write_json(trial / "exclusion.json", exclusion)
        record["exclusion_verified"] = True
    else:
        for mid in task["eligible_cohort_ids"]:
            require(rpc.call("get", id=mid) is not None, f"A missing cohort ID {mid}")
    return record


def read_jsonl(path):
    require(Path(path).is_file(), f"missing telemetry: {Path(path).name}")
    return [json.loads(line) for line in Path(path).read_text().splitlines() if line.strip()]


def telemetry(hooks, ledger, cohort_ids):
    prompt_hooks = [h for h in hooks if h["event"] == "UserPromptSubmit"]
    recalls = [e for e in ledger if e.get("event") in {"injected", "recall_empty"}]
    require(prompt_hooks, "missing prompt hook capture")
    require(len(prompt_hooks) == len(recalls), "incomplete prompt-hook/recall ledger join")
    cohort_ids = set(map(str, cohort_ids))
    injected, total, empties, lane_ms, hook_ms, timeouts = 0, 0, 0, [], [], 0
    for hook, event in zip(prompt_hooks, recalls):
        require(hook["exit_code"] == 0, "hook execution failed")
        require(hook["session_id"] == event.get("session_id"), "telemetry session mismatch")
        require(
            hook["started_ms"] <= event.get("ts", 0) <= hook["ended_ms"],
            "ledger outside hook time window",
        )
        require(
            isinstance(event.get("hook_ms"), (int, float))
            and isinstance(event.get("lane_ms"), dict),
            "missing hook/recall latency",
        )
        # IDs in the actual printed context must agree with the ledger, not just RPC candidates.
        ids = set(map(str, event.get("ids", [])))
        actual = set(re.findall(r"\[\w+\]#(\d+) \[\d+%\]", hook["stdout"]))
        require(actual == ids, "hook output and injection ledger disagree")
        require(event.get("event") != "injected" or ids, "empty injected event")
        require(event.get("event") != "recall_empty" or not ids, "invalid empty event")
        injected += len(ids & cohort_ids)
        total += len(ids)
        empties += event["event"] == "recall_empty"
        lane_ms.extend(event["lane_ms"].values())
        hook_ms.append(event["hook_ms"])
        require(isinstance(event.get("lane_timeout"), dict), "missing lane timeout telemetry")
        timeouts += sum(bool(v) for v in event["lane_timeout"].values())
    for hook in hooks:
        if hook["event"] != "UserPromptSubmit":
            ids = set(re.findall(r"#(\d+) \[\d+%\]", hook["stdout"]))
            injected += len(ids & cohort_ids)
            total += len(ids)
    return {
        "cohort_injections": injected,
        "total_injections": total,
        "empty_turns": empties,
        "recall_ms": lane_ms,
        "hook_ms": hook_ms,
        "lane_failures": timeouts,
        "prompt_turns": len(prompt_hooks),
        "complete": True,
    }


def quantile(values, q):
    if not values:
        return None
    values = sorted(values)
    index = (len(values) - 1) * q
    lo, hi = math.floor(index), math.ceil(index)
    return round(values[lo] + (values[hi] - values[lo]) * (index - lo), 2)


def aggregate(
    tasks, events, trials, *, dry_run=False, unresolved=(), isolation_ok=True, isolation="strict"
):
    voided = [e for e in events if e.get("voided_reason")]
    events = [e for e in events if not e.get("voided_reason")]
    reasons = []
    if voided:
        reasons.append("voided trials count as missing")
    if dry_run:
        reasons.append("dry-run fixture; no model experiment")
    if len(tasks) != 20 or trials != 3:
        reasons.append("official panel requires 20 tasks and 3 trials")
    if unresolved:
        reasons.append("unresolved cohort provenance")
    if not isolation_ok:
        reasons.append("OS isolation unavailable")
    expected = {
        (t["id"], trial, arm) for t in tasks for trial in range(1, trials + 1) for arm in ("A", "B")
    }
    keys = [(e["task"], e["trial"], e["arm"]) for e in events]
    if set(keys) != expected or len(keys) != len(expected):
        reasons.append("incomplete or duplicate paired outcomes")
    if any(e.get("error") or not e.get("telemetry", {}).get("complete") for e in events):
        reasons.append("execution or telemetry failure")
    if any(e.get("success") not in (0, 1) for e in events):
        reasons.append("missing deterministic grader outcome")
    for task in tasks:
        if not any(
            e.get("telemetry", {}).get("cohort_injections", 0) > 0
            for e in events
            if e["arm"] == "A" and e["task"] == task["id"]
        ):
            reasons.append(f"task {task['id']}: A has no confirmed cohort exposure")
    if any(
        not e.get("future_exclusion_verified")
        or e.get("future_telemetry", {}).get("cohort_injections", 0) != 0
        for e in events
    ):
        reasons.append("future exclusion/exposure failed")
    if any(
        e.get("telemetry", {}).get("cohort_injections", 0) != 0 or not e.get("exclusion_verified")
        for e in events
        if e["arm"] == "B"
    ):
        reasons.append("B cohort exclusion/exposure failed")
    rows, delta = [], 0
    missing = bool(voided)
    for task in tasks:
        selected = [e for e in events if e["task"] == task["id"]]
        a_valid = [e for e in selected if e["arm"] == "A" and e.get("success") in (0, 1)]
        b_valid = [e for e in selected if e["arm"] == "B" and e.get("success") in (0, 1)]
        a, b = sum(e["success"] for e in a_valid), sum(e["success"] for e in b_valid)
        complete = len(a_valid) == trials and len(b_valid) == trials
        row_delta = (a - b) / trials if complete else None
        missing |= not complete
        delta += row_delta or 0
        rows.append(
            {
                "task": task["id"],
                "a": a,
                "b": b,
                "a_observed": len(a_valid),
                "b_observed": len(b_valid),
                "a_missing": trials - len(a_valid),
                "b_missing": trials - len(b_valid),
                "delta": row_delta,
                "cohort_ids": task.get("eligible_cohort_ids", []),
                "future_ids": task.get("future_ids", []),
                "cohort_injections": {
                    arm: sum(
                        e.get("telemetry", {}).get("cohort_injections", 0)
                        for e in selected
                        if e["arm"] == arm
                    )
                    for arm in ("A", "B")
                },
            }
        )
    net = sum((e.get("success", 0) or 0) * (1 if e["arm"] == "A" else -1) for e in events)
    verdict = (
        "NO VERDICT: " + "; ".join(dict.fromkeys(reasons))
        if reasons
        else (
            "RETAIN automatic admission"
            if net >= 3 * trials
            else "RETIRE tested free-form writers (queue admission and native learning)"
        )
    )
    if isolation == "home-audit":
        verdict += "; CAVEAT: " + CAVEAT
    metrics = {}
    for arm in ("A", "B"):
        ts = [e.get("telemetry", {}) for e in events if e["arm"] == arm]
        metrics[arm] = {
            k: sum(t.get(k, 0) for t in ts)
            for k in ("cohort_injections", "total_injections", "empty_turns", "lane_failures")
        }
        future_injections = sum(
            e.get("future_telemetry", {}).get("cohort_injections", 0)
            for e in events
            if e["arm"] == arm
        )
        metrics[arm]["future_injections"] = future_injections
        total = metrics[arm]["total_injections"]
        metrics[arm]["future_recall_share"] = future_injections / total if total else None
        for key in ("recall_ms", "hook_ms"):
            values = [v for t in ts for v in t.get(key, [])]
            metrics[arm][key] = {"median": quantile(values, 0.5), "p95": quantile(values, 0.95)}
    return {
        "delta": None if missing else delta,
        "net_successes": None if missing else net,
        "voided_trials": len(voided),
        "rows": rows,
        "verdict": verdict,
        "metrics": metrics,
    }


def render_table(summary, events, trials):
    lines = [
        "# Automatic-learning paired results",
        "",
        f"Trials per arm: {trials}",
        "",
        "| Task | A successes / observed | B successes / observed | Missing A/B | Δ |",
        "| --- | ---: | ---: | --- | ---: |",
    ]
    for row in summary["rows"]:
        change = "missing" if row["delta"] is None else f"{row['delta']:+.3f}"
        lines.append(
            f"| {row['task']} | {row['a']}/{row['a_observed']} | "
            f"{row['b']}/{row['b_observed']} | {row['a_missing']}/{row['b_missing']} | {change} |"
        )
    lines += [
        "",
        f"Δ = {summary['delta']}; net successes = {summary['net_successes']}; "
        f"voided trials = {summary['voided_trials']}",
        "",
        "| Task | Trial | Arm | Success | Cohort / total injections | Empty turns | Error |",
        "| --- | ---: | --- | ---: | ---: | ---: | --- |",
    ]
    for e in events:
        t = e.get("telemetry", {})
        lines.append(
            f"| {e['task']} | {e['trial']} | {e['arm']} | {e.get('success')} | "
            f"{t.get('cohort_injections', 0)} / {t.get('total_injections', 0)} | "
            f"{t.get('empty_turns', 0)} | {str(e.get('voided_reason') or e.get('error', '')).replace('|', '/')} |"
        )
    lines += [
        "",
        "Telemetry (milliseconds; recall distribution includes individual lanes):",
        "",
        "```json",
        json.dumps(summary["metrics"], indent=2),
        "```",
        "",
        summary["verdict"],
        "",
    ]
    return "\n".join(lines)


def stop_replica(env, log):
    """Reap only the daemon started for this trial; never signal an unrelated PID."""
    pidfile = Path(env["CHITTA_EVAL_MIND"]) / "replica.pid"
    pid = int(pidfile.read_text()) if pidfile.exists() else None
    with Path(log).open("a") as out:
        p = subprocess.Popen(
            ["bash", str(ROOT / "scripts/eval-replica.sh"), "stop"],
            env=env,
            stdout=out,
            stderr=out,
            stdin=subprocess.DEVNULL,
        )
        deadline = time.monotonic() + 75
        while p.poll() is None:
            if pid:
                try:
                    os.waitpid(pid, os.WNOHANG)
                except ChildProcessError:
                    pass
            if time.monotonic() > deadline:
                p.terminate()
                raise ValueError("scratch launcher stop timed out")
            time.sleep(0.1)
        require(p.returncode == 0, "scratch daemon did not stop cleanly")


def start_replica(env, source, log):
    env["CHITTA_LIVE_MIND"] = str(source)  # Launcher's source variable; always the frozen COPY.
    with socket.socket() as port:
        port.bind(("127.0.0.1", 0))
        env["CHITTA_EVAL_PORT"] = str(port.getsockname()[1])
    with Path(log).open("w") as out:
        p = subprocess.run(
            ["bash", str(ROOT / "scripts/eval-replica.sh"), "start"],
            env=env,
            stdout=out,
            stderr=out,
            stdin=subprocess.DEVNULL,
            timeout=720,
        )
    require(p.returncode == 0, "scratch replica startup failed; see replica-launch.log")
    return RPC(env["CHITTA_SOCKET_PATH"], writable=True)


def prepare_hooks(visible, env, work, live, config):
    shutil.copytree(ROOT / "hooks", visible / "hooks")
    for name in ("hook_capture.py", "isolation.py", "common.py", "audit.py"):
        shutil.copyfile(ROOT / "benchmarks/learning" / name, visible / name)
    hooks = {}
    for event, filename in (
        ("UserPromptSubmit", "prompt-hook.sh"),
        ("PreToolUse", "-"),
        ("PostToolUse", "post-bash-hook.sh"),
        ("PostToolUseFailure", "post-bash-hook.sh"),
    ):
        argv = [
            sys.executable,
            str(visible / "hook_capture.py"),
            event,
            str(visible / "hooks" / filename) if filename != "-" else "-",
        ]
        hooks[event] = [
            {
                "matcher": "" if event == "UserPromptSubmit" else "*",
                "hooks": [{"type": "command", "command": shlex.join(argv), "timeout": 30}],
            }
        ]
    settings = {"hooks": hooks, "disableAllHooks": False, "enableAllProjectMcpServers": False}
    policy = {
        "work": str(work),
        "roots": [str(work), str(visible)],
        "home": env["HOME"],
        "protected": [
            str(visible / "hooks"),
            str(visible / "state"),
            env["HOME"],
            str(visible / "hook_capture.py"),
            str(visible / "audit.py"),
            str(visible / "common.py"),
            str(visible / "isolation.py"),
            str(visible / "trial_mcp.py"),
            str(visible / "mcp.json"),
            str(visible / "claude"),
            str(visible / "chitta"),
        ],
        "live_targets": [
            live["socket"],
            live["mind"],
            "/home/kbd606/.claude",
            ":9481",
            ":7681",
            ":8765",
        ],
    }
    write_json(env["LEARNING_AUDIT_POLICY"], policy)
    if config.get("isolation") == "home-audit":
        settings["permissions"] = {
            "defaultMode": config["permission_mode"],
            "allow": config["allowed_tools"],
            "deny": deny_rules(policy["roots"], policy["live_targets"][:3]),
        }
    settings_path = Path(env["CLAUDE_CONFIG_DIR"]) / "settings.json"
    write_json(settings_path, settings)
    # Only this trial's stdio chitta bridge; it has no path discovery or auto-start.
    shutil.copyfile(ROOT / "benchmarks/learning/trial_mcp.py", visible / "trial_mcp.py")
    write_json(
        visible / "mcp.json",
        {
            "mcpServers": {
                "chitta": {
                    "command": sys.executable,
                    "args": [str(visible / "trial_mcp.py")],
                    "env": {"CHITTA_SOCKET_PATH": env["CHITTA_SOCKET_PATH"]},
                }
            }
        },
    )
    return settings_path


def execute_agent(
    task, trial_number, arm, work, visible, env, config, dry_run, sandbox_ok, settings=None
):
    if settings is None:
        settings = prepare_hooks(visible, env, work, config["live_paths"], config)
    if dry_run:
        # Exercise the actual prompt and Bash hooks. Only the model is substituted.
        session = f"fixture-{task['id']}-{trial_number}-{arm}"
        payload = {
            "session_id": session,
            "cwd": str(work),
            "prompt": task["prompt"],
            "hook_event_name": "UserPromptSubmit",
        }
        command(
            [
                sys.executable,
                visible / "hook_capture.py",
                "UserPromptSubmit",
                visible / "hooks/prompt-hook.sh",
            ],
            cwd=work,
            env=env,
            input=json.dumps(payload),
            timeout=40,
        )
        fixture = task["fixture"][arm][trial_number - 1]
        require(isinstance(fixture["exit_code"], int), "fixture needs an exit code")
        # Fixture file edits simulate agent work; neither the grader nor known_good is consulted.
        for name, content in fixture.get("writes", {}).items():
            p = work / name
            require(p.resolve().is_relative_to(work.resolve()), "fixture write escapes worktree")
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(content)
        result = command(
            ["/bin/sh", "-c", f"exit {fixture['exit_code']}"], cwd=work, env=env, check=False
        )
        payload = {
            "session_id": session,
            "cwd": str(work),
            "hook_event_name": "PostToolUse",
            "tool_name": "Bash",
            "tool_input": {"command": "true"},
            "tool_response": {"exit_code": result.returncode, "stdout": "", "stderr": ""},
        }
        command(
            [sys.executable, visible / "hook_capture.py", "PreToolUse", "-"],
            cwd=work,
            env=env,
            input=json.dumps({**payload, "hook_event_name": "PreToolUse"}),
        )
        if fixture.get("void_out_of_root_read"):
            forged = {
                **payload,
                "hook_event_name": "PreToolUse",
                "tool_name": "Read",
                "tool_input": {"file_path": "/home/kbd606/.claude/projects/fabricated.jsonl"},
            }
            command(
                [sys.executable, visible / "hook_capture.py", "PreToolUse", "-"],
                cwd=work,
                env=env,
                input=json.dumps(forged),
            )
        command(
            [
                sys.executable,
                visible / "hook_capture.py",
                "PostToolUse",
                visible / "hooks/post-bash-hook.sh",
            ],
            cwd=work,
            env=env,
            input=json.dumps(payload),
            timeout=40,
        )
        return result
    require(
        config.get("isolation") == "home-audit" or sandbox_ok,
        "OS isolation is required before invoking Claude",
    )
    require(
        env.get("ANTHROPIC_API_KEY") or env.get("CLAUDE_CODE_OAUTH_TOKEN"),
        "provide env-only Claude credentials",
    )
    argv = [
        config["claude_bin"],
        "-p",
        task["prompt"],
        "--model",
        config["model"],
        "--output-format",
        "stream-json",
        "--verbose",
        "--include-hook-events",
        "--disable-slash-commands",
        "--max-turns",
        str(config["max_turns"]),
        "--max-budget-usd",
        str(config["budget_usd"]),
        "--no-session-persistence",
        "--setting-sources",
        "",
        "--settings",
        str(settings),
        "--strict-mcp-config",
        "--mcp-config",
        str(visible / "mcp.json"),
        "--tools",
        "Bash,Read,Edit,Write,Glob,Grep",
        "--permission-mode",
        config["permission_mode"],
        "--allowedTools",
        ",".join(config["allowed_tools"]),
    ]
    # Only the pinned executable is copied; no ~/.claude config or transcripts.
    binary = visible / "claude"
    shutil.copyfile(config["claude_bin"], binary)
    binary.chmod(0o700)
    argv[0] = str(binary)
    cli = visible / "chitta"
    shutil.copyfile(config["chitta_bin"], cli)
    cli.chmod(0o700)
    env["CHITTA_BIN"] = str(cli)
    if config.get("isolation") == "home-audit":
        return command(argv, cwd=work, env=env, timeout=config["timeout_s"], check=False)
    argv = [sys.executable, visible / "isolation.py", "--network-exec", *argv]
    invocation = sandbox_command(visible, work, argv, config.get("runtime_roots", []))
    # Bind only the scratch socket, keeping all snapshot bytes and hidden graders outside.
    separator = invocation.index("--")
    invocation[separator:separator] = [
        "--ro-bind",
        env["CHITTA_SOCKET_PATH"],
        env["CHITTA_SOCKET_PATH"],
    ]
    return command(invocation, cwd=work, env=env, timeout=config["timeout_s"], check=False)


def run_trial(task, trial_number, arm, run_dir, frozen, live, dry_run, sandbox_ok):
    artifacts = run_dir / "trials" / f"{task['id']}-{trial_number}-{arm}"
    artifacts.mkdir(parents=True)
    # Linux Unix sockets have a 108-byte path limit. Results paths need not be short.
    trial = Path(tempfile.mkdtemp(prefix="clrn-", dir="/tmp"))
    config = {**frozen["manifest"]["config"], "live_paths": live}
    env = private_environment(trial, live, config)
    record = {
        "task": task["id"],
        "trial": trial_number,
        "arm": arm,
        "success": None,
        "started_ms": now_ms(),
        "exclusion_verified": False,
        "private_trial": str(trial),
    }
    source = frozen["cohort"]["store"]["mind"]
    stopped = False
    broker = None
    audit_policy, audit_pins = None, {}
    try:
        verify_code_pins(frozen["manifest"])
        rpc = start_replica(env, source, trial / "replica-launch.log")
        future = task["future_ids"]
        record.update(prepare_task_arm(rpc, task, arm, trial))
        visible = trial / "visible"
        proxy_path = visible / "state/recall.sock"
        broker = Broker(proxy_path, env["CHITTA_SOCKET_PATH"]).start()
        env["CHITTA_SOCKET_PATH"] = str(proxy_path)
        guard_environment(env, live, trial)
        with task_worktree(task, visible) as work:
            settings = prepare_hooks(visible, env, work, live, config)
            audit_policy = read_json(env["LEARNING_AUDIT_POLICY"])
            controls = [
                settings,
                Path(env["LEARNING_AUDIT_POLICY"]),
                visible / "mcp.json",
                *visible.glob("*.py"),
                *(visible / "hooks").rglob("*.sh"),
            ]
            audit_pins = {str(p): digest(p) for p in controls}
            result = execute_agent(
                task, trial_number, arm, work, visible, env, config, dry_run, sandbox_ok, settings
            )
            (trial / "agent.stdout").write_text(result.stdout)
            (trial / "agent.stderr").write_text(result.stderr)
            record["agent_exit_code"] = result.returncode
            # An ordinary unsuccessful model attempt is a scored failure, not missing telemetry.
            hooks = read_jsonl(env["LEARNING_HOOK_LOG"])
            ledger = read_jsonl(Path(env["CHITTA_DB_PATH"]) / "outcome_ledger.jsonl")
            record["telemetry"] = telemetry(hooks, ledger, task["eligible_cohort_ids"])
            record["future_telemetry"] = telemetry(hooks, ledger, future)
            require(
                record["future_telemetry"]["cohort_injections"] == 0,
                "future memory exposed after exclusion",
            )
            if not dry_run:
                output = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
                require(
                    any(e.get("type") == "result" for e in output), "missing Claude result envelope"
                )
            install_grader(task, work)
            grade = command(
                task["grader"]["command"],
                cwd=work,
                env=env,
                check=False,
                timeout=task["grader"].get("timeout_s", 60),
            )
            (trial / "grader.stdout").write_text(grade.stdout)
            (trial / "grader.stderr").write_text(grade.stderr)
            record["grader_exit_code"] = grade.returncode
            record["success"] = int(grade.returncode == 0)
            shutil.copyfile(
                Path(env["CHITTA_DB_PATH"]) / "outcome_ledger.jsonl", trial / "outcome_ledger.jsonl"
            )
            shutil.copyfile(env["LEARNING_HOOK_LOG"], trial / "hook-output.jsonl")
    except (ValueError, OSError, KeyError, subprocess.TimeoutExpired) as exc:
        record["error"] = str(exc)
    finally:
        if broker:
            broker.close()
        try:
            stop_replica(env, trial / "replica-launch.log")
            stopped = True
        except (ValueError, OSError) as exc:
            record["error"] = (record.get("error", "") + "; " + str(exc)).strip("; ")
    if config.get("isolation") == "home-audit":
        try:
            record["voided_reason"] = audit_trial(
                read_jsonl(env["LEARNING_HOOK_LOG"]),
                read_history(env["HISTFILE"]),
                audit_policy or {},
            )
        except (OSError, ValueError, KeyError) as exc:
            record["voided_reason"] = f"audit unavailable: {exc}"
        if any(
            not Path(p).is_file() or digest(p) != expected for p, expected in audit_pins.items()
        ):
            record["voided_reason"] = "audit controls changed during trial"
        if record["voided_reason"]:
            record["success"] = None
    record["ended_ms"] = now_ms()
    write_json(artifacts / "outcome.json", record)
    for path in trial.iterdir():
        if path.is_file():
            shutil.copyfile(path, artifacts / path.name)
    for name in (
        "hook-output.jsonl",
        "outcome_ledger.jsonl",
        "bash-history.jsonl",
        "audit-policy.json",
    ):
        path = trial / "visible/state" / name
        if path.is_file():
            shutil.copyfile(path, artifacts / name)
    daemon_log = trial / "replica/replica.log"
    if daemon_log.is_file():
        shutil.copyfile(daemon_log, artifacts / "replica.log")
    # Never unlink a store until its own daemon was confirmed stopped.
    if stopped:
        shutil.rmtree(trial)
    return record


def verify_code_pins(manifest):
    for rel, expected in manifest["code_sha256"].items():
        require(digest(ROOT / rel) == expected, f"pinned code changed: {rel}")


def load_frozen(path):
    path = Path(path)
    require(path.is_file(), "frozen manifest required")
    manifest = read_json(path)
    for name in ("tasks", "cohort"):
        require(
            digest(path.parent / f"{name}.json") == manifest[f"{name}_sha256"],
            f"{name} hash mismatch",
        )
    tasks, cohort = read_json(path.parent / "tasks.json"), read_json(path.parent / "cohort.json")
    validate_freeze(tasks, cohort, manifest["config"], fixture=manifest["fixture"])
    require(
        manifest.get("task_cohorts") == {t["id"]: task_manifest(t, cohort) for t in tasks},
        "per-task manifest membership mismatch",
    )
    verify_code_pins(manifest)
    require(
        str(Path(sys.executable).resolve()) == str(Path(manifest["python"]["path"]).resolve())
        and digest(sys.executable) == manifest["python"]["sha256"],
        "Python runtime changed",
    )
    validate_runtime_roots(
        manifest["config"].get("runtime_roots", []),
        [*live_paths().values(), ROOT, cohort["store"]["mind"], *[t["repo"] for t in tasks]],
    )
    for root, expected in manifest.get("runtime_sha256", {}).items():
        require(tree_identity(root) == expected, "runtime dependency tree changed")
    for binary in manifest["binaries"].values():
        require(
            digest(binary["path"]) == binary["sha256"], f"pinned binary changed: {binary['path']}"
        )
    require(family(cohort["store"]["mind"]) == cohort["store"], "frozen source family changed")
    require(
        command([manifest["config"]["claude_bin"], "--version"]).stdout.strip()
        == manifest["config"]["claude_version"],
        "Claude version changed",
    )
    return {"manifest": manifest, "tasks": tasks, "cohort": cohort}


def run(manifest_path, out=None, *, dry_run=False, trials=3, isolation=None):
    frozen = load_frozen(manifest_path)
    mode = frozen["manifest"]["isolation"]
    require(isolation is None or isolation == mode, "isolation differs from frozen manifest")
    require(mode == frozen["manifest"]["config"]["isolation"], "isolation pin mismatch")
    if out is None:
        out = (
            Path(
                os.environ.get(
                    "CHITTA_LEARNING_OUT", "/projects/caeg/scratch/kbd606/tmp/learning-results"
                )
            )
            / f"run-{now_ms()}"
        )
    require(trials > 0, "trials must be positive")
    require(
        dry_run or not frozen["manifest"]["fixture"], "fixture manifest forbids model execution"
    )
    require(dry_run or trials == 3, "official runs require three trials from the start")
    require(
        not dry_run or frozen["manifest"]["fixture"], "dry-run needs an explicit fixture manifest"
    )
    live = live_paths()
    source = Path(frozen["cohort"]["store"]["mind"]).resolve()
    require(not source.is_relative_to(Path(live["mind"])), "source must be a frozen replica COPY")
    out = Path(out).resolve()
    require(not out.exists(), "run directory already exists")
    for forbidden in (live["home"], live["mind"], str(source)):
        f = Path(forbidden).resolve()
        require(not out.is_relative_to(f) and not f.is_relative_to(out), "unsafe run output path")
    isolation_error = probe() if mode == "strict" else None
    require(dry_run or isolation_error is None, f"OS isolation unavailable: {isolation_error}")
    # Adopt/reap our own launcher children so stop can distinguish exited daemons from zombies.
    require(ctypes.CDLL(None).prctl(36, 1, 0, 0, 0) == 0, "cannot become scratch child subreaper")
    out.mkdir(parents=True)
    rng = random.Random(frozen["manifest"]["config"]["seed"])
    schedule = []
    for task in frozen["tasks"]:
        for trial in range(1, trials + 1):
            arms = ["A", "B"]
            rng.shuffle(arms)
            schedule.extend((task, trial, arm) for arm in arms)
    run_manifest = {
        **frozen["manifest"],
        "frozen_manifest_sha256": digest(manifest_path),
        "dry_run": dry_run,
        "run_trials": trials,
        "isolation_error": isolation_error,
        "schedule": [[t["id"], n, arm] for t, n, arm in schedule],
        "started_ms": now_ms(),
    }
    write_json(out / "manifest.json", run_manifest)
    write_json(out / "tasks.json", frozen["tasks"])
    write_json(out / "cohort.json", frozen["cohort"])
    events = []
    for task, trial, arm in schedule:
        event = run_trial(task, trial, arm, out, frozen, live, dry_run, isolation_error is None)
        events.append(event)
        with (out / "events.jsonl").open("a") as stream:
            stream.write(json.dumps(event) + "\n")
        print(
            json.dumps(
                {
                    k: event.get(k)
                    for k in ("task", "trial", "arm", "success", "voided_reason", "error")
                }
            ),
            flush=True,
        )
    unchanged = family(source) == frozen["cohort"]["store"]
    try:
        verify_code_pins(frozen["manifest"])
    except ValueError:
        unchanged = False
    summary = aggregate(
        frozen["tasks"],
        events,
        trials,
        dry_run=dry_run,
        unresolved=frozen["cohort"]["unresolved"],
        isolation_ok=isolation_error is None and unchanged,
        isolation=mode,
    )
    write_json(out / "summary.json", summary)
    (out / "table.md").write_text(render_table(summary, events, trials))
    run_manifest.update(
        ended_ms=now_ms(), events_sha256=digest(out / "events.jsonl"), source_unchanged=unchanged
    )
    write_json(out / "manifest.json", run_manifest)
    print(summary["verdict"])
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    r = sub.add_parser("run")
    r.add_argument("--manifest", required=True)
    r.add_argument("--out")
    r.add_argument("--isolation", choices=("strict", "home-audit"))
    r.add_argument("--dry-run", action="store_true")
    r.add_argument("--trials", type=int, default=3)
    p = sub.add_parser("report")
    p.add_argument("run")
    args = parser.parse_args()
    if args.action == "report":
        directory = Path(args.run)
        manifest = read_json(directory / "manifest.json")
        require(
            digest(directory / "events.jsonl") == manifest.get("events_sha256"),
            "incomplete or changed run events",
        )
        for name in ("tasks", "cohort"):
            require(
                digest(directory / f"{name}.json") == manifest[f"{name}_sha256"],
                f"changed run {name}",
            )
        tasks = read_json(directory / "tasks.json")
        cohort = read_json(directory / "cohort.json")
        events = read_jsonl(directory / "events.jsonl")
        summary = aggregate(
            tasks,
            events,
            manifest["run_trials"],
            dry_run=manifest["dry_run"],
            unresolved=cohort["unresolved"],
            isolation_ok=not manifest["isolation_error"] and manifest["source_unchanged"],
            isolation=manifest["isolation"],
        )
        print(render_table(summary, events, manifest["run_trials"]))
    else:
        run(
            args.manifest,
            args.out,
            dry_run=args.dry_run,
            trials=args.trials,
            isolation=args.isolation,
        )


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError, subprocess.TimeoutExpired) as exc:
        raise SystemExit(f"NO VERDICT: {exc}") from exc
