from __future__ import annotations

import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from evolve.cycle import Budget, main  # noqa: E402
from evolve.proposals import candidate  # noqa: E402
from evolve.store import MemoryStore  # noqa: E402


class CycleFlowTests(unittest.TestCase):
    def test_full_cycle_preregisters_builds_baseline_and_guards_publication(self):
        for open_pr, after_score, expected in [
            (False, 0.8, "accept"),
            (True, 0.8, "accept"),
            (True, 0.6, "reject"),
        ]:
            with (
                self.subTest(open_pr=open_pr, expected=expected),
                tempfile.TemporaryDirectory() as directory,
            ):
                repo = Path(directory)
                subprocess.run(
                    ["git", "init", "-b", "main", str(repo)], check=True, capture_output=True
                )
                (repo / "benchmarks").mkdir()
                (repo / "benchmarks/noise.json").write_text(
                    '{"metrics":{"mean_nDCG":{"band":0.02}}}'
                )
                subprocess.run(["git", "-C", str(repo), "add", "."], check=True)
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
                        "-m",
                        "base",
                    ],
                    check=True,
                    capture_output=True,
                )
                backlog = repo / "backlog.json"
                backlog.write_text(
                    json.dumps(
                        [
                            candidate(
                                "Native change", "Test a bounded queue", "mean_nDCG", 0.1, []
                            ).to_dict()
                        ]
                    )
                )
                codex = repo / "codex"
                codex.write_text("""#!/usr/bin/env python3
import pathlib, subprocess, sys
root = pathlib.Path(sys.argv[sys.argv.index('-C') + 1])
(root / 'chitta/src').mkdir(parents=True)
(root / 'chitta/src/change.cpp').write_text('// fixture change\\n')
subprocess.run(['git', '-C', str(root), 'add', '.'], check=True)
subprocess.run(['git', '-C', str(root), '-c', 'user.name=Test', '-c', 'user.email=test@example.com', 'commit', '-m', 'candidate'], check=True)
""")
                codex.chmod(0o755)
                store = Mock()
                store.recall.return_value = []
                store.remember.side_effect = [
                    "proposal-memory",
                    "bet-memory",
                    "resolution-memory",
                    "verdict-memory",
                ]
                before = dict(
                    metrics={"mean_nDCG": 0.7}, snapshot="frozen", config={}, grade_pass=True
                )
                after = dict(before, metrics={"mean_nDCG": after_score})
                publish = []
                real_run = Budget.run

                def run(
                    budget,
                    cmd,
                    cwd,
                    *args,
                    store=store,
                    publish=publish,
                    real_run=real_run,
                    **kwargs,
                ):
                    if cmd[0] == "codex":
                        self.assertEqual(store.remember.call_args.args[0], "forward-bet")
                    if cmd[:3] == ["git", "submodule", "update"]:
                        return subprocess.CompletedProcess(cmd, 0, "")
                    if cmd[:2] == ["git", "push"] or cmd[0] == "gh":
                        publish.append(cmd)
                        return subprocess.CompletedProcess(cmd, 0, "")
                    return real_run(budget, cmd, cwd, *args, **kwargs)

                with (
                    patch.dict(os.environ, {"PATH": str(repo) + os.pathsep + os.environ["PATH"]}),
                    patch("evolve.cycle.MemoryStore", return_value=store),
                    patch("evolve.cycle.gate_commands", return_value=[]),
                    patch("evolve.cycle.validate_binaries"),
                    patch("evolve.cycle.replica_env", return_value={}),
                    patch("evolve.cycle.evaluate", side_effect=[before, after]) as evaluate,
                    patch.object(Budget, "run", run),
                    contextlib.redirect_stdout(io.StringIO()),
                ):
                    args = ["--repo", str(repo), "--backlog", str(backlog)] + (
                        ["--open-pr"] if open_pr else []
                    )
                    self.assertEqual(main(args), 1 if expected == "reject" else 0)
                values = list(repo.glob(".evolve/cycles/*/verdict.json"))
                self.assertEqual(len(values), 1)
                verdict = json.loads(values[0].read_text())
                self.assertEqual(verdict["verdict"], expected)
                self.assertEqual(verdict["bet_id"], "bet-memory")
                self.assertEqual(len(publish), 2 if open_pr and expected == "accept" else 0)
                self.assertIn(".evolve/baselines", str(evaluate.call_args_list[0].args[1]))
                self.assertIn(".evolve/worktrees", str(evaluate.call_args_list[1].args[1]))
                self.assertEqual(
                    [c.args[0] for c in store.remember.call_args_list],
                    ["proposal", "forward-bet", "bet-resolution", "verdict"],
                )

    def test_memory_listing_paginates_and_writes_json_string(self):
        store = MemoryStore("chitta")
        first = [
            dict(id=str(n), realm="project:chitta-evolve", tags=["verdict"], content="{}")
            for n in range(100)
        ]
        with patch.object(
            store, "call", side_effect=[{"memories": first}, {"memories": []}]
        ) as call:
            self.assertEqual(len(store.recall("verdict")), 100)
            self.assertEqual(call.call_args.args[-1], "100")
        with patch.object(store, "call", return_value={"id": "new"}) as call:
            store.remember("proposal", {"id": "canonical"})
            args = call.call_args.args
            self.assertEqual(args[args.index("--tags") + 1], '["proposal"]')
            content = args[args.index("--content") + 1]
            self.assertTrue(content.startswith(" "))
            self.assertEqual(json.loads(content), {"id": "canonical"})

    def test_wrong_realm_and_unrelated_rpc_reply_are_rejected(self):
        store = MemoryStore("chitta")
        for response in ({"realm": "wrong", "results": []}, {"messages": []}):
            with (
                patch.object(store, "call", return_value=response),
                self.assertRaises(RuntimeError),
            ):
                store.recall("ceiling", "project:cc-soul")

    def test_private_replica_pins_binaries_and_stops_only_its_clone(self):
        from evolve.cycle import evaluate, validate_binaries

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            worktree = root / "worktree"
            (worktree / "bin").mkdir(parents=True)
            with self.assertRaises(ValueError):
                validate_binaries(worktree)
            for name in ("chitta", "chittad"):
                path = worktree / "bin" / name
                path.write_text("#!/bin/sh\nexit 0\n")
                path.chmod(0o755)
            validate_binaries(worktree)
            (worktree / "bin/chitta").unlink()
            (worktree / "bin/chitta").symlink_to("/bin/true")
            with self.assertRaises(ValueError):
                validate_binaries(worktree)
            clone = root / "private-clone"
            clone.mkdir()
            env = dict(CHITTA_EVAL_MIND=str(root / "source"), PATH=os.environ["PATH"])
            with (
                patch("evolve.cycle.tempfile.mkdtemp", return_value=str(clone)),
                patch("evolve.cycle.replica_env", return_value={}) as prepare,
                patch("evolve.cycle.measure", return_value={"score": 0.8}),
                patch(
                    "evolve.cycle.subprocess.run", return_value=subprocess.CompletedProcess([], 0)
                ) as stop,
            ):
                result = evaluate(root, worktree, root, Budget(1), False, "after", env)
            self.assertEqual(result, {"score": 0.8})
            overrides = prepare.call_args.args[2]
            self.assertEqual(overrides["CHITTAD_BIN"], str(worktree / "bin/chittad"))
            self.assertEqual(overrides["CHITTA_LIVE_MIND"], str(root / "source"))
            self.assertEqual(stop.call_args.kwargs["env"]["CHITTA_EVAL_MIND"], str(clone))
            self.assertEqual(stop.call_args.args[0][-1], "stop")
            self.assertFalse(clone.exists())
