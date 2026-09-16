"""Generated tool coverage, deterministic output, and stale-file detection."""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "gen_tools_static", ROOT / "scripts/gen-tools-static.py"
)
generator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(generator)


class StaticToolsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.daemon = json.loads((ROOT / "contracts/daemon-tools.json").read_text())

    def test_output_is_current_and_input_order_independent(self):
        expected = (ROOT / "chitta-mcp/tools_static.py").read_text()
        self.assertEqual(generator.generate(self.daemon), expected)
        self.assertEqual(generator.generate(list(reversed(self.daemon))), expected)

    def test_daemon_coverage_composite_handlers_and_contract(self):
        try:
            import server
            import tools_static
            from tools_composite import COMPOSITE
        except ImportError as exc:
            self.skipTest(f"MCP SDK unavailable: {exc}")
        self.assertEqual({t.name for t in tools_static.TOOLS}, {t["name"] for t in self.daemon})
        for tool in COMPOSITE:
            self.assertTrue(callable(server.COMPOSITE_HANDLERS[tool["name"]]))
        rows = [
            {key: tool.model_dump()[key] for key in ("name", "description", "inputSchema")}
            for tool in tools_static.TOOLS + tools_static.COMPOSITE_TOOLS
        ]
        actual = (
            json.dumps(sorted(rows, key=lambda row: row["name"]), indent=1, sort_keys=True) + "\n"
        )
        self.assertEqual(actual, (ROOT / "contracts/mcp-tools.json").read_text())

    def test_docs_use_current_policy_and_frozen_schemas_without_rpc(self):
        import tool_policy

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "scripts").mkdir()
            (root / "docs").mkdir()
            (root / "scripts/gen-tools-docs.py").write_text(
                (ROOT / "scripts/gen-tools-docs.py").read_text()
            )
            (root / "scripts/site_common.py").write_text(
                (ROOT / "scripts/site_common.py").read_text()
            )
            (root / "CHANGELOG.md").write_text("## [0.0.0]\n")
            with (
                mock.patch.object(generator, "ROOT", root),
                mock.patch.object(subprocess, "run", side_effect=AssertionError("unexpected RPC")),
            ):
                generator.generate_docs()
            markdown = (root / "docs/API.md").read_text()
            html = (root / "docs/tools.html").read_text()
            self.assertIn(f"{len(tool_policy.CORE_TOOLS)} tools are listed", markdown)
            self.assertIn("scripts/gen-tools-static.py --docs", markdown)
            self.assertIn("Generated from frozen contracts", markdown)
            self.assertNotIn("TOOL_SPECS", markdown + html)
            self.assertIn("| `ledger_op` | advanced |", markdown)
            self.assertIn("<code>repl_execute</code> (advanced)", html)
            self.assertTrue(
                "### `remember_typed` *(gateway, via advanced)*" in markdown,
                "hidden composite badge missing",
            )

    def test_stale_check_does_not_write(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "tools.py"
            cmd = [
                sys.executable,
                str(ROOT / "scripts/gen-tools-static.py"),
                "--output",
                str(output),
            ]
            self.assertEqual(subprocess.run(cmd, capture_output=True).returncode, 0)
            output.write_text(output.read_text() + "# stale\n")
            stale = output.read_bytes()
            result = subprocess.run(cmd + ["--check"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertIn("stale:", result.stdout)
            self.assertEqual(output.read_bytes(), stale)
            output.unlink()
            self.assertEqual(subprocess.run(cmd + ["--check"], capture_output=True).returncode, 1)
            self.assertFalse(output.exists())

    def test_invalid_sources_and_unhandled_composites_fail_closed(self):
        for tools in ([], [self.daemon[0], self.daemon[0]]):
            with self.assertRaises(ValueError):
                generator.generate(tools)
        with mock.patch.object(generator, "composite_handlers", return_value=set()):
            with self.assertRaisesRegex(ValueError, "composites without handlers"):
                generator.generate(self.daemon)

    def test_serialization_preserves_literals_and_escapes(self):
        tools = self.daemon + [
            {
                "name": "synthetic",
                "description": 'Quotes " newline\n backslash \\ unicode λ',
                "inputSchema": {"type": "object", "properties": {"on": {"default": True}}},
            }
        ]
        namespace = {}
        try:
            exec(generator.generate(tools), namespace)
        except ImportError as exc:
            self.skipTest(f"MCP SDK unavailable: {exc}")
        tool = next(t for t in namespace["TOOLS"] if t.name == "synthetic")
        self.assertEqual(tool.description, tools[-1]["description"])
        self.assertIs(tool.inputSchema["properties"]["on"]["default"], True)
