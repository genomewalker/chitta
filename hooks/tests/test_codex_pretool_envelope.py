"""Exercise the real Codex adapter against legacy daemon and safety responses."""

import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


class CodexPretoolEnvelopeTests(unittest.TestCase):
    def test_responses_and_exit_codes(self):
        cases = [
            (
                {"updatedInput": {"command": "echo capped"}},
                0,
                {"updatedInput": {"command": "echo capped"}, "permissionDecision": "allow"},
            ),
            (
                {
                    "permissionDecision": "deny",
                    "updatedInput": {},
                    "permissionDecisionReason": "unsafe",
                },
                0,
                {"permissionDecision": "deny", "permissionDecisionReason": "unsafe"},
            ),
            (
                {"permissionDecision": "block", "additionalContext": "unsafe"},
                2,
                {
                    "permissionDecision": "deny",
                    "additionalContext": "unsafe",
                    "permissionDecisionReason": "unsafe",
                },
            ),
            ({"additionalContext": "context only"}, 0, {"additionalContext": "context only"}),
            (
                {"permissionDecision": "ask", "updatedInput": {}},
                0,
                {"permissionDecision": "ask", "updatedInput": {}},
            ),
        ]
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            wrapper = directory / "codex-pretool-wrapper.sh"
            shutil.copyfile(Path(__file__).parents[1] / wrapper.name, wrapper)
            hook = directory / "pre-tool-hook.sh"
            hook.write_text('#!/bin/bash\nprintf "%s" "$FIXTURE"\nexit "$FIXTURE_RC"\n')
            hook.chmod(0o755)
            for original, status, expected in cases:
                with self.subTest(original=original):
                    original["hookEventName"] = "PreToolUse"
                    expected["hookEventName"] = "PreToolUse"
                    result = subprocess.run(
                        ["bash", str(wrapper), "Bash"],
                        input="{}",
                        text=True,
                        capture_output=True,
                        check=False,
                        env=dict(
                            os.environ,
                            FIXTURE=json.dumps({"hookSpecificOutput": original}),
                            FIXTURE_RC=str(status),
                        ),
                    )
                    self.assertEqual(result.returncode, status)
                    self.assertEqual(json.loads(result.stdout), {"hookSpecificOutput": expected})
            for raw, status in [("", 0), ("daemon unavailable", 0), ("broken", 7)]:
                result = subprocess.run(
                    ["bash", str(wrapper), "Bash"],
                    input="{}",
                    text=True,
                    capture_output=True,
                    check=False,
                    env=dict(os.environ, FIXTURE=raw, FIXTURE_RC=str(status)),
                )
                self.assertEqual((result.stdout, result.returncode), (raw, status))
