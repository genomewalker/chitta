from __future__ import annotations

import contextlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from evolve.cycle import Budget, main, parse_survey, self_check_in_diff  # noqa: E402
from evolve.proposals import candidate  # noqa: E402
from evolve.selector import effort_from_history  # noqa: E402


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
    ).stdout.strip()


class SurveyTests(unittest.TestCase):
    def setUp(self):
        self.cards = [candidate(str(n), "mechanism " + str(n), "metric", 0.1, []) for n in range(3)]
        self.valid = dict(
            chosen=self.cards[1].id,
            tractability=0.6,
            abandoned=[
                dict(id=p.id, reason="costly check") for p in (self.cards[0], self.cards[2])
            ],
        )

    def test_strict_survey_response(self):
        for text in (json.dumps(self.valid), "```json\n" + json.dumps(self.valid) + "\n```"):
            self.assertEqual(parse_survey(text, self.cards), self.valid)
        invalid = [
            dict(self.valid, chosen="unknown"),
            dict(self.valid, chosen=[]),
            dict(self.valid, abandoned=[]),
            dict(self.valid, abandoned=self.valid["abandoned"] * 2),
            dict(self.valid, extra=True),
            dict(self.valid, chosen=None),
            dict(self.valid, abandoned=[dict(id=self.cards[0].id, reason=" ")]),
        ]
        invalid.extend(
            dict(self.valid, tractability=x)
            for x in (-1, 2, True, "0.5", float("nan"), float("inf"))
        )
        for value in invalid:
            with self.subTest(value=value), self.assertRaises(ValueError):
                parse_survey(json.dumps(value), self.cards)
        for text in (
            "[]",
            "{}",
            json.dumps(self.valid) + "\nprose",
            '{"chosen":null,"chosen":null}',
        ):
            with self.assertRaises(ValueError):
                parse_survey(text, self.cards)

    def test_skip_timeout_and_invalid_survey_never_register_or_implement(self):
        for mode in ("skip", "timeout", "invalid", "dirty", "memory_failure"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                git(root, "init", "-b", "main")
                git(root, "commit", "--allow-empty", "-m", "base")
                cards = [self.cards[0], replace(self.cards[1], prior_effort="some")]
                backlog = root / "backlog.json"
                backlog.write_text(json.dumps([p.to_dict() for p in cards]))
                store = Mock()
                store.recall.return_value = []
                store.remember.return_value = "verdict-id"
                if mode == "memory_failure":
                    store.remember.side_effect = RuntimeError("memory offline")

                def survey(
                    args, worktree, prompt, artifacts, phase, budget, mode=mode, cards=cards
                ):
                    self.assertEqual(phase, "survey")
                    self.assertLessEqual(budget.remaining(), 0.05 * 60)
                    if mode == "timeout":
                        raise TimeoutError("survey cap reached")
                    if mode == "invalid":
                        return "not json"
                    if mode == "dirty":
                        (worktree / "unwanted").write_text("mutation")
                    return json.dumps(
                        dict(
                            chosen=None,
                            tractability=0,
                            abandoned=[dict(id=p.id, reason="too costly") for p in cards],
                        )
                    )

                with (
                    patch("evolve.cycle.MemoryStore", return_value=store),
                    patch("evolve.cycle.agent_output", side_effect=survey) as agent,
                    contextlib.redirect_stdout(io.StringIO()),
                ):
                    self.assertEqual(
                        main(
                            [
                                "--repo",
                                str(root),
                                "--backlog",
                                str(backlog),
                                "--survey-minutes",
                                "0.05",
                                "--max-minutes",
                                "1",
                            ]
                        ),
                        1 if mode == "memory_failure" else 0,
                    )
                self.assertEqual(agent.call_count, 1)
                self.assertEqual([c.args[0] for c in store.remember.call_args_list], ["verdict"])
                path = next(root.glob(".evolve/cycles/*/verdict.json"))
                result = json.loads(path.read_text())
                self.assertFalse((path.parent / "spec.md").exists())
                if mode in ("skip", "memory_failure"):
                    self.assertEqual(result["verdict"], "skipped:no_tractable_candidate")
                    self.assertEqual(effort_from_history(cards[0], [result]), "some")
                    self.assertEqual(effort_from_history(cards[1], [result]), "exhausted")
                    self.assertEqual(
                        json.loads((path.parent / "survey.json").read_text()), result["survey"]
                    )
                else:
                    self.assertEqual(result["verdict"], "inconclusive")
                    self.assertNotIn("prior_effort_updates", result)

    def test_survey_budget_is_capped_by_cycle(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            git(root, "init", "-b", "main")
            git(root, "commit", "--allow-empty", "-m", "base")
            backlog = root / "backlog.json"
            backlog.write_text(json.dumps([self.cards[0].to_dict()]))
            store = Mock()
            store.recall.return_value = []
            store.remember.return_value = "verdict-id"

            def survey(*args):
                self.assertLessEqual(args[-1].remaining(), 3)
                raise TimeoutError("cycle deadline")

            with (
                patch("evolve.cycle.MemoryStore", return_value=store),
                patch("evolve.cycle.agent_output", side_effect=survey),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                self.assertEqual(
                    main(
                        [
                            "--repo",
                            str(root),
                            "--backlog",
                            str(backlog),
                            "--max-minutes",
                            "0.05",
                            "--survey-minutes",
                            "20",
                        ]
                    ),
                    0,
                )


class SelfCheckTests(unittest.TestCase):
    def test_claim_must_name_changed_test_in_touched_module(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            git(root, "init", "-b", "main")
            test = root / "module/tests/test_bounds.py"
            test.parent.mkdir(parents=True)
            test.write_text(
                "def test_old():\n    assert 1 == 1\n\ndef test_other():\n    assert 2 == 2\n"
            )
            git(root, "add", ".")
            git(root, "commit", "-m", "base")
            base = git(root, "rev-parse", "HEAD")
            (root / "module/queue.py").write_text("capacity = 2\n")
            test.write_text(test.read_text().replace("assert 2 == 2", "assert 2 <= 3"))
            git(root, "add", ".")
            git(root, "commit", "-m", "candidate")
            paths = ["module/queue.py", "module/tests/test_bounds.py"]

            def check(claim, changed=paths):
                with contextlib.redirect_stdout(io.StringIO()):
                    return self_check_in_diff(claim, root, base, changed, Budget(1))

            valid = "SELF_CHECK: module/tests/test_bounds.py::test_other"
            self.assertEqual(check(valid), valid.removeprefix("SELF_CHECK: "))
            for claim in (
                "Done",
                valid + "\n" + valid,
                "SELF_CHECK: module/tests/test_bounds.py::test_old",
                "SELF_CHECK: module/tests/test_bounds.py::test_missing",
                "SELF_CHECK: ../module/tests/test_bounds.py::test_other",
                "SELF_CHECK: module/queue.py::test_other",
            ):
                self.assertIsNone(check(claim))
            self.assertIsNone(check(valid, [paths[1], "unrelated/queue.py"]))
            # Adding a comment with a plausible test name must not supply a test.
            test.write_text(test.read_text() + "\n# def test_fake(): pass\n")
            git(root, "add", ".")
            git(root, "commit", "-m", "comment")
            self.assertIsNone(check("SELF_CHECK: module/tests/test_bounds.py::test_fake"))
            test.write_text(
                test.read_text()
                + "\nclass Bounds:\n    def test_new(self):\n        assert 3 > 2\n"
            )
            git(root, "add", ".")
            git(root, "commit", "-m", "new method")
            self.assertTrue(check("SELF_CHECK: module/tests/test_bounds.py::Bounds::test_new"))

    def test_native_test_functions_and_document_impostors(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            git(root, "init", "-b", "main")
            git(root, "commit", "--allow-empty", "-m", "base")
            base = git(root, "rev-parse", "HEAD")
            (root / "module/tests").mkdir(parents=True)
            (root / "module/queue.cpp").write_text("// changed mechanism")
            source = "void test_bounds() {\n    CHECK(size <= cap);\n}\n"
            for suffix in ("cpp", "md"):
                (root / ("module/tests/test_bounds." + suffix)).write_text(source)
            git(root, "add", ".")
            git(root, "commit", "-m", "checks")
            paths = [
                "module/queue.cpp",
                "module/tests/test_bounds.cpp",
                "module/tests/test_bounds.md",
            ]
            for suffix, expected in (("cpp", True), ("md", False)):
                with contextlib.redirect_stdout(io.StringIO()):
                    result = self_check_in_diff(
                        "SELF_CHECK: module/tests/test_bounds." + suffix + "::test_bounds",
                        root,
                        base,
                        paths,
                        Budget(1),
                    )
                self.assertEqual(bool(result), expected)
