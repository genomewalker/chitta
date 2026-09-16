#!/usr/bin/env python3
"""Recovery assertions on an owned copy. Never points a fault at the source.

Default source is the frozen learning cut; --fixture is CTest's native synthetic
snapshot writer. The test-only preload library stops an armed snapshot write or
returns ENOSPC, without production fault switches or privileged mounts.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import fcntl
import json
import os
import re
import resource
import shlex
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FROZEN = Path("/projects/caeg/scratch/kbd606/tmp/learning-cut-20260915-frozen")


def run(cmd, *, env=None, timeout=30, **kw):
    return subprocess.run(
        cmd, env=env, timeout=timeout, capture_output=True, text=True, check=True, **kw
    )


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def until(predicate, seconds=45):
    start = time.monotonic()
    while time.monotonic() - start < seconds:
        if predicate():
            return time.monotonic() - start
        time.sleep(0.02)
    raise AssertionError(f"condition did not become true within {seconds}s")


class Skip(Exception):
    pass


class Harness:
    def __init__(self, args):
        self.args = args
        self.storage = tempfile.TemporaryDirectory(prefix=".chaos-", dir=args.scratch_root)
        self.runtime = tempfile.TemporaryDirectory(prefix="c7-")
        self.root = Path(self.storage.name)
        self.local = Path(self.runtime.name)
        self.mind = self.local / "m"
        (self.root / "mind").mkdir()
        self.mind.symlink_to(self.root / "mind", target_is_directory=True)
        self.field = self.mind / "chitta-field"
        self.arm = self.local / "full"
        self.pause = self.local / "pause"
        self.env = dict(os.environ)
        for key in list(self.env):
            if key.startswith(("CHITTA_", "CC_SOUL_", "MIND")) or key == "LD_PRELOAD":
                self.env.pop(key)
        self.env.update(
            HOME=str(self.local / "home"),
            XDG_RUNTIME_DIR=str(self.mind / "run"),
            CHITTA_DB_PATH=str(self.mind),
            CHITTA_QUEUE=str(self.mind / "queue.jsonl"),
            CHITTA_BIN=str(args.cli),
            CHITTAD_BIN=str(args.daemon),
            CHITTA_HINT_ENRICHER="/bin/true",
            CHITTA_EVAL_MIND=str(self.mind),
            CHITTA_EVAL_PORT=str(free_port()),
            CHITTA_EVAL_START_TIMEOUT="90",
            CHITTA_STORE_LOCK="1",
            CHITTA_CHAOS_STORE=str(self.field.resolve()),
            CHITTA_CHAOS_ENOSPC_ARM=str(self.arm),
            CHITTA_CHAOS_PAUSE_ARM=str(self.pause),
            LD_PRELOAD=str(args.fault_library),
            PATH=str(Path(sys.executable).parent) + ":" + os.environ["PATH"],
        )
        (self.local / "home/.claude/mind").mkdir(parents=True)
        self.process = None
        self.pid = None
        self.mcp = None
        self.embed_server = None
        self.logs = []
        self.results = []
        self.acknowledged = {}
        self.last_recovery = None
        self.pool = concurrent.futures.ThreadPoolExecutor(max_workers=2)

    def setup(self):
        source = self.args.source.resolve()
        model = self.args.model
        if self.args.fixture:
            source = self.root / "source"
            source.mkdir()
            run([str(self.args.fixture), str(source / "chitta-field")], env=self.env)
            model = self.local / "fixture.gguf"
            model.touch()
            dimension = self.args.dimension

            class Embed(BaseHTTPRequestHandler):
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

            self.embed_server = ThreadingHTTPServer(("127.0.0.1", 0), Embed)
            threading.Thread(target=self.embed_server.serve_forever, daemon=True).start()
            self.env["CHITTA_EMBED_URL"] = f"http://127.0.0.1:{self.embed_server.server_port}"
        assert (source / "chitta-field").is_dir(), source
        assert source != self.mind.resolve()
        self.env.update(
            CHITTA_LIVE_MIND=str(source),
            CHITTA_EVAL_EMBED_MODEL=str(model),
            CHITTA_EMBED_MODEL=str(model),
        )
        # Bootstrap once only. Subsequent starts MUST reopen the faulted copy.
        try:
            run(["bash", str(ROOT / "scripts/eval-replica.sh"), "start"], env=self.env, timeout=120)
        finally:
            metadata = self.mind / "replica.env"
            if metadata.exists():
                values = dict(line.split("=", 1) for line in metadata.read_text().splitlines())
                self.pid = int(values["CHITTA_EVAL_PID"])
                self.env["CHITTA_SOCKET_PATH"] = shlex.split(values["CHITTA_EVAL_SOCKET"])[0]
        self.ready()

    def command(self):
        return [
            str(self.args.daemon),
            "daemon",
            "--foreground",
            "--path",
            str(self.mind),
            "--no-distill",
            "--no-enrich",
            "--no-hygiene",
            "--no-autonomous",
            "--no-embed-interval",
            "--embed-model",
            self.env["CHITTA_EMBED_MODEL"],
            "--rpc-port",
            self.env["CHITTA_EVAL_PORT"],
            "--http-port",
            "0",
        ]

    def log_file(self, name):
        path = self.root / name
        handle = path.open("a")
        self.logs.append(handle)
        return handle

    def start(self):
        assert self.pid is None
        log = self.log_file("restart.log")
        self.process = subprocess.Popen(
            self.command(), env=self.env, stdin=subprocess.DEVNULL, stdout=log, stderr=log
        )
        self.pid = self.process.pid
        self.last_recovery = self.ready()
        return self.last_recovery

    def kill(self):
        if self.pid is not None:
            # Only a pid captured from our own private replica or Popen.
            try:
                os.kill(self.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            if self.process:
                self.process.wait(timeout=10)
                self.process = None
            else:

                def terminated():
                    proc = Path(f"/proc/{self.pid}")
                    try:
                        # An orphaned leader can be a zombie while worker threads
                        # still own store locks. Wait for the whole thread group.
                        return (proc / "stat").read_text().split()[2] == "Z" and len(
                            list((proc / "task").iterdir())
                        ) == 1
                    except (FileNotFoundError, ProcessLookupError):
                        return True

                until(terminated, 10)
            self.pid = None

    def rpc(self, tool, *, timeout=10, **arguments):
        request = {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {"name": tool, "arguments": arguments},
        }
        result = run(
            [str(self.args.cli), "--socket-path", self.env["CHITTA_SOCKET_PATH"]],
            env=self.env,
            input=json.dumps(request) + "\n",
            timeout=timeout,
        )
        response = json.loads(result.stdout)
        assert "error" not in response, response
        payload = response["result"]
        assert not payload.get("isError"), payload
        return payload.get("structured", payload)

    def ready(self):
        def probe():
            if self.process and self.process.poll() is not None:
                raise AssertionError((self.root / "restart.log").read_text()[-1800:])
            try:
                self.rpc("health_check", timeout=1)
                return True
            except (subprocess.SubprocessError, ValueError, AssertionError):
                return False

        return until(probe, 90)

    def remember(self, label):
        text = f"chaos recovery invariant {label} {self.local.name}"
        answer = self.rpc("remember", content=text, realm="chaos", type="wisdom")
        # Contract is an exact ID lookup; approximate recall is not a durability oracle.
        mid = answer.get("id", answer.get("memory_id"))
        assert mid is not None, answer
        self.acknowledged[str(mid)] = text
        return str(mid)

    def verify(self, ids=None):
        for mid in ids or self.acknowledged:
            response = self.rpc("get", id=mid)
            assert self.acknowledged[mid] in json.dumps(response), (mid, response)

    def case(self, name, invariant, function):
        start = time.monotonic()
        self.last_recovery = None
        self.case_details = {}
        try:
            recovery = function()
            row = dict(
                case=name,
                status="PASS",
                invariant=invariant,
                recovery_s=round(recovery, 3) if recovery is not None else None,
            )
        except Skip as exc:
            row = dict(
                case=name, status="SKIP", invariant=invariant, reason=str(exc), recovery_s=None
            )
        except (OSError, ValueError, KeyError, AssertionError, subprocess.SubprocessError) as exc:
            row = dict(
                case=name,
                status="FAIL",
                invariant=invariant,
                reason=str(exc)[-2000:],
                recovery_s=round(self.last_recovery, 3) if self.last_recovery is not None else None,
            )
        if self.case_details:
            row["details"] = self.case_details
        row["elapsed_s"] = round(time.monotonic() - start, 3)
        self.results.append(row)
        print(json.dumps(row), flush=True)

    def snapshot_kill(self):
        for n in range(3):
            self.remember(f"prefix-{n}")
        before = {p.name: p.read_bytes() for p in self.field.glob("MANIFEST.[12]")}
        self.pause.touch()
        saving = self.pool.submit(self.rpc, "compact_wal", timeout=30)
        until(lambda: Path(f"/proc/{self.pid}/stat").read_text().split()[2] == "T", 25)
        assert list(self.field.glob("*.tmp")), "not stopped inside snapshot publication"
        assert before == {p.name: p.read_bytes() for p in self.field.glob("MANIFEST.[12]")}
        self.kill()
        try:
            saving.result(timeout=5)
        except (AssertionError, subprocess.SubprocessError, ValueError):
            pass
        start = time.monotonic()
        self.start()
        self.verify()
        return time.monotonic() - start

    def second_instance(self):
        # CLI lifecycle can reject before reaching the store; fixture opens the
        # same store directly so the recorded Rust holder is actually checked.
        if not self.args.fixture:
            fixture = ROOT / "bin/chaos_fixture_test"
        else:
            fixture = self.args.fixture
        start = time.monotonic()
        # The store waits up to CHITTA_STORE_LOCK_WAIT_S for a live holder
        # (2026-09-16); the refusal itself is what this case asserts.
        result = subprocess.run(
            [str(fixture), str(self.field)],
            env={**self.env, "CHITTA_STORE_LOCK_WAIT_S": "0"},
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            timeout=5,
        )
        assert result.returncode != 0
        assert "recorded holder:" in result.stderr and str(self.pid) in result.stderr, (
            result.stderr[-1000:]
        )
        self.verify()
        return time.monotonic() - start

    def wal_unlink(self):
        self.rpc("compact_wal", timeout=60)  # Previously acknowledged prefix is covered.
        self.remember("before-unlink-covered-next")
        self.rpc("compact_wal", timeout=60)
        self.remember("before-unlink-wal-only")
        # Locate the writer's open segment, not an arbitrary historic segment.
        segments = set()
        for fd in Path(f"/proc/{self.pid}/fd").iterdir():
            try:
                target = Path(os.readlink(fd))
                if target.parent == self.field.resolve() / "segments" and target.exists():
                    segments.add(target)
            except FileNotFoundError:
                pass
        assert segments, "could not locate active WAL descriptor"
        for segment in segments:
            segment.unlink()
        new = [self.remember(f"after-unlink-{n}") for n in range(3)]
        assert list((self.field / "segments").glob("*")), "no recovered WAL segment"
        self.kill()
        start = time.monotonic()
        self.start()
        self.verify(new)
        self.verify()
        recovery = time.monotonic() - start
        logs = "\n".join(
            p.read_text(errors="replace")
            for p in (self.root / "restart.log", self.mind / "replica.log")
            if p.exists()
        )
        copied = [int(n) for n in re.findall(r"recovered WAL segment .*: (\d+) bytes", logs)]
        assert copied and all(n > 0 for n in copied), "missing recovered-byte diagnostic"
        self.case_details["recovered_segment_bytes"] = copied
        return recovery

    def stale_lock(self):
        fs_type = run(["stat", "-f", "-c", "%T", str(self.root)]).stdout.strip()
        if fs_type not in ("nfs", "nfs4"):
            raise Skip(f"NFS required for retained server-lock model; found {fs_type}")
        self.kill()
        path = self.field / ".instance.lock"
        dead = int(Path("/proc/sys/kernel/pid_max").read_text())
        assert not Path(f"/proc/{dead}").exists()
        with path.open("r+") as held:

            def released():
                try:
                    fcntl.flock(held, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    return True
                except BlockingIOError:
                    return False

            until(released, 10)  # NFS may release the killed process's lock asynchronously.
            inode = path.stat().st_ino
            path.write_text(f"{dead} another-host.invalid\n")
            fixture = self.args.fixture or ROOT / "bin/chaos_fixture_test"
            refused = subprocess.run(
                [str(fixture), str(self.field)],
                env=self.env,
                stdin=subprocess.DEVNULL,
                capture_output=True,
                text=True,
                timeout=5,
            )
            assert refused.returncode != 0 and "another-host.invalid" in refused.stderr
            assert path.stat().st_ino == inode
            path.write_text(f"{dead} {socket.gethostname()}\n")
            start = time.monotonic()
            self.start()
            assert path.stat().st_ino != inode
            assert "stale instance lock" in (self.root / "restart.log").read_text()
            self.verify()
            return time.monotonic() - start

    def disk_full(self):
        self.remember("before-enospc")
        before = {p.name: p.read_bytes() for p in self.field.glob("MANIFEST.[12]")}
        self.arm.touch()
        try:
            try:
                self.rpc("compact_wal", timeout=30)
            except AssertionError as exc:
                assert "compaction failed" in str(exc), exc
            else:
                raise AssertionError("save reported success under ENOSPC")
            assert Path(str(self.arm) + ".hit").exists(), "ENOSPC seam was not reached"
            assert before == {p.name: p.read_bytes() for p in self.field.glob("MANIFEST.[12]")}
        finally:
            self.arm.unlink(missing_ok=True)
        self.kill()
        start = time.monotonic()
        self.start()
        self.verify()
        self.rpc("compact_wal", timeout=60)
        return time.monotonic() - start

    def queue_replay(self):
        content = f"queue replay idempotence {self.local.name}"
        item = {
            "tool": "observe",
            "args": {
                "content": content,
                "realm": "chaos",
                "category": "wisdom",
                "source": "mcp_tool",
            },
            "ack_id": self.local.name,
            "ts": int(time.time()),
        }
        queue = Path(self.env["CHITTA_QUEUE"])

        def rows():
            found = self.rpc("recall_keyword", query=content, realm="chaos", no_learn=True)[
                "results"
            ]
            return [row for row in found if row["text"] == content]

        if self.process is None:  # Bootstrap helper intentionally disables queues.
            self.kill()
            self.start()

        def drained():
            # Recovery first moves .processing back to the pending queue. Its
            # disappearance alone does not mean the replay was applied.
            return not queue.exists() and not Path(str(queue) + ".processing").exists()

        queue.write_text((json.dumps(item) + "\n") * 2)
        until(lambda: bool(rows()), 20)
        until(drained, 20)
        original = rows()
        assert len(original) == 1, original
        initial_state = self.rpc("get", id=original[0]["id"])
        ledger = Path(str(queue) + ".applied-acks")
        assert json.loads(ledger.read_text()) == [item["ack_id"]]
        recoveries = []
        self.rpc("compact_wal", timeout=60)
        self.kill()
        # get() reports time-decayed strength, not raw stored strength. Freeze
        # only this fixture's decay, through the existing state-update FFI,
        # before capturing the exact state that replay must preserve.
        fixture = self.args.fixture or ROOT / "bin/chaos_fixture_test"
        run(
            [str(fixture), str(self.field), "--zero-decay", original[0]["id"]],
            env=self.env,
            timeout=90,
        )
        self.start()
        original_state = self.rpc("get", id=original[0]["id"])
        assert original_state["confidence"] == initial_state["confidence"], original_state
        for _ in range(2):
            self.kill()
            # Interrupt after durable apply, before unlink; no checkpoint.
            Path(str(queue) + ".processing").write_text(json.dumps(item) + "\n")
            start = time.monotonic()
            self.start()
            until(drained, 20)
            replayed = rows()
            assert len(replayed) == 1 and replayed[0]["id"] == original[0]["id"], replayed
            replayed_state = self.rpc("get", id=original[0]["id"])
            assert replayed_state == original_state, {
                "before_replay": original_state,
                "after_replay": replayed_state,
            }
            recoveries.append(time.monotonic() - start)
        # A new rotation prunes the previous batch's ledger. An unseen ack
        # must still be applied, and repeated IDs within this batch apply once.
        next_item = dict(item, ack_id=item["ack_id"] + "-next")
        next_item["args"] = dict(item["args"], content=content + " next rotation")
        saves = [
            {
                "tool": "ledger_save",
                "ack_id": item["ack_id"] + "-save-" + version,
                "args": {"session_id": self.local.name, "project": "chaos", "snapshot": version},
            }
            for version in ("superseded", "latest")
        ]
        queue.write_text("".join(json.dumps(row) + "\n" for row in [next_item, next_item, *saves]))
        until(drained, 20)
        assert set(json.loads(ledger.read_text())) == {
            next_item["ack_id"],
            *(row["ack_id"] for row in saves),
        }
        saved = self.rpc(
            "ledger_load", session_id=self.local.name, project="chaos", include_snapshot=True
        )
        assert saved["snapshot"] == "latest", saved
        found = self.rpc("recall_keyword", query=content, realm="chaos", no_learn=True)["results"]
        assert len([r for r in found if r["text"] == next_item["args"]["content"]]) == 1
        self.case_details["snapshot_replay_s"] = [round(t, 3) for t in recoveries]
        self.case_details["ledger_pruned"] = True
        self.case_details["fixture_decay_disabled"] = True
        return recoveries[-1]  # Both repeated snapshot replays preserve exact state.

    def hook_restart(self):
        wrapper = self.local / "hook-cli"
        marker = self.local / "hook-entered"
        success = self.local / "hook-success"
        wrapper.write_text(
            '#!/bin/bash\nrecall=0\ncase " $* " in *recall*) recall=1; touch '
            + shlex.quote(str(marker))
            + ";; esac\n"
            + shlex.quote(str(self.args.cli))
            + ' "$@"\nrc=$?\nif [[ "$recall" == 1 && "$rc" == 0 ]]; then touch '
            + shlex.quote(str(success))
            + '; fi\nexit "$rc"\n'
        )
        wrapper.chmod(0o700)
        env = dict(self.env, CHITTA_BIN=str(wrapper), CHITTA_HOOK_BUDGET_MS="6000")
        query = "How do I recover the chaos daemon durable prefix after a failed snapshot save?"
        payload = json.dumps({"session_id": "chaos-hook-1", "prompt": query, "cwd": str(ROOT)})
        os.kill(self.pid, signal.SIGSTOP)
        begin = time.monotonic()
        hook = subprocess.Popen(
            ["bash", str(ROOT / "hooks/prompt-hook.sh")],
            env=env,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            hook.stdin.write(payload)
            hook.stdin.close()
            hook.stdin = None
            until(marker.exists, 4)
            self.kill()
            out, err = hook.communicate(timeout=max(0.1, 8 - (time.monotonic() - begin)))
            elapsed = time.monotonic() - begin
            assert hook.returncode == 0 and elapsed < 8, (elapsed, err[-500:])
            if out.strip():
                json.loads(out)
            self.start()
            marker.unlink()
            success.unlink(missing_ok=True)
            next_call = run(
                ["bash", str(ROOT / "hooks/prompt-hook.sh")],
                env=env,
                input=payload.replace("chaos-hook-1", "chaos-hook-2"),
                timeout=8,
            )
            assert marker.exists(), "next hook bypassed recall"
            assert success.exists(), "next hook never completed a successful recall CLI call"
            if next_call.stdout.strip():
                json.loads(next_call.stdout)
            self.verify()
            return elapsed
        finally:
            if hook.poll() is None:
                hook.kill()
                hook.wait()

    def mcp_restart(self):
        # Restart an owned HTTP MCP process, never the user's systemd unit/9481.
        try:
            import mcp  # noqa: F401
            import uvicorn  # noqa: F401
        except ImportError as exc:
            raise Skip(f"HTTP MCP dependencies unavailable: {exc}") from exc
        port = free_port()
        env = dict(self.env, CHITTA_MCP_PORT=str(port), MIND=str(self.mind))
        headers = {
            "Content-Type": "application/json",
            "Accept": "application/json, text/event-stream",
        }

        def post(body, session=None, timeout=4):
            request = urllib.request.Request(
                f"http://127.0.0.1:{port}/mcp/",
                data=json.dumps(dict(jsonrpc="2.0", **body)).encode(),
                headers=dict(headers, **({"Mcp-Session-Id": session} if session else {})),
            )
            return urllib.request.urlopen(request, timeout=timeout)

        def initialize():
            with post(
                {
                    "id": 1,
                    "method": "initialize",
                    "params": {
                        "protocolVersion": "2025-03-26",
                        "capabilities": {},
                        "clientInfo": {"name": "chaos", "version": "1"},
                    },
                }
            ) as response:
                session = response.headers["mcp-session-id"]
                assert session
            with post({"method": "notifications/initialized"}, session):
                pass
            return session

        def start_mcp():
            log = self.log_file("mcp.log")
            self.mcp = subprocess.Popen(
                [sys.executable, str(ROOT / "chitta-mcp/server.py"), "--http", "--port", str(port)],
                env=env,
                stdin=subprocess.DEVNULL,
                stdout=log,
                stderr=log,
            )

            def listening():
                assert self.mcp.poll() is None, (self.root / "mcp.log").read_text()[-1200:]
                try:
                    with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                        return True
                except OSError:
                    return False

            until(listening, 15)

        try:
            start_mcp()
            old = initialize()
            self.mcp.kill()
            self.mcp.wait(timeout=5)
            start_mcp()
            start = time.monotonic()
            call = {
                "id": 2,
                "method": "tools/call",
                "params": {"name": "health_check", "arguments": {}},
            }
            try:
                with post(call, old) as response:
                    assert response.status == 200
            except urllib.error.HTTPError as exc:
                assert exc.code == 404, exc
                assert "session" in exc.read().decode().lower()
            elapsed = time.monotonic() - start
            assert elapsed < 5
            with post(call, initialize()) as response:
                assert response.status == 200
                body = response.read().decode()
                assert "result" in body and '"isError":true' not in body, body
            return elapsed
        finally:
            if self.mcp and self.mcp.poll() is None:
                self.mcp.kill()
                self.mcp.wait(timeout=5)
            self.mcp = None

    def format_probe(self):
        env = dict(self.env, OPENBLAS_NUM_THREADS="2")
        if self.args.model.is_file():
            env["CHITTA_EMBED_MODEL"] = str(self.args.model)
        result = run([str(self.args.format_probe), str(self.args.daemon)], env=env, timeout=90)
        timings = [
            float(line.split(": ", 1)[1].split()[0])
            for line in result.stdout.splitlines()
            if line.startswith("format-id under embedding matrix load:")
        ]
        assert len(timings) == 3 and max(timings) < 1, result.stdout
        return max(timings)

    def close(self):
        if self.pid:
            self.kill()
        self.pool.shutdown(wait=True, cancel_futures=True)
        if self.embed_server:
            self.embed_server.shutdown()
            self.embed_server.server_close()
        for log in self.logs:
            log.close()
        if self.args.keep:
            self.storage._finalizer.detach()
            print(f"artifacts: {self.root}", flush=True)
        else:
            self.storage.cleanup()
        self.runtime.cleanup()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=FROZEN)
    parser.add_argument("--scratch-root", type=Path, default=ROOT)
    parser.add_argument("--daemon", type=Path, default=ROOT / "bin/chittad")
    parser.add_argument("--cli", type=Path, default=ROOT / "bin/chitta")
    parser.add_argument(
        "--fault-library", type=Path, default=ROOT / "chitta/build/libsave_fault_test.so"
    )
    parser.add_argument("--format-probe", type=Path, default=ROOT / "bin/subprocess_load_test")
    parser.add_argument("--fixture", type=Path, help="CTest only: native synthetic family writer")
    parser.add_argument("--dimension", type=int, default=768)
    parser.add_argument(
        "--model",
        type=Path,
        default=Path("/maps/projects/caeg/people/kbd606/models/nomic-embed-text-v1.5.gguf"),
    )
    parser.add_argument("--report", type=Path)
    parser.add_argument("--keep", action="store_true")
    parser.add_argument("--cases", default="snapshot,second,wal,lock,disk,queue,hook,mcp,format")
    args = parser.parse_args()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    for path in (args.daemon, args.cli, args.fault_library):
        if not path.is_file():
            parser.error(f"build required test artifact first: {path}")
    harness = Harness(args)
    cases = {
        "snapshot": (
            "acknowledged prefix survives SIGKILL inside snapshot write",
            harness.snapshot_kill,
        ),
        "second": (
            "second store instance refused with recorded holder PID",
            harness.second_instance,
        ),
        "wal": (
            "WAL-only prefix and post-unlink appends survive restart",
            harness.wal_unlink,
        ),
        "lock": (
            "NFS stale local holder replaced and logged; foreign holder refused",
            harness.stale_lock,
        ),
        "disk": (
            "ENOSPC leaves manifest unchanged; prefix recovers and later save succeeds",
            harness.disk_full,
        ),
        "queue": (
            "recovered queue entry preserves memory identity and state",
            harness.queue_replay,
        ),
        "hook": (
            "in-flight hook fails open within 8s; next hook reaches recall",
            harness.hook_restart,
        ),
        "format": (
            "production format probe completes below 1s under embedding/matrix load",
            harness.format_probe,
        ),
        "mcp": (
            "old HTTP session recovers or returns clear 404 within 5s; new session succeeds",
            harness.mcp_restart,
        ),
    }
    try:
        harness.setup()
        for name in args.cases.split(","):
            invariant, function = cases[name]
            harness.case(name, invariant, function)
            if harness.results[-1]["status"] == "FAIL":
                break  # Dependent faults must not turn a dirty state into false passes.
    finally:
        if args.report:
            args.report.write_text(json.dumps(harness.results, indent=2) + "\n")
        harness.close()
    if harness.results and all(row["status"] == "SKIP" for row in harness.results):
        return 77
    return int(any(row["status"] == "FAIL" for row in harness.results))


if __name__ == "__main__":
    sys.exit(main())
