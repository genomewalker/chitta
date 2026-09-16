"""Shared state and dependency overrides survive concern-module delegation."""

from __future__ import annotations

import ast
import asyncio
import json
import os
import unittest
from pathlib import Path
from unittest import mock

try:
    import server
except ImportError as exc:
    raise unittest.SkipTest(f"MCP SDK unavailable: {exc}") from exc


class RuntimeTests(unittest.TestCase):
    def test_all_delegated_runtime_dependencies_are_available(self):
        for name in (
            "mcp_health",
            "mcp_transport",
            "daemon_bridge",
            "code_tools",
            "memory_tools",
            "evolve_helpers",
            "recall_handlers",
            "session_ops",
            "tool_policy",
        ):
            module = getattr(server, name)
            tree = ast.parse(Path(module.__file__).read_text())
            for node in ast.walk(tree):
                if isinstance(node, ast.Attribute) and isinstance(node.value, ast.Name):
                    if node.value.id == "ctx":
                        self.assertTrue(hasattr(server, node.attr), f"{name}: {node.attr}")

    def test_daemon_rpc_uses_facade_client_and_origin_session(self):
        client = mock.Mock()
        client.call.return_value = json.dumps({"result": {"content": [{"text": "stored"}]}})
        with (
            mock.patch.object(server, "ensure_daemon", return_value=True),
            mock.patch.object(server, "client", client),
            mock.patch.object(server, "get_current_session_id", return_value="origin"),
        ):
            self.assertEqual(server.daemon_call("remember", {"tags": '["one"]'}), "stored")
        self.assertEqual(
            client.call.call_args.args[0]["params"],
            {
                "name": "remember",
                "arguments": {"tags": ["one"], "source_session": "origin"},
            },
        )

    def test_session_cache_is_shared_across_handlers(self):
        session = {"sessions": [{"session_id": "ppid-session", "pid": os.getppid()}]}
        with (
            mock.patch.dict(os.environ, {"CLAUDE_SESSION_ID": ""}),
            mock.patch.object(server, "current_session_id", None),
            mock.patch.object(server, "daemon_call", return_value=json.dumps(session)),
            mock.patch.object(server, "get_current_realm", return_value="project:test"),
        ):
            self.assertEqual(server.get_current_session_id(False), "ppid-session")
            self.assertEqual(server.current_session_id, "ppid-session")
            args = {"recipient": "other", "content": "hello"}
            self.assertIsNone(server.inject_message_session("msg_send", args))
            self.assertEqual(args["session_id"], "ppid-session")
            self.assertEqual(args["sender_session_id"], "ppid-session")
            self.assertEqual(args["sender_realm"], "project:test")

    def test_learning_gateway_keeps_public_handler_override(self):
        with mock.patch.object(server, "handle_learn_insight", return_value="learned") as handler:
            args = {"type": "insight", "content": "new"}
            self.assertEqual(server.handle_learn_gateway(args), "learned")
            handler.assert_called_once_with({"content": "new"})

    def test_memory_edit_preserves_argument_normalization(self):
        with mock.patch.object(server, "daemon_call", return_value="updated") as call:
            args = {"action": "set_type", "id": "18446744073709551615", "type": "wisdom"}
            self.assertEqual(server.handle_memory_edit_gateway(args), "updated")
            call.assert_called_once_with(
                "set_memory_type",
                {
                    "memory_id": "18446744073709551615",
                    "type": "wisdom",
                },
            )

    def test_transport_preserves_sdk_ownership_map_when_available(self):
        from mcp.server import streamable_http_manager

        for with_owners in (False, True):

            class Manager:
                def __init__(self, with_owners=with_owners, **kwargs):
                    if with_owners:
                        self._session_owners = {"session": "credential"}

            with (
                mock.patch.object(streamable_http_manager, "StreamableHTTPSessionManager", Manager),
                mock.patch.object(server, "_http_sessions", None),
            ):
                manager = server.create_http_session_manager()
                table = manager._server_instances
                self.assertIs(manager._session_creation_lock, table)
                self.assertIs(server._http_sessions, table)
                if with_owners:
                    self.assertIs(table.owners, manager._session_owners)
                else:
                    self.assertEqual(table.owners, {})


class DispatchTests(unittest.IsolatedAsyncioTestCase):
    async def test_registration_updates_state_used_by_transcript_search(self):
        with (
            mock.patch.object(server, "current_session_id", None),
            mock.patch.object(server, "daemon_call", return_value="registered"),
            mock.patch.object(server, "get_current_realm", return_value=None),
            mock.patch.object(server, "_sqz_compress", side_effect=lambda text, name: text),
            mock.patch.dict(
                server.COMPOSITE_HANDLERS, transcript_search=mock.Mock(return_value="hit")
            ),
        ):
            await server.call_tool("session_register", {"session_id": "shared-session"})
            result = await server.call_tool("transcript_search", {"query": "keyword"})
            self.assertEqual(result[0].text, "hit")
            server.COMPOSITE_HANDLERS["transcript_search"].assert_called_once_with(
                {
                    "query": "keyword",
                    "session_id": "shared-session",
                }
            )

    async def test_recall_remains_async_and_uses_facade_reranker(self):
        self.assertTrue(asyncio.iscoroutinefunction(server.handle_recall_gateway))
        with (
            mock.patch.object(server, "get_reranker", return_value=None),
            mock.patch.object(server, "daemon_call", return_value="recall result") as call,
        ):
            self.assertEqual(await server.handle_recall_gateway({"query": "q"}), "recall result")
            call.assert_called_once_with("hybrid_recall", {"query": "q"})
