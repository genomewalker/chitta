"""Local output cache and token context regression tests (no daemon)."""

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "chitta-mcp"))
from hook_tokens import weekly_line


class OutputTokensTest(unittest.TestCase):
    def test_cache_missing_invalid_and_valid(self):
        with tempfile.TemporaryDirectory() as tmp:
            state = Path(tmp)
            self.assertEqual(weekly_line(state), "")
            path = state / "token-ledger.json"
            path.write_text("broken")
            self.assertEqual(weekly_line(state), "")
            path.write_text(
                json.dumps(
                    {
                        "agents": {
                            "fable": {
                                "sessions": 12,
                                "avg_context_per_turn": 150000,
                                "total_cost": 34.5,
                            },
                            "astra": {"sessions": 7, "total_cost": 6},
                        }
                    }
                )
            )
            self.assertEqual(
                weekly_line(state),
                "[tokens] this week: fable 12s 150k/req $34.50; astra 7s $6.00\n",
            )

    def test_daily_cache_reuses_fresh_report(self):
        script = Path(__file__).resolve().parents[2] / "scripts/token-ledger.py"
        with tempfile.TemporaryDirectory() as tmp:
            cache = Path(tmp) / "token-ledger.json"
            env = dict(os.environ, HOME=tmp)
            subprocess.run(
                [sys.executable, str(script), "--json", "--cache-daily", str(cache)],
                env=env,
                check=True,
                stdout=subprocess.DEVNULL,
            )
            before = cache.stat().st_mtime_ns
            self.assertIn("[tokens] this week:", weekly_line(Path(tmp)))
            subprocess.run(
                [sys.executable, str(script), "--json", "--cache-daily", str(cache)],
                env=env,
                check=True,
                stdout=subprocess.DEVNULL,
            )
            self.assertEqual(cache.stat().st_mtime_ns, before)
