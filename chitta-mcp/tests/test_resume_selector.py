import json
import sys
import time
import unittest
from pathlib import Path

MCP_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(MCP_DIR))

import task_ledger  # noqa: E402
from fake_ledger import fake_ledger  # noqa: E402
from resume_selector import claim_selected, select_resume  # noqa: E402


class ResumeSelectorTests(unittest.TestCase):
    project = "/tmp/cc-soul-project"

    def test_selects_inactive_codex_and_reports_live_claude(self):
        now_ms = int(time.time() * 1000)
        data = {
            "sessions": [
                {
                    "session_id": "claude-live",
                    "client": "claude",
                    "project_dir": self.project,
                    "status": "active",
                    "is_live": True,
                    "last_heartbeat_ms": now_ms,
                }
            ],
            "transcripts": [
                {
                    "session_id": "claude-live",
                    "client": "claude",
                    "project_dir": self.project,
                    "transcript_path": "/tmp/claude.jsonl",
                },
                {
                    "session_id": "codex-ended",
                    "client": "codex",
                    "project_dir": self.project,
                    "transcript_path": "/tmp/codex.jsonl",
                },
            ],
            "thread_sessions": [
                {
                    "session_id": "codex-ended",
                    "client": "codex",
                    "project_dir": self.project,
                    "status": "ended",
                }
            ],
            "thread_leases": [],
        }
        result = select_resume(data, self.project, current_session_id="new-session")
        self.assertEqual(result["status"], "selected")
        self.assertEqual(result["selected"]["session_id"], "codex-ended")
        self.assertEqual(result["live_sessions"][0]["client"], "claude")

    def test_live_owner_locks_older_session_on_same_thread(self):
        now = time.time()
        now_ms = int(now * 1000)
        data = {
            "sessions": [
                {
                    "session_id": "claude-live",
                    "client": "claude",
                    "project_dir": self.project,
                    "thread_id": "thread-a",
                    "status": "active",
                    "is_live": True,
                    "last_heartbeat_ms": now_ms,
                },
                {
                    "session_id": "codex-live",
                    "client": "codex",
                    "project_dir": self.project,
                    "thread_id": "thread-b",
                    "status": "active",
                    "is_live": True,
                    "last_heartbeat_ms": now_ms,
                },
            ],
            "transcripts": [
                {
                    "session_id": "older-a",
                    "client": "codex",
                    "project_dir": self.project,
                    "thread_id": "thread-a",
                    "transcript_path": "/tmp/older-a.jsonl",
                }
            ],
            "thread_sessions": [],
            "thread_leases": [
                {"thread_id": "thread-a", "session_id": "claude-live", "expires_at": now + 600},
                {"thread_id": "thread-b", "session_id": "codex-live", "expires_at": now + 600},
            ],
        }
        result = select_resume(data, self.project, current_session_id="new-session")
        self.assertEqual(result["status"], "locked")
        self.assertEqual(result["candidates"][0]["lock_owner_session_id"], "claude-live")
        self.assertEqual(set(result["multimodel"]), {"claude", "codex"})

    def test_newer_other_project_is_never_selected(self):
        data = {
            "sessions": [],
            "transcripts": [
                {
                    "session_id": "genopack-new",
                    "client": "codex",
                    "project_dir": "/tmp/genopack",
                    "transcript_path": "/tmp/new.jsonl",
                },
                {
                    "session_id": "cc-soul-old",
                    "client": "codex",
                    "project_dir": self.project,
                    "transcript_path": "/tmp/old.jsonl",
                },
            ],
            "thread_sessions": [],
            "thread_leases": [],
        }
        result = select_resume(data, self.project, current_session_id="new-session")
        self.assertEqual(result["status"], "selected")
        self.assertEqual(result["selected"]["session_id"], "cc-soul-old")
        self.assertNotIn("genopack-new", [row["session_id"] for row in result["candidates"]])

    def test_equal_project_candidates_are_ambiguous(self):
        rows = [
            {
                "session_id": sid,
                "client": "codex",
                "project_dir": self.project,
                "transcript_path": f"/tmp/{sid}.jsonl",
            }
            for sid in ("one", "two")
        ]
        result = select_resume(
            {
                "sessions": [],
                "transcripts": rows,
                "thread_sessions": [],
                "thread_leases": [],
            },
            self.project,
            current_session_id="new-session",
        )
        self.assertEqual(result["status"], "ambiguous")
        self.assertIsNone(result["selected"])

    def test_unexpired_self_lease_still_locks_candidate(self):
        data = {
            "sessions": [],
            "transcripts": [
                {
                    "session_id": "temporarily-missing",
                    "client": "claude",
                    "project_dir": self.project,
                    "thread_id": "thread-a",
                    "transcript_path": "/tmp/missing.jsonl",
                }
            ],
            "thread_sessions": [],
            "thread_leases": [
                {
                    "thread_id": "thread-a",
                    "session_id": "temporarily-missing",
                    "expires_at": time.time() + 600,
                }
            ],
        }
        result = select_resume(data, self.project, current_session_id="new-session")
        self.assertEqual(result["status"], "locked")
        self.assertEqual(result["candidates"][0]["lock_owner_session_id"], "temporarily-missing")

    def test_registry_timeout_fails_closed(self):
        data = {
            "session_registry_available": False,
            "sessions": [],
            "transcripts": [
                {
                    "session_id": "possibly-live",
                    "client": "claude",
                    "project_dir": self.project,
                    "transcript_path": "/tmp/possibly-live.jsonl",
                }
            ],
            "thread_sessions": [],
            "thread_leases": [],
        }
        result = select_resume(data, self.project, current_session_id="new-session")
        self.assertEqual(result["status"], "registry_unavailable")
        self.assertIsNone(result["selected"])

    def test_lease_claim_is_exclusive(self):
        with fake_ledger():
            thread_id = task_ledger.thread_create("shared", "project:test")
            task_ledger.session_bind("claude", thread_id, client="claude")
            task_ledger.session_bind("codex", thread_id, client="codex")
            first = task_ledger.lease_claim(thread_id, "claude")
            second = task_ledger.lease_claim(thread_id, "codex")
            self.assertTrue(first["claimed"])
            self.assertFalse(second["claimed"])
            self.assertEqual(second["owner_session_id"], "claude")
            renewed = task_ledger.lease_claim(thread_id, "claude")
            self.assertEqual(renewed["generation"], first["generation"])
            task_ledger.session_close("claude")
            after_close = task_ledger.lease_claim(thread_id, "codex")
            self.assertTrue(after_close["claimed"])

    def test_failed_claim_does_not_bind_contender_to_owned_thread(self):
        with fake_ledger():
            thread_id = task_ledger.thread_create("owned", "project:test")
            task_ledger.session_bind("owner", thread_id, client="claude")
            task_ledger.lease_claim(thread_id, "owner")
            result = {"selected": {"thread_id": thread_id}}
            claim = claim_selected(result, "contender", "codex", self.project)
            self.assertFalse(claim["claimed"])
            self.assertIsNone(task_ledger.session_get("contender")["thread_id"])

    def test_session_rebind_preserves_registration_metadata(self):
        with fake_ledger():
            task_ledger.session_bind(
                "session",
                client="codex",
                metadata={"model": "gpt", "host": "node"},
            )
            thread_id = task_ledger.thread_create("work", "project:test")
            task_ledger.session_bind("session", thread_id)
            row = task_ledger.session_get("session")
            self.assertEqual(row["client"], "codex")
            self.assertEqual(json.loads(row["metadata_json"])["model"], "gpt")

    def test_claim_creates_thread_for_historical_session(self):
        with fake_ledger():
            result = {
                "selected": {
                    "session_id": "old-codex",
                    "client": "codex",
                    "project_dir": self.project,
                    "transcript_path": "/tmp/old-codex.jsonl",
                    "status": "ended",
                    "thread_id": "",
                }
            }
            claim = claim_selected(result, "new-claude", "claude", self.project)
            self.assertTrue(claim["claimed"])
            self.assertTrue(result["selected"]["thread_id"])
            owner = task_ledger.lease_list()[0]
            self.assertEqual(owner["session_id"], "new-claude")


if __name__ == "__main__":
    unittest.main()
