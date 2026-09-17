#!/usr/bin/env python3
"""Replay one UTC day of Claude Bash results through the real post-tool envelope.

The policy transport is replaced with an empty plan: this measures local summary
bytes, with no daemon writes. Raw frontend tool results cannot be rewritten.
"""

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "chitta-mcp"))
from hook_client import post_tool


class ReplayClient:
    def __init__(self, payload, state):
        self.payload, self.state, self.mind = payload, state, state
        self.output = ""

    def policy(self, *args, **kwargs):
        return {"stdout": ""}

    def apply(self, plan):
        self.output = plan["stdout"]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("transcript", type=Path)
    ap.add_argument("--day", required=True)
    ap.add_argument("--state", type=Path, required=True)
    args = ap.parse_args()
    commands = {}
    count = capped = original = projected = summary_chars = 0
    with args.transcript.open() as stream:
        for line in stream:
            try:
                row = json.loads(line)
            except ValueError:
                continue
            content = row.get("message", {}).get("content", [])
            if not isinstance(content, list):
                continue
            for block in content:
                if not isinstance(block, dict):
                    continue
                if block.get("type") == "tool_use" and block.get("name") == "Bash":
                    commands[block["id"]] = block.get("input", {}).get("command", "")
                if (
                    block.get("type") != "tool_result"
                    or block.get("tool_use_id") not in commands
                    or not row.get("timestamp", "").startswith(args.day)
                ):
                    continue
                text = block.get("content", "")
                if isinstance(text, list):
                    text = "\n".join(v.get("text", "") for v in text if isinstance(v, dict))
                if not isinstance(text, str):
                    continue
                client = ReplayClient(
                    {
                        "tool_name": "Bash",
                        "tool_input": {"command": "replay"},
                        "tool_response": text,
                    },
                    args.state,
                )
                post_tool(client)
                count += 1
                original += len(text)
                summary = (
                    json.loads(client.output)["hookSpecificOutput"]["additionalContext"]
                    if client.output
                    else ""
                )
                projected += len(summary) if summary else len(text)
                summary_chars += len(summary)
                capped += bool(summary)
    print(
        json.dumps(
            dict(
                day=args.day,
                bash_outputs=count,
                capped=capped,
                raw_chars=original,
                replacement_projection_chars=projected,
                summary_chars=summary_chars,
                actual_raw_plus_context_chars=original + summary_chars,
            )
        )
    )


if __name__ == "__main__":
    main()
