"""One isolated end-to-end ledger test, including crashes, snapshots and queue replay."""

from __future__ import annotations

import inspect
import json
import os
import sqlite3
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "chitta-mcp"))
import daemon_client  # noqa: E402
import task_ledger as ledger  # noqa: E402


def main():
    daemon = str(Path(sys.argv[1]).resolve())
    dimension = int(sys.argv[2])
    migration_source = os.environ.get("CHITTA_LEDGER_MIGRATION_SOURCE")
    evidence = os.environ.get("CHITTA_LEDGER_EVIDENCE")
    contract = json.loads((ROOT / "chitta-mcp/tests/ledger_contract.json").read_text())
    for name, signature in contract["signatures"].items():
        assert str(inspect.signature(getattr(ledger, name))) == signature, name

    class EmbedFixture(BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_GET(self):
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b'{"models": []}')

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            self.send_response(200)
            self.end_headers()
            self.wfile.write(
                json.dumps(
                    {"embeddings": [[1.0] + [0.0] * (dimension - 1) for _ in body["input"]]}
                ).encode()
            )

    embed = ThreadingHTTPServer(("127.0.0.1", 0), EmbedFixture)
    threading.Thread(target=embed.serve_forever, daemon=True).start()
    report = {"signature_diffs": {}, "key_set_diffs": {}, "restart": [], "timings_ms": {}}
    with tempfile.TemporaryDirectory(prefix="chitta-ledger-") as temp:
        root = Path(temp)
        for name in ("home", "run", "mind"):
            (root / name).mkdir()
        for name in (
            "CHITTA_RPC_PORT",
            "CHITTA_RPC_HOST",
            "CHITTA_SOCKET_PATH",
            "CHITTA_QUEUE",
            "CHITTA_QUEUE_PATH",
            "CHITTA_EMBED_GPU_ONLY",
            "CHITTA_HEADLESS",
            "CC_SOUL_HEADLESS",
        ):
            os.environ.pop(name, None)
        os.environ.update(
            HOME=str(root / "home"),
            XDG_RUNTIME_DIR=str(root / "run"),
            MIND_PATH=str(root / "mind"),
            CHITTA_DB_PATH=str(root / "mind"),
            CHITTA_NO_QUEUE="1",
            CHITTA_EMBED_URL=f"http://127.0.0.1:{embed.server_port}",
            CHITTA_EMBED_MODEL=str(root / "missing.gguf"),
        )
        os.environ["CHITTA_SOCKET_PATH"] = daemon_client.get_socket_path()
        process = None
        log = (root / "daemon.log").open("w")

        def raw(tool, **args):
            daemon_client._unavailable_until = 0
            result = daemon_client.daemon_call(tool, args, timeout=10)
            assert result is not None, (tool, args, (root / "daemon.log").read_text()[-1800:])
            return result

        def start(queue=False):
            nonlocal process
            env = dict(os.environ)
            if queue:
                env.pop("CHITTA_NO_QUEUE", None)
            process = subprocess.Popen(
                [
                    daemon,
                    "daemon",
                    "--foreground",
                    "--path",
                    str(root / "mind"),
                    "--no-distill",
                    "--no-enrich",
                    "--no-hygiene",
                    "--no-autonomous",
                    "--no-embed-interval",
                    "--rpc-port",
                    "0",
                    "--http-port",
                    "0",
                ],
                env=env,
                stdin=subprocess.DEVNULL,
                stdout=log,
                stderr=log,
            )
            deadline = time.monotonic() + 45
            while time.monotonic() < deadline:
                assert process.poll() is None, (root / "daemon.log").read_text()[-3000:]
                daemon_client._unavailable_until = 0
                ready = daemon_client.daemon_call(
                    "ledger_op", {"op": "counts", "args": {}}, timeout=0.2
                )
                if ready and "value" in ready:
                    return
                time.sleep(0.1)
            raise AssertionError((root / "daemon.log").read_text()[-3000:])

        def crash():
            process.kill()  # only this test-owned scratch daemon
            process.wait(timeout=10)

        def rows():
            return dict(
                threads=ledger.thread_list(limit=-1),
                thread_sessions=ledger.session_list(limit=-1),
                thread_leases=ledger.lease_list(False),
                artifacts=ledger.artifact_list(),
                inbox=sum(
                    (
                        ledger.inbox_list(state=state, limit=-1)
                        for state in ("pending", "delivered", "acked", "suppressed")
                    ),
                    [],
                ),
            )

        def snapshot_rows():
            return {
                name: sorted(values, key=lambda r: json.dumps(r, sort_keys=True))
                for name, values in rows().items()
            }

        try:
            start()
            # hook_session_start fans out through std::async threads that re-enter
            # ledger_op. Holding tool_ledger_op's stream_mutex across that dispatch
            # self-deadlocked the live daemon twice on 2026-09-20; a wedge shows here
            # as daemon_call's 10 s timeout. ledger_profile proves the launches ran.
            began = time.monotonic()
            session_start = raw(
                "ledger_op",
                op="hook_session_start",
                args={"input": {"session_id": "deadlock-check", "source": "startup"}, "realm": "test", "now": 0},
            )
            assert time.monotonic() - began < 8, session_start
            assert "ledger_profile" in json.dumps(session_start), session_start
            if migration_source:
                command = [
                    sys.executable,
                    str(ROOT / "chitta-mcp/task_ledger.py"),
                    "migrate",
                    "--from",
                    migration_source,
                ]
                first = json.loads(subprocess.check_output(command, text=True, timeout=120))
                second = json.loads(subprocess.check_output(command, text=True, timeout=120))
                assert first["rows"] == first["inserted"], first
                assert not any(second["inserted"].values()), second
                with sqlite3.connect(
                    Path(migration_source).resolve().as_uri() + "?mode=ro", uri=True
                ) as conn:
                    conn.row_factory = sqlite3.Row
                    imported = rows()
                    for table, actual in imported.items():
                        expected = [dict(row) for row in conn.execute("SELECT * FROM " + table)]
                        key = {
                            "threads": "thread_id",
                            "thread_sessions": "session_id",
                            "thread_leases": "thread_id",
                            "inbox": "item_id",
                            "artifacts": "artifact_id",
                        }[table]
                        assert {r[key]: r for r in actual} == {r[key]: r for r in expected}, table
                report["migration"] = {"first": first, "second": second, "all_values_equal": True}

            # Two sessions with the same exact key share one monotonic revision.
            cap = {
                "session_id": "capsule-a",
                "repository": "/projects/capsule-test/.git",
                "stream_id": "same-stream",
                "next_action": "run the gate",
            }
            saved = raw(
                "ledger_op", op="capsule_save", args={"expected_revision": 0, "capsule": cap}
            )["value"]
            assert saved["version"] == 2 and saved["revision"] == 1
            assert len(json.dumps(saved, separators=(",", ":")).encode()) <= 4096
            cap["session_id"] = "capsule-b"
            cap["repository"] = "/maps/projects/capsule-test/.git"
            saved = raw(
                "ledger_op", op="capsule_save", args={"expected_revision": 1, "capsule": cap}
            )["value"]
            assert saved["revision"] == 2
            try:
                stale = raw(
                    "ledger_op", op="capsule_save", args={"expected_revision": 1, "capsule": cap}
                )
            except Exception:
                stale = None
            assert not stale or "value" not in stale, "stale writer succeeded"
            current = json.loads(ledger.session_get("capsule-b")["metadata_json"])["handoff"]
            assert current["revision"] == 2
            # A queued Stop prepared at revision one cannot replace a milestone.
            queued = {
                "session_id": "capsule-b",
                "expected_revision": 1,
                "metadata": {"handoff": saved},
            }
            try:
                rejected = raw("ledger_op", op="session_bind", args=queued)
            except Exception:
                rejected = None
            assert not rejected or "value" not in rejected
            assert (
                json.loads(ledger.session_get("capsule-b")["metadata_json"])["handoff"]["revision"]
                == 2
            )

            for altered in (
                dict(saved, revision=3, session_id="wrong-session"),
                dict(saved, revision=1, stream_id="different-stream"),
            ):
                queued = {
                    "session_id": "capsule-b",
                    "expected_revision": 2 if altered["revision"] == 3 else 0,
                    "metadata": {"handoff": altered},
                }
                try:
                    rejected = raw("ledger_op", op="session_bind", args=queued)
                except Exception:
                    rejected = None
                assert not rejected or "value" not in rejected, "capsule identity changed"
            assert json.loads(ledger.session_get("capsule-b")["metadata_json"])["handoff"] == saved

            tid = ledger.thread_create("integration", "project:ledger", "fingerprint")
            assert tid
            assert ledger.thread_update(tid, title="updated", ignored="discard")
            assert not ledger.thread_update(tid, ignored="discard")
            assert ledger.thread_get(tid)["title"] == "updated"
            assert ledger.session_bind(
                "owner", tid, client="codex", project_dir=str(root), metadata={"model": "test"}
            )
            assert ledger.session_bind("owner", metadata={"host": "node"})
            assert json.loads(ledger.session_get("owner")["metadata_json"]) == {
                "model": "test",
                "host": "node",
            }
            assert ledger.session_bind("contender")
            claim = ledger.lease_claim(tid, "owner", ttl=30)
            conflict = ledger.lease_claim(tid, "contender")
            assert claim["claimed"] and not conflict["claimed"]
            assert sorted(claim) == contract["lease_claim_success"]
            assert sorted(conflict) == contract["lease_claim_conflict"]
            assert ledger.session_get("contender")["thread_id"] is None
            assert ledger.lease_claim(tid, "owner")["generation"] == claim["generation"]
            takeover = ledger.lease_claim(tid, "contender", force=True)
            assert takeover["generation"] == claim["generation"] + 1
            assert ledger.lease_release("contender", tid)
            assert ledger.lease_claim(tid, "owner")["claimed"]

            iid = ledger.inbox_push(
                "task", "completed", "done", "project:ledger", tid, {"ok": True}
            )
            global_iid = ledger.inbox_push("task", "note", "global", "")
            assert {i["item_id"] for i in ledger.inbox_list("project:ledger")} >= {iid, global_iid}
            assert ledger.inbox_ack(iid, "delivered") and ledger.inbox_ack(iid)
            assert ledger.inbox_list("project:ledger", "acked")[0]["acked_at"]
            artifact = root / "result.txt"
            artifact.write_text("ledger result\n")
            aid = ledger.artifact_register("task", str(artifact), thread_id=tid)
            child = ledger.artifact_register(
                "task", str(root / "missing"), thread_id=tid, parent_artifact_id=aid
            )
            assert ledger.artifact_list(path_glob=str(root / "*.txt"))[0]["artifact_id"] == aid
            assert [r["artifact_id"] for r in ledger.artifact_lineage(child)] == [child, aid]
            assert ledger.artifact_link(
                aid, child
            )  # legacy API permits cycles; traversal must stop
            assert len(ledger.artifact_lineage(child)) == 2
            assert ledger.thread_seal(tid)
            assert ledger.thread_get(tid)["sealed_at"]
            for table, values in rows().items():
                for row in values:
                    assert sorted(row) == contract["row_keys"][table], (table, row)
                report["key_set_diffs"][table] = {"added": [], "removed": []}

            # Bounded RPC pages, legacy Python list limits and unlimited artifact lists.
            for i in range(105):
                assert ledger.thread_create(f"page-{i}", "project:pages")
            assert len(ledger.thread_list("project:pages", limit=103)) == 103
            page = raw(
                "ledger_op", op="thread_list", args={"realm": "project:pages", "limit": 999}
            )["value"]
            assert len(page["rows"]) == 100 and page["after"]
            raw(
                "session_register",
                session_id="native",
                realm="project:ledger",
                pid=os.getpid(),
                client="codex",
                project_dir=str(root),
                metadata={"thread_id": tid},
            )
            assert ledger.lease_release("owner", tid)
            assert ledger.lease_claim(tid, "native", ttl=30)["claimed"]
            before = snapshot_rows()
            crash()
            start()
            after = snapshot_rows()
            if after != before:
                if evidence:
                    Path(evidence + ".diff").write_text(
                        json.dumps({"before": before, "after": after}, indent=2)
                    )
                raise AssertionError(
                    "WAL restart changed ledger rows: "
                    + repr(
                        {
                            k: (len(before[k]), len(after[k]))
                            for k in before
                            if before[k] != after[k]
                        }
                    )
                )
            report["restart"].append("acknowledged writes survived SIGKILL and WAL replay")
            # Existing compaction protects near-empty stores (minimum 100 memories).
            # Distinct private realms prevent embedding dedup in this synthetic fixture.
            for i in range(100):
                raw(
                    "remember",
                    content=f"Private ledger snapshot fixture memory {i}",
                    realm=f"project:ledger-snapshot-{i}",
                )
            compacted = raw("compact_wal")
            assert compacted["segments_deleted"] > 0, compacted
            report["snapshot_segments_deleted"] = compacted["segments_deleted"]
            # WAL suffix after a full snapshot must apply once, including deletions.
            assert ledger.thread_update(tid, title="after snapshot")
            raw("session_heartbeat", session_id="native")
            before = snapshot_rows()
            crash()
            start()
            assert snapshot_rows() == before, "snapshot/WAL suffix changed ledger rows"
            native = raw("session_list", status="all")["sessions"]
            assert any(s["session_id"] == "native" for s in native)
            report["restart"].append(
                "full snapshot + WAL suffix survived SIGKILL, including session_list"
            )

            # Measure actual PyPy startup + registry CLI + daemon acknowledgement.
            pypy = os.environ.get(
                "CHITTA_TEST_PYPY", "/maps/projects/fernandezguerra/apps/opt/conda/bin/python3"
            )
            if Path(pypy).is_file():
                samples = {"register": [], "heartbeat": [], "close": []}
                for iteration in range(7):
                    payload = json.dumps(
                        {
                            "session_id": f"timing-{iteration}",
                            "realm": "project:ledger",
                            "cwd": str(root),
                        }
                    )
                    for command in samples:
                        args = [pypy, str(ROOT / "chitta-mcp/session_registry.py"), command]
                        if command == "register":
                            args += ["--client", "codex"]
                        started = time.perf_counter()
                        result = subprocess.run(
                            args,
                            input=payload,
                            text=True,
                            capture_output=True,
                            check=True,
                            timeout=3,
                        )
                        samples[command].append((time.perf_counter() - started) * 1000)
                        value = json.loads(result.stdout)
                        assert value[
                            {"register": "registered", "heartbeat": "heartbeat", "close": "closed"}[
                                command
                            ]
                        ], value
                report["timings_ms"] = {
                    name: {
                        "median": round(statistics.median(values), 2),
                        "max": round(max(values), 2),
                        "samples": [round(v, 2) for v in values],
                    }
                    for name, values in samples.items()
                }

            # Queued hook performs no RPC. Restart with only the private queue enabled.
            crash()
            start(queue=True)
            import session_registry

            session_registry.CHITTA_QUEUE = root / "mind/queue.jsonl"
            old_expiry = next(
                lease["expires_at"]
                for lease in ledger.lease_list(False)
                if lease["session_id"] == "native"
            )
            assert session_registry.heartbeat({"session_id": "native"}, queued=True)["heartbeat"]
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                leases = ledger.lease_list(False)
                if any(
                    lease["session_id"] == "native" and lease["expires_at"] > old_expiry
                    for lease in leases
                ):
                    break
                time.sleep(0.1)
            else:
                raise AssertionError("queued heartbeat did not renew daemon lease")
            assert session_registry._queue_tool("session_deregister", {"session_id": "native"})
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                if ledger.session_get("native")["status"] == "ended":
                    break
                time.sleep(0.1)
            else:
                raise AssertionError("queued close did not update binding")
            assert not any(lease["session_id"] == "native" for lease in ledger.lease_list(False))
            report["queue"] = (
                "heartbeat renews binding/lease; deregister ends binding and deletes lease"
            )
            before = snapshot_rows()
            crash()
            start()
            assert snapshot_rows() == before, "queue lifecycle was not durable"
            report["restart"].append("queued lifecycle survived restart")
            print(json.dumps(report, sort_keys=True))
            if evidence:
                Path(evidence).write_text(json.dumps(report, indent=2) + "\n")
        finally:
            if process and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=10)
            log.close()
            if evidence:
                Path(evidence + ".log").write_text((root / "daemon.log").read_text())
            embed.shutdown()


if __name__ == "__main__":
    main()
