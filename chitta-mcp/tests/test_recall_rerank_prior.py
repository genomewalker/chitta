from __future__ import annotations

import math
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
try:
    import server
except ImportError as exc:  # test-hooks CI job has no MCP SDK
    raise unittest.SkipTest(f"MCP SDK unavailable: {exc}") from exc


def _blend(logit: float, hit: dict) -> float:
    return (1.0 / (1.0 + math.exp(-logit))) * server._kind_envelope(hit)


class RerankPriorTests(unittest.TestCase):
    def test_kind_prior_breaks_near_ties_but_not_strong_relevance(self):
        correction = {"type": "correction"}
        operational = {"type": "operational"}
        # Equal cross-encoder score: the correction wins.
        self.assertGreater(_blend(1.0, correction), _blend(1.0, operational))
        # A clearly more relevant fragment still wins on relevance alone.
        self.assertGreater(_blend(3.0, operational), _blend(0.0, correction))
        # Unknown kinds get the neutral envelope.
        self.assertEqual(server._kind_envelope({"type": "signal"}), 1.0)


if __name__ == "__main__":
    unittest.main()
