"""Trust boundary tests: split isolation, noise validity, immutable refs, and mining."""

from __future__ import annotations

import importlib.util
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

SMRITI = Path(__file__).resolve().parents[1]
ROOT = SMRITI.parents[1]
sys.path.insert(0, str(SMRITI))

import mine_tasks  # noqa: E402
import runner  # noqa: E402
import scorer  # noqa: E402
import split  # noqa: E402

spec = importlib.util.spec_from_file_location("noise", ROOT / "benchmarks/noise.py")
noise = importlib.util.module_from_spec(spec)
spec.loader.exec_module(noise)


class TestSplits(unittest.TestCase):
    def test_complete_initial_assignments_and_selection(self):
        assignments = split.load(tasks_dir=SMRITI / "tasks")
        self.assertEqual(len(split.select(assignments, [], "visible")), 9)
        self.assertEqual(
            split.select(assignments, [], "holdout"), [f"example-{n:03}" for n in range(10, 16)]
        )
        self.assertEqual(len(split.select(assignments, [], "all")), 15)
        with self.assertRaises(ValueError):
            split.select(assignments, ["example-010"], "visible")
        with self.assertRaises(ValueError):
            split.select(assignments, ["missing"], "all")

    def test_cli_defaults_visible_and_rejects_holdout_override(self):
        with tempfile.TemporaryDirectory() as scratch:
            env = dict(os.environ, SMRITI_SCRATCH=scratch)
            proc = subprocess.run(
                [
                    sys.executable,
                    str(SMRITI / "runner.py"),
                    "--trials",
                    "1",
                    "--condition",
                    "off",
                    "--dry-run",
                ],
                capture_output=True,
                text=True,
                env=env,
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertIn("example-009", proc.stdout)
            self.assertNotIn("example-010", proc.stdout)
            proc = subprocess.run(
                [
                    sys.executable,
                    str(SMRITI / "runner.py"),
                    "--trials",
                    "1",
                    "--task",
                    "example-010",
                    "--dry-run",
                ],
                capture_output=True,
                text=True,
                env=env,
            )
            self.assertEqual(proc.returncode, 2)

    def test_rotation_is_reproducible_balanced_and_audited(self):
        with tempfile.TemporaryDirectory() as scratch:
            directory = Path(scratch)
            events, manifests = [], []
            for index in range(2):
                manifest, history = directory / f"{index}.json", directory / f"{index}.jsonl"
                manifest.write_bytes(split.DEFAULT_PATH.read_bytes())
                event = split.rotate(manifest, history, 2, "seed", "2026-09-13")
                events.append(event)
                manifests.append(split.load(manifest))
                self.assertEqual(json.loads(history.read_text()), event)
                self.assertEqual(sum(s == "holdout" for s in manifests[-1].values()), 6)
                self.assertEqual(sum(manifests[-1][t] != split.load()[t] for t in manifests[-1]), 4)
                self.assertEqual(event["after"], split.fingerprint(manifests[-1]))
            self.assertEqual(events[0], events[1])
            self.assertEqual(manifests[0], manifests[1])
            before = manifest.read_bytes()
            with self.assertRaises(ValueError):
                split.rotate(manifest, history, 7, "seed", "2026-09-13")
            self.assertEqual(manifest.read_bytes(), before)

    def test_recorded_split_survives_rotation_and_legacy_is_labelled(self):
        record = {
            "task_id": "example-010",
            "condition": "off",
            "trial": 0,
            "passed": True,
            "tokens_used": 5,
            "split": "visible",
            "split_hash": "old",
        }
        report = scorer.score([record])
        self.assertEqual(report["per_split"]["visible"]["per_condition"]["off"]["n"], 1)
        self.assertEqual(report["per_split"]["holdout"]["per_condition"], {})
        self.assertEqual(report["split_hashes"], ["old"])
        del record["split"]
        report = scorer.score([record])
        self.assertIn("legacy", report["split_assignment_source"])
        self.assertEqual(report["per_split"]["holdout"]["per_condition"]["off"]["n"], 1)

    def test_resume_rejects_rotated_manifest(self):
        with tempfile.TemporaryDirectory() as scratch:
            path = Path(scratch) / "run.jsonl"
            path.write_text(json.dumps({"split_hash": "stale"}) + "\n")
            with self.assertRaisesRegex(ValueError, "cannot resume"):
                runner.run_all(
                    SMRITI / "tasks",
                    ["example-001"],
                    ["off"],
                    runner.EchoAdapter(),
                    Path(scratch),
                    1,
                    resume_path=path,
                )


class TestNoise(unittest.TestCase):
    def test_sample_sd_band_and_acceptance_threshold(self):
        result = noise.summary([1, 2, 3])
        self.assertEqual(result["mean"], 2)
        self.assertEqual(result["sd"], 1)
        self.assertEqual(result["band_95"], [2 - 1.96, 2 + 1.96])
        self.assertEqual(noise.band({"acceptance_ready": True, "metrics": {"x": result}}, "x"), 2)
        for bad in ([1], [1, float("nan")]):
            with self.assertRaises(ValueError):
                noise.summary(bad)
        with self.assertRaises(ValueError):
            noise.band({"acceptance_ready": False, "metrics": {"x": result}}, "x")

    def test_fixed_task_panel_removes_between_task_variation(self):
        records = [
            {"trial": trial, "condition": condition, "passed": passed, "tokens_used": tokens}
            for trial in range(3)
            for condition in ("off", "on")
            for passed, tokens in ((True, 10), (False, 100))
        ]
        metrics = noise.smriti_metrics(records)
        self.assertEqual(len(metrics), 4)
        self.assertEqual(metrics["smriti.on.sr"]["mean"], 0.5)
        self.assertEqual(metrics["smriti.off.tokens"]["mean"], 55)
        self.assertTrue(all(m["sd"] == 0 for m in metrics.values()))

    def test_strict_recall_is_pinned_read_only_and_fails_closed(self):
        grader = SimpleNamespace(GRADE_REALM="project:test", CHITTA_EVAL_SOCKET="/replica.sock")
        with patch.object(
            noise.subprocess, "run", return_value=SimpleNamespace(stdout='{"results": []}')
        ) as run:
            noise.strict_recall(grader, "query", 20, "hybrid")
            cmd = run.call_args.args[0]
            self.assertIn("--no-learn", cmd)
            self.assertEqual(cmd[-2:], ["--socket-path", "/replica.sock"])
            self.assertTrue(run.call_args.kwargs["check"])
        with patch.object(
            noise.subprocess, "run", return_value=SimpleNamespace(stdout='{"error":"offline"}')
        ):
            with self.assertRaises(ValueError):
                noise.strict_recall(grader, "query", 20)

    def test_echo_smoke_never_calls_chitta_or_agent(self):
        args = SimpleNamespace(
            agent="echo", golden_runs=2, trials=3, tasks=3, task=None, split="visible"
        )
        with (
            patch.dict(os.environ, {}, clear=True),
            patch.object(noise, "golden_runs", return_value=([0.3, 0.4], None)),
            patch.object(subprocess, "run") as process,
            redirect_stdout(io.StringIO()),
        ):
            process.return_value = SimpleNamespace(returncode=1)
            result = noise.calibrate(args)
        self.assertFalse(result["acceptance_ready"])
        self.assertIsNone(result["snapshot_id"])
        self.assertEqual(len(result["smriti_records"]), 18)
        # Only fixture checks may execute; all memory/agent operations are simulated.
        self.assertEqual(process.call_count, 18)
        self.assertTrue(all(call.kwargs.get("shell") for call in process.call_args_list))

    def test_failed_golden_smoke_records_unavailable_not_zero(self):
        args = SimpleNamespace(
            agent="echo", golden_runs=2, trials=3, tasks=3, task=None, split="visible"
        )
        with (
            patch.dict(os.environ, {}, clear=True),
            patch.object(noise, "golden_runs", side_effect=ValueError("offline")),
            patch.object(subprocess, "run") as process,
            redirect_stdout(io.StringIO()),
        ):
            process.return_value = SimpleNamespace(returncode=1)
            result = noise.calibrate(args)
        self.assertFalse(result["acceptance_ready"])
        self.assertEqual(result["metrics"]["golden.ndcg"]["n"], 0)
        self.assertIsNone(result["metrics"]["golden.ndcg"]["sd"])
        self.assertIn("offline", result["errors"][0])
        self.assertEqual(len(result["smriti_records"]), 18)

    def test_real_agent_without_replica_is_rejected_before_any_calls(self):
        args = SimpleNamespace(agent="claude-code", golden_runs=5)
        with patch.dict(os.environ, {}, clear=True), patch.object(noise, "golden_runs") as golden:
            with self.assertRaises(ValueError):
                noise.calibrate(args)
            golden.assert_not_called()


class GitTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.repo = Path(self.temporary.name) / "repo"
        self.repo.mkdir()
        self.git("init", "-q")
        self.git("config", "user.name", "Eval Test")
        self.git("config", "user.email", "eval@example.invalid")
        self.git("config", "core.hooksPath", "/dev/null")
        self.git("config", "commit.gpgsign", "false")

    def git(self, *args):
        return (
            subprocess.check_output(["git", "-C", str(self.repo), *args], stderr=subprocess.STDOUT)
            .decode()
            .strip()
        )

    def write(self, path, content):
        destination = self.repo / path
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(content)

    def commit(self, message):
        self.git("add", ".")
        self.git("commit", "-qm", message)
        return self.git("rev-parse", "HEAD")


class TestImmutability(GitTest):
    def setUp(self):
        super().setUp()
        self.write(
            "benchmarks/EVAL_IMMUTABLE.txt", (ROOT / "benchmarks/EVAL_IMMUTABLE.txt").read_text()
        )
        self.write("benchmarks/smriti/tasks/a/hidden/nested/check.py", "assert True\n")
        self.write("hooks/grade-recall.py", "old\n")
        self.base = self.commit("base")

    def check_refs(self, head):
        return subprocess.run(
            ["bash", str(ROOT / "scripts/check-eval-immutable.sh"), self.base, head],
            cwd=self.repo,
            capture_output=True,
            text=True,
        )

    def test_two_commits_block_then_accept_only_head_approval(self):
        self.write("hooks/grade-recall.py", "new\n")
        head = self.commit("change")
        self.assertEqual(self.check_refs(head).returncode, 1)
        self.git("commit", "--amend", "-qm", "change\n\nEval-Change-Approved: Reviewer")
        self.assertEqual(self.check_refs("HEAD").returncode, 0)
        self.write("ordinary.py", "ok\n")
        self.commit("no approval on head")
        self.assertEqual(self.check_refs("HEAD").returncode, 1)

    def test_nested_hidden_additions_deletions_and_policy_removal(self):
        self.write("benchmarks/smriti/tasks/new/hidden/deep/check.py", "new\n")
        (self.repo / "hooks/grade-recall.py").unlink()
        self.write("benchmarks/EVAL_IMMUTABLE.txt", "# emptied by candidate\n")
        result = self.check_refs(self.commit("remove protections"))
        self.assertEqual(result.returncode, 1)
        self.assertIn("hidden/deep/check.py", result.stdout)
        self.assertIn("hooks/grade-recall.py", result.stdout)

    def test_rename_cannot_escape_and_unprotected_change_passes(self):
        self.write("ordinary.py", "ok\n")
        self.assertEqual(self.check_refs(self.commit("ordinary change")).returncode, 0)
        self.git("mv", "hooks/grade-recall.py", "ordinary.py.bak")
        self.assertEqual(self.check_refs(self.commit("rename gold")).returncode, 1)

    def test_blank_approval_rejected(self):
        self.write("hooks/grade-recall.py", "new\n")
        self.assertEqual(
            self.check_refs(self.commit("change\n\nEval-Change-Approved: ")).returncode, 1
        )


class TestMining(GitTest):
    def test_convention_pair_yields_pre_tree_and_post_test_without_source_writes(self):
        self.write("settings.py", 'DEFAULT_FORMAT = "comma"\n')
        self.write("tests/test_settings.py", 'assert DEFAULT_FORMAT == "comma"\n')
        before = self.commit("initial")
        self.write("settings.py", 'DEFAULT_FORMAT = "pipe"\n')
        self.write("tests/test_settings.py", 'assert DEFAULT_FORMAT == "pipe"\n')
        after = self.commit("default format convention")
        out = Path(self.temporary.name) / "mined"
        original = self.git("status", "--porcelain")
        items = mine_tasks.mine_history(self.repo, out)
        self.assertEqual(len(items), 1)
        candidate = out / "tasks" / items[0]["id"]
        task = json.loads((candidate / "task.json").read_text())
        runner.validate_task(task)
        self.assertEqual(task["provenance"]["before"], before)
        self.assertEqual(task["provenance"]["after"], after)
        self.assertIn('"comma"', (candidate / "repo_fixture/settings.py").read_text())
        self.assertIn('"pipe"', (candidate / "hidden/tests/test_settings.py").read_text())
        self.assertFalse((candidate / "repo_fixture/tests").exists())
        self.assertEqual(original, self.git("status", "--porcelain"))
        self.assertEqual(after, self.git("rev-parse", "HEAD"))

    def test_message_alone_is_not_a_candidate(self):
        self.write("settings.py", 'DEFAULT_FORMAT = "comma"\n')
        self.commit("initial")
        self.write("settings.py", 'DEFAULT_FORMAT = "pipe"\n')
        self.commit("default convention")
        self.assertEqual(mine_tasks.mine_history(self.repo, Path(self.temporary.name) / "out"), [])

    def test_refuse_output_inside_tasks(self):
        self.write("settings.py", "pass\n")
        self.commit("initial")
        result = subprocess.run(
            [
                sys.executable,
                str(SMRITI / "mine_tasks.py"),
                str(self.repo),
                "--out",
                str(self.repo / "benchmarks/smriti/tasks"),
            ],
            capture_output=True,
        )
        self.assertEqual(result.returncode, 2)
        self.assertFalse((self.repo / "benchmarks").exists())

    def test_saddle_counts_and_no_command_execution(self):
        ledger = Path(self.temporary.name) / "ledger.jsonl"
        command = f"touch {self.temporary.name}/MUST_NOT_EXIST"
        ledger.write_text(
            "".join(
                json.dumps(
                    {
                        "event": "bash_outcome",
                        "session_id": "s",
                        "ts": i,
                        "exit_code": 1,
                        "cmd_head": command,
                    }
                )
                + "\n"
                for i in range(3)
            )
        )
        out = Path(self.temporary.name) / "out"
        result = mine_tasks.mine_saddles(self.repo, out, ledger)
        self.assertEqual(result["episodes"], 1)
        self.assertEqual(result["candidates"], 1)
        self.assertFalse((Path(self.temporary.name) / "MUST_NOT_EXIST").exists())
        task_file = next(out.glob("tasks/*/task.json"))
        task = json.loads(task_file.read_text())
        self.assertTrue(task["review_required"])
        self.assertEqual(task["check_cmd"], "bash check.sh")
        runner.validate_task(task)


if __name__ == "__main__":
    unittest.main()
