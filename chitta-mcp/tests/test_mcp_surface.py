"""Discovery budgets, complete tiering and unadvertised-call compatibility."""

from __future__ import annotations

import asyncio
import importlib.util
import io
import json
import re
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

try:
    import server
    import tool_policy
    from mcp.types import Tool
except ImportError as exc:
    # The MCP SDK is present where the server runs; CI installs it only in the
    # contract job, which runs this module explicitly.
    raise unittest.SkipTest(f"MCP SDK unavailable: {exc}") from exc

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("surface", ROOT / "scripts/check-mcp-surface.py")
surface = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(surface)


class SurfaceTests(unittest.TestCase):
    def test_budget_and_complete_disjoint_tiers(self):
        report = surface.measure()
        self.assertLessEqual(report["core"], 80)
        self.assertLessEqual(report["tokens"], 8000)
        self.assertEqual(report["core"] + report["advanced"] + report["hidden"], report["total"])
        for key in ("unclassified", "invalid_core", "duplicate_names", "overlapping_tiers"):
            self.assertEqual(report[key], [], key)

    def test_new_tools_default_to_advanced(self):
        import tools_static

        extra = Tool(name="future_tool", inputSchema={})
        namespace = {}
        with mock.patch.object(tools_static, "TOOLS", tools_static.TOOLS + [extra]):
            exec(
                compile(Path(tool_policy.__file__).read_text(), tool_policy.__file__, "exec"),
                namespace,
            )
        self.assertIn("future_tool", namespace["ADVANCED_TOOLS"])
        self.assertIn("future_tool", namespace["HIDDEN_TOOLS"])
        self.assertNotIn("future_tool", namespace["CORE_TOOLS"])

    def test_native_inventory_matches_registration_sources(self):
        registered = set()
        for path in (ROOT / "chitta/src/handlers").glob("register_*.cpp"):
            registered.update(re.findall(r'handlers_\["([a-z_0-9]+)"\]', path.read_text()))
        self.assertEqual(registered, tool_policy.DAEMON_HANDLER_NAMES)
        self.assertLessEqual(registered, tool_policy.ALL_TOOLS)
        self.assertIn("ledger_op", tool_policy.ADVANCED_TOOLS)

    def test_measure_uses_production_composite_override_and_null_cleaning(self):
        native = Tool(name="example", description="native", inputSchema={"required": None})
        composite = Tool(
            name="example", description="λ composite", inputSchema={"properties": {"x": None}}
        )
        hidden = Tool(name="hidden", inputSchema={})
        with (
            mock.patch.object(server, "TOOLS", [native, hidden]),
            mock.patch.object(server, "COMPOSITE_TOOLS", [composite]),
            mock.patch.object(server, "HIDDEN_TOOLS", {"hidden"}),
        ):
            tools = asyncio.run(server.list_tools())
            report = surface.measure()
        self.assertEqual(len(tools), 1)
        self.assertEqual(tools[0].description, "λ composite")
        self.assertEqual(tools[0].inputSchema, {"properties": {}})
        payload = json.dumps(
            {"tools": [t.model_dump(mode="json", exclude_none=True) for t in tools]},
            ensure_ascii=False,
            separators=(",", ":"),
        )
        self.assertEqual(report["characters"], len(payload))
        self.assertEqual(report["tokens"], (len(payload) + 3) // 4)
        self.assertGreater(report["bytes"], report["characters"])

    def test_thresholds_are_inclusive_and_fail_independently(self):
        report = surface.measure()
        with redirect_stdout(io.StringIO()):
            self.assertEqual(
                surface.main(
                    ["--max-core", str(report["core"]), "--max-tokens", str(report["tokens"])]
                ),
                0,
            )
            self.assertEqual(surface.main(["--max-core", str(report["core"] - 1)]), 1)
            self.assertEqual(surface.main(["--max-tokens", str(report["tokens"] - 1)]), 1)

    def test_hidden_direct_calls_keep_dispatch_and_arguments(self):
        # Every unadvertised daemon name still reaches the same dispatch path.
        names = server.HIDDEN_TOOLS - set(server.COMPOSITE_HANDLERS)
        with (
            mock.patch.object(server, "daemon_call", return_value="ok") as call,
            mock.patch.object(server, "inject_message_session", return_value=None),
            mock.patch.object(server, "get_current_realm", return_value=""),
            mock.patch.object(server, "_sqz_compress", side_effect=lambda text, *_: text),
        ):
            for name in sorted(names):
                with self.subTest(name=name):
                    asyncio.run(server.call_tool(name, {"sentinel": "unchanged"}))
                    self.assertEqual(call.call_args.args[:2], (name, {"sentinel": "unchanged"}))

    def test_advanced_gateway_keeps_local_composites(self):
        with (
            mock.patch.dict(
                server.COMPOSITE_HANDLERS, {"remember_typed": mock.Mock(return_value="local")}
            ),
            mock.patch.object(server, "daemon_call") as daemon,
        ):
            result = server.handle_advanced(
                {"tool": "remember_typed", "arguments": {"content": "test"}}
            )
            self.assertIn("local", result)
            server.COMPOSITE_HANDLERS["remember_typed"].assert_called_once_with({"content": "test"})
            daemon.assert_not_called()

    def test_promoted_tools_remain_callable_through_advanced(self):
        with mock.patch.object(server, "daemon_call", return_value="promoted") as call:
            result = server.handle_advanced({"tool": "get", "arguments": {"id": "123"}})
            self.assertIn("promoted", result)
            call.assert_called_once_with("get", {"id": "123"})
        self.assertIn("Unknown tool", server.handle_advanced({"tool": "advanced"}))

    def test_advanced_gateway_native_only_handler(self):
        with mock.patch.object(server, "daemon_call", return_value="native") as call:
            self.assertIn(
                "native",
                server.handle_advanced({"tool": "ledger_op", "arguments": {"op": "thread_list"}}),
            )
            call.assert_called_once_with("ledger_op", {"op": "thread_list"})
