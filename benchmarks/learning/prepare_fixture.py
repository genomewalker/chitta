#!/usr/bin/env python3
"""Build ONLY the two-task synthetic fixture on a private copy of the eval replica."""

from __future__ import annotations

import argparse
import ctypes
import json
import sys
from pathlib import Path

from audit import TOOLS
from common import REALM, command, digest, family, git, live_paths, now_ms, write_json
from freeze import add_task, classify, freeze, validate_task
from runner import private_environment, start_replica, stop_replica


def prepare(
    out,
    source,
    chitta_bin,
    chittad_bin,
    claude_bin,
    embed_model,
    isolation="strict",
    void_trial=False,
):
    out = Path(out).resolve()
    if out.exists():
        raise ValueError("fixture output must be new")
    out.mkdir(parents=True)
    config = {
        "model": "claude-sonnet-4-5-20250929",
        "claude_bin": claude_bin,
        "claude_version": command([claude_bin, "--version"]).stdout.strip(),
        "chitta_bin": chitta_bin,
        "chittad_bin": chittad_bin,
        "embed_model": embed_model,
        "max_turns": 8,
        "budget_usd": 1.0,
        "timeout_s": 120,
        "seed": 9152026,
        "isolation": isolation,
        "allowed_tools": TOOLS,
        "permission_mode": "dontAsk",
    }
    live = live_paths()
    if Path(source).resolve().is_relative_to(Path(live["mind"])):
        raise ValueError("fixture source must be the eval replica, never the live mind")
    env = private_environment(out / "source", live, config)
    ctypes.CDLL(None).prctl(36, 1, 0, 0, 0)
    ids, evidence = [], []
    try:
        rpc = start_replica(env, source, out / "source-launch.log")
        for name in ("saffron", "cobalt"):
            content = (
                f"LearningFixture {name} readiness convention: write ready to {name}.txt. "
                f"The {name} readiness marker must contain exactly ready followed by a newline."
            )
            result = rpc.call(
                "observe",
                title=f"LearningFixture {name} readiness",
                content=content,
                realm=REALM,
                category="wisdom",
                source="distillation",
                confidence=0.99,
            )
            mid = str(result["id"])
            ids.append(mid)
            memory = rpc.call("get", id=mid)
            trips = rpc.call("query_graph", subject=mid)["triplets"]
            evidence.append(classify(memory, trips, {}))
        rpc.call("compact_wal")
    finally:
        stop_replica(env, out / "source-launch.log")
    source_family = family(env["CHITTA_EVAL_MIND"])
    cut = now_ms()
    cohort = {
        "schema": 1,
        "fixture": True,
        "preview": False,
        "realm": REALM,
        "cut_timestamp_ms": cut,
        "store": source_family,
        "enumeration_stable": True,
        "ids": ids,
        "unresolved": [],
        "evidence": evidence,
        "fixture_note": "Only two synthetic observe source=distillation rows; not an official cohort",
    }
    write_json(out / "cohort-draft.json", cohort)
    repo = out / "task-repo"
    repo.mkdir()
    git(repo, "init", "--quiet", "-b", "fixture")
    git(repo, "config", "user.name", "Learning Fixture")
    git(repo, "config", "user.email", "fixture@example.invalid")
    for name in ("saffron", "cobalt"):
        (repo / f"{name}.txt").write_text("pending\n")
    git(repo, "add", ".")
    git(repo, "commit", "--quiet", "-m", "Fixture initial state")
    initial = git(repo, "rev-parse", "HEAD")
    for name in ("saffron", "cobalt"):
        (repo / f"{name}.txt").write_text("ready\n")
    git(repo, "commit", "--quiet", "-am", "Fixture known-good state")
    good = git(repo, "rev-parse", "HEAD")
    tasks_path = out / "tasks-draft.json"
    for index, name in enumerate(("saffron", "cobalt")):
        yes = {"exit_code": 0, "writes": {f"{name}.txt": "ready\n"}}
        no = {"exit_code": 1, "writes": {}}
        task = {
            "id": name,
            "repo": str(repo),
            "cwd_sha": initial,
            "known_good": good,
            "prompt": f"Apply the LearningFixture {name} readiness convention to {name}.txt.",
            "prompt_timestamp_ms": cut + index + 1,
            "transcript_sha256": digest(repo / f"{name}.txt"),
            "selection_rule": "next_eligible_graded_prompt",
            "selection_note": "Synthetic prospective pipeline fixture",
            "recall_inspected": False,
            "dependencies": [{"path": "/bin/sh", "sha256": digest("/bin/sh")}],
            "grader": {
                "command": ["/bin/sh", ".learning-grader/check.sh"],
                "files": {"check.sh": f'test "$(cat {name}.txt)" = ready\n'},
                "timeout_s": 10,
            },
            "fixture": {
                "A": [yes, yes if index == 0 else no],
                "B": [no, no if index == 0 else yes],
            },
        }
        if void_trial and index == 0:
            task["fixture"]["A"][1] = {**yes, "void_out_of_root_read": True}
        entry = out / f"{name}-entry.json"
        write_json(entry, task)
        add_task(entry, tasks_path)
    from common import read_json

    tasks = read_json(tasks_path)
    for task in tasks:
        task["validation"] = validate_task(task)
    write_json(tasks_path, tasks)
    write_json(out / "config.json", config)
    freeze(tasks_path, out / "cohort-draft.json", out / "config.json", out / "frozen", fixture=True)
    print(json.dumps({"fixture_manifest": str(out / "frozen/manifest.json"), "cohort_ids": ids}))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--out", required=True)
    p.add_argument("--source", required=True)
    for name in ("chitta-bin", "chittad-bin", "claude-bin", "embed-model"):
        p.add_argument("--" + name, required=True)
    p.add_argument("--isolation", choices=("strict", "home-audit"), default="strict")
    p.add_argument("--void-trial", action="store_true")
    args = p.parse_args()
    prepare(
        args.out,
        args.source,
        args.chitta_bin,
        args.chittad_bin,
        args.claude_bin,
        args.embed_model,
        args.isolation,
        args.void_trial,
    )


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError) as exc:
        sys.exit(f"fixture: {exc}")
