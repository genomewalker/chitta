"""Guard the benchmark's graph-only answer selection and conservative byte cost."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("codenav_run", Path(__file__).with_name("run.py"))
BENCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BENCH)


class ScoringTrustTest(unittest.TestCase):
    def test_ground_truth_reads_cannot_turn_a_miss_into_a_hit(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "answer.py").write_text("source contents are not retrieval evidence\n")
            calls = []

            def rpc(_socket, tool, args):
                calls.append((tool, args))
                if tool == "read_symbol":
                    return {"code": "the correct answer body"}
                if "question" not in args:
                    return {"text": "injected block"}
                return {
                    "indexed": True,
                    "symbols": [{"file": str(root / "wrong.py"), "name": "expected"}],
                    "text": "query block",
                }

            question = {
                "id": "test",
                "question": "find implementation",
                "file": "answer.py",
                "symbol": "expected",
            }
            with patch.object(BENCH, "rpc", rpc):
                result = BENCH.evaluate("unused", root, [question], repeats=2)
            self.assertEqual(result["score"], 0)
            self.assertEqual(
                result["with_bytes"], len("query blockinjected blockthe correct answer body")
            )
            self.assertEqual(result["without_bytes"], (root / "answer.py").stat().st_size)
            for tool, args in calls:
                if tool == "code_query" and "question" in args:
                    self.assertEqual(
                        args, {"question": "find implementation", "path": str(root), "limit": 8}
                    )

    def test_different_repeated_graph_answers_fail(self):
        replies = iter(
            [{"indexed": True, "symbols": []}, {"indexed": True, "symbols": [], "changed": True}]
        )
        with patch.object(BENCH, "rpc", lambda *_: next(replies)):
            with self.assertRaisesRegex(AssertionError, "nondeterministic"):
                BENCH.evaluate(
                    "unused", Path("/unused"), [{"id": "test", "question": "navigate"}], repeats=2
                )


if __name__ == "__main__":
    unittest.main()
