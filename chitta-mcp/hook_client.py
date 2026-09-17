#!/usr/bin/env python3
"""Hook envelopes and local I/O. Decisions and rendered context belong to chittad."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import time
from pathlib import Path

UNAVAILABLE = "[chitta] daemon unavailable; context not loaded.\n"


def setting(name, default=""):
    return os.environ.get("CHITTA_" + name, os.environ.get("CC_SOUL_" + name, default))


def read_text(path):
    try:
        return path.read_text().rstrip("\n")
    except OSError:
        return ""


def runtime_dir(mind):
    if setting("RUNTIME_LOCAL", "0") != "1":
        return mind
    value = 5381
    for byte in os.fsencode(mind.resolve()):
        value = (value * 33 + byte) & ((1 << 64) - 1)
    return Path(os.environ.get("XDG_RUNTIME_DIR", "/tmp")) / "chitta" / f"{value:016x}"


def append(path, value):
    """Keep the queue/outcome envelope local and append one complete JSONL record."""
    path.parent.mkdir(parents=True, exist_ok=True)
    data = (json.dumps(value, separators=(",", ":"), ensure_ascii=False) + "\n").encode()
    fd = os.open(path, os.O_WRONLY | os.O_APPEND | os.O_CREAT | os.O_CLOEXEC, 0o644)
    try:
        while data:
            written = os.write(fd, data)
            if written <= 0:
                raise OSError("short queue write")
            data = data[written:]
    finally:
        os.close(fd)


class Client:
    def __init__(self, payload):
        self.payload = payload
        self.exit_code = 0
        self.output = ""
        self.diagnostics = ""
        self.started = time.monotonic()
        self.deadline = self.started + float(setting("HOOK_BUDGET_MS", "6000")) / 1000
        self.mind = Path(setting("DB_PATH", str(Path.home() / ".claude/mind")))
        self.state = runtime_dir(self.mind)
        self.cli = setting("BIN", str(Path.home() / ".claude/bin/chitta"))
        self.timeout = float(setting("MAX_WAIT", "2"))
        self.session = payload.get("session_id") or "unknown"
        pinned = setting("HOOK_NOW")
        self.now_ms = (
            int(pinned) if pinned.isdigit() and len(pinned) == 13 else time.time_ns() // 1000000
        )
        self.queue_path = Path(
            setting("QUEUE", os.environ.get("CHITTA_QUEUE_PATH", str(self.state / "queue.jsonl")))
        )

    def rpc(self, tool, args, timeout=None):
        request = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {"name": tool, "arguments": args},
        }
        import signal

        proc = subprocess.Popen(
            [self.cli],
            stdin=subprocess.PIPE,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        try:
            raw, _ = proc.communicate(
                json.dumps(request),
                timeout=max(
                    0.001,
                    min(timeout if timeout else self.timeout, self.deadline - time.monotonic()),
                ),
            )
        except subprocess.TimeoutExpired:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            proc.communicate()
            raise
        if proc.returncode:
            raise subprocess.CalledProcessError(proc.returncode, [self.cli])
        response = json.loads(raw)
        if response.get("error"):
            raise ValueError("daemon rejected request")
        result = response.get("result", response)
        if result.get("isError"):
            raise ValueError("daemon rejected operation")
        return result.get("structured", result)

    def policy(self, operation, **state):
        value = self.rpc(
            "ledger_op",
            {
                "op": operation,
                "args": {
                    "input": self.payload,
                    "now": self.now_ms // 1000,
                    "cwd": os.getcwd(),
                    **state,
                },
            },
        ).get("value")
        if not isinstance(value, dict) or not isinstance(value.get("queue"), list):
            raise ValueError("invalid policy response")
        for field in ("stdout", "stderr"):
            if not isinstance(value.get(field), str):
                raise ValueError("invalid rendered output")
        return value

    def queue(self, tool, args):
        from uuid import uuid4

        if tool in {
            "remember",
            "forget",
            "import_soul",
            "learn_codebase",
            "long_task_event",
            "long_task_update",
            "msg_ack",
            "log_event",
        }:
            args = {"op": "hook_apply", "args": {"tool": tool, "args": args}}
            tool = "ledger_op"
        append(
            self.queue_path,
            {
                "ack_id": str(uuid4()),
                "tool": tool,
                "args": args,
                "ts": time.time_ns() // 1000000000,
            },
        )

    def outcome(self, event):
        target = self.state / (
            "outcome_ledger.tail"
            if setting("RUNTIME_LOCAL", "0") == "1"
            else "outcome_ledger.jsonl"
        )
        append(target, dict(event, ts=self.now_ms, session_id=self.session))

    def apply(self, plan):
        for item in plan["queue"]:
            self.queue(item["tool"], item["args"])
        for group, root in (("local", self.mind), ("state", self.state)):
            for name, value in plan.get(group, {}).items():
                if "/" in name or name in (".", ".."):
                    raise ValueError("invalid local state name")
                target = root / name
                if value is None:
                    target.unlink(missing_ok=True)
                else:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_text(value)
        for event in plan.get("shadow", []):
            target = self.mind / ".hook_shadow.jsonl"
            if target.is_file() and target.stat().st_size > 10485760:
                target.replace(self.mind / ".hook_shadow.jsonl.1")
            append(
                target,
                dict(
                    event, ts=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(self.now_ms / 1000))
                ),
            )
        notice = plan.get("notice")
        if isinstance(notice, dict):
            if "/" in notice["name"]:
                raise ValueError("invalid notice path")
            if not claim_notice(self.state / notice["name"], notice["identity"]):
                plan["stdout"] = ""
        self.exit_code = plan.get("exit_code", 0)
        if self.exit_code not in (0, 2):
            raise ValueError("invalid hook status")
        if isinstance(plan.get("outcome"), dict):
            self.outcome(plan["outcome"])
        self.output += plan["stdout"]
        self.diagnostics += plan["stderr"]


def post_tool(client):
    raw = client.payload
    if (client.mind / ".dump_bash_payload").exists():
        append(client.mind / "bash_payload_dump.jsonl", raw)
    command = raw.get("tool_input", {}).get("command", "")
    git = {}
    # Only inspect git when its command name occurs in the envelope. The daemon
    # decides whether this is a commit/milestone and how to render it.
    if "git" in command:
        for field, args in (
            ("git_branch", ["--abbrev-ref", "HEAD"]),
            ("git_commit", ["--short", "HEAD"]),
        ):
            proc = subprocess.run(
                ["git", "rev-parse", *args],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                timeout=2,
            )
            git[field] = proc.stdout.rstrip("\n") if proc.returncode == 0 else ""
    plan = client.policy(
        "hook_post_tool", local={".last_bash_cmd": read_text(client.mind / ".last_bash_cmd")}, **git
    )
    client.apply(plan)
    if plan.get("provenance"):
        # Config and file metadata are client-local. Existing provenance and
        # artifact adapters retain their bounded filesystem inspection.
        import provenance
        import task_ledger

        task_path = client.mind / ".pending_task_id"
        task = read_text(task_path)
        if "/" in task:
            raise ValueError("invalid task marker")
        if task:
            task_path.unlink(missing_ok=True)
        sid = client.session if "/" not in client.session else "unknown"
        thread = read_text(client.mind / f".current_thread_{sid}") or read_text(
            client.mind / ".current_thread_id"
        )
        before_path = client.mind / f".fs_snapshot_{task}"
        before = json.loads(read_text(before_path)) if task and before_path.is_file() else None
        output = raw.get("tool_result", {}).get("stdout", "") or raw.get("stdout", "") or ""
        data = provenance.extract(command, os.getcwd(), stdout=output, before_snapshot=before)
        if task:
            keys = "cmd cwd git_branch git_commit conda_env inputs outputs params config_files references job_id scheduler status_check_cmd completion_digest".split()
            client.queue(
                "long_task_update",
                {"task_id": task, "payload_patch": {k: data[k] for k in keys if k in data}},
            )
            for path in (data.get("outputs") or []) + (data.get("new_files") or []):
                task_ledger.artifact_register(task, path, thread_id=thread)
            before_path.unlink(missing_ok=True)


def pre_tool(client, matcher):
    import hashlib

    sid = client.session
    if "/" in sid or sid in (".", ".."):
        raise ValueError("invalid session marker")
    state = {}
    for root, prefixes in (
        (
            client.state,
            (
                ".turn_index_",
                ".saddle_",
                ".trace_cache_",
                ".read_cache_",
                ".allow_read_",
                ".enforce_",
                ".wakeup_count_",
                ".loop_count_",
            ),
        ),
        (client.mind, (".subagent_count_",)),
    ):
        for prefix in prefixes:
            name = prefix + sid
            if (root / name).exists():
                state[name] = read_text(root / name)
    turn = state.get(".turn_index_" + sid, "0")
    sentinel = ".soul_injected_" + sid + "_" + turn
    if "/" not in sentinel and (client.state / sentinel).exists():
        state[sentinel] = ""
    settings = {
        name: setting(name)
        for name in (
            "STRICT_MODE",
            "HOOK_ENFORCE",
            "ALLOW_READ",
            "DEEP_SEARCH",
            "SUBAGENT_BASH_RECALL",
            "PRETOOL_MIN_SIM",
            "FILE_TRACES",
            "AGENT_NO_FORCE",
            "AGENT_WARN",
            "AGENT_LIMIT",
            "LOOP_WARN",
            "LOOP_LIMIT",
        )
        if setting(name)
    }
    extra = {
        "matcher": matcher,
        "state": state,
        "settings": settings,
        "now_ms": client.now_ms,
        "strict_marker": (client.mind / ".strict_claude_style").exists(),
    }
    if matcher in ("Bash", "Read"):
        realm = subprocess.run(
            [client.cli, "realm_detect"],
            text=True,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            timeout=client.timeout,
            check=True,
        )
        extra["realm"] = realm.stdout.rstrip("\n")
    if matcher == "Bash":
        ledger = client.mind / "outcome_ledger.jsonl"
        extra["events"] = tail_events(ledger) if ledger.is_file() else []
    if matcher == "Read":
        path = Path(client.payload.get("tool_input", {}).get("file_path") or "")
        extra["file_exists"] = path.is_file()
        if path.is_file():
            with path.open("rb") as stream:
                extra["line_count"] = sum(
                    block.count(b"\n") for block in iter(lambda: stream.read(1048576), b"")
                )
            stamp = int(path.stat().st_mtime)
            extra["file_hash"] = hashlib.md5(f"{path}:{stamp}".encode()).hexdigest()[:16]
        shadow = client.mind / ".hook_shadow.jsonl"
        if shadow.is_file():
            with shadow.open() as stream:
                first = stream.readline()
                extra["shadow_count"] = 1 + sum(1 for _ in stream)
            try:
                from datetime import datetime

                extra["shadow_first_ts"] = int(
                    datetime.fromisoformat(
                        json.loads(first)["ts"].replace("Z", "+00:00")
                    ).timestamp()
                )
            except (ValueError, KeyError):
                pass
    plan = client.policy("hook_pre_tool", **extra)
    if plan.get("track_command"):
        from uuid import uuid4

        task = str(uuid4())
        client.mind.mkdir(parents=True, exist_ok=True)
        (client.mind / ".pending_task_id").write_text(task + "\n")
        try:
            subprocess.run(
                [
                    sys.executable,
                    "-S",
                    str(Path(__file__).with_name("provenance.py")),
                    "snapshot",
                    "--cwd",
                    os.getcwd(),
                    "--out",
                    str(client.mind / (".fs_snapshot_" + task)),
                ],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=3,
            )
        except subprocess.SubprocessError:
            pass
    client.apply(plan)
    if plan.get("write_guard"):
        guard = Path.home() / ".claude/bin/fp"
        if os.access(guard, os.X_OK):
            proc = subprocess.run(
                [str(guard), "--write-hook"],
                input=json.dumps(client.payload),
                text=True,
                timeout=client.timeout,
            )
            client.exit_code = proc.returncode


def lifecycle(client, family):
    path = Path(client.payload.get("transcript_path") or "")
    project = ""
    if client.payload.get("transcript_path"):
        for part in path.parent.name[1:].split("-"):
            candidate = project + "/" + part
            alternate = project + "-" + part
            project = (
                candidate
                if Path(candidate).is_dir()
                else alternate
                if Path(alternate).is_dir()
                else candidate
            )
    working = project if project and Path(project).is_dir() else os.getcwd()
    realm_result = subprocess.run(
        [client.cli, "realm_detect"],
        cwd=working,
        text=True,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        timeout=client.timeout,
        check=True,
    )
    state = {
        "realm": realm_result.stdout.rstrip("\n"),
        "session_id": client.payload.get("session_id") or path.stem,
        "has_transcript": path.is_file(),
        "pid": os.getppid(),
    }
    if family == "pre-compact":
        # Input projection only: preserve records, leaving extraction and
        # relevance decisions in the native compaction operation.
        records = []
        if path.is_file():
            with path.open("rb") as stream:
                size = os.fstat(stream.fileno()).st_size
                start = max(0, size - int(setting("STOP_MAX_INCREMENT_BYTES", "33554432")))
                stream.seek(start)
                if start:
                    stream.readline()
                for line in stream:
                    try:
                        row = json.loads(line)
                    except (ValueError, UnicodeError):
                        continue
                    if isinstance(row, dict):
                        records.append(row)
        state["transcript"] = records
        state["checkpoint_id"] = time.strftime(
            "compact-%Y%m%d-%H%M%S", time.localtime(client.now_ms / 1000)
        )
        operation = "hook_pre_compact"
        client.timeout = float(setting("MAX_WAIT", "15"))
    else:
        state["watch_paths"] = [
            str(Path(project) / name)
            for name in (
                "Snakefile",
                "Nextfile",
                "pyproject.toml",
                "Cargo.toml",
                "CMakeLists.txt",
                "package.json",
                "go.mod",
            )
            if project and (Path(project) / name).is_file()
        ]
        sid = client.session if "/" not in client.session else "unknown"
        state["agent_count"] = int(read_text(client.mind / f".subagent_count_{sid}") or "0")
        operation = "hook_compact_restore"
    plan = client.policy(operation, **state)
    client.apply(plan)


def tail_events(path):
    # Transport framing only. The daemon owns session/time filtering and shape
    # classification; discard partial JSONL records exactly like the reference.
    with path.open("rb") as stream:
        size = stream.seek(0, 2)
        offset = max(0, size - 1048576)
        stream.seek(offset)
        data = stream.read().split(b"\n")
    if offset:
        data = data[1:]
    rows = []
    for line in data[:-1][-4096:]:
        try:
            rows.append(json.loads(line))
        except (ValueError, UnicodeError):
            continue
    return rows


def claim_notice(path, identity):
    import fcntl

    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with path.open("a+") as handle:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
            handle.seek(0)
            if identity in handle.read().splitlines():
                return False
            handle.write(identity + "\n")
            return True
    except OSError:
        return False


def saddle_check(client):
    ledger, mind = Path(sys.argv[2]), Path(sys.argv[3])
    client.timeout = 0.3
    if not ledger.is_file():
        return 1
    episode = client.rpc(
        "ledger_op",
        {
            "op": "hook_saddle",
            "args": {
                "session_id": client.session,
                "cmd": client.payload.get("tool_input", {}).get("command", ""),
                "now_ms": client.now_ms,
                "minutes": float(sys.argv[4]),
                "min_fails": int(sys.argv[5]),
                "similarity": float(sys.argv[6]),
                "events": tail_events(ledger),
            },
        },
    ).get("value")
    if not isinstance(episode, dict):
        return 1
    if not claim_notice(runtime_dir(mind) / (".saddle_" + client.session), episode["saddle_id"]):
        return 1
    print(
        json.dumps(
            {
                "hookSpecificOutput": {
                    "hookEventName": "PreToolUse",
                    "additionalContext": episode["message"],
                }
            }
        )
    )
    return 0


def main():
    try:
        raw = sys.stdin.read()
        payload = json.loads(raw) if raw.strip() else {}
        if not isinstance(payload, dict):
            raise ValueError("invalid hook envelope")
        if setting("HEADLESS") and setting("HEADLESS") != "0":
            sys.stdout.write("{}")
            return 0
        client = Client(payload)
        if sys.argv[1] == "saddle-check":
            return saddle_check(client)
        if sys.argv[1] == "pre-tool":
            pre_tool(client, sys.argv[2])
        elif sys.argv[1] == "post-tool":
            post_tool(client)
        elif sys.argv[1] == "dream-sweep":
            from hook_maintenance import run

            run(client)
        elif sys.argv[1] == "prompt":
            from hook_prompt import run

            run(client)
        elif sys.argv[1] == "session-start":
            from hook_session import run

            run(client)
        elif sys.argv[1] in ("pre-compact", "compact-restore"):
            lifecycle(client, sys.argv[1])
        else:
            from hook_ancillary import run

            run(client, sys.argv[1], sys.argv[2:])
        sys.stdout.write(client.output)
        sys.stderr.write(client.diagnostics)
        return client.exit_code
    except (OSError, ValueError, KeyError, TypeError, AttributeError, subprocess.SubprocessError):
        sys.stdout.write(UNAVAILABLE)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
