"""Tests for the pieces extracted out of server.py.

The handler-map snapshot is the guard on that extraction: server.py stays the
registry, and the set of composite tool names it serves must not drift.
"""

import asyncio
import json
import sys
import threading
import time
import unittest
from pathlib import Path
from unittest import mock

MCP_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(MCP_DIR))

from recall_gateway import RERANK_FETCH_MUL, resolve_onnx_dir, rrf_merge  # noqa: E402
from soul_repl.sandbox import _numeric_memory_id  # noqa: E402

# Snapshot taken before the extraction, from the module that defined every
# handler inline. Adding or removing a composite tool is a deliberate act and
# must update this list in the same commit.
EXPECTED_COMPOSITE_HANDLERS = {
    "ack_memory",
    "advanced",
    "learn",
    "learn_analysis",
    "learn_approach",
    "learn_correction",
    "learn_insight",
    "learn_milestone",
    "learn_outcome",
    "learn_preference",
    "lookup",
    "memory_edit",
    "memory_outcome",
    "nack_memory",
    "read_function",
    "read_symbol",
    "recall",
    "recall_smart",
    "recall_spreading",
    "remember_typed",
    "research",
    "research_cycle",
    "research_store",
    "research_topics",
    "run_hint_enricher",
    "sadhana",
    "smart_context",
    "soul_repl",
    "symbol_callees",
    "symbol_callers",
    "transcript_search",
    "triplets",
    "verify_correction",
}


class HandlerMapSnapshotTests(unittest.TestCase):
    def test_composite_handler_names_unchanged(self):
        try:
            import server
        except ImportError as exc:
            # server.py needs the `mcp` SDK, which is only present in the conda
            # env the MCP server actually runs under. CI installs it; a bare
            # interpreter does not.
            self.skipTest(f"mcp SDK unavailable: {exc}")
        self.assertEqual(set(server.COMPOSITE_HANDLERS), EXPECTED_COMPOSITE_HANDLERS)

    def test_every_handler_is_callable(self):
        try:
            import server
        except ImportError as exc:
            self.skipTest(f"mcp SDK unavailable: {exc}")
        for name, handler in server.COMPOSITE_HANDLERS.items():
            self.assertTrue(callable(handler), f"{name} is not callable")


class RrfMergeTests(unittest.TestCase):
    def test_agreement_across_lanes_outranks_a_single_top_hit(self):
        lane_a = [{"memory_id": "top-of-a"}, {"memory_id": "shared"}]
        lane_b = [{"memory_id": "top-of-b"}, {"memory_id": "shared"}]
        merged = rrf_merge([lane_a, lane_b], limit=3)
        # "shared" places second in both lanes; 2/(60+2) beats a single 1/(60+1).
        self.assertEqual(merged[0]["memory_id"], "shared")

    def test_limit_truncates(self):
        lane = [{"memory_id": str(i)} for i in range(10)]
        self.assertEqual(len(rrf_merge([lane], limit=3)), 3)

    def test_falls_back_to_text_when_no_memory_id(self):
        lane_a = [{"text": "same passage"}]
        lane_b = [{"text": "same passage"}]
        merged = rrf_merge([lane_a, lane_b], limit=5)
        self.assertEqual(len(merged), 1, "identical text should fuse to one row")

    def test_empty_input(self):
        self.assertEqual(rrf_merge([], limit=5), [])
        self.assertEqual(rrf_merge([[], []], limit=5), [])

    def test_preserves_the_original_items(self):
        item = {"memory_id": "1", "text": "hello", "score": 0.5}
        self.assertEqual(rrf_merge([[item]], limit=1)[0], item)


class RerankConfigTests(unittest.TestCase):
    def test_fetch_multiplier_overfetches(self):
        self.assertGreater(RERANK_FETCH_MUL, 1)

    def test_onnx_dir_honors_env_override(self):
        import os

        prior = os.environ.get("CHITTA_RERANK_ONNX_DIR")
        os.environ["CHITTA_RERANK_ONNX_DIR"] = "/nonexistent/override"
        try:
            self.assertEqual(resolve_onnx_dir(), "/nonexistent/override")
        finally:
            if prior is None:
                del os.environ["CHITTA_RERANK_ONNX_DIR"]
            else:
                os.environ["CHITTA_RERANK_ONNX_DIR"] = prior


class RerankerExecutorTests(unittest.IsolatedAsyncioTestCase):
    async def test_predict_runs_off_the_event_loop(self):
        try:
            import server
        except ImportError as exc:
            self.skipTest(f"mcp SDK unavailable: {exc}")

        class FakeReranker:
            predict_thread = None

            def predict(self, pairs):
                self.predict_thread = threading.get_ident()
                return [0.1, 0.9]

        reranker = FakeReranker()
        candidates = [
            {"memory_id": "first", "text": "first passage"},
            {"memory_id": "second", "text": "second passage"},
        ]
        loop_thread = threading.get_ident()
        with (
            mock.patch.object(server, "get_reranker", return_value=reranker),
            mock.patch.object(
                server,
                "daemon_call",
                return_value=json.dumps({"results": candidates}),
            ),
            mock.patch.object(server, "_sqz_compress", side_effect=lambda text, _name: text),
        ):
            output = await server.call_tool(
                "recall", {"query": "test query", "strategy": "semantic", "limit": 1}
            )

        self.assertNotEqual(reranker.predict_thread, loop_thread)
        self.assertEqual(json.loads(output[0].text)["results"][0]["memory_id"], "second")


class LoopLagMonitorTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        try:
            import server
        except ImportError as exc:
            self.skipTest(f"mcp SDK unavailable: {exc}")
        self.server = server
        server._loop_lag_max_ms = 0.0
        server._loop_lag_over_count = 0

    async def test_monitor_counts_a_deliberate_loop_stall(self):
        with self.assertLogs("chitta-mcp", level="WARNING"):
            task = asyncio.create_task(self.server.monitor_loop_lag(interval_ms=5, warn_ms=10))
            await asyncio.sleep(0.01)
            time.sleep(0.04)
            await asyncio.sleep(0.02)
            await self.server._stop_loop_lag_monitor(task)

        stats = self.server.loop_lag_stats()
        self.assertGreaterEqual(stats["max_lag_ms"], 20)
        self.assertGreaterEqual(stats["over_count"], 1)

    async def test_health_check_exposes_loop_lag_counters(self):
        with (
            mock.patch.object(self.server, "daemon_call", return_value="daemon healthy"),
            mock.patch.object(self.server, "_sqz_compress", side_effect=lambda text, _name: text),
        ):
            output = await self.server.call_tool("health_check", {})

        text = output[0].text
        self.assertIn('"mcp_loop_lag"', text)
        self.assertIn('"max_lag_ms":0.0', text)
        self.assertIn('"over_count":0', text)


class LoopLagConfigTests(unittest.TestCase):
    def test_legacy_alias_and_chitta_precedence(self):
        import os

        try:
            import server
        except ImportError as exc:
            self.skipTest(f"mcp SDK unavailable: {exc}")
        with mock.patch.dict(
            os.environ,
            {"CC_SOUL_MCP_LAG_INTERVAL_MS": "17"},
            clear=True,
        ):
            self.assertEqual(
                server._lag_setting_ms(
                    "CHITTA_MCP_LAG_INTERVAL_MS", "CC_SOUL_MCP_LAG_INTERVAL_MS", 250
                ),
                17,
            )
        with mock.patch.dict(
            os.environ,
            {
                "CHITTA_MCP_LAG_INTERVAL_MS": "31",
                "CC_SOUL_MCP_LAG_INTERVAL_MS": "17",
            },
            clear=True,
        ):
            self.assertEqual(
                server._lag_setting_ms(
                    "CHITTA_MCP_LAG_INTERVAL_MS", "CC_SOUL_MCP_LAG_INTERVAL_MS", 250
                ),
                31,
            )


class NumericMemoryIdTests(unittest.TestCase):
    def test_plain_int_passes_through(self):
        self.assertEqual(_numeric_memory_id(42), 42)

    def test_padded_uuid_decodes_hex_suffix(self):
        self.assertEqual(_numeric_memory_id("00000000-0000-0000-0000-00000000002a"), 42)

    def test_unparseable_suffix_is_zero(self):
        self.assertEqual(_numeric_memory_id("00000000-0000-0000-0000-zzzz"), 0)

    def test_unrecognized_shapes_are_zero(self):
        self.assertEqual(_numeric_memory_id("some-uuid"), 0)
        self.assertEqual(_numeric_memory_id(None), 0)


if __name__ == "__main__":
    unittest.main()
