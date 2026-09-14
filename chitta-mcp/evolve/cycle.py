"""One budgeted proposal → isolated implementation → replica → human cycle."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shlex
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

from . import bets
from .proposals import dedupe, gather, normalize, number, persist
from .selector import choose, history, rank, table
from .store import MemoryStore, body

FROZEN = (
    "benchmarks/",
    "hooks/grade-recall",
    "scripts/eval-replica.sh",
    "scripts/check-eval-immutable.sh",
    "CONTRACTS.md",
)


class Budget:
    def __init__(self, minutes: float):
        if number(minutes) <= 0:
            raise ValueError("max-minutes must be positive")
        self.deadline = time.monotonic() + minutes * 60

    def remaining(self) -> float:
        seconds = self.deadline - time.monotonic()
        if seconds <= 0:
            raise TimeoutError("cycle wall-clock budget exhausted")
        return seconds

    def run(
        self,
        cmd: list[str],
        cwd: Path,
        log: Path | None = None,
        env: dict | None = None,
        check: bool = True,
        final_output: Path | None = None,
    ) -> subprocess.CompletedProcess:
        print("$ (cd " + shlex.quote(str(cwd)) + " && " + shlex.join(cmd) + ")", flush=True)
        self.remaining()
        if final_output is not None:
            # A previous invocation's final message must never complete this one.
            final_output.unlink(missing_ok=True)
        output = ""
        completed_by_file = False
        stable_signature = None
        stable_since = time.monotonic()
        with log.open("w") if log else open(os.devnull, "w") as stream:
            process = subprocess.Popen(
                cmd,
                cwd=cwd,
                env=env,
                text=True,
                stdin=subprocess.DEVNULL,
                stdout=stream if log else subprocess.PIPE,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            try:
                while True:
                    timeout = self.remaining()
                    try:
                        output, _ = process.communicate(timeout=min(timeout, 0.1))
                        break
                    except subprocess.TimeoutExpired:
                        if process.poll() is not None:
                            # The leader exited but descendants still hold its
                            # stdout pipe open; cleanup must not wait on EOF.
                            break
                        if final_output is None:
                            continue
                        try:
                            stat = final_output.stat()
                        except FileNotFoundError:
                            continue
                        signature = (stat.st_size, stat.st_mtime_ns)
                        if signature != stable_signature:
                            stable_signature = signature
                            stable_since = time.monotonic()
                        elif stat.st_size and time.monotonic() - stable_since >= 0.25:
                            # Codex writes -o after its final answer but can hang
                            # during MCP shutdown. Allow the write to settle.
                            completed_by_file = True
                            break
            except TimeoutError:
                raise TimeoutError("cycle budget exhausted during " + cmd[0]) from None
            finally:
                # Always reap our private group, even if its leader returned:
                # descendants can survive a successful exec or ignore SIGTERM.
                def signal_group(sig):
                    try:
                        os.killpg(process.pid, sig)
                    except ProcessLookupError:
                        pass

                signal_group(signal.SIGTERM)
                try:
                    output, _ = process.communicate(timeout=0.2)
                except subprocess.TimeoutExpired:
                    pass
                finally:
                    signal_group(signal.SIGKILL)
                    output, _ = process.communicate(timeout=1)
        returncode = process.returncode
        if completed_by_file and returncode in (-signal.SIGTERM, -signal.SIGKILL):
            returncode = 0
        result = subprocess.CompletedProcess(cmd, returncode, output or "")
        if check and result.returncode:
            raise subprocess.CalledProcessError(result.returncode, cmd, output=result.stdout)
        return result


def noise_bands(repo: Path, budget: Budget, metric: str) -> dict[str, float]:
    path = repo / "benchmarks/noise.json"
    bands = {}
    if path.exists():
        data = json.loads(path.read_text())
        for key, value in data.get("metrics", data).items():
            if isinstance(value, dict):
                value = value.get("band", value.get("noise_band"))
            if isinstance(value, (int, float)) and number(value) >= 0:
                bands[key] = number(value)
    helper = repo / "benchmarks/noise.py"
    if helper.exists():
        result = budget.run([sys.executable, str(helper), "band", metric], repo, check=False)
        if result.returncode == 0:
            try:
                value = json.loads(result.stdout)
                if isinstance(value, dict):
                    value = value.get("band", value.get("noise_band"))
                if number(value) >= 0:
                    bands[metric] = number(value)
            except (ValueError, TypeError):
                print("WARNING: noise helper returned an unsupported band")
    return bands


def immutable(repo: Path, budget: Budget, base: str = "main", head: str = "HEAD"):
    """The helper (benchmarks/check_eval_immutable.py) takes BASE HEAD and fails
    if an evaluator path changed between them without an Eval-Change-Approved
    trailer. On the bare repo before a candidate exists, BASE==HEAD is a wiring
    check; in a worktree it compares the candidate against main."""
    helper = repo / "scripts/check-eval-immutable.sh"
    if helper.exists():
        budget.run(["bash", str(helper), base, head], repo)
    else:
        print("WARNING: immutability helper absent; built-in frozen-path check remains active")


def changed_files(worktree: Path, base: str, budget: Budget) -> list[str]:
    result = budget.run(
        ["git", "diff", "--name-only", "--no-renames", "-z", base, "HEAD"], worktree
    )
    return [path for path in result.stdout.split("\0") if path]


def frozen_check(paths: list[str]):
    if any(path.startswith(FROZEN) or Path(path).name == "CONTRACTS.md" for path in paths):
        raise ValueError("candidate changed frozen evaluation or contracts")


def gate_commands(worktree: Path, paths: list[str]) -> list[tuple[Path, list[str]]]:
    commands = [
        (worktree, ["bash", "-n", path])
        for path in paths
        if path.endswith(".sh") and (worktree / path).exists()
    ]
    if any(p == "chitta-field" or p.startswith(("chitta/", "chitta-field/")) for p in paths):
        # build.sh sets the project's Rust toolchain/PyO3 environment. CMake
        # needs its release static library even for a C++-only change.
        commands += [
            (worktree / "chitta-field", ["bash", "build.sh", "build", "--release"]),
            (worktree, ["cmake", "-S", "chitta", "-B", "chitta/build"]),
            (worktree / "chitta", ["cmake", "--build", "build", "--parallel"]),
        ]
    if any(p.startswith("chitta-mcp/") for p in paths):
        commands.append(
            (worktree / "chitta-mcp", [test_python(), "-m", "unittest", "discover", "tests"])
        )
    return commands


def test_python() -> str:
    """server.py needs CPython >= 3.10 while this cycle may itself run under the
    PyPy 3.9 that `python3` resolves to here; CHITTA_PY wins, then the bioinfo
    env, then whatever we are running under."""
    for cand in (
        os.environ.get("CHITTA_PY", ""),
        "/maps/projects/fernandezguerra/apps/opt/conda/envs/bioinfo/bin/python3",
    ):
        if cand and os.access(cand, os.X_OK):
            return cand
    return sys.executable


def implement_command(args, worktree: Path, spec: str, output: Path) -> list[str]:
    if args.implementer == "codex":
        return [
            "codex",
            "exec",
            "-C",
            str(worktree),
            "--approve-for-me",
            "--skip-git-repo-check",
            "-m",
            args.model,
            "-c",
            "model_reasoning_effort=high",
            "-o",
            str(output),
            spec,
        ]
    return ["claude", "-p", spec]


def replica_env(repo: Path, budget: Budget, overrides: dict | None = None) -> dict:
    env = os.environ.copy()
    env.update(overrides or {})
    mind = Path(
        env.get("CHITTA_EVAL_MIND", "/projects/caeg/scratch/kbd606/tmp/chitta-eval-mind")
    ).resolve()
    live = Path.home() / ".claude/mind"
    if mind == live.resolve() or live.resolve() in mind.parents:
        raise ValueError("evaluation mind overlaps live memory")
    launcher = repo / "scripts/eval-replica.sh"
    result = budget.run(["bash", str(launcher), "status"], repo, env=env, check=False)
    if result.returncode:
        budget.run(["bash", str(launcher), "start"], repo, env=env)
    # Parse shell assignments as data: never source arbitrary metadata.
    metadata = {}
    for line in (mind / "replica.env").read_text().splitlines():
        if "=" in line:
            key, raw = line.split("=", 1)
            words = shlex.split(raw)
            if key.startswith("CHITTA_EVAL_") and len(words) == 1:
                metadata[key] = words[0]
    socket = Path(metadata.get("CHITTA_EVAL_SOCKET", "")).resolve()
    if (
        mind not in socket.parents
        or not socket.is_socket()
        or not metadata.get("CHITTA_EVAL_SNAPSHOT_ID")
    ):
        raise ValueError("replica metadata lacks an isolated socket or snapshot id")
    if Path(metadata.get("CHITTA_EVAL_MIND", str(mind))).resolve() != mind:
        raise ValueError("replica metadata points to a different mind")
    env.update(metadata)
    env.update(CHITTA_DB_PATH=str(mind), CHITTA_MIND=str(mind), CC_SOUL_DB_PATH=str(mind))
    env["CHITTA_SOCKET_PATH"] = str(socket)
    return env


def validate_binaries(worktree: Path):
    for name in ("chitta", "chittad"):
        path = worktree / "bin" / name
        if (
            not path.is_file()
            or not os.access(path, os.X_OK)
            or worktree.resolve() not in path.resolve().parents
        ):
            raise ValueError("missing or nonlocal evaluation binary: " + str(path))


def evaluate(
    repo: Path, worktree: Path, artifacts: Path, budget: Budget, real: bool, phase: str, env: dict
) -> dict:
    # Each measurement gets a fresh clone of the SAME committed replica family.
    # The existing replica is only a snapshot source. Candidate binaries never
    # replace its daemon, and grader writes cannot contaminate the other phase.
    mind = Path(tempfile.mkdtemp(prefix="chitta-evolve-"))
    with socket.socket() as port_socket:
        port_socket.bind(("127.0.0.1", 0))
        port = port_socket.getsockname()[1]
    overrides = dict(
        env,
        CHITTA_LIVE_MIND=env["CHITTA_EVAL_MIND"],
        CHITTA_EVAL_MIND=str(mind),
        CHITTA_EVAL_PORT=str(port),
        CHITTA_EVAL_START_TIMEOUT=str(max(1, int(budget.remaining()))),
    )
    if (worktree / "bin/chittad").is_file():
        overrides["CHITTAD_BIN"] = str(worktree / "bin/chittad")
        overrides["CHITTA_BIN"] = str(worktree / "bin/chitta")
        overrides["PATH"] = str(worktree / "bin") + os.pathsep + env["PATH"]
    try:
        isolated = replica_env(repo, budget, overrides)
        return measure(repo, worktree, artifacts, budget, real, phase, isolated)
    finally:
        # Stop only the private clone via the launcher's PID/path validation.
        # A short cleanup grace also applies when the experiment used its budget.
        try:
            stopped = subprocess.run(
                ["bash", str(repo / "scripts/eval-replica.sh"), "stop"],
                cwd=repo,
                env=overrides,
                capture_output=True,
                timeout=10,
            )
            for name in ("replica.env", "replica.log"):
                if (mind / name).exists():
                    shutil.copy2(mind / name, artifacts / (phase + "-" + name))
            if stopped.returncode == 0:
                shutil.rmtree(mind)
            else:
                print("WARNING: private replica cleanup failed; retained " + str(mind))
        except (OSError, subprocess.SubprocessError):
            print("WARNING: private replica cleanup incomplete; retained " + str(mind))


def measure(
    repo: Path, worktree: Path, artifacts: Path, budget: Budget, real: bool, phase: str, env: dict
) -> dict:
    # Keep the grader and gold IDs outside the implementer's worktree and never
    # read a checked-in stale results file as a measurement of this run.
    harness = artifacts / ("eval-" + phase)
    harness.mkdir()
    for name in ("grade-recall.py", "grade-recall-goldids.json"):
        shutil.copy2(repo / "hooks" / name, harness / name)
    result = budget.run(
        [sys.executable, str(harness / "grade-recall.py"), "--quiet", "--limit", "10"],
        worktree,
        artifacts / (phase + "-recall.log"),
        env=env,
        check=False,
    )
    report = json.loads((harness / "grade-recall-results.json").read_text())
    if (
        result.returncode not in (0, 1)
        or report.get("replica_snapshot_id") != env["CHITTA_EVAL_SNAPSHOT_ID"]
    ):
        raise ValueError("grader did not produce a valid report for this replica")
    if (
        report.get("metric") != "mean_nDCG"
        or not report.get("is_canonical_run")
        or number(report.get("n", 0)) <= 0
    ):
        raise ValueError("grader report is not a canonical nonempty measurement")
    metrics = {report["metric"]: number(report["score"])}
    if real:
        runner = repo / "benchmarks/smriti/runner.py"
        help_text = budget.run([sys.executable, str(runner), "--help"], worktree).stdout
        if "--split" not in help_text:
            raise ValueError("SMRITI runner lacks --split; refusing an unpartitioned holdout run")
        budget.run(
            [
                sys.executable,
                str(runner),
                "--split",
                "holdout",
                "--trials",
                "3",
                "--agent",
                "claude-code",
                "--output",
                str(artifacts / (phase + "-smriti")),
            ],
            worktree,
            artifacts / (phase + "-smriti.log"),
            env=env,
        )
        rows = []
        for path in (artifacts / (phase + "-smriti")).glob("*.jsonl"):
            rows.extend(json.loads(line) for line in path.read_text().splitlines() if line.strip())
        rates = {}
        for condition in ("on", "off"):
            group = [
                row for row in rows if row.get("condition") == condition and not row.get("dry_run")
            ]
            if not group or {row.get("trial") for row in group} != {0, 1, 2}:
                raise ValueError("SMRITI did not produce all three holdout trials")
            rates[condition] = sum(row["passed"] is True for row in group) / len(group)
        metrics.update(smriti_success_rate=rates["on"], delta_sr=rates["on"] - rates["off"])
    return dict(
        metrics=metrics,
        snapshot=report["replica_snapshot_id"],
        config={key: report.get(key) for key in ("version", "limit", "reranker", "n")},
        grade_pass=result.returncode == 0,
    )


def verdict_for(bet: dict, before: dict, after: dict, bands: dict) -> tuple[str, dict]:
    if before["snapshot"] != after["snapshot"] or before["config"] != after["config"]:
        return "inconclusive", {}
    deltas = {
        key: value - before["metrics"][key]
        for key, value in after["metrics"].items()
        if key in before["metrics"]
    }
    metric = bet["metric"]
    if metric not in deltas or metric not in bands:
        return "inconclusive", deltas
    delta = deltas[metric] if bet["direction"] == "increase" else -deltas[metric]
    # Guard the grader's quality metric even for another registered target.
    regression = any(
        value < -bands[key] for key, value in deltas.items() if key == "mean_nDCG" and key in bands
    )
    if regression or delta < -bands[metric] or not after["grade_pass"]:
        return "reject", deltas
    return ("accept" if delta > bands[metric] else "inconclusive"), deltas


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    result.add_argument("--backlog", type=Path, help="JSON array, replacing source gathering")
    result.add_argument("--max-minutes", type=float, default=120)
    result.add_argument("--implementer", choices=("codex", "claude"), default="codex")
    result.add_argument("--model", default="gpt-6-astra")
    result.add_argument("--explore-quota", type=float, default=0.3)
    result.add_argument("--c", type=float, default=1)
    result.add_argument("--real-eval", action="store_true")
    result.add_argument("--open-pr", action="store_true")
    result.add_argument("--dry-run", action="store_true")
    return result


def run(args) -> int:
    repo = args.repo.resolve()
    budget = Budget(args.max_minutes)
    store = MemoryStore(timeout=min(30, budget.remaining()))
    store.remaining = budget.remaining
    immutable(repo, budget, "HEAD", "HEAD")
    proposals, warnings = (
        ([normalize(p) for p in json.loads(args.backlog.read_text())], [])
        if args.backlog
        else gather(repo, store)
    )
    try:
        remembered = [body(r) for r in store.recall("proposal")]
        proposals = dedupe(proposals + [normalize(p) for p in remembered if "mechanism" in p])
        verdicts = history(store)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        if not args.dry_run:
            raise
        warnings.append("history unavailable in preview: " + str(exc))
        verdicts = []
    for warning in warnings:
        print("WARNING: " + warning)
    print(table(rank(proposals, verdicts, args.c)))
    proposal = choose(proposals, verdicts, args.explore_quota, args.c)
    if args.explore_quota and not any(p.source == "hypothesis" for p in proposals):
        print(
            "WARNING: hypothesis quota unavailable: no mechanism cards; exploration debt is retained"
        )
    bands = noise_bands(repo, budget, proposal.expected_gain["metric"])
    band = bands.get(proposal.expected_gain["metric"])
    if band is None:
        print("WARNING: no target noise band; verdict cannot be accept")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-%f")
    cycle_id = stamp + "-" + proposal.id
    branch = "evolve/auto-" + cycle_id
    artifacts = repo / ".evolve/cycles" / cycle_id
    worktree = repo / ".evolve/worktrees" / cycle_id
    artifacts.mkdir(parents=True)
    base = budget.run(["git", "rev-parse", "main^{commit}"], repo).stdout.strip()
    bet = dict(
        proposal_id=proposal.id,
        metric=proposal.expected_gain["metric"],
        predicted_delta=proposal.expected_gain["delta"],
        direction="increase" if proposal.expected_gain["delta"] >= 0 else "decrease",
        band_from_noise=band,
        metric_bands=bands,
        registered_ts="PREVIEW",
        cycle_id=cycle_id,
    )
    proposal_memory = bet_id = None
    if not args.dry_run:
        proposal_memory = persist(proposals, store)[proposal.id]
        bet_id, bet = bets.register(proposal, band, store, cycle_id, bands)
    template = (Path(__file__).parent / "spec_template.md").read_text()
    spec = template.format(
        title=proposal.title,
        worktree=worktree,
        branch=branch,
        base=base,
        minutes=budget.remaining() / 60,
        bet=json.dumps(bet, indent=2),
        proposal=json.dumps(proposal.to_dict(), indent=2),
    )
    spec_path = artifacts / "spec.md"
    spec_path.write_text(spec)
    (artifacts / "proposal.json").write_text(json.dumps(proposal.to_dict(), indent=2))
    commands = [
        ["git", "worktree", "add", "-b", branch, str(worktree), base],
        implement_command(args, worktree, spec, artifacts / "implementer-final.txt"),
    ]
    if args.dry_run:
        print("\nSPEC " + str(spec_path) + "\n" + spec)
        print(
            "PLAN: persist backlog; register forward bet; implement; gates; build pinned baseline if native; paired replica measurements; resolve bet; verdict memory"
        )
        for command in commands:
            print("$ " + shlex.join(command))
        print(
            "$ git diff --name-only "
            + base
            + " HEAD  # freeze check; bash -n for changed shell files"
        )
        for _, cmd in gate_commands(worktree, ["chitta/src/preview.cpp", "chitta-mcp/preview.py"]):
            print("$ " + shlex.join(cmd) + "  # conditional on changed paths")
        print("$ bash scripts/check-eval-immutable.sh  # if present, before and after")
        print("$ bash scripts/eval-replica.sh status || bash scripts/eval-replica.sh start")
        print(
            "$ CHITTA_EVAL_SOCKET=<validated-replica-socket> python3 hooks/grade-recall.py --quiet --limit 10  # baseline and candidate"
        )
        if args.real_eval:
            print(
                "$ CHITTA_EVAL_SOCKET=<validated-replica-socket> python3 benchmarks/smriti/runner.py --split holdout --trials 3 --agent claude-code"
            )
        if args.open_pr:
            print("$ git push -u origin " + branch + "  # accept only")
            print(
                "$ gh pr create --base main --head "
                + branch
                + " --body-file <verdict.md>  # accept only"
            )
        return 0
    verdict = "inconclusive"
    reason = "evaluation unavailable"
    deltas = {}
    resolution = None
    stage = "implementation"
    try:
        worktree.parent.mkdir(parents=True, exist_ok=True)
        budget.run(commands[0], repo)
        implementation_env = dict(os.environ, CHITTA_HEADLESS="1", CC_SOUL_HEADLESS="1")
        budget.run(
            commands[1],
            worktree,
            artifacts / "implementer.log",
            env=implementation_env,
            final_output=(artifacts / "implementer-final.txt")
            if args.implementer == "codex"
            else None,
        )
        if budget.run(
            ["git", "status", "--porcelain", "--untracked-files=all"], worktree
        ).stdout.strip():
            raise ValueError("implementer left uncommitted changes")
        if budget.run(["git", "branch", "--show-current"], worktree).stdout.strip() != branch:
            raise ValueError("implementer changed the assigned branch")
        budget.run(["git", "merge-base", "--is-ancestor", base, "HEAD"], worktree)
        paths = changed_files(worktree, base, budget)
        frozen_check(paths)
        if not paths:
            raise ValueError("implementer produced no committed change")
        if any(p == "chitta-field" or p.startswith(("chitta/", "chitta-field/")) for p in paths):
            budget.run(
                ["git", "submodule", "update", "--init", "--no-fetch", "chitta-field"], worktree
            )
        for n, (cwd, cmd) in enumerate(gate_commands(worktree, paths)):
            budget.run(cmd, cwd, artifacts / f"gate-{n}.log")
        immutable(worktree, budget, base, "HEAD")
        if budget.run(
            ["git", "status", "--porcelain", "--untracked-files=all"], worktree
        ).stdout.strip():
            raise ValueError("gates left uncommitted changes")
        stage = "evaluation"
        native = any(
            p == "chitta-field" or p.startswith(("chitta/", "chitta-field/")) for p in paths
        )
        baseline_worktree = worktree
        if native:
            # Build the pinned base only when native code changed. Comparing a
            # candidate to an arbitrary deployed version confounds the experiment.
            baseline_worktree = repo / ".evolve/baselines" / cycle_id
            baseline_worktree.parent.mkdir(parents=True, exist_ok=True)
            budget.run(["git", "worktree", "add", "--detach", str(baseline_worktree), base], repo)
            budget.run(
                ["git", "submodule", "update", "--init", "--no-fetch", "chitta-field"],
                baseline_worktree,
            )
            for n, (cwd, cmd) in enumerate(
                gate_commands(baseline_worktree, ["chitta/src/baseline.cpp"])
            ):
                budget.run(cmd, cwd, artifacts / f"baseline-build-{n}.log")
            validate_binaries(baseline_worktree)
            validate_binaries(worktree)
        eval_env = replica_env(repo, budget)
        baseline = evaluate(
            repo, baseline_worktree, artifacts, budget, args.real_eval, "before", eval_env
        )
        if baseline:
            after = evaluate(repo, worktree, artifacts, budget, args.real_eval, "after", eval_env)
            verdict, deltas = verdict_for(bet, baseline, after, bands)
            reason = "paired replica measurements compared with preregistered bands"
            # The grader exercises the native candidate in its private replica.
            # Installed hooks/MCP are not replaced, so changes to those paths
            # need a dedicated evaluator before they can receive an accept.
            uncovered = any(
                p.startswith(("hooks/", "chitta-mcp/")) and "/tests/" not in p for p in paths
            )
            if not native or uncovered:
                verdict, reason = (
                    "inconclusive",
                    "changed runtime paths are not fully exercised by this replica evaluator",
                )
            surprise = any(
                key != bet["metric"] and key in bands and abs(delta) > bands[key]
                for key, delta in deltas.items()
            )
            if surprise or bet["metric"] in deltas and band is not None:
                wisdom = "\n".join(
                    str(r.get("content", r.get("text", ""))) for r in store.recall("bet-resolution")
                )
                resolution = bets.resolve(
                    bet_id,
                    deltas,
                    store,
                    json.dumps(
                        dict(proposal_evidence=proposal.evidence, before=baseline, after=after)
                    ),
                    wisdom,
                    bet,
                )
    except TimeoutError as exc:
        verdict, reason = "inconclusive", str(exc)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        verdict, reason = ("inconclusive" if stage == "evaluation" else "reject"), str(exc)
    value = dict(
        cycle_id=cycle_id,
        proposal_id=proposal.id,
        proposal_memory_id=proposal_memory,
        bet_id=bet_id,
        source=proposal.source,
        verdict=verdict,
        reason=reason,
        measured_delta=deltas,
        resolution=resolution,
        branch=branch,
        base=base,
        spec_sha256=hashlib.sha256(spec.encode()).hexdigest(),
    )
    (artifacts / "verdict.json").write_text(json.dumps(value, indent=2))
    # Reserve one short bookkeeping call even if the implementer used its budget.
    store.timeout = 10
    store.remaining = None
    value["memory_id"] = store.remember("verdict", value)
    print(json.dumps(value, indent=2))
    if args.open_pr and verdict == "accept":
        immutable(worktree, budget, base, "HEAD")
        frozen_check(changed_files(worktree, base, budget))
        if budget.run(
            ["git", "status", "--porcelain", "--untracked-files=all"], worktree
        ).stdout.strip():
            raise ValueError("evaluation left uncommitted changes")
        body_path = artifacts / "pr.md"
        body_path.write_text(
            f"{proposal.title}\n\n{proposal.mechanism}\n\nHuman merge required.\n\n```json\n"
            + json.dumps(value, indent=2)
            + "\n```\n"
        )
        budget.run(["git", "push", "-u", "origin", branch], worktree)
        budget.run(
            [
                "gh",
                "pr",
                "create",
                "--base",
                "main",
                "--head",
                branch,
                "--title",
                proposal.title,
                "--body-file",
                str(body_path),
            ],
            worktree,
        )
    return 1 if verdict == "reject" else 0


def main(argv: list[str] | None = None) -> int:
    try:
        return run(parser().parse_args(argv))
    except (OSError, ValueError, RuntimeError, TimeoutError, subprocess.SubprocessError) as exc:
        print("evolve: " + str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
