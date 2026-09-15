from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from evolve.proposals import candidate, dedupe, normalize, persist, telemetry  # noqa: E402


class ProposalTests(unittest.TestCase):
    def test_normalization_dedupe_and_validation(self):
        first = candidate("  Fix   Recall ", "BOUND work", "recall_empty_rate", -0.2, ["one"])
        card = first.to_dict()
        card.update(id="external-id", title="fix recall", mechanism="bound work", evidence=["two"])
        second = normalize(card)
        self.assertEqual(first.id, second.id)
        self.assertEqual(dedupe([first, second])[0].evidence, ["one", "two"])
        for effort in (0, -1, float("nan")):
            card["cost"]["effort_h"] = effort
            with self.assertRaises(ValueError):
                normalize(card)

    def test_persist_dedupes_existing_memory(self):
        proposal = candidate("fix", "bound", "metric", 0.1, [])
        store = Mock()
        store.recall.return_value = [{"id": "mem-1", "content": json.dumps(proposal.to_dict())}]
        self.assertEqual(persist([proposal], store)[proposal.id], "mem-1")
        store.remember.assert_not_called()
        store.recall.return_value = []
        store.remember.return_value = "mem-2"
        self.assertEqual(persist([proposal], store)[proposal.id], "mem-2")
        self.assertEqual(store.remember.call_args.args[0], "proposal")

    def test_telemetry_denominators_and_unknown_evidence_size(self):
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            ledger, shadow = repo / "ledger", repo / "shadow"
            rows = [
                {"event": "injected", "lane_timeout": {"sem": True}},
                {"event": "injected", "lane_timeout": {"sem": False}},
                {"event": "recall_empty"},
                {"event": "bash_outcome", "exit_code": 1},
                {"event": "bash_outcome", "exit_code": 0},
                {"event": "unrelated"},
            ]
            ledger.write_text("\n".join(json.dumps(row) for row in rows) + "\nbroken")
            shadow.write_text(
                json.dumps({"accept": True})
                + "\n"
                + json.dumps({"accept": False, "evidence_bytes": 40000})
            )
            store = Mock(timeout=1)
            store.call.return_value = {"rpc_over_budget": 8}
            proposals = telemetry(repo, ledger, shadow, store, [])
            metrics = {p.expected_gain["metric"]: p for p in proposals}
            self.assertAlmostEqual(metrics["recall_empty_rate"].evidence[0]["rate"], 1 / 3)
            self.assertEqual(metrics["lane_timeout.sem"].evidence[0]["rate"], 0.5)
            self.assertEqual(metrics["bash_failure_rate"].evidence[0]["rate"], 0.5)
            self.assertIn("mdl_accept_rate.unknown", metrics)
            self.assertIn("mdl_accept_rate.large", metrics)

    def test_defaults_enums_and_derived_evidence(self):
        card = candidate("fix", "bound", "metric", 0.1, []).to_dict()
        del card["verifiability"], card["prior_effort"]
        proposal = normalize(card)
        self.assertEqual((proposal.verifiability, proposal.prior_effort), ("metric_only", "none"))
        self.assertFalse(proposal.internal_evidence)
        for evidence, expected in [
            ([{"source": s} for s in ("telemetry", "ledger", "memory")], True),
            ([{"source": "telemetry"}, {"source": "https://paper.test"}], False),
            ([{"path": "ledger"}], False),
            (["memory"], False),
        ]:
            card.update(evidence=evidence, internal_evidence=not expected)
            self.assertEqual(normalize(card).to_dict()["internal_evidence"], expected)
        for field in ("verifiability", "prior_effort"):
            for value in ("invalid", None, [], True):
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    normalize(dict(card, **{field: value}))

    def test_existing_cards_load_with_honest_factors(self):
        paths = list((Path(__file__).resolve().parents[1] / "evolve/proposals.d").glob("*.json"))
        self.assertGreaterEqual(len(paths), 3)
        for path in paths:
            proposal = normalize(json.loads(path.read_text()))
            self.assertEqual(proposal.source, "hypothesis")
            self.assertEqual(proposal.prior_effort, "none")
            self.assertFalse(proposal.internal_evidence)
            self.assertEqual(
                proposal.verifiability,
                "self_verifying" if "Slot-Based" in proposal.title else "metric_only",
            )
