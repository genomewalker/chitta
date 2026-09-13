"""Offline tests for HTTP protocol edges, literature cards, and immutable reviews."""

from __future__ import annotations

import contextlib
import datetime as dt
import io
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from evolve.bridge_client import BridgeClient, BridgeError, json_object, tool_text  # noqa: E402
from evolve.review import collect, parse_verdict, review_prompt, run_review  # noqa: E402
from evolve.sota_watch import (  # noqa: E402
    MemoryStore,
    memory_ids,
    paper_id,
    parse_papers,
    validate_card,
    watch,
)


def result(text):
    return {"content": [{"type": "text", "text": text}]}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def reply(self, code, body=b"", mime="application/json", **headers):
        self.send_response(code)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(body)))
        for key, value in headers.items():
            self.send_header(key, value)
        self.end_headers()
        try:
            self.wfile.write(body)
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_POST(self):
        obj = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.server.seen.append((self.path, dict(self.headers), obj))
        if self.path == "/mcp":
            self.reply(307, Location="/mcp/")
            return
        if self.path == "/bad-redirect":
            self.reply(307, Location="http://localhost:1/mcp/")
            return
        if self.path == "/auth-error":
            self.reply(401, b"sensitive-server-error")
            return
        if self.path == "/slow":
            time.sleep(0.2)
        if obj["method"] == "notifications/initialized":
            self.reply(202)
            return
        if obj["method"] == "initialize":
            response = {"protocolVersion": "2025-03-26", "capabilities": {}}
        elif obj["method"] == "tools/list":
            response = (
                {"tools": [{"name": "second"}]}
                if obj["params"].get("cursor")
                else {"tools": [{"name": "first"}], "nextCursor": "page2"}
            )
        else:
            response = result("ok")
        envelope = {"jsonrpc": "2.0", "id": obj["id"], "result": response}
        if self.path == "/rpc-error":
            envelope = {"id": obj["id"], "error": {"message": "sensitive-server-error"}}
        if self.path == "/tool-error" and obj["method"] == "tools/call":
            envelope["result"]["isError"] = True
        if self.path == "/wrong-id":
            envelope["id"] = 999
        raw = json.dumps(envelope).encode()
        if self.path == "/invalid":
            raw = b"not JSON"
        if self.path in ("/mcp/", "/sse"):
            # A keep-alive stream: matching result must return without waiting for EOF.
            notification = b'data: {"jsonrpc":"2.0","method":"notifications/progress"}\r\n\r\n'
            raw = b": ping\r\n" + notification + b"event: message\r\ndata: " + raw + b"\r\n\r\n"
            self.reply(200, raw, "text/event-stream", **{"Mcp-Session-Id": "fixture-session"})
        else:
            self.reply(200, raw)


class ClientTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        cls.server.daemon_threads = True
        cls.server.seen = []
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.url = "http://127.0.0.1:" + str(cls.server.server_port)

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()

    def client(self, path="/json", **kw):
        return BridgeClient(self.url + path, token="fixture-secret", **kw)

    def test_slash_sse_session_and_pagination(self):
        client = self.client("/mcp")
        self.assertEqual([t["name"] for t in client.list_tools()], ["first", "second"])
        self.assertTrue(client.url.endswith("/mcp/"))
        self.assertEqual(
            tool_text(client.call_tool("web_fetch", {"url": "https://example.org"})), "ok"
        )
        seen = self.server.seen[-1]
        self.assertEqual(seen[1]["Authorization"], "Bearer fixture-secret")
        self.assertEqual(seen[1]["Mcp-Session-Id"], "fixture-session")
        self.assertEqual(seen[1]["Mcp-Protocol-Version"], "2025-03-26")

    def test_json_response(self):
        self.assertEqual(len(self.client().list_tools()), 2)

    def test_errors_do_not_expose_bodies_or_token(self):
        for path in ("/bad-redirect", "/auth-error", "/rpc-error", "/wrong-id", "/invalid"):
            with self.subTest(path=path), self.assertRaises(BridgeError) as caught:
                self.client(path).list_tools()
            self.assertNotIn("fixture-secret", str(caught.exception))
            self.assertNotIn("sensitive-server-error", str(caught.exception))

    def test_tool_error(self):
        with self.assertRaises(BridgeError):
            self.client("/tool-error").call_tool("bad", {})

    def test_timeout(self):
        with self.assertRaises(BridgeError):
            self.client("/slow", timeout=0.03).list_tools()

    def test_token_env_then_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "token"
            path.write_text("file-secret\n")
            with patch.dict(os.environ, {"CHITTA_BRIDGE_TOKEN": "env-secret"}):
                self.assertEqual(BridgeClient(token_file=path)._token, "env-secret")
            with patch.dict(os.environ, {"CHITTA_BRIDGE_TOKEN": ""}):
                self.assertEqual(BridgeClient(token_file=path)._token, "file-secret")

    def test_multiline_sse_and_size_bound(self):
        raw = b'data: {"id": 7,\ndata: "result": {}}\n\n'
        self.assertEqual(
            BridgeClient._sse(io.BytesIO(raw), 7, time.monotonic() + 1), {"id": 7, "result": {}}
        )
        with patch("evolve.bridge_client.MAX_RESPONSE", 10), self.assertRaises(BridgeError):
            BridgeClient._sse(io.BytesIO(raw), 7, time.monotonic() + 1)
        with self.assertRaises(BridgeError):
            BridgeClient._sse(io.BytesIO(b""), 7, time.monotonic() + 1)

    def test_strict_object_and_structured_content(self):
        self.assertEqual(
            json_object('```json\n{"verdict":"pass"}\n```\n(Codex session: fixture)')["verdict"],
            "pass",
        )
        self.assertEqual(json.loads(tool_text({"structuredContent": {"ok": True}})), {"ok": True})
        for raw in ('prose {"verdict":"pass"}', '{"verdict":"pass"}{"verdict":"block"}'):
            with self.assertRaises(ValueError):
                json_object(raw)


EXTRACTED = {
    "mechanism": "Rerank a widened candidate pool",
    "already_have": True,
    "expected_gain": {
        "metric": "recall@20 absolute fraction",
        "delta": 0.1,
        "confidence": 0.2,
        "rationale": "Inventory already implements wide-pool reranking.",
    },
    "cost": {"effort_h": 2, "blast_radius": "retrieval stage"},
    "evidence": ["The abstract describes reranking a widened candidate pool."],
}


def arxiv_fixture(count=7):
    entries = [
        f"[{2609}.{index:05d}v2] Paper {index}\n  Published: 2026-09-10\n"
        f"  Abstract: A new retrieval mechanism\n  URL: https://arxiv.org/abs/2609.{index:05d}v2\n"
        for index in range(count)
    ]
    return "arXiv search: fixture\n\n" + "\n".join(entries)


class FakeMemory:
    def __init__(self, known=()):
        self.known = set(known)
        self.writes = []
        self.fail = False

    def recall(self, query="sota-card"):
        return self.known.copy()

    def remember(self, card):
        if self.fail:
            raise BridgeError("fixture memory failure")
        self.writes.append(card.copy())
        self.known.add(card["id"])
        return {"id": "memory-" + card["id"]}


class FakeLiterature:
    def __init__(self):
        self.calls = []
        self.invalid = False

    def call_tool(self, name, args):
        self.calls.append((name, args))
        if name == "lit_search_arxiv":
            return result(arxiv_fixture())
        if name == "lit_search_openalex":
            return result(
                "OpenAlex search: fixture\n\n[W1] Paper 0\n  DOI: https://doi.org/10.48550/arxiv.2609.00000\n"
            )
        if name == "paper_fetch":
            return result(
                "Abstract: This paper proposes widened retrieval candidates and reranking. " * 10
            )
        if name == "discuss":
            return result("invalid" if self.invalid else json.dumps(EXTRACTED))
        raise AssertionError(name)


class WatchTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.inventory = self.root / "inventory.md"
        self.inventory.write_text("Chitta implements wide-pool recall and cross-encoder reranking.")
        self.client = FakeLiterature()
        self.memory = FakeMemory()

    def run_watch(self, **kw):
        with contextlib.redirect_stderr(io.StringIO()):
            return watch(
                self.client,
                self.memory,
                ["agent memory", "retrieval"],
                self.root / "cards",
                self.inventory,
                today=dt.date(2026, 9, 13),
                **kw,
            )

    def test_cap_dedupe_inventory_and_already_have(self):
        self.memory.known.add("2609.00000")
        report = self.run_watch(max_papers=3)
        self.assertEqual(report["model_calls"], 3)
        self.assertEqual(len(self.memory.writes), 3)
        card = report["cards"][0]
        self.assertEqual(card["id"], "2609.00001")
        self.assertEqual(card["expected_gain"]["delta"], 0)
        self.assertEqual(card["source"], "https://arxiv.org/abs/2609.00001")
        self.assertTrue(card["inventory"]["sha256"])
        prompts = [args["message"] for name, args in self.client.calls if name == "discuss"]
        self.assertIn(self.inventory.read_text(), prompts[0])
        self.assertLess(len(prompts[0]), 42000)
        oa = [args for name, args in self.client.calls if name == "lit_search_openalex"]
        self.assertIn("from_publication_date:2026-08-30", oa[0]["filters"])

    def test_invalid_model_outputs_still_count_toward_cap(self):
        self.client.invalid = True
        report = self.run_watch(max_papers=3)
        self.assertEqual(report["model_calls"], 3)
        self.assertEqual(len(report["errors"]), 3)
        self.assertEqual(self.memory.writes, [])
        self.assertEqual(list((self.root / "cards").glob("*.json")), [])

    def test_pending_memory_is_recovered_without_another_model_call(self):
        self.memory.fail = True
        report = self.run_watch(max_papers=1)
        self.assertEqual(len(report["errors"]), 1)
        self.assertEqual(len(list((self.root / "cards").glob("*.json"))), 1)
        self.memory.fail = False
        self.client.calls.clear()
        self.run_watch(max_papers=0)
        self.assertEqual(len(self.memory.writes), 1)
        self.assertFalse(any(name == "discuss" for name, _ in self.client.calls))

    def test_bounds_and_concurrent_lock(self):
        for kwargs in ({"max_papers": 6}, {"max_papers": -1}, {"days": 0}):
            with self.assertRaises(ValueError):
                self.run_watch(**kwargs)
        (self.root / "cards" / ".sota-watch.lock").mkdir(parents=True)
        with self.assertRaises(BridgeError):
            self.run_watch(max_papers=1)

    def test_date_filter_and_version_canonicalization(self):
        start, end = dt.date(2026, 9, 11), dt.date(2026, 9, 13)
        self.assertEqual(parse_papers(arxiv_fixture(), "arxiv", start, end), [])
        self.assertEqual(paper_id("https://doi.org/10.48550/arxiv.2609.08599v3"), "2609.08599")
        self.assertNotIn("/", paper_id("https://doi.org/10.1234/a/../../bad"))
        self.assertTrue(paper_id("https://doi.org/10.1145/3806774.3832787").startswith("doi-"))
        self.assertTrue(paper_id("https://doi.org/10.1234/2609.08599").startswith("doi-"))
        oa = parse_papers(
            "OpenAlex search: fixture\n\n[W1] Ordinary DOI\n  DOI: https://doi.org/10.1145/3806774.3832787\n",
            "openalex",
            start,
            end,
        )
        self.assertEqual(oa[0]["source"], "https://doi.org/10.1145/3806774.3832787")
        with self.assertRaises(BridgeError):
            parse_papers("Error: timeout", "arxiv", start, end)

    def test_memory_realm_filter_and_large_string_ids(self):
        rows = [
            {"realm": realm, "text": 'sota-card {"id":"2609.00001","mechanism":"test"}'}
            for realm in ("other", "project:chitta-evolve")
        ]
        self.assertEqual(memory_ids({"results": rows[:1]}), set())
        self.assertEqual(memory_ids({"results": rows}), {"2609.00001"})

    def test_recovery_does_not_store_another_proposals_stream(self):
        cards = self.root / "cards"
        cards.mkdir()
        (cards / "other.json").write_text(json.dumps({"id": "other", "mechanism": "unrelated"}))
        self.run_watch(max_papers=0)
        self.assertEqual(self.memory.writes, [])

    def test_recall_rejects_wrong_shape_and_realm(self):
        memory = MemoryStore(Path("/fixture/chitta"))
        with patch("evolve.sota_watch.subprocess.run") as run:
            for data in ({"found": True, "record": "unrelated"}, {"realm": "", "results": []}):
                run.return_value = subprocess.CompletedProcess([], 0, json.dumps(data), "")
                with self.assertRaises(BridgeError):
                    memory.recall()

    def test_rejects_nan_and_schema_confusion(self):
        for invalid in (float("nan"), "0.2", True):
            card = json.loads(json.dumps(EXTRACTED))
            card["expected_gain"]["confidence"] = invalid
            with self.assertRaises(ValueError):
                validate_card(card, {"id": "1", "title": "Title", "source": "url"})

    def test_memory_cli_arguments_and_failure(self):
        memory = MemoryStore(Path("/fixture/chitta"))
        with patch("evolve.sota_watch.subprocess.run") as run:
            run.return_value = subprocess.CompletedProcess([], 0, '{"results":[]}', "")
            self.assertEqual(memory.recall(), set())
            args = run.call_args.args[0]
            self.assertIn("project:chitta-evolve", args)
            self.assertIn("--tag", args)
            run.return_value = subprocess.CompletedProcess(
                [], 0, '{"id":"123","realm":"project:chitta-evolve"}', ""
            )
            memory.remember({"id": "2609.00001"})
            self.assertTrue(run.call_args.args[0][-1].startswith("sota-card {"))
            run.return_value = subprocess.CompletedProcess([], 0, '{"error":"failed"}', "")
            with self.assertRaises(BridgeError):
                memory.recall()


class FakeReview:
    calls = []

    def list_tools(self):
        return [
            {
                "name": "review",
                "inputSchema": {
                    "properties": {"backend": {"description": "codex (default)"}, "model": {}}
                },
            },
            {"name": "codex_review", "inputSchema": {"properties": {"model": {}, "sandbox": {}}}},
        ]

    def call_tool(self, name, args):
        self.calls.append((name, args))
        if name == "review":
            return result("Error: unsupported Claude model")
        verdict = "block" if name == "codex_review" else "concern"
        return result(json.dumps({"verdict": verdict, "reasons": ["Fixture evaluator changes"]}))


class ReviewTests(unittest.TestCase):
    def test_two_reviewers_actual_routing_and_block_aggregation(self):
        FakeReview.calls = []
        inputs = {"base": "a", "head": "b", "diff": "+reward = 1", "artifacts": []}
        reviewed = run_review(FakeReview, Path("/fixture"), inputs)
        self.assertEqual(reviewed["overall"], "block")
        self.assertEqual(reviewed["reviewers"]["claude"]["verdict"], "concern")
        calls = dict(FakeReview.calls)
        self.assertEqual(calls["codex_review"]["model"], "gpt-6-astra")
        self.assertEqual(calls["codex_review"]["sandbox"], "read-only")
        self.assertEqual(calls["discuss"]["backend"], "claude")
        self.assertEqual(calls["codex_review"]["focus"], calls["discuss"]["message"])
        self.assertNotIn("base", calls["codex_review"])
        for phrase in ("Goodhart", "#<id> [pct%]", "snapshot/WAL", "pre-registered bet"):
            self.assertIn(phrase, review_prompt(inputs))

    def test_malformed_review_never_passes(self):
        for raw in ("looks good", "{}", '{"verdict":"pass","reasons":[]}', '{"verdict":"maybe"}'):
            self.assertEqual(parse_verdict(raw)["verdict"], "block")

    def test_missing_bet_evidence_prevents_overall_pass(self):
        class Passing(FakeReview):
            def call_tool(self, name, args):
                return result('{"verdict":"pass","reasons":["No code defects"]}')

        reviewed = run_review(Passing, Path("/fixture"), {"base": "a", "head": "b", "diff": ""})
        self.assertEqual(reviewed["overall"], "concern")
        self.assertEqual(len(reviewed["policy_concerns"]), 2)

    def test_transport_failure_blocks(self):
        class Failed(FakeReview):
            def call_tool(self, name, args):
                raise BridgeError("timeout")

        reviewed = run_review(Failed, Path("/fixture"), {"base": "a", "head": "b", "diff": ""})
        self.assertEqual(reviewed["overall"], "block")
        self.assertTrue(all(v["error"] for v in reviewed["reviewers"].values()))

    def test_collect_uses_head_and_committed_bet_not_dirty_worktree(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)

            def command(*args):
                return subprocess.run(
                    ["git", "-C", tmp, *args], check=True, capture_output=True, text=True
                ).stdout.strip()

            command("init", "-q")
            (repo / "bet.json").write_text('{"metric":"recall","threshold":0.1}')
            (repo / "agent.py").write_text("value = 1\n")
            command("add", ".")
            command(
                "-c",
                "user.name=Fixture",
                "-c",
                "user.email=fixture@example.org",
                "commit",
                "-qm",
                "base",
            )
            base = command("rev-parse", "HEAD")
            (repo / "agent.py").write_text("value = 2\n")
            (repo / "verdict.json").write_text('{"resolved":false}')
            command("add", ".")
            command(
                "-c",
                "user.name=Fixture",
                "-c",
                "user.email=fixture@example.org",
                "commit",
                "-qm",
                "head",
            )
            (repo / "agent.py").write_text("UNCOMMITTED SECRET\n")
            inputs = collect(repo, base, "HEAD", "verdict.json")
            self.assertIn("+value = 2", inputs["diff"])
            self.assertNotIn("UNCOMMITTED", inputs["diff"])
            self.assertTrue(
                any(
                    a["revision"] == "base" and a["path"] == "bet.json" for a in inputs["artifacts"]
                )
            )
            self.assertTrue(
                any(
                    a["revision"] == "head" and a["path"] == "verdict.json"
                    for a in inputs["artifacts"]
                )
            )
            with self.assertRaises(ValueError):
                collect(repo, base, "HEAD", "missing.json")
            with self.assertRaises(ValueError):
                collect(repo, "--bad-option", "HEAD")


if __name__ == "__main__":
    unittest.main()
