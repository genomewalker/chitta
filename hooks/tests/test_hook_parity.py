"""The migration gate must preserve bytes, failures and real timeouts."""

from __future__ import annotations

import importlib.util
import os
import signal
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("hook_parity", ROOT / "scripts/bench-hook-parity.py")
parity = importlib.util.module_from_spec(spec)
spec.loader.exec_module(parity)


class HookParityTest(unittest.TestCase):
    def test_exit_status_and_bytes_are_not_swallowed(self):
        with tempfile.TemporaryDirectory() as tmp:
            out, err, status = parity.run_hook(
                ["bash", "-c", "cat; printf 'error\\n\\n' >&2; exit 7"],
                b"payload\x00\n\n",
                dict(os.environ),
                tmp,
                2,
            )
        self.assertEqual(out, b"payload\x00\n\n")
        self.assertEqual(err, b"error\n\n")
        self.assertEqual(status["returncode"], 7)
        self.assertFalse(status["timed_out"])

    def test_deadline_reaps_background_pipe_holders(self):
        with tempfile.TemporaryDirectory() as tmp:
            _, _, status = parity.run_hook(
                ["bash", "-c", "sleep 60 & wait"],
                b"",
                dict(os.environ),
                tmp,
                0.1,
            )
        self.assertTrue(status["timed_out"])
        self.assertEqual(status["returncode"], -signal.SIGKILL)
        self.assertLess(status["wall_ms"], 2000)

    def test_clock_pins_now_but_preserves_explicit_date_parsing(self):
        env = dict(os.environ, CHITTA_HOOK_NOW="1789516800123", TZ="UTC")
        command = 'source "$1/hooks/lib.sh"; date +%s%3N; date -d @123 +%s; date -u +%FT%TZ'
        result = subprocess.run(
            ["bash", "-c", command, "_", str(ROOT)],
            env=env,
            capture_output=True,
            check=True,
        )
        self.assertEqual(result.stdout, b"1789516800123\n123\n2026-09-16T00:00:00Z\n")

    def test_invalid_clock_is_ignored(self):
        env = dict(os.environ, CHITTA_HOOK_NOW="invalid")
        subprocess.run(
            ["bash", "-c", 'source "$1/hooks/lib.sh"; ! declare -F date', "_", str(ROOT)],
            env=env,
            capture_output=True,
            check=True,
        )


if __name__ == "__main__":
    unittest.main()
