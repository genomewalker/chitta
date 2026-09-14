from __future__ import annotations

import json
import math
import sys
import unittest
from dataclasses import replace
from pathlib import Path
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from evolve.proposals import candidate  # noqa: E402
from evolve.selector import choose, effort_from_history, history, rank, shortlist  # noqa: E402


class SelectTests(unittest.TestCase):
    def setUp(self):
        self.strong = candidate("strong", "bound", "metric", -0.8, [])
        self.weak = candidate("weak", "bound", "metric", 0.001, [], "hypothesis")

    def test_order_and_ucb_tries(self):
        self.assertEqual(rank([self.weak, self.strong], [])[0][0], self.strong)
        verdicts = [{"proposal_id": self.strong.id}] * 20
        rows = rank([self.strong, self.weak], verdicts)
        self.assertEqual(rows[0][0], self.weak)
        self.assertAlmostEqual(rows[0][1], 0.00025 * 0.7 + math.sqrt(math.log(21)))

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

    def test_each_factor_flips_ranking(self):
        high = self.strong
        low = candidate("lower", "other", "metric", 0.6, [])
        for field, value in (("verifiability", "judgement"), ("prior_effort", "some")):
            self.assertEqual(rank([high, low], [], c=0)[0][0], high)
            self.assertEqual(rank([replace(high, **{field: value}), low], [], c=0)[0][0], low)
        close = replace(
            low, expected_gain=dict(low.expected_gain, delta=0.7), evidence=[{"source": "ledger"}]
        )
        self.assertEqual(rank([high, close], [], c=0)[0][0], close)
        self_verifying = replace(low, verifiability="self_verifying")
        self.assertEqual(rank([high, self_verifying], [], c=0)[0][0], self_verifying)

    def test_exhausted_can_only_win_through_quota_even_with_huge_utility(self):
        exhausted = replace(
            self.strong,
            prior_effort="exhausted",
            source="hypothesis",
            expected_gain=dict(self.strong.expected_gain, delta=100),
        )
        self.assertEqual(rank([exhausted, self.weak], [], c=0)[0][0], exhausted)
        self.assertEqual(choose([exhausted, self.weak], [], quota=0, c=0), self.weak)
        self.assertEqual(choose([exhausted, self.weak], [], quota=1, c=0), exhausted)
        self.assertEqual(shortlist([exhausted, self.weak], [], quota=0, c=0), [self.weak])
        with self.assertRaises(ValueError):
            choose([exhausted], [], quota=0)
        exhausted = replace(exhausted, source="telemetry")
        with self.assertRaises(ValueError):
            choose([exhausted], [], quota=1)

    def test_history_refutations_match_mechanism_despite_renamed_title(self):
        store = Mock()
        one = dict(
            cycle_id="1",
            proposal_id="old-title-id",
            mechanism=" BOUND ",
            verdict="inconclusive",
            resolution={"outcome": "refuted"},
        )
        two = dict(one, cycle_id="2")
        store.recall.return_value = [
            dict(id=n, content=json.dumps(v)) for n, v in enumerate((one, one, two))
        ]
        verdicts = history(store)
        self.assertEqual(len(verdicts), 2)
        self.assertEqual(effort_from_history(self.strong, verdicts[:1]), "some")
        self.assertEqual(effort_from_history(self.strong, verdicts), "exhausted")
        self.assertEqual(rank([self.strong], verdicts)[0][0].prior_effort, "exhausted")
        self.assertEqual(effort_from_history(self.strong, [dict(one, mechanism="other")]), "none")
        self.assertEqual(effort_from_history(self.strong, [dict(one, resolution=None)]), "none")
        legacy = dict(proposal_id=self.strong.id, verdict="refuted")
        self.assertEqual(effort_from_history(self.strong, [legacy]), "some")

    def test_skipped_survey_bumps_effort_without_spending_quota(self):
        skipped = dict(
            cycle_id="skip",
            proposal_id=None,
            verdict="skipped:no_tractable_candidate",
            survey={"chosen": None},
            prior_effort_updates=[
                dict(id=self.strong.id, mechanism=self.strong.mechanism, prior_effort="some")
            ],
        )
        store = Mock()
        store.recall.return_value = [dict(content=json.dumps(skipped))] * 2
        self.assertEqual(len(history(store)), 1)
        self.assertEqual(effort_from_history(self.strong, history(store)), "some")
        self.assertEqual(choose([self.strong, self.weak], history(store), quota=1).id, self.weak.id)
        skipped["prior_effort_updates"][0]["prior_effort"] = "exhausted"
        self.assertEqual(effort_from_history(self.strong, [skipped]), "exhausted")

    def test_survey_candidates_preserve_quota(self):
        self.assertEqual(shortlist([self.strong, self.weak], [], quota=1), [self.weak])
        self.assertEqual(shortlist([self.strong, self.weak], [], quota=0, k=1), [self.strong])

    def test_one_abandonment_event_per_mechanism_even_with_aliases(self):
        event = dict(
            verdict="skipped:no_tractable_candidate",
            prior_effort_updates=[
                dict(id=p.id, mechanism=p.mechanism, prior_effort="some")
                for p in (self.strong, self.weak)
            ],
        )
        self.assertEqual(effort_from_history(self.strong, [event]), "some")
