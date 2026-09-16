"""Real code-index regression and optional repository coverage on an empty mind."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


def main():
    daemon, cli = [str(Path(p).resolve()) for p in sys.argv[1:3]]
    with tempfile.TemporaryDirectory(prefix="chitta-code-index-") as tmp:
        base = Path(tmp)
        env = dict(os.environ)
        for key in list(env):
            if key.startswith(("CHITTA_", "CC_SOUL_")):
                env.pop(key)

        class EmbedFixture(BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def do_GET(self):
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b'{"models": []}')

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                vectors = [[1.0] + [0.0] * 767 for _ in body["input"]]
                self.send_response(200)
                self.end_headers()
                self.wfile.write(json.dumps({"embeddings": vectors}).encode())

        server = ThreadingHTTPServer(("127.0.0.1", 0), EmbedFixture)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        env.update(
            HOME=str(base / "home"),
            XDG_RUNTIME_DIR=str(base / "run"),
            CHITTA_DB_PATH=str(base / "mind"),
            CHITTA_EMBED_URL="http://127.0.0.1:" + str(server.server_port),
            CHITTA_EMBED_MODEL=str(base / "missing.gguf"),
            OPENBLAS_NUM_THREADS="1",
            OMP_NUM_THREADS="1",
            RAYON_NUM_THREADS="1",
        )
        for name in ("home", "run", "mind"):
            (base / name).mkdir()
        log = (base / "daemon.log").open("w")
        proc = subprocess.Popen(
            [
                daemon,
                "daemon",
                "--foreground",
                "--path",
                str(base / "mind"),
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

        def call(tool, **args):
            req = {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "tools/call",
                "params": {"name": tool, "arguments": args},
            }
            reply = subprocess.run(
                [cli],
                input=json.dumps(req) + "\n",
                text=True,
                capture_output=True,
                env=env,
                timeout=90,
                check=True,
            )
            response = json.loads(reply.stdout)
            assert "error" not in response, response
            result = response["result"]
            assert not result.get("isError"), result
            return result.get("structured", result)

        try:
            for _ in range(120):
                try:
                    call("enrichment_status")
                    break
                except (AssertionError, ValueError, subprocess.SubprocessError):
                    assert proc.poll() is None, (base / "daemon.log").read_text()[-2000:]
                    time.sleep(0.25)
            else:
                raise AssertionError(
                    "scratch daemon did not start: " + (base / "daemon.log").read_text()[-5000:]
                )
            repo = base / "repo"
            repo.mkdir()
            subprocess.run(["git", "init", "-q", str(repo)], check=True)
            source = repo / "a.py"
            source.write_text("def alpha():\n    return beta()\n\ndef beta():\n    return 1\n")
            (repo / "b.py").write_text("def second():\n    return 2\n")
            indexed = call(
                "learn_codebase", path=str(source), project="project:fixture", incremental=True
            )
            assert indexed["files_total"] == 2, indexed
            assert call("codebase_overview", project="fixture")["symbols"] == 3
            assert call("codebase_overview", project="project:fixture")["symbols"] == 3
            call("learn_codebase", path=str(repo))
            assert call("codebase_overview", project="fixture")["project"] == "fixture"
            context = call("code_context", path=str(source))
            assert context["file_symbols"] == 2 and context["total_symbols"] == 3, context
            source.write_text("def renamed():\n    return 3\n")
            call("learn_codebase", path=str(source), project="fixture", incremental=True)
            assert not call("find_symbol", name="alpha")["symbols"]
            assert call("find_symbol", name="renamed")["count"] == 1
            source.unlink()
            call("learn_codebase", path=str(source), project="fixture", incremental=True)
            assert not call("find_symbol", name="renamed")["symbols"]
            assert call("clear_codebase", project="fixture")["rc"] == 0
            assert not call("code_query", path=str(repo))["indexed"]
            if len(sys.argv) > 3:
                root = Path(sys.argv[3]).resolve()
                started = time.perf_counter()
                result = call("learn_codebase", path=str(root), project="chitta")
                elapsed = time.perf_counter() - started
                extensions = {
                    ".nf",
                    ".jl",
                    ".R",
                    ".r",
                    ".sh",
                    ".bash",
                    ".c",
                    ".h",
                    ".cpp",
                    ".hpp",
                    ".cc",
                    ".cxx",
                    ".hxx",
                    ".py",
                    ".pyw",
                    ".js",
                    ".jsx",
                    ".mjs",
                    ".ts",
                    ".tsx",
                    ".go",
                    ".rs",
                    ".java",
                    ".rb",
                    ".cs",
                    ".swift",
                    ".lua",
                    ".md",
                    ".markdown",
                    ".mdown",
                }
                tracked = (
                    subprocess.check_output(
                        ["git", "-C", str(root), "ls-files", "-z", "--recurse-submodules"]
                    )
                    .decode()
                    .split("\0")
                )
                expected = {
                    str(root / f)
                    for f in tracked
                    if Path(f).suffix in extensions
                    or Path(f).suffix.lower() in {".f", ".for", ".f77", ".f90", ".f95", ".f03", ".f08"}
                    or Path(f).name == ".Rprofile"
                }
                present = set(call("codebase_overview", project="chitta")["indexed_files"])
                result["tracked_supported"] = len(expected)
                result["tracked_indexed"] = len(present & expected)
                result["missing"] = sorted(expected - present)
                result["coverage"] = len(present & expected) / len(expected)
                assert result["coverage"] >= 0.95, result
                result["wall_seconds"] = round(elapsed, 3)
                result["index_bytes"] = sum(
                    p.stat().st_size for p in (base / "mind").rglob("*") if p.is_file()
                )
                print(json.dumps(result, sort_keys=True))
                assert elapsed <= 60, result
            print("incremental full repair, project aliases, rename and deletion passed")
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=60)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
            log.close()
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    main()
