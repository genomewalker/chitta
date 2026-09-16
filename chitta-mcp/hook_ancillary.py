"""Local file/terminal envelopes for daemon-owned ancillary hook policy."""

import hashlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path

from hook_client import append, read_text, setting


def realm(client, cwd=None):
    proc = subprocess.run(
        [client.cli, "realm_detect"],
        cwd=cwd,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        timeout=client.timeout,
        check=True,
    )
    return proc.stdout.rstrip("\n")


def terminal(args):
    return subprocess.run(
        ["zellij", *args],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        timeout=2,
        check=True,
    ).stdout


def run(client, family, args):
    state = {"family": family}
    if family == "post-commit":
        root = Path(__file__).resolve().parent.parent
        bootstrap = root / "bootstrap/cc-soul.soul"
        changed = subprocess.run(
            ["git", "-C", str(root), "diff-tree", "--no-commit-id", "--name-only", "-r", "HEAD"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=client.timeout,
            check=True,
        ).stdout
        state.update(
            bootstrap_exists=bootstrap.is_file(),
            bootstrap_file=str(bootstrap),
            changed_files=changed,
        )
    elif family == "memory-intercept":
        state["timestamp"] = time.strftime("%Y-%m-%d %H:%M", time.localtime(client.now_ms / 1000))
        prepared = client.policy("hook_ancillary", **state)
        if not prepared.get("memory_sync"):
            return
        client.apply(prepared)
        if "import" in prepared:
            client.rpc("remember", prepared["import"])
            client.diagnostics += prepared["imported_notice"]
        state["render"] = True
    elif family == "persona":
        state.update(task=args[0] if args else "", persona_realm=args[1] if len(args) > 1 else "")
    elif family == "run-ledger":
        from datetime import datetime
        from uuid import uuid4

        client.payload["session_id"] = (
            os.environ.get("CLAUDE_SESSION_ID") or client.payload.get("session_id") or str(uuid4())
        )
        path = Path(client.payload.get("transcript_path") or "")
        state.update(
            transcript=read_text(path) if path.is_file() else "",
            genome=setting("GENOME_ID", "none"),
            has_entries=bool(args and args[0] == "has-entries"),
            timestamp=datetime.fromtimestamp(client.now_ms / 1000)
            .astimezone()
            .isoformat(timespec="seconds"),
        )
    elif family == "span-capture":
        if not args or not Path(args[0]).is_file():
            return
        snapshot = json.loads(Path(args[0]).read_text())
        if not isinstance(snapshot, dict) or snapshot.get("format") != "cc-soul-stop-snapshot-v1":
            raise ValueError("span capture requires the bounded Stop snapshot")
        state.update(snapshot=snapshot, last_user=args[1] if len(args) > 1 else "")
        # Existing files can contain prior records at the same pinned second.
        sid = snapshot.get("session_id", "unknown")
        if "/" not in sid:
            path = client.mind / "spans" / f"{sid}-{client.now_ms // 1000}.jsonl"
            if path.is_file():
                state["prior_count"] = len(path.read_bytes().splitlines())
    elif family == "subagent-stop":
        if not client.payload.get("agent_id"):
            return
        state.update(
            realm=realm(client),
            transcript_exists=Path(client.payload.get("agent_transcript_path") or "").is_file(),
        )
    elif family in ("file-changed", "codebase-learn"):
        raw = (
            client.payload.get("file_path")
            if family == "file-changed"
            else client.payload.get("tool_input", {}).get("file_path")
        )
        if not raw:
            return
        path = Path(raw)
        state.update(
            realm=realm(client, path.parent if family == "file-changed" else None),
            file_exists=path.is_file(),
        )
        stamp_name = ".reindex_" + hashlib.md5((str(path.parent) + "\n").encode()).hexdigest()[:16]
        stamp_path = client.mind / stamp_name
        state.update(
            last_index=int(read_text(stamp_path) or "0"),
            rate_limit=int(setting("REINDEX_RATE_LIMIT", "300")),
        )
    elif family == "bash-history":
        state["command"] = (
            args[0] if args else client.payload.get("tool_input", {}).get("command", "")
        )
    elif family == "shepherd-stop":
        path = client.mind / ".shepherd_active"
        if not path.is_file():
            return
        config = json.loads(path.read_text())
        if not config.get("task_id"):
            return
        pane = config.get("pane_name", "pipeline-main")
        session = config.get("session") or os.environ.get("ZELLIJ_SESSION", "zellij-agent")
        layout = terminal(["-s", session, "action", "dump-layout"])
        if json.dumps(pane) not in layout:
            return
        state.update(
            pane_name=pane,
            pane_tail="\n".join(
                terminal(
                    ["-s", session, "action", "dump-screen", "/dev/stdout", "--full"]
                ).splitlines()[-5:]
            ),
        )
    elif family == "shepherd":
        unknown = set(args) - {"-v", "--verbose", "-n", "--dry-run"}
        if unknown:
            raise ValueError("unknown shepherd option")
        session = os.environ.get("ZELLIJ_SESSION", "zellij-agent")
        if not any(
            line.startswith(session) for line in terminal(["list-sessions", "-n"]).splitlines()
        ):
            return
        rows = client.policy("hook_ancillary", family="shepherd", list_tasks=True)["tasks"]
        layout = terminal(["-s", session, "action", "dump-layout"])
        panes = []
        for task in rows:
            task_id = task.get("task_id", "")
            if not task_id.startswith("shepherd-"):
                continue
            # The pane reference is transport addressing, not workflow policy.
            import re

            found = re.search(r"pane:([^\s,]+)", str(task.get("work_items", "")))
            if found and json.dumps(found[1]) in layout:
                panes.append(
                    {
                        "task_id": task_id,
                        "text": terminal(
                            ["-s", session, "action", "dump-screen", "/dev/stdout", "--full"]
                        ),
                    }
                )
        state["panes"] = panes
    plan = client.policy("hook_ancillary", **state)
    dry = family == "shepherd" and (
        set(args) & {"-n", "--dry-run"} or os.environ.get("SHEPHERD_DRY_RUN") == "true"
    )
    if dry:
        print(json.dumps(plan), file=sys.stderr)
        return
    client.apply(plan)
    if family == "memory-intercept" and "memory_file" in plan:
        Path(client.payload["tool_input"]["file_path"]).write_text(plan["memory_file"])
    if family == "span-capture":
        name = plan["span_name"]
        if "/" in name:
            raise ValueError("invalid span path")
        for record in plan["spans"]:
            append(client.mind / "spans" / name, record)
    if "index_stamp" in plan:
        stamp_path.parent.mkdir(parents=True, exist_ok=True)
        stamp_path.write_text(str(plan["index_stamp"]) + "\n")
    if "log_event" in plan:
        client.rpc("log_event", plan["log_event"])
    if "history" in plan:
        with (Path.home() / ".claude_bash_history").open("a") as stream:
            stream.write(
                time.strftime("# %Y-%m-%d %H:%M:%S\n", time.localtime(client.now_ms / 1000))
                + plan["history"]
                + "\n"
            )
    for alert in plan.get("alerts", []):
        client.rpc("msg_send", alert)
    if family == "shepherd":
        subprocess.run(
            [sys.executable, str(Path(__file__).with_name("poller.py")), "poll_once"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            timeout=30,
        )
