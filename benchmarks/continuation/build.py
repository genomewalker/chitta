#!/usr/bin/env python3
"""Extract consecutive real-session handoffs; never synthesize missing sessions or labels."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path

DEFAULT_SOURCE = Path.home() / ".claude/projects"
DEFAULT_OUTPUT = Path("/projects/caeg/scratch/kbd606/tmp/continuation-fixture")
TRIVIAL_TOOLS = {"TodoWrite", "ToolSearch", "ExitPlanMode", "EnterPlanMode"}


def text_content(content):
    if isinstance(content, str):
        return content
    if not isinstance(content, list):
        return ""
    return "\n".join(b.get("text", "") for b in content if b.get("type") == "text")


def nontrivial(block):
    if block.get("type") != "tool_use" or block.get("name") in TRIVIAL_TOOLS:
        return False
    args = block.get("input", {})
    if block.get("name") in {"Bash", "exec_command"}:
        cmd = args.get("command", args.get("cmd", "")).strip()
        return bool(cmd) and cmd not in {
            "pwd",
            "date",
            "true",
            "ls",
            "git status",
            "git status --short",
        }
    return bool(args)


def ledger_state(value):
    """Only explicit structured next-action/capsule data, never a thread title."""
    if isinstance(value, str):
        try:
            value = json.loads(value)
        except ValueError:
            return None
    if not isinstance(value, dict):
        return None
    if isinstance(value.get("next_action"), str):
        return value
    if isinstance(value.get("next_steps"), list) and value["next_steps"]:
        if isinstance(value["next_steps"][0], str):
            return {"next_action": value["next_steps"][0]}
    for key in ("handoff", "metadata", "metadata_json", "value", "args"):
        found = ledger_state(value.get(key))
        if found is not None:
            return found
    return None


def read_transcript(path):
    raw = path.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    sessions = {}
    ledger_calls = {}
    for line in raw.splitlines():
        try:
            event = json.loads(line)
        except ValueError:
            continue
        if event.get("isSidechain") or event.get("type") not in {"user", "assistant"}:
            continue
        sid = event.get("sessionId", path.stem)
        known_ledger_calls = ledger_calls.setdefault(sid, set())
        message = event.get("message", {})
        content = message.get("content", [])
        session = sessions.setdefault(
            sid,
            {
                "id": sid,
                "transcript": str(path),
                "sha256": digest,
                "start": event.get("timestamp", ""),
                "project": str(path.parent.resolve()),
                "realm": path.parent.name,
                "first_prompt": "",
                "first_tool": None,
                "last_assistant": "",
                "last_ledger": None,
                "branch": event.get("gitBranch", ""),
                "first_branch": "",
                "cwd": event.get("cwd", ""),
            },
        )
        stamp = event.get("timestamp", "")
        if stamp and (not session["start"] or stamp < session["start"]):
            session["start"] = stamp
        explicit_realm = event.get("realm")
        if isinstance(explicit_realm, str) and explicit_realm:
            session["realm"] = explicit_realm
        visible = text_content(content).strip()
        if event.get("gitBranch"):
            session["branch"] = event["gitBranch"]
        if event["type"] == "user":
            if (
                not session["first_prompt"]
                and visible
                and not event.get("isMeta")
                and not event.get("isCompactSummary")
            ):
                session["first_prompt"] = visible
                session["first_branch"] = event.get("gitBranch", "")
        else:
            if visible:
                session["last_assistant"] = visible
            if isinstance(content, list):
                for block in content:
                    if (
                        session["first_prompt"]
                        and session["first_tool"] is None
                        and nontrivial(block)
                    ):
                        session["first_tool"] = {
                            "name": block["name"],
                            "input": block.get("input", {}),
                        }
                    tool_input = block.get("input", {})
                    is_ledger = (
                        "ledger" in block.get("name", "") or tool_input.get("tool") == "ledger_op"
                    )
                    if block.get("name") == "Bash":
                        is_ledger = bool(
                            re.search(
                                r"\bchitta\s+ledger_(op|load|save)\b", tool_input.get("command", "")
                            )
                        )
                    if block.get("type") == "tool_use" and is_ledger:
                        known_ledger_calls.add(block.get("id"))
                        found = ledger_state(block.get("input", {}))
                        if found is not None:
                            session["last_ledger"] = found
        # Tool results may carry the last ledger state. Require an explicit
        # metadata envelope so arbitrary prose/quoted plans never become labels.
        if isinstance(content, list):
            for block in content:
                if (
                    block.get("type") == "tool_result"
                    and block.get("tool_use_id") in known_ledger_calls
                ):
                    found = ledger_state(text_content(block.get("content", "")))
                    if found is not None:
                        session["last_ledger"] = found
    return list(sessions.values())


def build(source, count=20):
    # Accept one project for focused tests, or the projects root. Never recurse
    # into subagent transcripts; only direct project/session JSONL files count.
    paths = sorted({*source.glob("*.jsonl"), *source.glob("*/*.jsonl")})
    sessions = [s for p in paths for s in read_transcript(p)]
    projects = {}
    for session in sessions:
        if session["start"]:
            projects.setdefault(session["project"], []).append(session)
    pairs = []
    for project, rows in projects.items():
        rows.sort(key=lambda s: (s["start"], s["id"]))
        for previous, following in zip(rows, rows[1:]):
            if not following["first_prompt"]:
                continue
            pairs.append(
                {
                    "project": project,
                    "realm": previous["realm"],
                    "previous": {
                        k: v for k, v in previous.items() if k not in {"first_prompt", "first_tool"}
                    },
                    "next": {
                        "branch": following["first_branch"],
                        **{
                            k: v
                            for k, v in following.items()
                            if k
                            in {"id", "transcript", "sha256", "start", "first_prompt", "first_tool"}
                        },
                    },
                }
            )
    pairs.sort(key=lambda p: (p["next"]["start"], p["next"]["id"]))
    return {
        "version": 2,
        "requested": count,
        "available": len(pairs),
        "transcripts": len(paths),
        "projects": len(projects),
        "pairs": pairs[-count:],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args()
    output = args.output_dir.resolve()
    repo = Path(__file__).resolve().parents[2]
    if (
        output == repo
        or repo in output.parents
        or output == args.source.resolve()
        or args.source.resolve() in output.parents
    ):
        parser.error("fixture output must be outside the repository and transcript source")
    result = build(args.source)
    output.mkdir(parents=True, exist_ok=True)
    (output / "pairs.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"extracted {len(result['pairs'])}/20 pairs ({result['available']} available)")


if __name__ == "__main__":
    main()
