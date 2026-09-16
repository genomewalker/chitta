"""Audit regression cases: capped pagination, missing metadata, dangling provenance."""

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "coverage", Path(__file__).resolve().parents[2] / "scripts/provenance-coverage.py"
)
coverage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(coverage)


class ProvenanceTests(unittest.TestCase):
    def test_source_labels_are_not_sessions(self):
        def trip(predicate, obj):
            return {"subject": "1", "predicate": predicate, "object": obj}

        result = coverage.evidence([trip("source", "distillation")], "1", {"1"})
        self.assertEqual(
            result, {"session": False, "source": True, "covered": True, "writer": "distillation"}
        )
        self.assertFalse(coverage.evidence([trip("source", None)], "1", {"1"})["covered"])
        self.assertFalse(
            coverage.evidence([trip("source_session", "session:")], "1", {"1"})["session"]
        )
        self.assertFalse(coverage.evidence([trip("derived_from", "2")], "1", {"1"})["covered"])
        self.assertTrue(coverage.evidence([trip("derived_from", "2")], "1", {"1", "2"})["covered"])
        self.assertTrue(
            coverage.evidence([trip("source_session", "session:abc")], "1", {"1"})["session"]
        )

    def test_seven_day_boundaries_and_unknown_dates(self):
        now = 10 * 86400000
        rows = [
            dict(covered=True, session=False, source=True, writer="writer", created_at_ms=ts)
            for ts in (now, now - 7 * 86400000, now - 7 * 86400000 - 1, None, now + 1)
        ]
        report = coverage.summarize(rows, now)
        self.assertEqual(report["overall"]["total"], 5)
        self.assertEqual(report["last_7_days"]["total"], 2)
        self.assertEqual(report["unknown_creation_time"], 1)
        self.assertEqual(report["future_creation_time"], 1)

    def test_server_page_cap_is_not_eof(self):
        class FakeCLI:
            def call(self, name, **args):
                if name == "list_memories_brief":
                    # A server may return fewer than the requested 1000.
                    return {"memories": [{"id": args["offset"] + 1}] if args["offset"] < 2 else []}
                if name == "memory_provenance":
                    return {"meta": {"id": args["id"], "created_at_ms": 1}}
                return {"triplets": []}

        result = coverage.audit(FakeCLI(), 100, 1000)
        self.assertEqual(result["overall"]["total"], 2)
        self.assertTrue(result["coverage_is_lower_bound"])
        self.assertFalse(result["session_coverage_complete"])
        self.assertEqual(result["memories_exposing_session_field"], 0)


if __name__ == "__main__":
    unittest.main()
