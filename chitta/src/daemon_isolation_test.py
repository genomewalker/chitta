"""Exercise real daemon tag recall and queue isolation with synthetic state only."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


def main():
    daemon, cli, lib = map(lambda x: str(Path(x).resolve()), sys.argv[1:4])
    dimension = int(sys.argv[4])

    class EmbedFixture(BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_GET(self):
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b'{"models": []}')

        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            vectors = [[1.0] + [0.0] * (dimension - 1) for _ in body["input"]]
            self.send_response(200)
            self.end_headers()
            self.wfile.write(json.dumps({"embeddings": vectors}).encode())

    embed_server = ThreadingHTTPServer(("127.0.0.1", 0), EmbedFixture)
    threading.Thread(target=embed_server.serve_forever, daemon=True).start()
    artifact_dir = os.environ.get("CHITTA_TEST_ARTIFACT_DIR")
    temporary = tempfile.TemporaryDirectory(prefix="chitta-isolation-")
    root = Path(artifact_dir or temporary.name)
    root.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    for key in ("CHITTA_QUEUE", "CHITTA_QUEUE_PATH", "CHITTA_NO_QUEUE",
                "MIND_PATH", "CHITTA_EMBED_URL", "CHITTA_EMBED_GPU_ONLY"):
        env.pop(key, None)
    mind = root / "mind"
    mind.mkdir(exist_ok=True)
    env.update(HOME=str(root / "home"), XDG_RUNTIME_DIR=str(Path(temporary.name) / "run"),
               CHITTA_EMBED_URL="http://127.0.0.1:" + str(embed_server.server_port),
               CHITTA_DB_PATH=str(mind), CHITTA_EMBED_MODEL=str(root / "missing.gguf"))
    Path(env["XDG_RUNTIME_DIR"]).mkdir(exist_ok=True)
    Path(env["HOME"]).mkdir(exist_ok=True)

    def call(tool, **args):
        request = {"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                   "params": {"name": tool, "arguments": args}}
        result = subprocess.run([cli], env=env, input=json.dumps(request) + "\n",
                                capture_output=True, text=True, timeout=15, check=True)
        response = json.loads(result.stdout)
        assert "error" not in response, response
        assert not response["result"].get("isError", False), response
        return response["result"].get("structured", response["result"])

    def hook_path(extra=None):
        test_env = dict(env, **(extra or {}))
        return subprocess.check_output(
            ["bash", "-c", 'source "$1"; get_queue_file', "test", lib],
            env=test_env, stdin=subprocess.DEVNULL, text=True).strip()

    assert hook_path() == str(mind / "queue.jsonl")
    assert hook_path({"MIND_PATH": str(root / "other")}) == str(root / "other/queue.jsonl")
    assert hook_path({"CHITTA_QUEUE": str(root / "explicit"),
                      "CHITTA_QUEUE_PATH": str(root / "legacy")}) == str(root / "explicit")

    def queued(path, content):
        path.write_text(json.dumps({"tool": "observe", "args": {
            "content": content, "realm": "project:queue", "category": "wisdom",
            "source": "mcp_tool"},
            "ack_id": content, "ts": int(time.time())}) + "\n")

    def run_case(name, queue, disabled=False):
        log_path = root / (name + ".log")
        trace_path = root / (name + ".trace")
        command = [daemon, "daemon", "--foreground", "--path", str(mind),
                   "--no-distill", "--no-enrich", "--no-hygiene",
                   "--no-autonomous", "--no-embed-interval", "--rpc-port", "0", "--http-port", "0"]
        tracer = shutil.which("strace")
        if tracer:
            command = [tracer, "-f", "-e", "trace=%file", "-o", str(trace_path)] + command
        with log_path.open("w") as log:
            proc = subprocess.Popen(command, env=env, stdin=subprocess.DEVNULL,
                                    stdout=log, stderr=log, start_new_session=True)
            try:
                deadline = time.monotonic() + 45
                while True:
                    assert proc.poll() is None, log_path.read_text()[-4000:]
                    try:
                        call("health_check")
                        break
                    except (subprocess.SubprocessError, ValueError):
                        if time.monotonic() >= deadline:
                            raise AssertionError(log_path.read_text()[-4000:]) from None
                        time.sleep(0.2)
                if name == "default":
                    for realm in ("project:a", "project:b", "brahman"):
                        call("remember", content="tagrealmcanary shared fact " + realm,
                             realm=realm, tags="shared-tag", type="wisdom")
                    call("remember", content="tagrealmcanary untagged", realm="project:a")
                    for strategy in ("keyword", "field", "fused"):
                        for query in ("tagrealmcanary", "zzzznomatchingwordszzzz"):
                            for realm in ("project:a", "project:b", "brahman"):
                                response = call("recall", query=query, tag="shared-tag", realm=realm,
                                                strategy=strategy, no_learn=True, limit=1)
                                rows = response["results"]
                                assert len(rows) == 1, response
                                assert rows[0]["realm"] == realm, response
                    rows = call("recall", query="zzzznomatchingwordszzzz", tag="shared-tag",
                                no_learn=True)["results"]
                    assert {row["realm"] for row in rows} == {"project:a", "project:b", "brahman"}
                    assert not call("recall", query="tagrealmcanary", tag="absent-tag",
                                    no_learn=True)["results"]
                    assert not call("recall", query="zzzznomatchingwordszzzz", tag="shared-tag",
                                    realm="project:missing", no_learn=True)["results"]
                    # Queued ledger_op used to be silently ignored. Require a
                    # durable capsule round-trip through the real queue dispatch.
                    cap_args = {"session_id": "queued-handoff", "project_dir": "/synthetic/project",
                                "branch": "feature", "next_action": "Next: verify durable queue",
                                "artifact_paths": ["fixture.cpp"], "blocker": "", "saved_at": 100}
                    prepared = call("ledger_op", op="hook_handoff_prepare", args=cap_args)["value"]
                    subprocess.run([cli, "queue_write", "ledger_op", json.dumps(prepared)],
                                   env=env, stdin=subprocess.DEVNULL, capture_output=True, check=True)
                    deadline = time.monotonic() + 10
                    while True:
                        card = call("ledger_op", op="hook_handoff_context",
                                    args={"project_dir": "/synthetic/project", "branch": "feature"})["value"]["text"]
                        if "Next: verify durable queue" in card:
                            break
                        assert time.monotonic() < deadline, "queued ledger_op was not applied"
                        time.sleep(0.1)
                    turn = call("ledger_op", op="hook_turn", args={"session_id": "queued-handoff",
                                "role": "assistant", "content": "synthetic turn\n", "turn_index": 1,
                                "tools_used": [], "files_touched": [], "has_error": False})
                    assert turn["value"]["event_id"] > 0
                    print("LEDGER: queued capsule applied and native turn event appended")
                    print("TAG: two project realms + brahman, 18 scoped recalls passed; unscoped preserved")
                    for tool in ("recall", "smart_recall", "hybrid_recall", "recall_keyword"):
                        args = {"query": "tagrealmcanary", "no_learn": True}
                        if tool == "recall":
                            args.update(query="zzzznomatchingwordszzzz", tag="shared-tag")
                        response = call(tool, **args)
                        (root / (tool + ".json")).write_text(json.dumps(response, indent=2))
                if disabled:
                    time.sleep(1)
                    assert queue.exists(), "CHITTA_NO_QUEUE consumed the queue"
                    assert "Queue processor DISABLED" in log_path.read_text()
                else:
                    deadline = time.monotonic() + 15
                    while True:
                        rows = call("recall_keyword", query=name + "queuecanary",
                                    realm="project:queue", no_learn=True)["results"]
                        if rows:
                            break
                        assert time.monotonic() < deadline, log_path.read_text()[-3000:]
                        time.sleep(0.2)
                    assert "path=" + str(queue) + ")" in log_path.read_text()
                print("QUEUE:", name, "passed", queue)
            finally:
                # Stop only this owned scratch daemon via its isolated socket.
                subprocess.run([cli, "shutdown"], env=env, stdin=subprocess.DEVNULL,
                               capture_output=True, timeout=20)
                try:
                    proc.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, 15)
                    proc.wait(timeout=10)
        if tracer:
            trace = trace_path.read_text()
            assert "/tmp/chitta-queue.jsonl" not in trace, "scratch touched legacy shared queue"
            print("QUEUE: strace confirms no legacy shared queue or recovery siblings accessed")

    queue = mind / "queue.jsonl"
    # Exercise native writing without any queue override.
    subprocess.run([cli, "queue_write", "observe", json.dumps({
        "content": "defaultqueuecanary", "realm": "project:queue",
        "category": "wisdom", "source": "mcp_tool"})],
        env=env, stdin=subprocess.DEVNULL, check=True)
    assert queue.exists()
    # Recover the private fast/slow siblings; malformed input must dead-letter
    # alongside this queue. Nothing is ever seeded in the real shared /tmp queue.
    Path(str(queue) + ".processing").write_text("malformed-json\n")
    Path(str(queue) + ".slow.processing").write_text("malformed-json\n")
    run_case("default", queue)
    assert Path(str(queue) + ".failed").exists(), "missing queue-local dead letters"
    explicit = root / "explicit.jsonl"
    env["CHITTA_QUEUE"] = str(explicit)
    env["CHITTA_QUEUE_PATH"] = str(root / "unused-legacy.jsonl")
    queued(explicit, "explicitqueuecanary")
    run_case("explicit", explicit)
    env.pop("CHITTA_QUEUE")
    legacy = Path(env["CHITTA_QUEUE_PATH"])
    queued(legacy, "legacyqueuecanary")
    run_case("legacy", legacy)
    env.pop("CHITTA_QUEUE_PATH")
    env["CHITTA_NO_QUEUE"] = "1"
    queued(queue, "disabledqueuecanary")
    run_case("disabled", queue, disabled=True)
    print("daemon_isolation_test: passed")
    embed_server.shutdown()
    embed_server.server_close()
    temporary.cleanup()


if __name__ == "__main__":
    main()
