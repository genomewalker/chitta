"""Milestone writes must receive a durable acknowledgement."""

import subprocess
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import capsule_cli  # noqa: E402


class CapsuleCliTests(unittest.TestCase):
    def test_explicit_revision_and_payload(self):
        reply = subprocess.CompletedProcess([], 0, '{"value":{"version":2,"revision":4}}')
        with patch("capsule_cli.subprocess.run", return_value=reply) as run:
            value = capsule_cli.ledger("capsule_save", {"expected_revision": 3})
        self.assertEqual(value["revision"], 4)
        self.assertIn('"expected_revision": 3', run.call_args.args[0][-2])
        self.assertEqual(run.call_args.kwargs["stdin"], subprocess.DEVNULL)

    def test_error_reply_is_not_acknowledgement(self):
        reply = subprocess.CompletedProcess([], 0, '{"error":"revision mismatch"}')
        with patch("capsule_cli.subprocess.run", return_value=reply):
            with self.assertRaisesRegex(ValueError, "acknowledge"):
                capsule_cli.ledger("capsule_save", {})

    def test_transport_failure_propagates(self):
        with patch(
            "capsule_cli.subprocess.run", side_effect=subprocess.TimeoutExpired("chitta", 20)
        ):
            with self.assertRaises(subprocess.TimeoutExpired):
                capsule_cli.ledger("capsule_save", {})
