"""Bounded session policy and real SDK admission/expiry integration."""

from __future__ import annotations

import asyncio
import json
import os
import unittest
from unittest import mock

try:
    import httpx
    import server
except ImportError as exc:  # test-hooks CI job has no MCP SDK / httpx
    raise unittest.SkipTest(f"MCP SDK or httpx unavailable: {exc}") from exc


class SessionTableTests(unittest.IsolatedAsyncioTestCase):
    def table(self, idle=10, cap=2):
        return server.HttpSessionTable({}, idle_s=idle, max_sessions=cap)

    def add(self, table, key):
        transport = mock.Mock(is_terminated=False, terminate=mock.AsyncMock())
        table[key] = transport
        table.owners[key] = "owner"
        return transport

    async def test_idle_activity_and_owner_cleanup(self):
        table = self.table()
        with mock.patch.object(server.time, "monotonic", return_value=0):
            old = self.add(table, "old")
            active = self.add(table, "active")
        with mock.patch.object(server.time, "monotonic", return_value=9):
            self.assertIs(table["active"], active)
        with mock.patch.object(server.time, "monotonic", return_value=11):
            await table.reap()
        self.assertEqual(set(table), {"active"})
        self.assertEqual(set(table.owners), {"active"})
        old.terminate.assert_awaited_once()
        active.terminate.assert_not_awaited()
        self.assertEqual(table.stats()["expired"], 1)

    async def test_capacity_evicts_least_recently_used(self):
        table = self.table()
        first = self.add(table, "first")
        second = self.add(table, "second")
        table["first"]
        async with table:
            self.add(table, "third")
        self.assertEqual(set(table), {"first", "third"})
        second.terminate.assert_awaited_once()
        first.terminate.assert_not_awaited()
        self.assertEqual(table.stats()["evicted"], 1)

    async def test_concurrent_admission_never_exceeds_capacity(self):
        table = self.table(cap=2)

        async def admit(key):
            async with table:
                await asyncio.sleep(0)
                self.add(table, key)
                self.assertLessEqual(len(table), 2)

        await asyncio.gather(*(admit(str(n)) for n in range(10)))
        self.assertEqual(table.stats()["evicted"], 8)

    async def test_closed_and_sdk_removed_sessions_do_not_leak_metadata(self):
        table = self.table()
        self.add(table, "closed").is_terminated = True
        self.add(table, "crashed")
        del table["crashed"]
        await table.reap()
        self.assertFalse(table)
        self.assertFalse(table.owners)
        self.assertFalse(table.last_seen)

    async def test_health_counters_and_invalid_settings(self):
        table = self.table()
        self.add(table, "session")
        with mock.patch.object(server, "_http_sessions", table):
            result = json.loads(server._with_loop_lag_health("{}"))
        self.assertEqual(result["mcp_sessions"]["active"], 1)
        for value in ("0", "-1", "nan", "inf", "bad"):
            with mock.patch.dict(os.environ, TEST_SETTING=value), self.assertLogs("chitta-mcp"):
                self.assertEqual(server._session_setting("TEST_SETTING", 1800, float), 1800)


class SDKSessionTests(unittest.IsolatedAsyncioTestCase):
    async def test_initialize_cap_expire_and_expired_id_is_404(self):
        with mock.patch.dict(
            os.environ, CHITTA_MCP_MAX_SESSIONS="1", CHITTA_MCP_SESSION_IDLE_S="60"
        ):
            manager = server.create_http_session_manager()
        table = manager._server_instances
        headers = {"accept": "application/json, text/event-stream"}
        initialize = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {
                "protocolVersion": "2025-03-26",
                "capabilities": {},
                "clientInfo": {"name": "test", "version": "1"},
            },
        }
        async with manager.run():
            async with httpx.AsyncClient(
                transport=httpx.ASGITransport(app=manager.handle_request),
                base_url="http://localhost",
            ) as client:
                first = await client.post("/mcp", headers=headers, json=initialize)
                self.assertEqual(first.status_code, 200)
                first_id = first.headers["mcp-session-id"]
                first_transport = table.get(first_id)
                second = await client.post("/mcp", headers=headers, json=initialize)
                self.assertEqual(second.status_code, 200)
                self.assertEqual(table.stats()["active"], 1)
                self.assertEqual(table.stats()["evicted"], 1)
                self.assertTrue(first_transport.is_terminated)
                response = await client.post(
                    "/mcp",
                    headers=dict(headers, **{"mcp-session-id": first_id}),
                    json={"jsonrpc": "2.0", "method": "notifications/initialized"},
                )
                self.assertEqual(response.status_code, 404)
                second_id = second.headers["mcp-session-id"]
                table.last_seen[second_id] -= 61
                response = await client.post(
                    "/mcp",
                    headers=dict(headers, **{"mcp-session-id": second_id}),
                    json={"jsonrpc": "2.0", "method": "notifications/initialized"},
                )
                self.assertEqual(response.status_code, 404)
                self.assertEqual(table.stats()["expired"], 1)
                await client.post("/mcp", headers=headers, json=initialize)
                table.idle_s = 0.05
                monitor = asyncio.create_task(table.monitor())
                try:
                    await asyncio.sleep(0.12)
                finally:
                    await server._stop_loop_lag_monitor(monitor)
                self.assertEqual(table.stats()["active"], 0)
                self.assertEqual(table.stats()["expired"], 2)
        server._http_sessions = None
