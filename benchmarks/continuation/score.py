#!/usr/bin/env python3
"""Strict retrospective continuation scoring; previous-session data is the only predictor input."""

from __future__ import annotations

import argparse
import json
import re
import shlex
import subprocess
from pathlib import Path

RULE = (
    "The capsule next_action must name an exact target path or the complete command of "
    "the next session's first non-trivial tool call. Tool-name matches count only with "
    "the same exact target path. Basenames, paraphrases and semantic similarity do not count. "
    "Artifacts/branch fields alone do not count. Missing actions or tool calls are misses. "
    "A known branch mismatch is a miss because SessionStart suppresses that capsule."
)


def capsule(previous):
    # Execute the production selector, without sourcing/running the Stop hook.
    hook = Path(__file__).resolve().parents[2] / "hooks/stop-core.sh"
    source = hook.read_text()
    selector = re.search(
        r"action=\$\(printf '%s\\n' \"\$visible\" \| awk '(.*?)' \| head -c 400\)", source, re.S
    )
    if selector is None:
        raise ValueError("production handoff selector not found; refusing a divergent predictor")
    result = subprocess.run(
        ["awk", selector.group(1)],
        input=previous["last_assistant"] + "\n",
        text=True,
        capture_output=True,
        check=True,
    )
    action = result.stdout.strip().encode()[:400].decode(errors="ignore")
    if action:
        return {"next_action": action, "source": "production-visible-plan"}
    ledger = previous.get("last_ledger") or {}
    if ledger.get("verified", True) and isinstance(ledger.get("next_action"), str):
        return {"next_action": ledger["next_action"][:400], "source": "recorded-ledger"}
    return {"next_action": "", "source": "no-explicit-action"}


def exact_mention(text, literal):
    return (
        bool(literal)
        and re.search(r"(?<![\w./-])" + re.escape(literal) + r"(?![\w./-])", text) is not None
    )


def score(candidate, following):
    action = candidate.get("next_action", "")
    tool = following.get("first_tool") or {}
    args = tool.get("input", {})
    if not action or not tool:
        return False
    command = args.get("command", args.get("cmd", ""))
    if isinstance(command, str) and command and exact_mention(action, command):
        return True
    if isinstance(command, str) and command:
        try:
            tokens = shlex.split(command)
        except ValueError:
            tokens = []
        # Literal file arguments also satisfy the exact-file rule. Do not turn
        # shell code, URLs, variables or a generic interpreter into target files.
        for index, token in enumerate(tokens):
            if token.startswith("--") and "=" in token:
                token = token.split("=", 1)[1]
            if index == 0 and not token.endswith((".py", ".sh", ".rb", ".pl")):
                continue
            if re.fullmatch(r"(?:[./\w -]+/)?[\w -]+\.[\w.-]+", token) and exact_mention(
                action, token
            ):
                return True
    for key in ("file_path", "path", "target_path", "notebook_path"):
        target = args.get(key)
        if (
            isinstance(target, str)
            and ("/" in target or "." in target)
            and exact_mention(action, target)
        ):
            return True
    return False


def evaluate(fixture):
    rows = []
    for pair in fixture["pairs"]:
        candidate = capsule(pair["previous"])
        previous_branch = pair["previous"].get("branch", "")
        next_branch = pair["next"].get("branch", "")
        if not candidate["next_action"]:
            reason = "missing plan line"
        elif previous_branch and next_branch and previous_branch != next_branch:
            reason = "wrong branch"
        elif not pair["next"].get("first_tool"):
            reason = "missing non-trivial tool call"
        elif not score(candidate, pair["next"]):
            reason = "unrelated next action"
        else:
            reason = "exact action match"
        rows.append(
            {
                "previous_id": pair["previous"]["id"],
                "next_id": pair["next"]["id"],
                "capsule": candidate,
                "project": pair.get("project", ""),
                "previous_branch": previous_branch,
                "next_branch": next_branch,
                "correct": reason == "exact action match",
                "reason": reason,
            }
        )
    hits = sum(row["correct"] for row in rows)
    return {
        "rule": RULE,
        "correct": hits,
        "cases": len(rows),
        "required_cases": 20,
        "gate": len(rows) == 20 and hits >= 18,
        "mode": "retrospective-production-selector-replay",
        "rows": rows,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fixture", type=Path)
    args = parser.parse_args()
    print(json.dumps(evaluate(json.loads(args.fixture.read_text())), indent=2))


if __name__ == "__main__":
    main()
