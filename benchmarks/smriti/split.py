#!/usr/bin/env python3
"""Versioned SMRITI assignments and deterministic, audited split rotation."""

from __future__ import annotations

import argparse
import hashlib
import json
from datetime import date
from pathlib import Path

DEFAULT_PATH = Path(__file__).with_name("split.json")


def load(path: Path = DEFAULT_PATH, tasks_dir: Path | None = None) -> dict:
    assignments = json.loads(path.read_text())
    if not isinstance(assignments, dict) or not assignments:
        raise ValueError("split manifest must be a nonempty task-to-split mapping")
    if any(value not in ("visible", "holdout") for value in assignments.values()):
        raise ValueError("each task must be visible or holdout")
    if tasks_dir is not None:
        tasks = {p.parent.name for p in tasks_dir.glob("*/task.json")}
        if tasks != set(assignments):
            raise ValueError(f"split/task mismatch: {sorted(tasks ^ set(assignments))}")
    return assignments


def fingerprint(assignments: dict) -> str:
    return hashlib.sha256(json.dumps(assignments, sort_keys=True).encode()).hexdigest()


def select(assignments: dict, requested: list, split_name: str) -> list:
    unknown = set(requested) - set(assignments)
    if unknown:
        raise ValueError(f"tasks absent from split manifest: {sorted(unknown)}")
    wrong = [t for t in requested if split_name != "all" and assignments[t] != split_name]
    if wrong:
        raise ValueError(f"tasks outside {split_name} split: {wrong}; select --split explicitly")
    return requested or sorted(
        t for t, s in assignments.items() if split_name == "all" or s == split_name
    )


def rotate(path: Path, history: Path, n: int, seed: str, day: str) -> dict:
    date.fromisoformat(day)
    before = load(path)
    groups = {s: [t for t in before if before[t] == s] for s in ("visible", "holdout")}
    if n < 1 or n > min(map(len, groups.values())):
        raise ValueError("N must be positive and fit in both splits")

    # Hash ordering is stable across Python implementations and versions.
    def order(task):
        return hashlib.sha256(json.dumps([seed, day, task]).encode()).hexdigest(), task

    moved = {s: sorted(ts, key=order)[:n] for s, ts in groups.items()}
    after = dict(before)
    for source, tasks in moved.items():
        for task in tasks:
            after[task] = "holdout" if source == "visible" else "visible"
    event = {
        "date": day,
        "seed": seed,
        "n": n,
        "moved_from": moved,
        "before": fingerprint(before),
        "after": fingerprint(after),
    }
    # Write-ahead audit: a failed manifest write leaves a detectable pending event.
    with history.open("a") as stream:
        stream.write(json.dumps(event, sort_keys=True) + "\n")
    temporary = path.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(after, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)
    return event


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    command = commands.add_parser("rotate", help="exchange N tasks in each direction")
    command.add_argument("-n", "--count", type=int, required=True)
    command.add_argument("--seed", required=True)
    command.add_argument("--date", default=date.today().isoformat())
    command.add_argument("--manifest", type=Path, default=DEFAULT_PATH)
    command.add_argument("--history", type=Path)
    args = parser.parse_args()
    try:
        event = rotate(
            args.manifest,
            args.history or args.manifest.with_name("split-history.jsonl"),
            args.count,
            args.seed,
            args.date,
        )
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    print(json.dumps(event, indent=2))


if __name__ == "__main__":
    main()
