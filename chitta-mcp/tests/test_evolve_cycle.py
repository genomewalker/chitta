from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from evolve.cycle import Budget, frozen_check, gate_commands, main, verdict_for  # noqa: E402
from evolve.proposals import candidate  # noqa: E402
from evolve.store import MemoryStore  # noqa: E402


class CycleTests(unittest.TestCase):
    def test_dry_cycle_writes_spec_without_implementer_or_memory_writes(self):
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            subprocess.run(
                ["git", "init", "-b", "main", str(repo)], capture_output=True, check=True
            )
            subprocess.run(
                [
                    "git",
                    "-C",
                    str(repo),
                    "-c",
                    "user.name=Test",
                    "-c",
                    "user.email=test@example.com",
                    "commit",
                    "--allow-empty",
                    "-m",
                    "base",
                ],
                capture_output=True,
                check=True,
            )
            stub = repo / "chitta"
            stub.write_text(
                "#!/usr/bin/env python3\nimport json,sys\nassert 'remember' not in sys.argv\nprint(json.dumps({'results': []}))\n"
            )
            stub.chmod(0o755)
            codex = repo / "codex"
            codex.write_text("#!/bin/sh\ntouch IMPLEMENTER_WAS_RUN\nexit 99\n")
            codex.chmod(0o755)
            backlog = repo / "backlog.json"
            backlog.write_text(
                json.dumps(
                    [
                        candidate(
                            "Synthetic change", "Bound a queue", "mean_nDCG", 0.1, ["fixture"]
                        ).to_dict()
                    ]
                )
            )
            with patch.dict(
                os.environ,
                {"PATH": str(repo) + os.pathsep + os.environ["PATH"], "CHITTA_BIN": str(stub)},
            ):
                self.assertEqual(
                    main(["--repo", str(repo), "--backlog", str(backlog), "--dry-run"]), 0
                )
            specs = list(repo.glob(".evolve/cycles/*/spec.md"))
            self.assertEqual(len(specs), 1)
            text = specs[0].read_text()
            self.assertIn("Synthetic change", text)
            self.assertIn("Do not touch systemd", text)
            self.assertIn("PREVIEW", text)
            self.assertFalse((repo / "IMPLEMENTER_WAS_RUN").exists())
            self.assertFalse((repo / ".evolve/worktrees").exists())

    def test_failed_immutability_stops_before_gather(self):
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            (repo / "scripts").mkdir()
            (repo / "scripts/check-eval-immutable.sh").write_text("exit 1\n")
            with patch("evolve.cycle.gather") as gather:
                self.assertEqual(main(["--repo", str(repo), "--dry-run"]), 1)
                gather.assert_not_called()

    def test_frozen_paths_and_conditional_native_gates(self):
        for path in (
            "benchmarks/noise.json",
            "hooks/grade-recall.py",
            "scripts/check-eval-immutable.sh",
            "sub/CONTRACTS.md",
        ):
            with self.assertRaises(ValueError):
                frozen_check([path])
        self.assertEqual(gate_commands(Path("/tmp"), ["docs/a.md"]), [])
        commands = gate_commands(Path("/tmp"), ["chitta/src/a.cpp"])
        self.assertIn((Path("/tmp/chitta"), ["cmake", "--build", "build", "--parallel"]), commands)

    def test_verdict_requires_paired_target_and_noise(self):
        bet = dict(metric="mean_nDCG", direction="increase")
        before = dict(metrics={"mean_nDCG": 0.7}, snapshot="a", config={}, grade_pass=True)
        after = dict(before, metrics={"mean_nDCG": 0.8})
        self.assertEqual(verdict_for(bet, before, after, {"mean_nDCG": 0.02})[0], "accept")
        self.assertEqual(verdict_for(bet, before, after, {})[0], "inconclusive")
        self.assertEqual(
            verdict_for(bet, before, dict(after, snapshot="b"), {"mean_nDCG": 0.02})[0],
            "inconclusive",
        )
        after["metrics"]["mean_nDCG"] = 0.6
        self.assertEqual(verdict_for(bet, before, after, {"mean_nDCG": 0.02})[0], "reject")

    def test_timeout_and_memory_write_realm(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(TimeoutError):
                Budget(0.001).run(
                    [sys.executable, "-c", "import time; time.sleep(10)"], Path(directory)
                )
        with patch("subprocess.run") as run:
            run.return_value.stdout = '{"id":"42"}'
            self.assertEqual(MemoryStore("chitta").remember("verdict", {"realm": "bad"}), "42")
            args = run.call_args.args[0]
            self.assertEqual(args[args.index("--realm") + 1], "project:chitta-evolve")
            self.assertEqual(args[args.index("--type") + 1], "signal")
