#!/usr/bin/env python3
"""Replay one UTC day of Bash results through CLI-local output_cap."""

import argparse
import json
import os
import subprocess
from pathlib import Path


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("transcript", type=Path)
    ap.add_argument("--day", required=True)
    ap.add_argument("--state", type=Path, required=True)
    ap.add_argument("--cli", type=Path, default=Path("bin/chitta").resolve())
    args = ap.parse_args()
    env = dict(os.environ, CHITTA_DB_PATH=str(args.state), CHITTA_RUNTIME_LOCAL="0")
    commands = {}
    count = capped = original = projected = unchanged = 0
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
                raw = text.encode()
                result = subprocess.run(
                    [str(args.cli), "output_cap"],
                    input=raw,
                    capture_output=True,
                    check=True,
                    env=env,
                ).stdout
                count += 1
                original += len(text)
                projected += len(result.decode())
                if result == raw:
                    unchanged += 1
                else:
                    capped += 1
                    ref = result.decode().split("§")[1].removeprefix("ref:")
                    restored = subprocess.run(
                        [str(args.cli), "output_ref", "--hash", ref],
                        capture_output=True,
                        check=True,
                        env=env,
                    ).stdout
                    assert restored == raw
                if len(text) <= int(env.get("CHITTA_OUTPUT_CAP_CHARS", "6000")):
                    assert result == raw
    print(
        json.dumps(
            dict(
                day=args.day,
                bash_outputs=count,
                capped=capped,
                raw_chars=original,
                thread_chars=projected,
                unchanged=unchanged,
                reduction_pct=round(100 * (1 - projected / original), 2),
                round_trip="PASS",
            )
        )
    )


if __name__ == "__main__":
    main()
