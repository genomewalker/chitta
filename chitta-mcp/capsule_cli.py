#!/usr/bin/env python3
"""Explicit milestone checkpoints through the daemon's durable capsule gateway."""

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path


def ledger(op, args):
    reply = subprocess.run(
        [
            os.environ.get("CHITTA_BIN", "chitta"),
            "ledger_op",
            "--op",
            op,
            "--args",
            json.dumps(args),
            "--json",
        ],
        stdin=subprocess.DEVNULL,
        capture_output=True,
        text=True,
        timeout=20,
        check=True,
    )
    value = json.loads(reply.stdout)
    if "value" not in value:
        raise ValueError("daemon did not acknowledge capsule operation")
    return value["value"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    save = commands.add_parser("save")
    save.add_argument("file", type=Path, help="Explicit capsule fields as JSON")
    save.add_argument("--expected-revision", type=int, required=True)
    args = parser.parse_args()
    try:
        capsule = json.loads(args.file.read_text())
        print(
            json.dumps(
                ledger(
                    "capsule_save",
                    {
                        "capsule": capsule,
                        "expected_revision": args.expected_revision,
                    },
                ),
                separators=(",", ":"),
            )
        )
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        print(f"capsule save failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
