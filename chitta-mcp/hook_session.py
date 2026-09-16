"""SessionStart filesystem/process envelope; all context policy lives in chittad."""

import json
import os
import shlex
import subprocess
import time
from pathlib import Path

from hook_ancillary import realm
from hook_client import read_text, setting


def project_dir(payload):
    explicit = payload.get("cwd") or payload.get("project_dir")
    if explicit:
        return str(Path(explicit).resolve()) if Path(explicit).is_dir() else explicit
    transcript = payload.get("transcript_path")
    if not transcript:
        return ""
    path = ""
    for part in Path(transcript).parent.name[1:].split("-"):
        candidate, alternate = path + "/" + part, path + "-" + part
        path = (
            candidate
            if Path(candidate).is_dir()
            else alternate
            if Path(alternate).is_dir()
            else candidate
        )
    return path


def detached(command, cwd=None):
    return subprocess.Popen(
        command,
        cwd=cwd,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        # Match shell background jobs: the invoking frontend owns this process group.
        start_new_session=False,
    )


def run(client):
    sid = client.payload.get("session_id", "")
    if "/" in sid or sid in (".", ".."):
        raise ValueError("invalid session marker")
    project = project_dir(client.payload)
    exists = bool(project) and Path(project).is_dir()
    branch = ""
    if exists:
        for args in (
            ["symbolic-ref", "--quiet", "--short", "HEAD"],
            ["rev-parse", "--short", "HEAD"],
        ):
            result = subprocess.run(
                ["git", "-C", project, *args],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
                timeout=2,
            )
            if result.returncode == 0:
                branch = result.stdout.rstrip("\n")
                break
    session_realm = realm(client, project if exists else None)
    marker = client.mind / ".pending_subagent_start"
    age = client.now_ms // 1000 - int(marker.stat().st_mtime) if marker.exists() else 999
    if client.payload.get("source", "startup") == "startup":
        marker.unlink(missing_ok=True)
    failed_path = client.mind / ".failed_observations.jsonl"
    failed = []
    if failed_path.is_file():
        for line in failed_path.read_text().splitlines():
            try:
                value = json.loads(line)
                if isinstance(value, dict):
                    failed.append(value)
            except ValueError:
                continue
    memory = Path.home() / ".claude/projects" / project.replace("/", "-") / "memory/MEMORY.md"

    plan = client.policy(
        "hook_session_start",
        realm=session_realm,
        project_dir=project,
        project_exists=exists,
        branch=branch,
        has_transcript=Path(client.payload.get("transcript_path") or "").is_file(),
        pid=os.getppid(),
        host=os.environ.get("HOSTNAME", ""),
        model=setting("MODEL"),
        subagent_age=age,
        strict_default=setting("STRICT_MODE_DEFAULT", "1"),
        utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(client.now_ms / 1000)),
        turn=read_text(client.state / (".turn_index_" + sid)) or "0",
        local={".correction_surfaces": read_text(client.mind / ".correction_surfaces")},
        failed_observations=failed,
        memory_file=read_text(memory),
    )
    if plan.get("reset_patterns"):
        for root, pattern in ((client.mind, ".stop_dedup_*"), (client.state, ".size_warned_*")):
            for path in root.glob(pattern):
                path.unlink(missing_ok=True)
    client.apply(plan)
    profile = setting("LEDGER_PROFILE")
    if profile and "ledger_profile" in plan:
        Path(profile).write_text(json.dumps(plan["ledger_profile"]) + "\n")
    if plan.get("retry_ack"):
        failed_path.unlink(missing_ok=True)
    if sid:
        env_file = client.mind / f".session_env_{os.getpid()}"
        env_file.parent.mkdir(parents=True, exist_ok=True)
        env_file.write_text(
            "".join(
                f"export {key}={shlex.quote(str(value))}\n"
                for key, value in {
                    "CLAUDE_SESSION_ID": sid,
                    "CLAUDE_TRANSCRIPT_PATH": client.payload.get("transcript_path", ""),
                    "CLAUDE_REALM": session_realm,
                    "CLAUDE_PID": os.getppid(),
                }.items()
            )
        )
        env_file.chmod(0o600)
    if not plan.get("maintenance"):
        return
    root = Path(setting("PLUGIN_DIR", str(Path(__file__).resolve().parent.parent)))
    for name, cwd in (("auto-index.sh", project if exists else None), ("realm-retag.sh", None)):
        script = root / "scripts" / name
        if script.is_file() and os.access(script, os.X_OK) and (name != "auto-index.sh" or exists):
            detached([str(script)], cwd)
    staging = client.mind / ".distill_staging"
    for path in staging.glob("*.json"):
        (staging / "archive").mkdir(exist_ok=True)
        path.replace(staging / "archive" / path.name)
    notify = root / "scripts/msg-notify.sh"
    if sid and sid != "default" and notify.is_file():
        pid_file = client.mind / ".msg_notify_pids" / (sid + ".pid")
        old = read_text(pid_file)
        if old.isdigit():
            try:
                command = Path(f"/proc/{old}/cmdline").read_bytes().split(b"\0")
                if (
                    any(part.endswith(b"/msg-notify.sh") for part in command)
                    and os.fsencode(sid) in command
                ):
                    import signal

                    os.kill(int(old), signal.SIGTERM)
            except OSError:
                pass
        detached(["bash", str(notify), sid, "5"])
