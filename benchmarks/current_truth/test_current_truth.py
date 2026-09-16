"""Offline scoring, failure handling, endpoint isolation, and provenance regressions."""

from __future__ import annotations

import importlib.util
import json
import os
import socket
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import run


class CurrentTruthTests(unittest.TestCase):
    def test_panel_and_pinned_citations(self):
        panel = run.load_panel()
        root = Path(__file__).resolve().parents[2]
        for q in panel["questions"] + panel["regression_fixtures"]:
            path = q["citation"].split("#")[0].split(":")[0].strip()
            source = subprocess.check_output(
                ["git", "show", f"{panel['source_commit']}:{path}"], cwd=root, text=True
            )
            if "#" in q["citation"]:
                self.assertIn(q["citation"].split("#", 1)[1].strip(), source)
            else:
                self.assertLessEqual(int(q["citation"].rsplit(":", 1)[1]), len(source.splitlines()))

    def test_matchers_and_top_three(self):
        q = run.load_panel()["questions"][0]
        hit = {"id": "18446744073709551615", "content": q["expected_answer"]}
        self.assertTrue(run.score(q, [hit])["correct"])
        self.assertFalse(run.score(q, [{"id": 1, "content": "unrelated"}] * 3 + [hit])["correct"])
        self.assertTrue(run.match({"type": "ids", "value": [hit["id"]]}, hit))
        self.assertFalse(run.match({"type": "exact", "value": "not present"}, hit))

    def test_abstention_and_conflict(self):
        q = dict(run.load_panel()["questions"][24])
        q["wrong_answer_traps"] = [{"type": "regex", "value": r"p99=\d+"}]
        self.assertEqual(run.score(q, [])["outcome"], "abstain-correct")
        self.assertEqual(run.score(q, [{"id": 1, "text": "p99=717"}])["outcome"], "wrong-confident")
        q["expected_answer"] = "717"
        q["matcher"] = {"type": "exact", "value": "717"}
        row = run.score(q, [{"id": 1, "content": "p99=717"}])
        self.assertFalse(row["correct"])
        self.assertEqual(run.aggregate([row])["utility"], -2)

    def test_transport_and_envelope_fail_closed(self):
        for envelope in (None, {}, {"error": "oops"}, {"results": [{}]}, {"results": [1]}):
            with patch.object(run.subprocess, "run") as mock:
                mock.return_value.stdout = json.dumps(envelope)
                with self.assertRaises(ValueError):
                    run.recall("q", "/private/socket")
        with patch.object(
            run.subprocess, "run", side_effect=subprocess.TimeoutExpired("chitta", 60)
        ):
            with self.assertRaises(subprocess.TimeoutExpired):
                run.recall("q", "/private/socket")

    def test_recall_no_learn_and_json(self):
        with patch.object(run.subprocess, "run") as mock:
            mock.return_value.stdout = '{"results": []}'
            self.assertEqual(run.recall("question", "/private/socket"), [])
            cmd = mock.call_args.args[0]
            for flag in ("--no-learn", "--json", "--realm", "--socket-path"):
                self.assertIn(flag, cmd)
            self.assertEqual(cmd[cmd.index("--limit") + 1], "3")
            self.assertEqual(mock.call_args.kwargs["env"]["CHITTA_CLI_AUTOSTART"], "0")

    def test_socket_rejects_live_and_symlink(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            mind = base / "eval"
            mind.mkdir()
            with socket.socket(socket.AF_UNIX) as sock:
                sock.bind(str(base / "live.sock"))
                alias = mind / "alias.sock"
                alias.symlink_to(base / "live.sock")
                with patch.dict(
                    os.environ,
                    {"CHITTA_EVAL_MIND": str(mind), "CHITTA_EVAL_SOCKET": str(alias)},
                    clear=True,
                ):
                    with self.assertRaises(ValueError):
                        run.endpoint()
                    self.assertEqual(run.endpoint(True), str(base / "live.sock"))
            with socket.socket(socket.AF_UNIX) as sock:
                sock.bind(str(mind / "replica.sock"))
                env = {
                    "CHITTA_EVAL_MIND": str(mind),
                    "CHITTA_EVAL_SOCKET": str(mind / "replica.sock"),
                }
                with patch.dict(os.environ, env, clear=True):
                    self.assertEqual(run.endpoint(), str(mind / "replica.sock"))
                    os.environ["CHITTA_LIVE_MIND"] = str(mind)
                    with self.assertRaises(ValueError):
                        run.endpoint()

    def test_noise_uses_panel_samples(self):
        spec = importlib.util.spec_from_file_location(
            "truth_noise", Path(__file__).parents[1] / "noise.py"
        )
        noise = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(noise)
        stats = noise.summary([0.25, 0.5, 0.75])
        self.assertEqual(stats["mean"], 0.5)
        self.assertEqual(stats["sd"], 0.25)
        self.assertEqual(
            noise.band(
                {"acceptance_ready": True, "metrics": {"current_truth.p3": stats}},
                "current_truth.p3",
            ),
            0.5,
        )


if __name__ == "__main__":
    unittest.main()
