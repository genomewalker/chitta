import json
import sys
import tempfile
import unittest
from pathlib import Path

MCP_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(MCP_DIR))

import task_ledger  # noqa: E402
from fake_ledger import fake_ledger  # noqa: E402
from thread_inference import (  # noqa: E402
    extract_user_turns,
    fingerprint,
    infer,
    snapshot_user_turns,
)


class TranscriptFormatTests(unittest.TestCase):
    def _write(self, rows):
        handle = tempfile.NamedTemporaryFile(mode="w", suffix=".jsonl", delete=False)
        with handle:
            for row in rows:
                handle.write(json.dumps(row) + "\n")
        self.addCleanup(Path(handle.name).unlink, missing_ok=True)
        return handle.name

    def test_extracts_claude_user_turns(self):
        path = self._write(
            [
                {
                    "type": "user",
                    "message": {"content": [{"type": "text", "text": "work on cc-soul bridge"}]},
                },
                {"type": "assistant", "message": {"content": [{"type": "text", "text": "ok"}]}},
                {"type": "user", "message": {"content": "track Claude sessions too"}},
            ]
        )
        self.assertEqual(
            extract_user_turns(path),
            [
                "work on cc-soul bridge",
                "track Claude sessions too",
            ],
        )

    def test_extracts_codex_and_strips_environment_noise(self):
        path = self._write(
            [
                {"type": "session_meta", "payload": {"cwd": "/tmp/project"}},
                {
                    "type": "response_item",
                    "payload": {
                        "type": "message",
                        "role": "user",
                        "content": [
                            {
                                "type": "input_text",
                                "text": "<environment_context>noise</environment_context>implement shared leases",
                            }
                        ],
                    },
                },
                {
                    "type": "response_item",
                    "payload": {
                        "type": "message",
                        "role": "assistant",
                        "content": [{"type": "output_text", "text": "working"}],
                    },
                },
            ]
        )
        self.assertEqual(extract_user_turns(path), ["implement shared leases"])

    def test_loads_bounded_stop_snapshot(self):
        # stop-transcript-snapshot.py's add_user() already runs every value
        # through the shared sanitizer before it lands in user_turns, so a
        # real snapshot's entries arrive here pre-cleaned — snapshot_user_turns
        # must not mangle or need to re-clean them.
        path = self._write([])
        Path(path).write_text(json.dumps({"user_turns": ["fix stop hook", "run tests"]}))
        self.assertEqual(snapshot_user_turns(path), ["fix stop hook", "run tests"])

    def test_bounded_stop_snapshot_dedups_without_recleaning(self):
        path = self._write([])
        Path(path).write_text(
            json.dumps({"user_turns": ["run tests", "run tests", "fix stop hook"]})
        )
        self.assertEqual(snapshot_user_turns(path), ["run tests", "fix stop hook"])

    def test_creates_parallel_thread_instead_of_taking_live_lease(self):
        with fake_ledger():
            texts = [
                "implement shared session transcript leases",
                "check claude codex session ownership",
                "prevent resume of live thread ownership",
            ]
            thread_id = task_ledger.thread_create(
                "shared session leases",
                "project:test",
                json.dumps(fingerprint(texts)),
            )
            task_ledger.session_bind("claude-owner", thread_id, client="claude")
            task_ledger.lease_claim(thread_id, "claude-owner")
            path = self._write([{"type": "user", "message": {"content": text}} for text in texts])
            result = infer(
                path,
                "project:test",
                session_id="codex-other",
                client="codex",
                project_dir="/tmp/project",
            )
            self.assertEqual(result["action"], "create")
            self.assertEqual(result["reason"], "matching_thread_locked")
            self.assertEqual(result["owner_session_id"], "claude-owner")
            self.assertNotEqual(result["thread_id"], thread_id)
            self.assertEqual(
                task_ledger.session_get("codex-other")["thread_id"], result["thread_id"]
            )


if __name__ == "__main__":
    unittest.main()
