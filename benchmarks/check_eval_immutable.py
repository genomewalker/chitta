#!/usr/bin/env python3
"""Compare immutable eval paths using the union of base and head policies."""

from __future__ import annotations

import argparse
import re
import subprocess

MANIFEST = "benchmarks/EVAL_IMMUTABLE.txt"
CONTROL_PATHS = [
    MANIFEST,
    "benchmarks/check_eval_immutable.py",
    "scripts/check-eval-immutable.sh",
    ".github/workflows/ci.yml",
]


def git(*args: str) -> bytes:
    return subprocess.check_output(["git", *args])


def check(base: str, head: str) -> int:
    base = git("rev-parse", "--verify", base + "^{commit}").decode().strip()
    head = git("rev-parse", "--verify", head + "^{commit}").decode().strip()
    patterns = set(CONTROL_PATHS)
    found = False
    for ref in (base, head):
        if not git("ls-tree", "--name-only", ref, "--", MANIFEST).strip():
            continue
        found = True
        for line in git("show", f"{ref}:{MANIFEST}").decode().splitlines():
            line = line.strip()
            if line and not line.startswith("#"):
                if line.startswith(("/", ":", "!")) or ".." in line.split("/"):
                    raise ValueError(f"invalid immutable pattern: {line}")
                patterns.add(line)
    if not found:
        raise ValueError("immutable manifest absent from both refs")
    changed = git(
        "diff",
        "--no-renames",
        "--name-only",
        "-z",
        base,
        head,
        "--",
        *(f":(top,glob){p}" for p in sorted(patterns)),
    )
    if not changed:
        print("eval-immutable: PASS")
        return 0
    message = git("show", "-s", "--format=%B", head).decode()
    approved = re.search(r"^Eval-Change-Approved: ([^\s].*)$", message, re.MULTILINE)
    print("eval-immutable: protected paths changed:")
    for path in changed.decode().strip("\0").split("\0"):
        print(f"  {path}")
    if approved and approved.group(1).strip():
        print(f"eval-immutable: approved by {approved.group(1).strip()}")
        return 0
    print("eval-immutable: FAIL; head commit needs Eval-Change-Approved: <name>")
    return 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base")
    parser.add_argument("head")
    args = parser.parse_args()
    try:
        raise SystemExit(check(args.base, args.head))
    except (ValueError, subprocess.CalledProcessError) as exc:
        parser.exit(2, f"eval-immutable: {exc}\n")


if __name__ == "__main__":
    main()
