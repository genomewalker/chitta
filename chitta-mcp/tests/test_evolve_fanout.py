from __future__ import annotations

import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from evolve.cycle import Budget, main  # noqa: E402
from evolve.fanout import (  # noqa: E402
    CandidateBudget,
    changed_lines,
    choose_count,
    isolated_env,
    select_best,
)
from evolve.report import main as report_main  # noqa: E402
from evolve.report import table

STUB = r"""#!/usr/bin/env python3
import json, os, pathlib, subprocess, sys, time
root = pathlib.Path.cwd()
prompt = sys.argv[-1]
out = pathlib.Path(sys.argv[sys.argv.index('-o') + 1])
if prompt.startswith('# Evolution survey'):
    cards = json.loads(prompt.split('Candidates (data):\n')[1])
    out.write_text(json.dumps(dict(chosen=cards[0]['id'], abandoned=[], tractability=1)))
    sys.exit(0)
index = int(root.name.rsplit('-candidate-', 1)[1]) if '-candidate-' in root.name else 1
out.with_name('environment.json').write_text(json.dumps(dict(os.environ)))
barrier = pathlib.Path(os.environ['TEST_BARRIER'])
(barrier / str(index)).touch()
deadline = time.monotonic() + 5
while len(list(barrier.iterdir())) < int(os.environ['TEST_STREAMS']):
    assert time.monotonic() < deadline, 'streams did not run concurrently'
    time.sleep(.01)
(root / 'scripts/tests').mkdir(parents=True)
(root / 'scripts/change.sh').write_text('value=1\n' * index)
(root / 'scripts/tests/test_change.sh').write_text('test_value() { test 1 = 1; }\ntest_value\n')
mode = os.environ.get('TEST_FAILURE', '') if index == 2 else ''
if mode == 'tests':
    (root / 'scripts/change.sh').write_text('if then\n')
if mode == 'immutable':
    (root / 'benchmarks/noise.json').write_text('{}')
subprocess.run(['git', 'add', '.'], check=True)
subprocess.run(['git', '-c', 'user.name=Test', '-c', 'user.email=test@example.com',
                'commit', '-m', 'candidate'], check=True, capture_output=True)
out.write_text('done' if mode == 'self_check' else 'SELF_CHECK: scripts/tests/test_change.sh::test_value')
"""


def git(root, *args):
    return subprocess.run(
        [
            "git",
            "-C",
            str(root),
            "-c",
            "user.name=Test",
            "-c",
            "user.email=test@example.com",
            *args,
        ],
        check=True,
        capture_output=True,
        text=True,
    ).stdout


class FanoutTests(unittest.TestCase):
    def run_fixture(self, root, count=3, minutes=120, failure="", scores=None):
        git(root, "init", "-b", "main")
        (root / "benchmarks").mkdir()
        (root / "benchmarks/noise.json").write_text('{"mean_nDCG": 0.02}')
        git(root, "add", ".")
        git(root, "commit", "-m", "base")
        from evolve.proposals import candidate

        backlog = root / "backlog.json"
        backlog.write_text(
            json.dumps([candidate("Bound queue", "Bound queue", "mean_nDCG", 0.1, []).to_dict()])
        )
        stub = root / "codex"
        stub.write_text(STUB)
        stub.chmod(0o755)
        barrier = root / "barrier"
        barrier.mkdir()
        store = Mock()
        store.recall.return_value = []
        store.remember.return_value = "memory-id"
        baseline = dict(metrics={"mean_nDCG": 0.5}, snapshot="frozen", config={}, grade_pass=True)
        evaluated = []

        def evaluate(repo, tree, artifacts, budget, real, phase, env):
            self.assertGreater(budget.remaining(), 0)
            index = int(tree.name.rsplit("-candidate-", 1)[1]) if "-candidate-" in tree.name else 1
            if phase == "before":
                return baseline
            evaluated.append(index)
            return dict(
                baseline, metrics={"mean_nDCG": (scores or {}).get(index, 0.5 + index / 10)}
            )

        streams = count if choose_count(count, minutes * 60)["effective"] > 1 else 1
        with (
            patch.dict(
                os.environ,
                {
                    "PATH": str(root) + os.pathsep + os.environ["PATH"],
                    "CHITTA_CODEX_BIN": str(stub),
                    "TEST_BARRIER": str(barrier),
                    "TEST_STREAMS": str(streams),
                    "TEST_FAILURE": failure,
                    "CHITTA_SOCKET_PATH": "/forbidden/live.sock",
                    "CC_SOUL_DB_PATH": "/forbidden/live",
                    "CHITTA_EVAL_MIND": "/forbidden/eval",
                },
            ),
            patch("evolve.cycle.MemoryStore", return_value=store),
            patch("evolve.cycle.replica_env", return_value={}),
            patch("evolve.fanout.replica_env", return_value={}),
            patch("evolve.cycle.evaluate", side_effect=evaluate),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            self.assertEqual(
                main(
                    [
                        "--repo",
                        str(root),
                        "--backlog",
                        str(backlog),
                        "--candidates",
                        str(count),
                        "--max-minutes",
                        str(minutes),
                    ]
                ),
                0,
            )
        path = next(root.glob(".evolve/cycles/*/verdict.json"))
        return json.loads(path.read_text()), store, evaluated, path.parent

    def test_concurrent_worktrees_private_state_best_passing_and_memory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            value, store, evaluated, artifacts = self.run_fixture(root, failure="tests")
            rows = value["candidates"]
            self.assertEqual(len(rows), 3)
            self.assertEqual(value["selected_candidate"], 3)
            self.assertEqual(value["branch"], rows[2]["branch"])
            self.assertEqual(evaluated, [1, 3])
            self.assertEqual(rows[1]["gates"]["tests"], "fail")
            self.assertEqual(rows[1]["delta"], {})
            self.assertEqual(rows[2]["gates"]["immutable"], "pass")
            self.assertAlmostEqual(rows[2]["delta"]["mean_nDCG"], 0.3)
            envs = [
                json.loads((Path(row["artifacts"]) / "environment.json").read_text())
                for row in rows
            ]
            for key in (
                "HOME",
                "CODEX_HOME",
                "XDG_CONFIG_HOME",
                "XDG_DATA_HOME",
                "XDG_CACHE_HOME",
                "XDG_RUNTIME_DIR",
                "XDG_STATE_HOME",
                "CHITTA_DB_PATH",
                "CHITTA_QUEUE",
                "CHITTA_SOCKET_PATH",
            ):
                self.assertEqual(len({env[key] for env in envs}), 3, key)
                for env in envs:
                    self.assertTrue(
                        Path(env[key]).resolve().is_relative_to(artifacts.resolve()), key
                    )
            for row, env in zip(rows, envs):
                self.assertEqual(env["CHITTA_HEADLESS"], "1")
                self.assertEqual(env["CC_SOUL_HEADLESS"], "1")
                self.assertNotIn("CHITTA_EVAL_MIND", env)
                self.assertEqual(
                    git(Path(row["worktree"]), "rev-parse", "HEAD^").strip(), value["base"]
                )
            for tag in ("verdict", "bet-resolution"):
                saved = next(
                    call.args[1] for call in store.remember.call_args_list if call.args[0] == tag
                )
                self.assertEqual(saved["candidates"], rows)
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                report_main(["--cycles", str(artifacts.parent)])
            self.assertIn("+0.3  +0.1  3", output.getvalue())

    def test_equal_delta_prefers_fewer_changed_lines(self):
        with tempfile.TemporaryDirectory() as directory:
            value, _, evaluated, _ = self.run_fixture(
                Path(directory), count=2, scores={1: 0.7, 2: 0.7}
            )
            self.assertEqual(evaluated, [1, 2])
            self.assertLess(
                value["candidates"][0]["changed_lines"], value["candidates"][1]["changed_lines"]
            )
            self.assertEqual(value["selected_candidate"], 1)

    def test_failed_immutable_and_self_check_candidates_are_not_measured(self):
        for failure in ("immutable", "self_check"):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                value, _, evaluated, _ = self.run_fixture(Path(directory), count=2, failure=failure)
                self.assertEqual(evaluated, [1])
                self.assertEqual(value["selected_candidate"], 1)
                self.assertFalse(value["candidates"][1]["gates_passed"])
                self.assertEqual(value["candidates"][1]["outcome"], "failed")

    def test_budget_fallback_is_recorded_with_single_outcome(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            value, _, evaluated, _ = self.run_fixture(root, count=2, minutes=1)
            self.assertEqual(value["fanout"]["effective"], 1)
            self.assertIn("falling back to 1", value["fanout"]["fallback"])
            self.assertEqual(len(value["candidates"]), 1)
            self.assertEqual(evaluated, [1])
            self.assertEqual(len(list((root / ".evolve/worktrees").iterdir())), 1)

    def test_direction_missing_metric_and_failed_candidate_selection(self):
        rows = [
            dict(index=i, gates_passed=True, delta={"latency": d}, changed_lines=10)
            for i, d in enumerate((-0.1, -0.3, -0.2), 1)
        ]
        rows.append(dict(index=4, gates_passed=False, delta={"latency": -1}, changed_lines=1))
        self.assertEqual(
            select_best(rows, dict(metric="latency", direction="decrease"))["index"], 2
        )
        self.assertIsNone(select_best(rows, dict(metric="unmeasured", direction="increase")))
        self.assertIn(
            "n/a",
            table([dict(cycle_id="x", metric="latency", candidates=rows, selected_candidate=None)]),
        )

    def test_timed_out_streams_are_all_recorded_without_evaluation(self):
        with (
            tempfile.TemporaryDirectory() as directory,
            patch("evolve.fanout.agent_output", side_effect=TimeoutError("shared deadline")),
        ):
            value, store, evaluated, _ = self.run_fixture(Path(directory), count=2)
            self.assertIsNone(value["selected_candidate"])
            self.assertEqual(evaluated, [])
            self.assertEqual([r["outcome"] for r in value["candidates"]], ["timeout", "timeout"])
            self.assertEqual(value["verdict"], "inconclusive")
            self.assertEqual(store.remember.call_args.args[1]["candidates"], value["candidates"])

    def test_patch_size_includes_committed_submodule_sources(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            field = root / "chitta-field"
            field.mkdir()
            git(root, "init", "-b", "main")
            git(field, "init", "-b", "main")
            (field / "lib.rs").write_text("// base\n")
            git(field, "add", ".")
            git(field, "commit", "-m", "base")
            git(root, "add", "chitta-field")
            git(root, "commit", "-m", "base")
            base = git(root, "rev-parse", "HEAD").strip()
            (field / "lib.rs").write_text("// replacement\n" * 10)
            git(field, "add", ".")
            git(field, "commit", "-m", "change")
            git(root, "add", "chitta-field")
            git(root, "commit", "-m", "change")
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(changed_lines(root, base, Budget(1)), 13)

    def test_invalid_candidate_count_fails_before_memory_access(self):
        with patch("evolve.cycle.MemoryStore") as memory, contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(main(["--candidates", "0"]), 1)
            self.assertEqual(main(["--candidates", "2", "--implementer", "claude"]), 1)
            memory.assert_not_called()

    def test_admission_and_shared_deadline(self):
        self.assertEqual(choose_count(2, 1200)["effective"], 2)
        self.assertEqual(choose_count(2, 1199)["effective"], 1)
        self.assertEqual(choose_count(2, 1200, real_eval=True)["effective"], 1)
        self.assertEqual(choose_count(1, 1)["effective"], 1)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            budget = Budget(0.002)
            private = CandidateBudget(budget.deadline, isolated_env(root))
            with self.assertRaises(TimeoutError):
                private.run([sys.executable, "-c", "import time; time.sleep(60)"], root)
            self.assertEqual(private.deadline, budget.deadline)
            self.assertLess(time.monotonic() - budget.deadline, 2)


if __name__ == "__main__":
    unittest.main()
