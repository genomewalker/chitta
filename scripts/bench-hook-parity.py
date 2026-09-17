#!/usr/bin/env python3
"""Compare complete hook stdout, stderr and exit status on a private replica.

No output normalization: the hooks pin presentation clocks explicitly. Wall
latency is measured independently with perf_counter_ns. The daemon must already
be running via eval-replica.sh, over a private copy, with CHITTA_RECALL_NOW pinned.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "hooks/tests/fixtures/parity"


def load(path):
    return json.loads(path.read_text())


def replica_environment(mind, now):
    """Require eval-replica metadata and verify the actual process, not a flag."""
    values = {}
    for line in (mind / "replica.env").read_text().splitlines():
        key, value = line.split("=", 1)
        values[key] = shlex.split(value)[0]
    pid = int(values["CHITTA_EVAL_PID"])
    argv = Path(f"/proc/{pid}/cmdline").read_bytes().split(b"\0")
    encoded = os.fsencode(str(mind))
    if b"--path" not in argv or argv[argv.index(b"--path") + 1] != encoded:
        raise ValueError("replica pid does not own the specified private mind")
    environ = dict(
        part.split(b"=", 1)
        for part in Path(f"/proc/{pid}/environ").read_bytes().split(b"\0")
        if b"=" in part
    )
    if environ.get(b"CHITTA_RECALL_NOW") != str(now).encode():
        raise ValueError("replica CHITTA_RECALL_NOW differs from fixture clock")
    if int(values["CHITTA_EVAL_PORT"]) in (7432, 7433, 9481):
        raise ValueError("use an explicitly private replica port")
    sock = Path(values["CHITTA_EVAL_SOCKET"])
    if not sock.is_socket() or not sock.is_relative_to(mind):
        raise ValueError("replica socket must exist inside the private mind")
    return values


def run_hook(command, payload, env, cwd, timeout):
    """Preserve failures/signals; reap this hook's background process group."""
    started = time.perf_counter_ns()
    proc = subprocess.Popen(
        command,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
        cwd=cwd,
        start_new_session=True,
    )
    timed_out = False
    try:
        stdout, stderr = proc.communicate(payload, timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        os.killpg(proc.pid, signal.SIGKILL)
        stdout, stderr = proc.communicate()
    finally:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    # Lane spans overlap: compare the longest lane with the external wall time.
    rendered = stdout.decode("utf-8", errors="replace").replace("\\n", "\n")
    timing = re.search(r"\| t:([^|\n]+)", rendered)
    spans = {
        name: int(ms)
        for name, ms in re.findall(
            r"(sem|ctx|hyb|kw|corr|corrk|xr|total)=(\d+)", timing[1] if timing else ""
        )
    }
    return (
        stdout,
        stderr,
        {
            "returncode": proc.returncode,
            "reported_ms": spans,
            "timed_out": timed_out,
            "wall_ms": round((time.perf_counter_ns() - started) / 1_000_000, 3),
        },
    )


def prepare(work, cli, socket, now):
    marker = work / ".hook-parity-owned"
    if work.exists():
        if not marker.is_file():
            raise ValueError("refusing to reset an unowned work directory")
        shutil.rmtree(work)
    work.mkdir(parents=True)
    marker.touch()
    home = work / "home"
    mind = home / ".claude/mind"
    for path in (mind, work / "runtime", work / "bin", work / "project"):
        path.mkdir(parents=True, exist_ok=True)
    (work / "project/.cc-soul-realm").write_text("project:cc-soul\n")
    # Private HOME isolates all local state, including paths that ignore DB_PATH.
    # Do not inherit aliases, headless flags, or live daemon connection settings.
    env = {
        k: v
        for k, v in os.environ.items()
        if not k.startswith(("CHITTA_", "CC_SOUL_", "CLAUDE_", "CODEX_"))
    }
    env.update(
        {
            "HOME": str(home),
            "XDG_RUNTIME_DIR": str(work / "runtime"),
            "TMPDIR": str(work),
            "CHITTA_DB_PATH": str(mind),
            "CHITTA_SOCKET_PATH": socket,
            "CHITTA_QUEUE": str(work / "queue.jsonl"),
            "CHITTA_REALM": "project:cc-soul",
            "CHITTA_LEAN": "1",
            "CHITTA_PLUGIN_DIR": str(ROOT),
            "CHITTA_HOOK_NOW": str(now),
            "CHITTA_RECALL_NOW": str(now),
            "CHITTA_TASK_LEDGER": str(work / "tasks.sqlite"),
            "OPENBLAS_NUM_THREADS": "1",
            "OMP_NUM_THREADS": "1",
            "RAYON_NUM_THREADS": "1",
            "TZ": "UTC",
            "LC_ALL": "C.UTF-8",
            "CHITTA_SQZ_DEDUP": "0",
            "CHITTA_DISABLE_CONSOLIDATION": "1",
        }
    )
    # Pin Python for subprocesses too. Never invoke the hook's historical pkill:
    # it has no notification process in this private fixture environment.
    (work / "bin/python3").symlink_to(sys.executable)
    (work / "bin/pkill").write_text("#!/bin/sh\nexit 0\n")
    (work / "bin/pkill").chmod(0o755)
    wrapper = work / "bin/chitta"
    wrapper.write_text(
        "#!/bin/bash\nexec "
        + shlex.quote(str(cli))
        + " --socket-path "
        + shlex.quote(socket)
        + ' "$@"\n'
    )
    wrapper.chmod(0o755)
    env["CHITTA_BIN"] = str(wrapper)
    env["PATH"] = str(work / "bin") + os.pathsep + env["PATH"]
    return env


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("record", "check", "measure"))
    parser.add_argument("--replica", type=Path, required=True)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--hooks", type=Path, default=ROOT / "hooks")
    parser.add_argument("--reference-hooks", type=Path)
    parser.add_argument("--require-pipeline", action="store_true")
    parser.add_argument("--require-ledger", action="store_true")
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, default=FIXTURES / "baseline")
    parser.add_argument("--results", type=Path, required=True)
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--warmup", type=int, default=4)
    parser.add_argument("--only", action="append")
    args = parser.parse_args()
    args.hooks = args.hooks.resolve(strict=True)
    if args.reference_hooks:
        args.reference_hooks = args.reference_hooks.resolve(strict=True)
        if args.mode == "record":
            parser.error("--reference-hooks is for check or measure, not baseline recording")
    config = load(FIXTURES / "suite.json")
    now = config["now_ms"]
    replica = args.replica.resolve()
    work = args.work.resolve()
    if not work.is_relative_to(Path("/tmp")) or work == Path("/tmp"):
        parser.error("--work must be a private subdirectory of /tmp")
    if replica == work or replica.is_relative_to(work) or work.is_relative_to(replica):
        parser.error("hook work and replica mind must be disjoint")
    metadata = replica_environment(replica, now)
    cli = args.cli.resolve(strict=True)
    if args.repeat < 2 or args.warmup < 0:
        parser.error("--repeat must be at least 2; --warmup must be nonnegative")
    args.results.mkdir(parents=True, exist_ok=True)
    if args.mode == "record":
        args.baseline.mkdir(parents=True, exist_ok=True)
    failures = []
    rows = []
    for iteration in range(-args.warmup, args.repeat):
        for case in config["cases"]:
            name = case["name"]
            if args.only and name not in args.only:
                continue

            def execute(hook_root, case=case, name=name):
                env = prepare(work, cli, metadata["CHITTA_EVAL_SOCKET"], now)
                env.update(case.get("env", {}))
                if args.require_ledger:
                    env["CHITTA_LEDGER_PROFILE"] = str(work / "ledger-profile.json")
                if args.mode == "measure":
                    env.pop("CHITTA_HOOK_NOW", None)
                if args.mode == "measure" or args.require_pipeline:
                    env["CHITTA_HOOK_PROFILE"] = str(work / "prompt-profile.json")
                for rel, content in case.get("state", {}).items():
                    target = work / "home/.claude/mind" / rel
                    target.write_text(content)
                    os.utime(target, (now / 1000, now / 1000))
                transcript = (
                    (FIXTURES / case.get("transcript", "transcript.jsonl"))
                    .read_text()
                    .replace("@WORK@", str(work))
                )
                (work / "transcript.jsonl").write_text(transcript)
                payload = (
                    (FIXTURES / case["input"]).read_bytes().replace(b"@WORK@", os.fsencode(work))
                )
                # Setup runs outside hook timing and only after replica identity
                # checks. These synthetic rows live solely in the private copy.
                for setup in config.get("ledger_setup", []) + case.get("ledger_setup", []):
                    setup_args = json.dumps(setup["args"]).replace("@WORK@", str(work))
                    subprocess.run(
                        [
                            str(cli),
                            "--socket-path",
                            metadata["CHITTA_EVAL_SOCKET"],
                            "ledger_op",
                            "--op",
                            setup["op"],
                            "--args",
                            setup_args,
                            "--json",
                        ],
                        env=env,
                        stdin=subprocess.DEVNULL,
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.PIPE,
                        timeout=10,
                        check=True,
                    )
                stdout, stderr, result = run_hook(
                    ["bash", str(hook_root / case["hook"]), *case.get("args", [])],
                    payload,
                    env,
                    work / "project",
                    case.get("timeout_s", 15),
                )
                if args.mode == "measure" and name in ("bash", "codex-bash"):
                    _, _, empty = run_hook(
                        ["bash", "-c", "while IFS= read -r line; do :; done"],
                        payload,
                        env,
                        work / "project",
                        case.get("timeout_s", 15),
                    )
                    result["empty_reader_ms"] = empty["wall_ms"]
                    result["added_wall_ms"] = round(result["wall_ms"] - empty["wall_ms"], 3)
                return stdout, stderr, result

            reference = execute(args.reference_hooks) if args.reference_hooks else None
            stdout, stderr, result = execute(args.hooks)
            row = {"name": name, "iteration": iteration, **result}
            if args.require_ledger and (
                name.startswith(("session-", "stop")) or name == "codex-stop"
            ):
                ledger_profile = work / "ledger-profile.json"
                if not ledger_profile.is_file():
                    failures.append(f"{iteration}:{name}: daemon ledger assembly did not run")
                else:
                    row["ledger_assembly_ms"] = load(ledger_profile).get("assembly_ms")
                if ledger_profile.is_file() and (name.startswith("stop") or name == "codex-stop"):
                    queue = work / "queue.jsonl"
                    writes = (
                        [json.loads(line) for line in queue.read_text().splitlines()]
                        if queue.is_file()
                        else []
                    )
                    if not any(
                        item.get("tool") == "ledger_op"
                        and item.get("args", {}).get("op") == "hook_turn"
                        for item in writes
                    ):
                        failures.append(f"{iteration}:{name}: turn did not use ledger_op")
                    prepared = load(ledger_profile)["value"]
                    if not any(
                        item.get("tool") == "ledger_op" and item.get("args") == prepared
                        for item in writes
                    ):
                        failures.append(
                            f"{iteration}:{name}: assembled capsule was not queued unchanged"
                        )
            profile = work / "prompt-profile.json"
            if (
                args.require_pipeline
                and name in ("prompt", "codex-prompt")
                and not profile.is_file()
            ):
                failures.append(f"{iteration}:{name}: single-call pipeline did not run")
            if profile.is_file():
                data = load(profile)
                retrieval = data.get("retrieval", {})
                row["daemon_ms"] = {
                    "embedding": retrieval.get("embedding_ms"),
                    "retrieval_including_embedding": retrieval.get("retrieval_ms"),
                    "admission": data.get("admission_ms"),
                }
                row["lane_status"] = {
                    lane: value.get("status") for lane, value in retrieval.get("lanes", {}).items()
                }
                if args.require_pipeline and data.get("count", 0):
                    ledger = work / "home/.claude/mind/outcome_ledger.jsonl"
                    events = (
                        [json.loads(line) for line in ledger.read_text().splitlines()]
                        if ledger.is_file()
                        else []
                    )
                    injected = [event for event in events if event.get("event") == "injected"]
                    expected_ms = {
                        lane: value["ms"] if args.mode == "measure" else 0
                        for lane, value in retrieval.get("lanes", {}).items()
                    }
                    expected_timeout = {
                        lane: value["timed_out"]
                        for lane, value in retrieval.get("lanes", {}).items()
                    }
                    if (
                        not injected
                        or injected[-1].get("lane_ms") != expected_ms
                        or injected[-1].get("lane_timeout") != expected_timeout
                    ):
                        failures.append(
                            f"{iteration}:{name}: lane accounting differs from daemon response"
                        )
            if reference:
                row["reference"] = reference[2]
                if reference[2]["timed_out"] or reference[2]["returncode"] != 0:
                    failures.append(f"{iteration}:{name}: reference process failed")
                if case.get("contains") and case["contains"].encode() not in reference[0]:
                    failures.append(f"{iteration}:{name}: reference missing expected output")
            rows.append(row)
            status = {key: result[key] for key in ("returncode", "timed_out")}
            outputs = {
                "stdout": stdout,
                "stderr": stderr,
                "status.json": (json.dumps(status, sort_keys=True) + "\n").encode(),
            }
            for suffix, content in outputs.items():
                (args.results / f"{iteration}-{name}.{suffix}").write_bytes(content)
                baseline = args.baseline / f"{name}.{suffix}"
                if reference:
                    ref_status = {key: reference[2][key] for key in ("returncode", "timed_out")}
                    ref_outputs = {
                        "stdout": reference[0],
                        "stderr": reference[1],
                        "status.json": (json.dumps(ref_status, sort_keys=True) + "\n").encode(),
                    }
                    (args.results / f"{iteration}-{name}.reference.{suffix}").write_bytes(
                        ref_outputs[suffix]
                    )
                    if args.mode != "measure" and content != ref_outputs[suffix]:
                        failures.append(f"{iteration}:{name}.{suffix}: paired reference mismatch")
                    continue
                if iteration < 0:
                    continue
                if args.mode == "record" and iteration == 0:
                    baseline.write_bytes(content)
                elif args.mode != "measure" and (
                    not baseline.is_file() or baseline.read_bytes() != content
                ):
                    failures.append(f"{iteration}:{name}.{suffix}")
            if result["timed_out"] or result["returncode"] != 0:
                failures.append(f"{iteration}:{name}: process failed ({result['returncode']})")
            if case.get("contains") and case["contains"].encode() not in stdout:
                failures.append(f"{iteration}:{name}: missing expected nonempty output")
            print(
                f"{name}: rc={result['returncode']} bytes={len(stdout)}/{len(stderr)} wall={result['wall_ms']} ms"
            )
    if not rows:
        parser.error("no fixtures selected")
    report = {
        "reference_hook_root": str(args.reference_hooks) if args.reference_hooks else None,
        "reference_hook_sha256": {
            p.name: hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(args.reference_hooks.glob("*.sh"))
        }
        if args.reference_hooks
        else {},
        "replica": metadata,
        "now_ms": now,
        "rows": rows,
        "failures": failures,
        "cli_sha256": hashlib.sha256(cli.read_bytes()).hexdigest(),
        "hook_root": str(args.hooks.resolve()),
        "hook_sha256": {
            p.name: hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(args.hooks.glob("*.sh"))
        },
    }
    (args.results / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    if failures:
        print("parity FAILED: " + ", ".join(failures), file=sys.stderr)
        return 1
    print(
        "timing captured"
        if args.mode == "measure"
        else f"parity byte-identical: {args.repeat} repetitions"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as exc:
        print(f"hook parity: {exc}", file=sys.stderr)
        sys.exit(2)
