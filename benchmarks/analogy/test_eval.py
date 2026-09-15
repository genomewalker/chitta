"""Regression tests for unsupported answers and failed negative calls."""
from __future__ import annotations

import unittest
from unittest.mock import patch

import baseline
import run


class EvaluationTests(unittest.TestCase):
    def setUp(self):
        self.edge = dict(id=7, subject="c", predicate="p", object="d", weight=0.9,
                         valid_from_ms=1, source_memory_id=42)
        self.ground = dict(edges=[self.edge], answers=["d"], relations=[dict(predicate="p")],
                           missing_grounding=[])
        self.task = dict(id="test", style="proportional", params={}, expected=[dict(answer="old-label")])
        self.support = dict(triplet_id=7, subject="c", predicate="p", object="d", memory_id=42)
        self.hit = dict(answer="d", predicate="p", id=42, edges=[self.support])

    def evaluate(self, payload):
        with patch.object(run, "rpc_probe", return_value=payload):
            return run.evaluate(self.task, "unused", "unused", "rpc", self.ground)

    def test_complete_answers_replace_incomplete_original_labels(self):
        row = self.evaluate(dict(results=[self.hit], reason=None))
        self.assertTrue(row["hit_at_1"])
        self.assertEqual(row["unsupported_answers"], 0)

    def test_unlinked_answer_or_false_memory_is_unsupported(self):
        for bad in (dict(self.hit, edges=[]), dict(self.hit, id=99),
                    dict(self.hit, edges=[dict(self.support, subject="elsewhere")]),
                    dict(self.hit, edges=[dict(self.support, memory_id=99)])):
            self.assertEqual(self.evaluate(dict(results=[bad]))["unsupported_answers"], 1)

    def test_all_returned_rows_are_audited(self):
        row = self.evaluate(dict(results=[self.hit] * 3 + [dict(answer="unsupported")]))
        self.assertEqual(row["unsupported_answers"], 1)

    def test_negative_error_or_missing_reason_is_never_abstention(self):
        self.task["style"] = "negative"
        self.assertFalse(self.evaluate(dict(results=[]))["abstention_correct"])
        self.assertTrue(self.evaluate(dict(results=[], reason="no_target_relation"))["abstention_correct"])
        with patch.object(run, "rpc_probe", side_effect=ValueError("service error")):
            row = run.evaluate(self.task, "unused", "unused", "rpc", self.ground)
        self.assertFalse(row["abstention_correct"])
        self.assertTrue(row["error"])

    def test_missing_grounding_is_a_miss(self):
        self.ground["missing_grounding"] = [dict(subject="missing")]
        self.assertFalse(self.evaluate(dict(results=[self.hit]))["hit_at_3"])

    def test_exact_join_keeps_all_predicates_and_neighbours(self):
        source = [dict(predicate="p", object="b"), dict(predicate="q", object="b"),
                  dict(predicate="unrelated", object="other")]
        target = [self.edge, dict(self.edge, predicate="q", object="second"),
                  dict(self.edge, predicate="unrelated", object="noise")]
        relations, edges = baseline.join(source, target, "b")
        self.assertEqual(len(relations), 2)
        self.assertEqual({e["object"] for e in edges}, {"d", "second"})


if __name__ == "__main__":
    unittest.main()
