from __future__ import annotations

import math
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from evolve.proposals import candidate  # noqa: E402
from evolve.selector import choose, rank  # noqa: E402


class SelectTests(unittest.TestCase):
    def setUp(self):
        self.strong = candidate("strong", "bound", "metric", -0.8, [])
        self.weak = candidate("weak", "bound", "metric", 0.001, [], "hypothesis")

    def test_order_and_ucb_tries(self):
        self.assertEqual(rank([self.weak, self.strong], [])[0][0], self.strong)
        verdicts = [{"proposal_id": self.strong.id}] * 20
        rows = rank([self.strong, self.weak], verdicts)
        self.assertEqual(rows[0][0], self.weak)
        self.assertAlmostEqual(rows[0][1], 0.00025 + math.sqrt(math.log(21)))

    def test_quota_every_prefix_and_missing_hypotheses(self):
        verdicts = []
        for n in range(1, 31):
            selected = choose([self.strong, self.weak], verdicts, 0.3, c=0)
            verdicts.append(dict(proposal_id=selected.id, source=selected.source))
            self.assertGreaterEqual(
                sum(v["source"] == "hypothesis" for v in verdicts), math.ceil(0.3 * n - 1e-12)
            )
        self.assertEqual(sum(v["source"] == "hypothesis" for v in verdicts), 9)
        self.assertEqual(choose([self.strong], [], 1), self.strong)
        self.assertEqual(choose([self.strong, self.weak], [], 0), self.strong)
        for quota in (-0.1, 1.1, float("nan")):
            with self.assertRaises(ValueError):
                choose([self.strong], [], quota)
