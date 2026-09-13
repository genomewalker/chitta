#!/usr/bin/env python3
"""Mine review-required convention task skeletons without changing the source repository."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MESSAGE_PATTERN = "convention|rename|default|contract|format"
CONVENTION = re.compile(
    r"\b[A-Z][A-Z0-9_]+\s*=|--[a-z][a-z-]+|\bdefault\b|[f%]?['\"][^'\"\n]+['\"]", re.I
)
EVIDENCE = re.compile(r"--[a-z][a-z-]+|\b[A-Z][A-Z0-9_]+\b|['\"]([^'\"\n]+)['\"]")
ASSERTION = re.compile(r"\bassert\w*\b|\bexpect\b|\[\[|\btest\s", re.I)


def git(repo: Path, *args: str) -> bytes:
    return subprocess.check_output(
        ["git", "-C", str(repo), *args], env=dict(os.environ, GIT_OPTIONAL_LOCKS="0")
    )


def is_test(path: str) -> bool:
    p = Path(path)
    return (
        "tests" in p.parts
        or "test" in p.parts
        or "hidden" in p.parts
        or p.name.startswith("test_")
        or p.stem.endswith("_test")
    )


def evidence(text: str) -> set:
    return {m.group(1) or m.group(0) for m in EVIDENCE.finditer(text)}


def added_lines(diff: str) -> str:
    return "\n".join(
        line[1:]
        for line in diff.splitlines()
        if line.startswith("+") and not line.startswith("+++")
    )


def safe_write(root: Path, relative: str, content: bytes, executable: bool = False):
    path = root / relative
    if Path(relative).is_absolute() or ".." in Path(relative).parts:
        raise ValueError(f"unsafe tree path: {relative}")
    if not path.resolve().is_relative_to(root.resolve()):
        raise ValueError(f"path escapes output: {relative}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)
    if executable:
        path.chmod(0o755)


def tree_files(repo: Path, ref: str) -> dict:
    entries = {}
    for row in git(repo, "ls-tree", "-rz", ref).split(b"\0"):
        if not row:
            continue
        meta, path = row.split(b"\t", 1)
        mode, kind, oid = meta.decode().split()
        if kind == "blob" and mode in ("100644", "100755"):
            entries[path.decode()] = (mode, oid)
    return entries


def write_task(
    candidate: Path, task_id: str, prompt: str, check_cmd: str, provenance: dict, memory: str = ""
):
    task = {
        "id": task_id,
        "repo_fixture": "repo_fixture",
        "prompt": prompt,
        "check_cmd": check_cmd,
        "planted_memories": (
            [{"content": memory, "realm": f"smriti-{task_id}", "type": "wisdom"}] if memory else []
        ),
        "tags": ["candidate", "convention" if memory else "saddle"],
        "review_required": True,
        "provenance": provenance,
    }
    safe_write(candidate, "task.json", (json.dumps(task, indent=2) + "\n").encode())
    (candidate / "repo_fixture").mkdir(exist_ok=True)


def check_command(path: str) -> str:
    if path.endswith(".py"):
        # unittest handles modules without a __main__ block as well.
        return (
            f"python3 -m unittest discover -s {shlex.quote(str(Path(path).parent))} "
            f"-p {shlex.quote(Path(path).name)} -v"
        )
    if path.endswith(".sh"):
        return f"bash {shlex.quote(path)}"
    return "false # TODO: configure the project test runner for " + shlex.quote(path)


def mine_history(repo: Path, out: Path) -> list:
    commits = (
        git(
            repo,
            "log",
            "HEAD",
            "--no-merges",
            "--format=%H",
            "--extended-regexp",
            "--regexp-ignore-case",
            f"--grep={MESSAGE_PATTERN}",
        )
        .decode()
        .splitlines()
    )
    candidates = []
    for sha in commits:
        parents = git(repo, "rev-list", "--parents", "-n", "1", sha).decode().split()
        if len(parents) != 2:
            continue
        parent = parents[1]
        paths = (
            git(repo, "diff", "--no-renames", "--name-only", "-z", parent, sha).decode().split("\0")
        )
        tests = [p for p in paths if p and is_test(p)]
        sources = [
            p
            for p in paths
            if p
            and not is_test(p)
            and Path(p).suffix in (".py", ".sh", ".cpp", ".hpp", ".h", ".rs", ".js", ".ts")
        ]
        if not tests or not sources:
            continue
        source_evidence = {}
        for source in sources:
            diff = git(
                repo, "diff", "--no-ext-diff", "--no-textconv", "-U0", parent, sha, "--", source
            ).decode(errors="replace")
            changed = added_lines(diff)
            if CONVENTION.search(changed):
                source_evidence[source] = evidence(changed)
        if not source_evidence:
            continue
        selected_test, matched_sources, shared = None, [], set()
        for test in tests:
            diff = git(
                repo, "diff", "--no-ext-diff", "--no-textconv", "-U0", parent, sha, "--", test
            ).decode(errors="replace")
            assertions = "\n".join(
                line for line in added_lines(diff).splitlines() if ASSERTION.search(line)
            )
            tokens = evidence(assertions)
            matched = [s for s, ev in source_evidence.items() if ev & tokens]
            if matched:
                selected_test, matched_sources = test, matched
                shared = set().union(*(source_evidence[s] & tokens for s in matched))
                break
        if not selected_test:
            continue
        before, after = tree_files(repo, parent), tree_files(repo, sha)
        if selected_test not in after or not any(s in before for s in matched_sources):
            continue
        task_id = f"candidate-{sha[:12]}"
        candidate = out / "tasks" / task_id
        if not candidate.resolve().is_relative_to(out.resolve()):
            raise ValueError("candidate path escapes scratch output")
        if candidate.exists():
            raise ValueError(f"output already exists: {candidate}; use a fresh scratch directory")
        subject = git(repo, "show", "-s", "--format=%s", sha).decode().strip()
        # Sparse pre-change fixture: all changed production source files plus
        # package markers. No historical tests or post-change implementation.
        fixture_paths = sorted(set(sources) & set(before))
        for source in list(fixture_paths):
            for directory in Path(source).parents:
                init = str(directory / "__init__.py")
                if init in before and init not in fixture_paths:
                    fixture_paths.append(init)
        for source in fixture_paths:
            mode, oid = before[source]
            safe_write(
                candidate / "repo_fixture",
                source,
                git(repo, "cat-file", "blob", oid),
                mode == "100755",
            )
        mode, oid = after[selected_test]
        safe_write(
            candidate / "hidden",
            selected_test,
            git(repo, "cat-file", "blob", oid),
            mode == "100755",
        )
        provenance = {
            "kind": "git",
            "repo": str(repo),
            "before": parent,
            "after": sha,
            "subject": subject,
            "test": selected_test,
            "fixture_files": sorted(fixture_paths),
            "shared_assertion_evidence": sorted(shared),
            "fixture_is_sparse": True,
            "review": "Add dependencies, reduce the hidden test, rewrite the prompt to hide the convention, and verify pre-fail/post-pass.",
        }
        write_task(
            candidate,
            task_id,
            f"Update {', '.join(matched_sources)} to satisfy the project's current convention. Preserve unrelated behavior.",
            check_command(selected_test),
            provenance,
            f"[convention candidate] {subject}. Test-asserted evidence: {', '.join(sorted(shared))}",
        )
        candidates.append({"id": task_id, **provenance})
    return candidates


def mine_saddles(repo: Path, out: Path, ledger: Path | None) -> dict:
    cmd = [sys.executable, str(ROOT / "chitta-mcp/saddle_detector.py"), "report", "--json"]
    if ledger is not None:
        cmd += ["--ledger", str(ledger)]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode:
        return {"available": False, "error": proc.stderr.strip(), "candidates": 0, "episodes": None}
    report = json.loads(proc.stdout)
    count = 0
    for episode in report["top_episodes"]:
        command = episode.get("cmd_head", "").strip()
        if not command:
            continue
        key = json.dumps([episode["session_id"], episode["start_ts"], command])
        task_id = "candidate-saddle-" + hashlib.sha256(key.encode()).hexdigest()[:12]
        candidate = out / "tasks" / task_id
        if not candidate.resolve().is_relative_to(out.resolve()):
            raise ValueError("candidate path escapes scratch output")
        if candidate.exists():
            raise ValueError(f"output already exists: {candidate}")
        # Never execute ledger text. The detector stores only a truncated head;
        # preserve that limitation in the skeleton for human reconstruction.
        safe_write(
            candidate / "hidden",
            "check.sh",
            ("#!/usr/bin/env bash\nset -euo pipefail\n" + command + "\n").encode(),
        )
        write_task(
            candidate,
            task_id,
            f"Make the failing command succeed: {command}",
            "bash check.sh",
            {
                "kind": "saddle",
                "repo": str(repo),
                "episode": episode,
                "command_is_truncated_head": True,
                "review": "Recover full command, intent, cwd, and a failing fixture; audit command before execution.",
            },
        )
        count += 1
    return {
        "available": True,
        "episodes": report["episodes"],
        "failing_events": report["failing_events"],
        "candidates": count,
        "report_limit": 8,
        "report": report,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("repo", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument(
        "--from-saddles", action="store_true", help="mine detector report instead of git history"
    )
    parser.add_argument("--ledger", type=Path)
    args = parser.parse_args()
    try:
        repo = Path(
            git(args.repo.resolve(), "rev-parse", "--show-toplevel").decode().strip()
        ).resolve()
        out = args.out.resolve()
        if out.is_relative_to(repo) or out.is_relative_to(ROOT / "benchmarks/smriti/tasks"):
            raise ValueError("--out must be scratch outside the source repo and benchmark tasks")
        out.mkdir(parents=True, exist_ok=True)
        if args.from_saddles:
            result = mine_saddles(repo, out, args.ledger)
            name = "saddle-report.json"
        else:
            candidates = mine_history(repo, out)
            result = {"candidates": len(candidates), "items": candidates}
            name = "mining-report.json"
        safe_write(out, name, (json.dumps(result, indent=2) + "\n").encode())
        print(
            json.dumps({k: v for k, v in result.items() if k not in ("items", "report")}, indent=2)
        )
        if result.get("items"):
            print("example:", result["items"][0]["id"], result["items"][0]["subject"])
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        parser.exit(2, f"mine-tasks: {exc}\n")


if __name__ == "__main__":
    main()
