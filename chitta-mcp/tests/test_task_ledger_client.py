"""Transport and fail-open contracts, without a live daemon or SQLite fallback."""

from __future__ import annotations

import json
import os
import socket
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import daemon_client  # noqa: E402
import session_registry  # noqa: E402
import task_ledger  # noqa: E402


class ClientTests(unittest.TestCase):
    def setUp(self):
        daemon_client._unavailable_until = 0
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = str(Path(self.temp.name) / "daemon.sock")
        self.env = patch.dict(
            os.environ,
            {
                "HOME": self.temp.name,
                "CHITTA_SOCKET_PATH": self.path,
                "CHITTA_TASK_LEDGER": str(Path(self.temp.name) / "must-not-exist.db"),
            },
        )
        self.env.start()
        self.addCleanup(self.env.stop)
        self.addCleanup(setattr, daemon_client, "_unavailable_until", 0)

    def test_missing_daemon_returns_empty_and_never_creates_sqlite(self):
        self.assertIsNone(task_ledger.thread_create("offline"))
        self.assertEqual(task_ledger.thread_list(), [])
        self.assertIsNone(task_ledger.session_get("missing"))
        self.assertEqual(task_ledger.lease_claim("missing", "missing"), {})
        self.assertFalse(task_ledger.session_bind("offline"))
        result = session_registry.register(
            {"session_id": "offline", "realm": "test", "cwd": self.temp.name}, "codex"
        )
        self.assertFalse(result["registered"])
        self.assertFalse(Path(os.environ["CHITTA_TASK_LEDGER"]).exists())
        self.assertFalse(hasattr(task_ledger, "connect"))

    def test_stalled_socket_has_one_cumulative_hook_budget(self):
        server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        server.bind(self.path)
        server.listen(1)
        done = threading.Event()

        def stall():
            connection, _ = server.accept()
            with connection:
                connection.recv(4096)
                done.wait(1)

        thread = threading.Thread(target=stall, daemon=True)
        thread.start()
        try:
            started = time.monotonic()
            result = session_registry.register(
                {"session_id": "offline", "realm": "test", "cwd": self.temp.name}, "codex"
            )
            elapsed = time.monotonic() - started
            self.assertFalse(result["registered"])
            self.assertLess(elapsed, 0.2)
        finally:
            done.set()
            thread.join(timeout=2)
            server.close()

    def test_warming_daemon_does_not_count_as_registration(self):
        response = json.dumps({"result": {"structured": {"status": "warming_up"}}})
        with (
            patch.object(daemon_client.ChittaClient, "connect", return_value=True),
            patch.object(daemon_client.ChittaClient, "call", return_value=response),
        ):
            result = session_registry.register(
                {"session_id": "warming", "realm": "test", "cwd": self.temp.name}, "codex"
            )
        self.assertFalse(result["registered"])

    def test_realm_detection_stays_local_to_the_hook_project(self):
        project = Path(self.temp.name) / "repository"
        nested = project / "src"
        nested.mkdir(parents=True)
        (project / ".git").write_text("gitdir: /unused/worktree/admin\n")
        env = dict(os.environ)
        env.pop("CHITTA_REALM", None)
        with (
            patch.dict(os.environ, env, clear=True),
            patch.object(
                session_registry, "_run_chitta", side_effect=AssertionError("realm must be local")
            ),
        ):
            self.assertEqual(session_registry._detect_realm(str(nested)), "project:repository")
            (nested / ".cc-soul-realm").write_text(" project:configured \nignored second line\n")
            self.assertEqual(session_registry._detect_realm(str(nested)), "project:configured")
            with patch.dict(os.environ, {"CHITTA_REALM": "project:environment"}):
                self.assertEqual(session_registry._detect_realm(str(nested)), "project:environment")
            self.assertEqual(session_registry._detect_realm(self.temp.name), "brahman")

    def test_endpoint_resolution_errors_fail_open(self):
        with patch.object(daemon_client, "get_socket_path", side_effect=PermissionError("denied")):
            self.assertIsNone(daemon_client.daemon_call("ledger_op", {"op": "counts"}))

    def test_http_dns_or_body_stall_cannot_hold_hook(self):
        done = threading.Event()
        entered = threading.Event()

        def stall(_request):
            entered.set()
            done.wait(1)
            return None

        try:
            with (
                patch.dict(os.environ, {"CHITTA_SOCKET_PATH": "", "CHITTA_RPC_PORT": "7432"}),
                patch.object(daemon_client.ChittaHttpClient, "call", side_effect=stall),
            ):
                started = time.monotonic()
                self.assertIsNone(
                    daemon_client.daemon_call("ledger_op", {"op": "counts"}, timeout=0.04)
                )
                self.assertLess(time.monotonic() - started, 0.15)
                self.assertTrue(entered.is_set())
        finally:
            done.set()

    def test_queued_heartbeat_does_not_contact_daemon(self):
        queue = Path(self.temp.name) / "queue.jsonl"
        with (
            patch.object(session_registry, "CHITTA_QUEUE", queue),
            patch.object(session_registry, "_run_chitta", side_effect=AssertionError("RPC")),
            patch.object(session_registry, "session_get", side_effect=AssertionError("ledger RPC")),
        ):
            self.assertTrue(
                session_registry.heartbeat({"session_id": "queued"}, queued=True)["heartbeat"]
            )
        item = json.loads(queue.read_text())
        self.assertEqual(item["tool"], "session_heartbeat")
        self.assertEqual(item["args"]["session_id"], "queued")
