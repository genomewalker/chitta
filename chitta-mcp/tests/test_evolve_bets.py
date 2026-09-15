from __future__ import annotations

import sys
import unittest
from pathlib import Path
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from evolve.bets import outcome, register, resolve  # noqa: E402
from evolve.proposals import candidate  # noqa: E402


class BetTests(unittest.TestCase):
    def setUp(self):
        self.store = Mock()
        self.store.remember.return_value = "bet-id"
        self.proposal = candidate("reduce", "bound work", "failure_rate", -0.2, [])
        self.ident, self.bet = register(
            self.proposal, 0.05, self.store, metric_bands={"mean_nDCG": 0.03}
        )

    def test_registration_precedes_resolution_and_links(self):
        self.assertEqual(self.bet["direction"], "decrease")
        self.assertIn("registered_ts", self.bet)
        result = resolve(self.ident, -0.18, self.store, bet=self.bet)
        self.assertEqual(result["outcome"], "confirmed")
        self.assertEqual(result["bet_id"], "bet-id")
        self.assertEqual(
            [c.args[0] for c in self.store.remember.call_args_list],
            ["forward-bet", "bet-resolution"],
        )

    def test_refuted_wrong_direction_noise_or_wrong_magnitude(self):
        for delta in (0.2, -0.01, -0.5):
            self.assertEqual(outcome(self.bet, delta)["outcome"], "refuted")
        self.bet["band_from_noise"] = None
        with self.assertRaises(ValueError):
            outcome(self.bet, -0.2)

    def test_surprise_only_on_unpredicted_banded_metric(self):
        measured = {"failure_rate": -0.2, "mean_nDCG": 0.08}
        result = outcome(self.bet, measured, "cycle evidence", "existing wisdom")
        self.assertEqual(result["outcome"], "surprise")
        self.assertEqual(result["unexpected"], {"mean_nDCG": 0.08})
        self.assertFalse(result["novel"])
        self.assertEqual(set(result), {"outcome", "measured_delta", "unexpected", "novel"})
        self.assertFalse(outcome(self.bet, measured, "short evidence")["novel"])
        self.assertEqual(
            outcome(self.bet, {"failure_rate": -0.2, "unbanded": 999})["outcome"], "confirmed"
        )
