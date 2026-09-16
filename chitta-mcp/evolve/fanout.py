"""Independent implementation streams with one deadline and measured selection."""

from __future__ import annotations

import hashlib
import os
import subprocess
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from .cycle import Budget, agent_output, evaluate_candidate, implementation_gates, replica_env
from .proposals import number


def choose_count(requested: int, seconds: float, real_eval: bool = False) -> dict:
    # Admission floor, not a runtime prediction: ten minutes of concurrent
    # implementation/gates, then five minutes per paired evaluation (twenty
    # with holdout trials). All work still shares the actual cycle deadline.
    reserve = requested * (1200 if real_eval else 300)
    fits = requested == 1 or seconds >= 600 + reserve
    return dict(
        requested=requested,
        effective=requested if fits else 1,
        evaluation_reserve_seconds=reserve if fits and requested > 1 else 0,
        fallback=None
        if fits
        else (
            f"requested {requested} candidates need at least {(600 + reserve) / 60:g} "
            f"remaining minutes; {seconds / 60:.1f} remain; falling back to 1"
        ),
    )


def isolated_env(root: Path) -> dict:
    """No inherited memory endpoints, Codex config, hook state or task IDs."""
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("CHITTA_", "CC_SOUL_", "CODEX_", "XDG_", "CLAUDE_"))
    }
    for key, relative in {
        "HOME": "home",
        "CODEX_HOME": "codex",
        "XDG_CONFIG_HOME": "config",
        "XDG_CACHE_HOME": "cache",
        "XDG_DATA_HOME": "data",
        "XDG_STATE_HOME": "state",
        "XDG_RUNTIME_DIR": "runtime",
        "TMPDIR": "tmp",
    }.items():
        path = root / relative
        path.mkdir(parents=True, mode=0o700)
        env[key] = str(path)
    mind = Path(env["HOME"]) / ".claude/mind"
    mind.mkdir(parents=True)
    # Keep explicit API authentication, never shared config/session directories.
    if os.environ.get("CODEX_API_KEY"):
        env["CODEX_API_KEY"] = os.environ["CODEX_API_KEY"]
    env.update(
        CHITTA_DB_PATH=str(mind),
        CC_SOUL_DB_PATH=str(mind),
        CHITTA_MIND=str(mind),
        CHITTA_SOCKET_PATH=str(root / "runtime/daemon.sock"),
        CHITTA_SOCKET=str(root / "runtime/daemon.sock"),
        CHITTA_QUEUE=str(root / "queue"),
        CHITTA_HEADLESS="1",
        CC_SOUL_HEADLESS="1",
    )
    return env


class CandidateBudget(Budget):
    def __init__(self, deadline: float, env: dict):
        self.deadline, self.env = deadline, env

    def run(self, *args, **kwargs):
        kwargs.setdefault("env", self.env)
        return super().run(*args, **kwargs)


def changed_lines(worktree: Path, base: str, budget: Budget) -> int:
    rows = budget.run(
        ["git", "diff", "--numstat", "--no-renames", "-z", base, "HEAD"], worktree
    ).stdout.split("\0")
    total = 0
    for row in filter(None, rows):
        added, removed, path = row.split("\t", 2)
        if path == "chitta-field":
            # A gitlink is two lines regardless of the size of its Rust patch.
            # Count the committed source changes too, using the pinned gitlinks.
            refs = [
                budget.run(["git", "rev-parse", ref + ":chitta-field"], worktree).stdout.strip()
                for ref in (base, "HEAD")
            ]
            nested = budget.run(
                ["git", "diff", "--numstat", "--no-renames", "-z", *refs],
                worktree / "chitta-field",
            ).stdout
            for entry in filter(None, nested.split("\0")):
                a, d, _ = entry.split("\t", 2)
                total += int(a) + int(d) if a != "-" else 2**63
        # Binary changes cannot claim zero lines to win a tie.
        total += int(added) + int(removed) if added != "-" else 2**63
    return total


def select_best(results: list[dict], bet: dict) -> dict | None:
    eligible = [row for row in results if row["gates_passed"] and bet["metric"] in row["delta"]]
    sign = 1 if bet["direction"] == "increase" else -1
    return min(
        eligible,
        key=lambda row: (
            -sign * number(row["delta"][bet["metric"]]),
            row["changed_lines"],
            row["index"],
        ),
        default=None,
    )


def run_candidates(
    args, repo, survey_tree, branch, base, spec, artifacts, budget, bet, bands, plan, results
):
    """Retain every outcome, including setup failures/timeouts; never merge."""
    deadline = budget.deadline - plan["evaluation_reserve_seconds"]
    # Git's shared worktree metadata uses locks; set up worktrees serially.
    # Account for every slot even if setup exhausts the budget.
    for index in range(1, plan["effective"] + 1):
        root = artifacts / f"candidate-{index}"
        tree = survey_tree.with_name(survey_tree.name + f"-candidate-{index}")
        results.append(
            dict(
                index=index,
                worktree=str(tree),
                branch=branch + f"-candidate-{index}",
                artifacts=str(root),
                gates_passed=False,
                gates={
                    name: "not_run"
                    for name in (
                        "committed_change",
                        "frozen_paths",
                        "immutable",
                        "tests",
                        "self_check",
                        "clean_after_gates",
                    )
                },
                delta={},
                changed_lines=None,
                outcome="not_started",
                reason="",
            )
        )

    def failed(row, exc):
        row.update(
            outcome="timeout" if isinstance(exc, TimeoutError) else "failed", reason=str(exc)
        )
        for key, status in row["gates"].items():
            if status == "running":
                row["gates"][key] = "fail"

    errors = (OSError, ValueError, RuntimeError, subprocess.SubprocessError)
    ready = []
    for row in results:
        try:
            root, tree = Path(row["artifacts"]), Path(row["worktree"])
            root.mkdir()
            env = isolated_env(root / "isolation")
            private = CandidateBudget(deadline, env)
            private.run(["git", "worktree", "add", "-b", row["branch"], str(tree), base], repo)
            prompt = spec.replace(str(survey_tree), str(tree)).replace(
                "Branch: " + branch + ";", "Branch: " + row["branch"] + ";"
            )
            prompt += (
                "\nIsolated search: do not inspect other worktrees, cycle artifacts, live memory,\n"
                "or other streams. Do not use shared task memory or write evolution memories.\n"
                "Use only this worktree and private HOME/state. The caller records outcomes.\n"
            )
            (root / "spec.md").write_text(prompt)
            row["spec_sha256"] = hashlib.sha256(prompt.encode()).hexdigest()
            ready.append((row, tree, root, private, prompt))
        except errors as exc:
            failed(row, exc)

    def implement(item):
        row, tree, root, private, prompt = item
        try:
            row["outcome"] = "implementing"
            final = agent_output(args, tree, prompt, root, "implementer", private)
            paths, check = implementation_gates(
                tree, row["branch"], base, final, root, private, row["gates"]
            )
            row.update(paths=paths, self_check=check)
            if not check:
                raise ValueError("no changed module test matches SELF_CHECK")
            row["changed_lines"] = changed_lines(tree, base, private)
            row.update(gates_passed=True, outcome="gated")
        except errors as exc:
            failed(row, exc)

    if ready:
        with ThreadPoolExecutor(max_workers=len(ready)) as pool:
            list(pool.map(implement, ready))
    eval_env = None
    for row in results:
        if not row["gates_passed"]:
            continue
        try:
            if eval_env is None:
                eval_env = replica_env(repo, budget)
            verdict, delta, reason, before, after = evaluate_candidate(
                repo,
                Path(row["worktree"]),
                Path(row["artifacts"]),
                budget,
                args.real_eval,
                Path(row["worktree"]).name,
                base,
                row["paths"],
                bet,
                bands,
                eval_env,
            )
            row.update(
                outcome="measured",
                verdict=verdict,
                delta=delta,
                reason=reason,
                before=before,
                after=after,
            )
        except errors as exc:
            failed(row, exc)
    return select_best(results, bet)
