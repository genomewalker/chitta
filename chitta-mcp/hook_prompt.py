"""Prompt state, local model transport, and acknowledged file updates."""

import fcntl
import json
import os
import re
import sys
import time
from pathlib import Path

from hook_client import append, read_text, setting


def clean_query(text):
    return re.sub(
        r"<(task-notification|system-reminder|command-name|command-message|local-command-\w+)[^>]*>.*?</\1>",
        "",
        text,
        flags=re.DOTALL | re.IGNORECASE,
    ).strip()


def project_realm(cwd):
    explicit = setting("REALM")
    if explicit:
        return explicit
    current = Path(cwd)
    marker = current / ".cc-soul-realm"
    value = read_text(marker).splitlines()[0] if marker.is_file() else ""
    while not value and str(current) != "/":
        git = current / ".git"
        if git.exists():
            value = "project:" + current.name
            if git.is_file():
                directory = read_text(git).removeprefix("gitdir: ")
                if "/.git/worktrees/" in directory:
                    value = "project:" + Path(directory.split("/.git/worktrees/", 1)[0]).name
            break
        current = current.parent
    match = re.match(r"^[a-z][a-z0-9_]*:[A-Za-z0-9_./-]+", value)
    return match[0] if match else "brahman"


def run(client):
    sid = (
        client.payload.get("session_id")
        or os.environ.get("CLAUDE_SESSION_ID")
        or Path(client.payload.get("transcript_path") or "").stem
        or "unknown"
    )
    if "/" in sid or sid in (".", ".."):
        raise ValueError("invalid session marker")
    client.session = sid
    client.payload["session_id"] = sid
    query = client.payload.get("prompt") or ""
    if not query:
        return
    local = {}
    for root, names in (
        (
            client.state,
            [
                ".last_user_message",
                ".last_auto_store_ts",
                *[
                    prefix + sid
                    for prefix in (
                        ".ctx_window_",
                        ".last_stop_time_",
                        ".size_warned_",
                        ".last_store_turn_",
                        ".injected_hashes_",
                    )
                ],
            ],
        ),
        (client.mind, [".session_active", ".gaps_surfaced"]),
    ):
        for name in names:
            path = root / name
            if path.exists():
                local[name] = read_text(path)
    settings = {
        name: setting(name)
        for name in (
            "MIN_QUERY_TOKENS",
            "CTX_LANE",
            "ABLATE_LANES",
            "UNKNOWN_SILENCE",
            "ANCHOR_ENFORCE",
            "ADMIT_DEBUG",
            "KW_SINGLE_TOKEN_MIN",
            "C2_SMALL_REALM",
            "C2_SMALL_REALM_MAXN",
            "C2_SMALL_REALM_MINPCT",
            "CHECKPOINT_INTERVAL",
            "CACHE_TTL_MIN",
            "STORE_INTERVAL",
            "DISCIPLINE_ENFORCE",
            "ENRICH_INTERVAL",
            "MAX_OUTPUT_CHARS",
        )
        if setting(name)
    }
    transcript = Path(client.payload.get("transcript_path") or "")
    notify = client.mind / ".msg_notify" / sid
    heartbeat = client.state / (".hb_" + sid)
    client.state.mkdir(parents=True, exist_ok=True)
    turn_path = client.state / (".turn_index_" + sid)
    turn_lock = client.state / (".turn_index_" + sid + ".lock")
    with turn_lock.open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        turn = int(read_text(turn_path) or "0")
        state = {
            "input": client.payload,
            "clean_query": clean_query(query),
            "classifier_present": (client.mind / "hook-classifier.bin").is_file(),
            "local": local,
            "settings": settings,
            "turn": turn,
            "now": client.now_ms // 1000,
            "realm": project_realm(client.payload.get("cwd") or os.getcwd()),
            "pid": os.getppid(),
            "transcript_size": transcript.stat().st_size if transcript.is_file() else 0,
            "notify": read_text(notify),
            "pin_timings": bool(setting("HOOK_NOW")),
            "remaining_ms": max(0, int((client.deadline - time.monotonic()) * 1000)),
            "lane_budget_ms": int(client.timeout * 1000),
            "heartbeat_age": client.now_ms // 1000 - int(heartbeat.stat().st_mtime)
            if heartbeat.exists()
            else 999999,
        }
        # The daemon bounds its lane wait at lane_budget_ms - 250; leave the reply
        # room to arrive instead of killing it at exactly the lane budget.
        response = client.rpc("prompt_context", {"state": state}, timeout=client.timeout + 0.75)
        plan = response["hook"]
        if not isinstance(plan, dict) or not isinstance(plan.get("queue"), list):
            raise ValueError("invalid prompt reply")
        if plan.get("skip"):
            return
        if "outcome" in plan:
            plan["outcome"]["hook_ms"] = (
                0 if setting("HOOK_NOW") else round((time.monotonic() - client.started) * 1000)
            )
        client.apply(plan)
        turn_path.write_text(str(turn + 1) + "\n")
        if plan.get("heartbeat"):
            heartbeat.touch()
        if notify.is_file():
            notify.write_text("")
    for row in plan.get("anchor_shadow", []):
        append(client.mind / ".inj_anchor_shadow.jsonl", row)
    if "hint_metric" in plan:
        append(
            client.mind / "hint_metrics.jsonl",
            dict(
                plan["hint_metric"],
                ts=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(client.now_ms / 1000)),
            ),
        )
    metrics_path = client.mind / ".hook_metrics.json"
    with (client.mind / ".hook_metrics.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        metrics = json.loads(read_text(metrics_path) or '{"turns_total":0,"turns_ingested":0}')
        metrics["turns_total"] += 1
        metrics["turns_ingested"] += 1
        metrics_path.write_text(json.dumps(metrics) + "\n")
    profile = setting("HOOK_PROFILE")
    if profile:
        response.pop("hook", None)
        Path(profile).write_text(json.dumps(response) + "\n")
    # Local inference programs own their model/process resources. The daemon
    # selects when they run; all input paths remain client-provided.
    root = Path(__file__).resolve().parent.parent
    commands = []
    thinking = Path.home() / ".claude/bin/chitta_thinking"
    if plan.get("thinking") and transcript.is_file() and os.access(thinking, os.X_OK):
        commands.append([str(thinking), "--transcript", str(transcript)])
    model = Path(
        setting("HINT_MODEL", str(Path.home() / ".claude/models/chitta-hint-qwen-q4_k_m.gguf"))
    )
    hint = root / "scripts/hint_realtime.py"
    if plan.get("hint") and transcript.is_file() and model.is_file() and hint.is_file():
        commands.append(
            [
                "timeout",
                "-k",
                "5",
                "35",
                sys.executable,
                str(hint),
                "--transcript",
                str(transcript),
                "--session",
                sid,
                "--turns",
                "5",
                "--model",
                str(model),
                "--chitta-bin",
                client.cli,
                "--mind-path",
                str(client.mind),
            ]
        )
    for command in commands:
        from hook_session import detached

        detached(command)
