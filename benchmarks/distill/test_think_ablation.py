"""Checks for the quality gate's relation accounting."""
import unittest
import contextlib
import io
import json
import tempfile
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from think_ablation import agreement, relation_summary, analyze


class AgreementTest(unittest.TestCase):
    def test_coverage_counts_memories_and_deduplicates(self):
        t = ["a", "uses", "b"]
        self.assertEqual(relation_summary([[t, t], []]), {
            "n": 2, "relations": 1, "relations_per_memory": .5,
            "covered_memories": 1, "coverage": .5})
        self.assertIsNone(relation_summary([])["coverage"])

    def test_control_is_independent_and_incomplete_review_keeps_thinking_on(self):
        with tempfile.TemporaryDirectory(prefix="p13-analysis-", dir="/projects/caeg/scratch/kbd606/tmp") as temp:
            root = Path(temp)
            (root / "manifest.json").write_text('{}')
            (root / "parser").write_text('stub')
            for folder in ("responses", "control-responses"):
                (root / folder).mkdir()
            for folder, think, content in (("responses", True, "baseline"),
                                          ("responses", False, "baseline"),
                                          ("control-responses", True, "different")):
                row = {"id": "a", "think": think, "num_predict": 8192,
                       "latency_seconds": 1, "generated_tokens_including_thinking": 10,
                       "thinking_chars": 0, "ssl_chars": 8, "ssl_lines": {"PATTERN": 1},
                       "response": {"done_reason": "stop", "message": {"content": content}}}
                (root / folder / f"a-{int(think)}-8192.json").write_text(json.dumps(row))
            with patch("think_ablation.parse", side_effect=lambda binary, body: [[body, "uses", "b"]]):
                with contextlib.redirect_stdout(io.StringIO()):
                    analyze(SimpleNamespace(output=root, parser=root / "parser"), {"rule": "original"})
            report = json.loads(next(root.glob("report-*.json")).read_text())
            self.assertEqual(report["comparisons"]["8192"]["relaxed"]["f1"], 1)
            self.assertEqual(report["self_agreement"]["relaxed"]["f1"], 0)
            self.assertEqual(report["arms"]["True-8192"]["n"], 1)
            self.assertEqual(report["arms"]["control-True-8192"]["n"], 1)
            self.assertFalse(report["self_agreement"]["valid"])
            self.assertTrue(report["default_think"])

    def test_empty_is_not_evidence(self):
        self.assertEqual(agreement([([], [])], True)["f1"], 0)

    def test_relaxed_normalizes_not_semantics(self):
        pairs = [([['Build_Cache', 'leads_to', 'Fast-build']],
                  [['build cache', 'leads_to', 'fast build']])]
        self.assertEqual(agreement(pairs, False)["f1"], 0)
        self.assertEqual(agreement(pairs, True)["f1"], 1)
        self.assertEqual(agreement([([['a', 'uses', 'b']], [['a', 'causes', 'b']])], True)["f1"], 0)

    def test_dedup_is_per_memory(self):
        t = ['a', 'uses', 'b']
        self.assertEqual(agreement([([t, t], [t]), ([t], [])], False),
                         {"matched": 1, "on": 2, "off": 1, "f1": 2 / 3})


if __name__ == '__main__':
    unittest.main()
