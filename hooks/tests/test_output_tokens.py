"""Local output cache and token context regression tests (no daemon)."""

import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "chitta-mcp"))
from hook_outputs import add_context, summarize
from hook_tokens import weekly_line


class OutputTokensTest(unittest.TestCase):
    def test_both_frontends_round_trip(self):
        text = "λ line\n" * 2000
        for result in (
            {"tool_result": {"stdout": text}},
            {"tool_response": text},
            {"tool_response": {"stdout": text}},
        ):
            with tempfile.TemporaryDirectory() as tmp:
                state = Path(tmp)
                summary = summarize(dict(result, tool_name="Bash"), state)
                digest = hashlib.sha256(text.encode()).hexdigest()
                self.assertIn(f"§ref:{digest[:12]}§ {len(text)} chars", summary)
                self.assertEqual((state / "outputs" / digest).read_bytes(), text.encode())
                self.assertLess(len(summary), 6000)
                self.assertEqual((state / "outputs" / digest).stat().st_mode & 0o777, 0o600)
                merged = json.loads(
                    add_context('{"hookSpecificOutput":{"additionalContext":"failure"}}', summary)
                )
                self.assertTrue(
                    merged["hookSpecificOutput"]["additionalContext"].startswith("failure\n")
                )

    def test_boundaries_and_long_lines(self):
        with (
            tempfile.TemporaryDirectory() as tmp,
            patch.dict(os.environ, {"CHITTA_OUTPUT_CAP_CHARS": "6000"}),
        ):
            state = Path(tmp)
            self.assertEqual(summarize({"tool_response": "x" * 6000}, state), "")
            self.assertFalse((state / "outputs").exists())
            for text in ("x" * 6001, "x\n" * 5000):
                self.assertLessEqual(len(summarize({"tool_response": text}, state)), 6000)
            self.assertEqual(
                summarize({"tool_name": "Read", "tool_response": "x" * 9000}, state), ""
            )
            self.assertEqual(add_context("unchanged", ""), "unchanged")

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

    def test_daemon_failure_keeps_reference(self):
        from hook_client import Client, post_tool

        with tempfile.TemporaryDirectory() as tmp, patch.dict(os.environ, {"CHITTA_DB_PATH": tmp}):
            client = Client({"tool_name": "Bash", "tool_response": "x" * 7000})
            with patch.object(client, "policy", side_effect=ValueError("offline")):
                post_tool(client)
            self.assertIn("§ref:", client.output)
            self.assertIn("daemon unavailable", client.diagnostics)
