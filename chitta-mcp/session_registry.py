#!/usr/bin/env python3
"""Shared Claude/Codex session registration and liveness adapter."""

from __future__ import annotations

import argparse
import json
import os
import socket
import subprocess
import sys
import time
from collections.abc import Iterator
from pathlib import Path
from typing import Any
from uuid import uuid4

sys.path.insert(0, str(Path(__file__).parent))
from task_ledger import (  # noqa: E402
    lease_claim,
    lease_list,
    session_bind,
    session_close,
    session_get,
    session_touch,
)
from task_ledger import (
    session_list as ledger_session_list,
)

CHITTA_BIN = os.environ.get("CHITTA_BIN", str(Path.home() / ".claude" / "bin" / "chitta"))
_MIND = Path(os.environ.get("MIND_PATH") or os.environ.get("CHITTA_DB_PATH") or Path.home() / ".claude" / "mind")
CHITTA_QUEUE = Path(os.environ.get("CHITTA_QUEUE") or os.environ.get("CHITTA_QUEUE_PATH") or _MIND / "queue.jsonl")


def _read_json_stdin() -> dict[str, Any]:
    try:
        raw = sys.stdin.read()
        value = json.loads(raw) if raw.strip() else {}
        return value if isinstance(value, dict) else {}
    except (OSError, json.JSONDecodeError):
        return {}


def _run_chitta(
    args: list[str], cwd: str | None = None, timeout_seconds: float = 2.0
) -> dict[str, Any] | None:
    try:
        proc = subprocess.run(
            [CHITTA_BIN, *args, "--json"],
            cwd=cwd or None,
            capture_output=True,
            text=True,
            timeout=timeout_seconds,
        )
        if proc.returncode == 0 and proc.stdout.strip():
            value = json.loads(proc.stdout)
            return value if isinstance(value, dict) else {"items": value}
    except (OSError, subprocess.SubprocessError, json.JSONDecodeError):
        pass
    return None


def _latest(paths: list[Path]) -> str:
    existing = [p for p in paths if p.is_file()]
    if not existing:
        return ""
    try:
        return str(max(existing, key=lambda p: p.stat().st_mtime))
    except OSError:
        return str(existing[0])


def _claude_candidates(session_id: str) -> list[Path]:
    return list((Path.home() / ".claude" / "projects").glob(f"*/{session_id}.jsonl"))


def _codex_candidates(session_id: str) -> list[Path]:
    root = Path.home() / ".codex" / "sessions"
    return list(root.rglob(f"*{session_id}.jsonl")) if root.is_dir() else []


def resolve_transcript_path(session_id: str, client: str, supplied: str = "") -> str:
    if supplied and Path(supplied).is_file():
        return str(Path(supplied).resolve())
    if client == "claude":
        return _latest(_claude_candidates(session_id))
    if client == "codex":
        return _latest(_codex_candidates(session_id))
    return _latest(_claude_candidates(session_id) + _codex_candidates(session_id))


def _cwd_from_codex_transcript(path: str) -> str:
    if not path:
        return ""
    try:
        with open(path, encoding="utf-8") as handle:
            first = json.loads(handle.readline())
        if first.get("type") == "session_meta":
            return str(first.get("payload", {}).get("cwd", ""))
    except (OSError, json.JSONDecodeError, AttributeError):
        pass
    return ""


def iter_ancestor_pids(max_depth: int = 32) -> Iterator[int]:
    """Yield this process's ancestor PIDs, nearest first, starting at the parent.

    Reads the fourth field of /proc/<pid>/stat, skipping past the comm field —
    a process name may itself contain spaces or parentheses, so the parse starts
    after the LAST ')'. Stops at init, at a cycle, at max_depth, or as soon as
    /proc stops answering.
    """
    seen: set[int] = set()
    pid = os.getppid()
    for _ in range(max_depth):
        if pid <= 1 or pid in seen:
            return
        seen.add(pid)
        yield pid
        try:
            stat = Path(f"/proc/{pid}/stat").read_text(encoding="utf-8")
            pid = int(stat[stat.rfind(")") + 2 :].split()[1])
        except (OSError, ValueError, IndexError):
            return


def _ancestor_client_pid(client: str) -> int:
    """Find the long-lived Claude/Codex ancestor instead of the hook subprocess."""
    wanted = "claude" if client == "claude" else "codex"
    for pid in iter_ancestor_pids():
        try:
            cmdline = (
                Path(f"/proc/{pid}/cmdline")
                .read_bytes()
                .replace(b"\0", b" ")
                .decode("utf-8", "replace")
                .lower()
            )
        except OSError:
            # Process exited mid-walk; the chain above it is no longer useful.
            break
        if wanted in cmdline and "session_registry.py" not in cmdline:
            return pid
    return os.getppid()


def _detect_realm(project_dir: str) -> str:
    result = _run_chitta(["realm_detect"], cwd=project_dir or None)
    if result:
        for key in ("realm", "detected_realm", "project"):
            if result.get(key):
                return str(result[key])
    name = Path(project_dir).name if project_dir else ""
    return f"project:{name}" if name else "brahman"


def register(input_data: dict[str, Any], client: str) -> dict[str, Any]:
    session_id = str(input_data.get("session_id") or "")
    if not session_id:
        return {"registered": False, "reason": "missing_session_id"}
    supplied_path = str(input_data.get("transcript_path") or "")
    transcript_path = resolve_transcript_path(session_id, client, supplied_path)
    project_dir = str(input_data.get("cwd") or input_data.get("project_dir") or "")
    if not project_dir and client == "codex":
        project_dir = _cwd_from_codex_transcript(transcript_path)
    if not project_dir:
        project_dir = os.getcwd()
    try:
        project_dir = str(Path(project_dir).resolve())
    except OSError:
        pass

    prior = session_get(session_id) or {}
    thread_id = str(input_data.get("thread_id") or prior.get("thread_id") or "")
    model = str(
        input_data.get("model")
        or os.environ.get("CHITTA_MODEL")
        or os.environ.get("CC_SOUL_MODEL", "")
    )
    host = socket.gethostname()
    metadata = {
        "client": client,
        "model": model,
        "host": host,
        "thread_id": thread_id,
        "hook_source": str(input_data.get("source") or input_data.get("hook_event_name") or ""),
    }
    realm = str(input_data.get("realm") or _detect_realm(project_dir))
    pid = _ancestor_client_pid(client)
    queue_args = {
        "session_id": session_id,
        "realm": realm,
        "pid": pid,
        "project_dir": project_dir,
        "transcript_path": transcript_path,
        "client": client,
        "kind": client,
        "metadata": metadata,
    }
    args = [
        "session_register",
        "--session_id",
        session_id,
        "--realm",
        realm,
        "--pid",
        str(pid),
        "--project_dir",
        project_dir,
        "--metadata",
        json.dumps(metadata, separators=(",", ":")),
    ]
    if transcript_path:
        args.extend(["--transcript_path", transcript_path])
    daemon_result = _run_chitta(args, cwd=project_dir)
    daemon_queued = daemon_result is None and _queue_tool("session_register", queue_args)
    transcript_result = None
    transcript_queued = False
    if transcript_path:
        transcript_result = _run_chitta(
            [
                "transcript_register",
                "--session_id",
                session_id,
                "--transcript_path",
                transcript_path,
                "--realm",
                realm,
            ],
            cwd=project_dir,
        )
        if transcript_result is None:
            transcript_queued = _queue_tool(
                "transcript_register",
                {
                    "session_id": session_id,
                    "transcript_path": transcript_path,
                    "realm": realm,
                },
            )
    session_bind(
        session_id=session_id,
        thread_id=thread_id or None,
        client=client,
        project_dir=project_dir,
        transcript_path=transcript_path,
        metadata=metadata,
    )
    lease = lease_claim(thread_id, session_id) if thread_id else None
    return {
        "registered": daemon_result is not None or daemon_queued,
        "registration_mode": "direct"
        if daemon_result is not None
        else ("queued" if daemon_queued else "failed"),
        "session_id": session_id,
        "client": client,
        "model": model,
        "realm": realm,
        "pid": pid,
        "project_dir": project_dir,
        "transcript_path": transcript_path,
        "thread_id": thread_id,
        "lease": lease,
        "transcript_registered": transcript_result is not None or transcript_queued,
        "transcript_registration_mode": "direct"
        if transcript_result is not None
        else ("queued" if transcript_queued else "failed"),
    }


def _queue_tool(tool: str, args: dict[str, Any]) -> bool:
    item = {
        "ack_id": str(uuid4()),
        "tool": tool,
        "args": args,
        "ts": int(time.time() * 1000),
    }
    try:
        # One compact O_APPEND write keeps the hook path independent of daemon
        # RPC latency while retaining the durable queue's normal semantics.
        line = json.dumps(item, separators=(",", ":")) + "\n"
        fd = os.open(CHITTA_QUEUE, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
        try:
            os.write(fd, line.encode("utf-8"))
        finally:
            os.close(fd)
        return True
    except OSError:
        return False


def _queue_heartbeat(session_id: str, metadata: dict[str, Any]) -> bool:
    return _queue_tool(
        "session_heartbeat",
        {"session_id": session_id, "metadata": metadata},
    )


def heartbeat(input_data: dict[str, Any], queued: bool = False) -> dict[str, Any]:
    session_id = str(input_data.get("session_id") or "")
    if not session_id:
        return {"heartbeat": False, "reason": "missing_session_id"}
    row = session_get(session_id) or {}
    metadata = {
        "thread_id": str(row.get("thread_id") or ""),
        "client": str(row.get("client") or ""),
    }
    if queued:
        heartbeat_ok = _queue_heartbeat(session_id, metadata)
    else:
        heartbeat_ok = (
            _run_chitta(
                [
                    "session_heartbeat",
                    "--session_id",
                    session_id,
                    "--metadata",
                    json.dumps(metadata, separators=(",", ":")),
                ]
            )
            is not None
        )
    session_touch(session_id)
    return {"heartbeat": heartbeat_ok, "queued": queued, "session_id": session_id, **metadata}


def close(input_data: dict[str, Any]) -> dict[str, Any]:
    session_id = str(input_data.get("session_id") or "")
    if not session_id:
        return {"closed": False, "reason": "missing_session_id"}
    result = _run_chitta(["session_deregister", "--session_id", session_id])
    queued = result is None and _queue_tool(
        "session_deregister",
        {"session_id": session_id},
    )
    session_close(session_id, str(input_data.get("status") or "ended"))
    return {
        "closed": result is not None or queued,
        "mode": "direct" if result is not None else ("queued" if queued else "failed"),
        "session_id": session_id,
    }


def inventory() -> dict[str, Any]:
    session_result = _run_chitta(["session_list", "--status", "all"])
    transcript_result = _run_chitta(["transcript_list"])
    sessions = session_result or {}
    transcripts = transcript_result or {}
    return {
        "session_registry_available": session_result is not None,
        "transcript_registry_available": transcript_result is not None,
        "sessions": sessions.get("sessions", []),
        "transcripts": transcripts.get("transcripts", []),
        "thread_sessions": ledger_session_list(limit=200),
        "thread_leases": lease_list(active_only=False),
    }


def _cli() -> None:
    parser = argparse.ArgumentParser(prog="session_registry")
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("register")
    p.add_argument("--client", choices=("claude", "codex"), required=True)
    heartbeat_parser = sub.add_parser("heartbeat")
    heartbeat_parser.add_argument("--queued", action="store_true")
    sub.add_parser("close")
    sub.add_parser("inventory")
    args = parser.parse_args()

    input_data = {} if args.cmd == "inventory" else _read_json_stdin()
    if args.cmd == "register":
        result = register(input_data, args.client)
    elif args.cmd == "heartbeat":
        result = heartbeat(input_data, args.queued)
    elif args.cmd == "close":
        result = close(input_data)
    else:
        result = inventory()
    print(json.dumps(result, default=str))


if __name__ == "__main__":
    _cli()
