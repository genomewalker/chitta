#!/usr/bin/env python3
"""Measure 200 concurrent real writes and recall on a private eval-replica copy.

The caller starts the copy using scripts/eval-replica.sh with private
CHITTA_EVAL_MIND/CHITTA_EVAL_PORT and worktree CHITTAD_BIN/CHITTA_BIN.
No default socket or live-daemon fallback is permitted.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import datetime as dt
import hashlib
import json
import math
import os
import shlex
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from pathlib import Path


def rpc(sock, tool, arguments, timeout=120):
    started = time.monotonic()
    with socket.socket(socket.AF_UNIX) as conn:
        conn.settimeout(timeout)
        conn.connect(str(sock))
        conn.sendall(
            (
                json.dumps(
                    {
                        "jsonrpc": "2.0",
                        "id": 1,
                        "method": "tools/call",
                        "params": {"name": tool, "arguments": arguments},
                    }
                )
                + "\n"
            ).encode()
        )
        with conn.makefile("rb") as stream:
            reply = json.loads(stream.readline())
    if (
        "error" in reply
        or "result" not in reply
        or reply.get("result", {}).get("isError")
        or reply.get("result", {}).get("structured", {}).get("status") == "warming_up"
    ):
        raise RuntimeError(json.dumps(reply)[:500])
    return (time.monotonic() - started) * 1000, reply.get("result", {})


class WriteConnection:
    """Multiplex 200 concurrent RPCs through one of the daemon's 32 sockets."""

    def __init__(self, sock):
        self.conn = socket.socket(socket.AF_UNIX)
        self.conn.settimeout(180)
        self.conn.connect(str(sock))
        self.lock = threading.Lock()
        self.pending = {}
        self.reader = threading.Thread(target=self.receive, daemon=True)
        self.reader.start()

    def receive(self):
        try:
            with self.conn.makefile("rb") as stream:
                for line in stream:
                    reply = json.loads(line)
                    with self.lock:
                        item = self.pending.pop(reply["id"], None)
                    if item is None:
                        continue
                    started, future = item
                    if (
                        "error" in reply
                        or "result" not in reply
                        or reply.get("result", {}).get("isError")
                        or reply.get("result", {}).get("structured", {}).get("status")
                        == "warming_up"
                    ):
                        future.set_exception(RuntimeError(json.dumps(reply)[:500]))
                    else:
                        future.set_result(
                            ((time.monotonic() - started) * 1000, reply.get("result", {}))
                        )
        except (OSError, ValueError) as exc:
            with self.lock:
                for _, future in self.pending.values():
                    future.set_exception(exc)
                self.pending.clear()
        finally:
            with self.lock:
                for _, future in self.pending.values():
                    future.set_exception(
                        RuntimeError("write connection closed before acknowledgement")
                    )
                self.pending.clear()

    def remember(self, identity, args):
        future = concurrent.futures.Future()
        with self.lock:
            self.pending[identity] = (time.monotonic(), future)
            self.conn.sendall(
                (
                    json.dumps(
                        {
                            "jsonrpc": "2.0",
                            "id": identity,
                            "method": "tools/call",
                            "params": {"name": "remember", "arguments": args},
                        }
                    )
                    + "\n"
                ).encode()
            )
        return future.result(timeout=180)

    def close(self):
        try:
            self.conn.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass  # An already-closed daemon connection is reported by the reader.
        self.conn.close()
        self.reader.join(timeout=5)


def scratch(mind):
    mind = mind.resolve(strict=True)
    metadata = {}
    for line in (mind / "replica.env").read_text().splitlines():
        key, value = line.split("=", 1)
        metadata[key] = shlex.split(value)[0]
    if Path(metadata["CHITTA_EVAL_MIND"]).resolve() != mind:
        raise ValueError("replica metadata does not match --mind")
    pid = int((mind / "replica.pid").read_text())
    argv = Path(f"/proc/{pid}/cmdline").read_bytes().decode().split("\0")
    if (
        "daemon" not in argv
        or "--path" not in argv
        or Path(argv[argv.index("--path") + 1]).resolve() != mind
    ):
        raise ValueError("pid is not this scratch daemon")
    if mind == (Path.home() / ".claude/mind").resolve() or mind.name == "chitta-eval-mind":
        raise ValueError("use a private COPY, not the live or frozen evaluation mind")
    sock = Path(metadata["CHITTA_EVAL_SOCKET"])
    if not sock.resolve().is_relative_to((mind / "run").resolve()):
        raise ValueError("socket must belong to this scratch runtime")
    # Runtime may be a node-local symlink beneath an NFS mind. Verify the peer
    # PID as well as paths before sending any request that could mutate state.
    with socket.socket(socket.AF_UNIX) as probe:
        probe.connect(str(sock))
        peer = probe.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
        if int.from_bytes(peer[:4], byteorder=sys.byteorder) != pid:
            raise ValueError("socket peer is not the checked scratch daemon")
    return sock, pid


def restart_copy(mind, kill_after_ack=False):
    """Restart the existing scratch store; never recopy the pristine family."""
    sock, pid = scratch(mind)
    argv = Path(f"/proc/{pid}/cmdline").read_bytes().decode().rstrip("\0").split("\0")
    inherited = {}
    for item in Path(f"/proc/{pid}/environ").read_bytes().split(b"\0"):
        if b"=" in item:
            key, value = item.decode().split("=", 1)
            if key.startswith("CHITTA_") or key in ("XDG_RUNTIME_DIR", "LD_LIBRARY_PATH"):
                inherited[key] = value
    env = os.environ.copy()
    env.update(inherited)
    for knob in (
        "CHITTA_RUNTIME_LOCAL",
        "CHITTA_EMBED_WRITE_WORKERS",
        "CHITTA_EMBED_WRITE_DEPTH",
        "CHITTA_EMBED_WRITE_WAIT_MS",
    ):
        if knob in os.environ:
            env[knob] = os.environ[knob]
    os.kill(pid, signal.SIGKILL if kill_after_ack else signal.SIGTERM)
    deadline = time.monotonic() + 25
    while Path(f"/proc/{pid}").exists():
        try:
            status = Path(f"/proc/{pid}/stat").read_text().split()[2]
            if status == "Z" and len(list(Path(f"/proc/{pid}/task").iterdir())) <= 1:
                break
        except FileNotFoundError:
            break
        if time.monotonic() > deadline:
            raise TimeoutError("scratch daemon did not stop")
        time.sleep(0.02)
    # The old replica.pid is overwritten only after its checked process exits.
    with (mind / "replica.log").open("ab") as log:
        started = time.monotonic()
        proc = subprocess.Popen(
            argv,
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
    (mind / "replica.pid").write_text(str(proc.pid) + "\n")
    metadata = mind / "replica.env"
    text = metadata.read_text()
    text = (
        "\n".join(
            f"CHITTA_EVAL_PID={proc.pid}" if line.startswith("CHITTA_EVAL_PID=") else line
            for line in text.splitlines()
        )
        + "\n"
    )
    metadata.write_text(text)
    deadline = time.monotonic() + 600
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"scratch restart exited {proc.returncode}")
        try:
            rpc(
                sock, "recall", {"query": "memory startup", "limit": 1, "no_learn": True}, timeout=1
            )
            return (time.monotonic() - started) * 1000
        except (OSError, ValueError, RuntimeError):
            time.sleep(0.05)
    raise TimeoutError("scratch restart did not become ready")


def ordered_ids(value):
    if isinstance(value, dict):
        result = [str(v) for k, v in value.items() if k in ("id", "memory_id")]
        return result + [
            i for k, v in value.items() if k not in ("id", "memory_id") for i in ordered_ids(v)
        ]
    if isinstance(value, list):
        return [i for v in value for i in ordered_ids(v)]
    return []


def wait_ready(sock):
    deadline = time.monotonic() + 600
    while time.monotonic() < deadline:
        try:
            rpc(
                sock, "recall", {"query": "memory startup", "limit": 1, "no_learn": True}, timeout=1
            )
            return
        except (OSError, ValueError, RuntimeError):
            time.sleep(0.05)
    raise TimeoutError("scratch daemon is still warming up")


def restart_measurement(mind, durability=False):
    sock, _ = scratch(mind)
    wait_ready(sock)
    queries = [
        "memory recall performance",
        "durable queue recovery",
        "embedding context pool",
        "session handoff provenance",
        "snapshot WAL restart",
        "task ledger state",
        "hook admission policy",
        "semantic search ranking",
        "model embedding identity",
        "outcome learning credit",
        "code intelligence symbols",
        "belief correction",
        "NFS instance lock",
        "runtime placement",
        "bounded worker backpressure",
        "read cache invalidation",
        "distillation transcript",
        "MCP tool schema",
        "golden recall benchmark",
        "daemon startup latency",
    ]

    def panel():
        return [
            ordered_ids(rpc(sock, "recall", {"query": q, "limit": 5, "no_learn": True})[1])
            for q in queries
        ]

    fixed_params = {
        "query": "chitta recall performance lock contention",
        "realm": "project:cc-soul",
        "limit": 5,
        "no_learn": True,
    }

    def fixed_panel():
        return [rpc(sock, "recall", fixed_params)[1] for _ in range(20)]

    def warmed_panel(fetch=panel):
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            _, first = rpc(sock, "health_check", {})
            misses = first.get("structured", {}).get("embed_cache", {}).get("misses")
            results = fetch()
            _, last = rpc(sock, "health_check", {})
            after_misses = last.get("structured", {}).get("embed_cache", {}).get("misses")
            if misses is not None and misses == after_misses:
                return results
            time.sleep(0.1)
        raise TimeoutError("20-query identity panel could not warm its embedding cache")

    # Compare cached semantic results, never a timed-out BM25 fallback against
    # a completed embedding. Cache-miss counters verify every query was warm.
    before = warmed_panel()
    fixed_before = warmed_panel(fixed_panel)
    token = "phase6-durable-" + uuid.uuid4().hex
    timer_observed = None
    if durability:
        fixture = mind / "phase6-timer-fixture"
        fixture.mkdir(exist_ok=True)
        symbol = "phase6_timer_" + uuid.uuid4().hex
        (fixture / "timer.py").write_text(f"def {symbol}():\n    return 6\n")
        rpc(sock, "learn_codebase", {"path": str(fixture), "project": symbol, "max_files": 1})
        restart_copy(mind, kill_after_ack=True)
        _, found = rpc(sock, "find_symbol", {"name": symbol})
        # A SIGKILL may retain timer-synced/page-cached writes. Record only;
        # asserting their loss would be an invalid durability test.
        timer_observed = symbol in json.dumps(found)
    if durability:
        # remember is a dispatcher-synced write. Kill immediately upon its ack.
        _, ack = rpc(
            sock, "remember", {"content": token, "type": "episode", "realm": "phase6-durability"}
        )
        acknowledged = ordered_ids(ack)
        if not acknowledged:
            raise AssertionError("durable write returned no memory ID")
    elapsed = restart_copy(mind, kill_after_ack=durability)
    after = warmed_panel()
    fixed_after = warmed_panel(fixed_panel)
    matches = sum(a == b and bool(a) for a, b in zip(before, after))
    if durability:
        _, restored = rpc(sock, "get", {"id": acknowledged[0]})
        if token not in json.dumps(restored):
            raise AssertionError("acknowledged durable memory did not survive SIGKILL")
    return {
        "restart_ms": elapsed,
        "cached_start_target_met": elapsed <= 5000,
        "ordered_recall_identity": f"{matches}/20",
        "fixed_query_params": fixed_params,
        "fixed_query_response_identity": f"{sum(a == b and bool(ordered_ids(a)) for a, b in zip(fixed_before, fixed_after))}/20",
        "fixed_query_ordered_ids_identity": f"{sum(ordered_ids(a) == ordered_ids(b) and bool(ordered_ids(a)) for a, b in zip(fixed_before, fixed_after))}/20",
        "queries": queries,
        "before_ids": before,
        "after_ids": after,
        "durable_sigkill_survived": True if durability else None,
        "timer_synced_symbol_observed": timer_observed,
    }


def summary(samples):
    return {
        "n": len(samples),
        "p50_ms": statistics.median(samples) if samples else None,
        "p95_ms": sorted(samples)[math.ceil(0.95 * len(samples)) - 1] if samples else None,
    }


def self_test():
    """Compile fixtures against the real headers; all data lives under /tmp."""
    root = Path(__file__).resolve().parents[1]
    compiler = os.environ.get(
        "CXX",
        "/maps/projects/fernandezguerra/apps/opt/conda/envs/bioinfo/bin/x86_64-conda-linux-gnu-g++",
    )
    source = r"""
#include <chitta/queue_path.hpp>
#include <chitta/embed_queue.hpp>
#include <cassert>
#include <iostream>
struct Mock : chitta::VakYantra {
    std::promise<void> started, release;
    std::shared_future<void> gate = release.get_future().share();
    chitta::Artha transform(const std::string& text) override {
        if (text == "hold") { started.set_value(); gate.wait(); }
        chitta::Artha a; a.nu.data = {1.0f}; return a;
    }
    bool ready() const override { return true; }
    size_t dimension() const override { return 1; }
};
int main(int argc, char** argv) {
    if (argc > 2) {
        if (std::string(argv[1]) == "path") std::cout << chitta::queue_path_for_mind(argv[2]);
        else { chitta::RuntimeLedger ledger(argv[2]); ledger.flush(true); }
        return 0;
    }
    setenv("CHITTA_EMBED_WRITE_WORKERS", "1", 1);
    setenv("CHITTA_EMBED_WRITE_DEPTH", "1", 1);
    setenv("CHITTA_EMBED_WRITE_WAIT_MS", "30", 1);
    auto mock = std::make_shared<Mock>();
    chitta::EmbedQueue queue(mock);
    auto first = std::async(std::launch::async, [&] { return queue.write("hold"); });
    mock->started.get_future().wait();
    auto second = std::async(std::launch::async, [&] { return queue.write("queued"); });
    for (int i = 0; i < 100 && queue.inflight_count() < 2; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    bool timed_out = false;
    try { queue.write("overflow"); } catch (const std::runtime_error&) { timed_out = true; }
    assert(timed_out);
    assert(!queue.query("reader", std::chrono::milliseconds(100)).empty());
    assert(!queue.query("queued", std::chrono::milliseconds(100)).empty());
    assert(second.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready);
    mock->release.set_value();
    assert(!first.get().empty() && !second.get().empty());
    setenv("CHITTA_EMBED_WRITE_WORKERS", "0", 1);
    chitta::EmbedQueue legacy(std::make_shared<Mock>());
    assert(!legacy.bounded_writes());
    assert(!legacy.query("legacy").empty());
}
"""
    with tempfile.TemporaryDirectory(prefix="p6-fixture-") as directory:
        base = Path(directory)
        cpp, binary = base / "fixture.cpp", base / "fixture"
        cpp.write_text(source)
        subprocess.run(
            [
                compiler,
                "-std=c++20",
                "-pthread",
                "-O0",
                "-I",
                str(root / "chitta/include"),
                str(cpp),
                "-o",
                str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)], check=True, timeout=30)
        mind = base / "mind"
        mind.mkdir()
        env = os.environ.copy()
        env.update(CHITTA_DB_PATH=str(mind), XDG_RUNTIME_DIR=str(base / "runtime"))
        for key in ("CHITTA_QUEUE", "CHITTA_QUEUE_PATH", "MIND_PATH"):
            env.pop(key, None)
        for placement in ("0", "1"):
            env["CHITTA_RUNTIME_LOCAL"] = placement
            cpp_path = subprocess.check_output([str(binary), "path", str(mind)], env=env, text=True)
            hook_path = subprocess.check_output(
                ["bash", "-c", 'source "$1"; get_queue_file', "_", str(root / "hooks/lib.sh")],
                env=env,
                text=True,
            ).strip()
            assert cpp_path == hook_path, (cpp_path, hook_path)
            subprocess.run(
                [
                    "bash",
                    "-c",
                    'source "$1"; ledger_append \'{"event":"fixture"}\' test',
                    "_",
                    str(root / "hooks/outcome-ledger.sh"),
                ],
                env=env,
                check=True,
            )
            if placement == "0":
                assert len((mind / "outcome_ledger.jsonl").read_text().splitlines()) == 1
            else:
                tail = Path(hook_path).parent / "outcome_ledger.tail"
                assert tail.exists()
                subprocess.run([str(binary), "flush", str(mind)], env=env, check=True)
                durable = mind / "outcome_ledger.jsonl"
                assert len(durable.read_text().splitlines()) == 2
                # Simulate SIGKILL after durable append but before publishing offset.
                offset = len(tail.read_bytes())
                destination = len(durable.read_bytes())
                record = b'{"event":"crash-window"}\n'
                with tail.open("ab") as out:
                    out.write(record)
                Path(str(tail) + ".intent").write_text(f"{offset} {len(record)} {destination}\n")
                with durable.open("ab") as out:
                    out.write(record)
                subprocess.run([str(binary), "flush", str(mind)], env=env, check=True)
                subprocess.run([str(binary), "flush", str(mind)], env=env, check=True)
                assert durable.read_bytes().count(record) == 1
                assert int(Path(str(tail) + ".offset").read_text()) == offset + len(record)
                assert not Path(str(tail) + ".intent").exists()
                large = json.dumps({"event": "large", "data": "x" * (1024 * 1024)}) + "\n"
                with tail.open("a") as out:
                    out.write(large)
                subprocess.run([str(binary), "flush", str(mind)], env=env, check=True)
                assert durable.read_text().endswith(large)
        env["CHITTA_QUEUE"] = str(base / "explicit.jsonl")
        assert (
            subprocess.check_output([str(binary), "path", str(mind)], env=env, text=True)
            == env["CHITTA_QUEUE"]
        )
    print("placement, ledger crash replay, bounded admission and reader isolation: passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--restart-only", action="store_true")
    parser.add_argument("--durability-test", action="store_true")
    parser.add_argument("--mind", type=Path)
    parser.add_argument("--label")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--writers", type=int, default=200)
    parser.add_argument("--samples", type=int, default=40)
    parser.add_argument(
        "--settle-seconds",
        type=float,
        default=30,
        help="keep sampling after write acknowledgements for asynchronous embeddings",
    )
    parser.add_argument(
        "--drain-timeout",
        type=float,
        default=300,
        help="maximum sampling duration while waiting for pending embeddings",
    )
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    if not args.mind or not args.label or not args.output:
        parser.error("--mind, --label and --output are required")
    if (
        not 1 <= args.writers <= 1000
        or args.samples < 1
        or args.settle_seconds < 0
        or args.drain_timeout <= args.settle_seconds
    ):
        parser.error("invalid writer/sample/settle limits")
    if args.restart_only or args.durability_test:
        result = restart_measurement(args.mind, args.durability_test)
        result.update(label=args.label, date=dt.datetime.now(dt.timezone.utc).isoformat())
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result, indent=2))
        return
    sock, pid = scratch(args.mind)
    wait_ready(sock)
    binary_sha256 = hashlib.sha256(Path(f"/proc/{pid}/exe").read_bytes()).hexdigest()
    _, initial_health = rpc(sock, "health_check", {})
    if initial_health.get("structured", {}).get("pending_count") != 0:
        raise RuntimeError("scratch has an existing embedding backlog; drain it before measuring")
    queries = [
        "memory recall performance",
        "durable queue recovery",
        "embedding context pool",
        "session handoff provenance",
        "snapshot WAL restart",
    ]
    baseline, loaded, writes, errors = [], [], [], []
    for i in range(args.samples):
        elapsed, _ = rpc(
            sock, "recall", {"query": queries[i % len(queries)], "limit": 5, "no_learn": True}
        )
        baseline.append(elapsed)
    barrier = threading.Barrier(args.writers + 1)
    nonce = uuid.uuid4().hex
    connection = WriteConnection(sock)
    writer_loaded = []

    def remember(i):
        barrier.wait(timeout=60)
        return connection.remember(
            i + 1000,
            {
                "content": f"Phase 6 stress {nonce} item {i}: {queries[i % len(queries)]}. "
                "A bounded embedding worker preserves reader responsiveness while writing a durable memory.",
                "type": "episode",
                "realm": f"stress:{nonce}",
            },
        )

    started = time.monotonic()
    pending_after = None
    drained_at = None
    last_pending_check = 0.0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.writers) as pool:
        futures = [pool.submit(remember, i) for i in range(args.writers)]
        barrier.wait(timeout=60)
        settled_at = None
        i = 0
        while time.monotonic() - started < args.drain_timeout:
            if settled_at is None and all(f.done() for f in futures):
                settled_at = time.monotonic() + args.settle_seconds
            if settled_at is not None and time.monotonic() - last_pending_check >= 1:
                last_pending_check = time.monotonic()
                try:
                    _, health = rpc(sock, "health_check", {})
                    pending_after = health.get("structured", {}).get("pending_count")
                    if pending_after == 0 and drained_at is None:
                        drained_at = time.monotonic() - started
                except (OSError, ValueError, RuntimeError) as exc:
                    errors.append(f"drain health: {exc}")
            if settled_at is not None and time.monotonic() >= settled_at and pending_after == 0:
                break
            try:
                elapsed, _ = rpc(
                    sock,
                    "recall",
                    {"query": queries[i % len(queries)], "limit": 5, "no_learn": True},
                )
                loaded.append(elapsed)
                if settled_at is None:
                    writer_loaded.append(elapsed)
            except (OSError, ValueError, RuntimeError) as exc:
                errors.append(f"recall: {exc}")
            i += 1
            time.sleep(0.02)
        for future in futures:
            try:
                elapsed, _ = future.result()
                writes.append(elapsed)
            except (OSError, ValueError, RuntimeError) as exc:
                errors.append(f"remember: {exc}")
    connection.close()
    try:
        _, health = rpc(sock, "health_check", {})
        pending_after = health.get("structured", {}).get("pending_count")
    except (OSError, ValueError, RuntimeError) as exc:
        errors.append(f"final health: {exc}")
        pending_after = None
    # Actual embedding evidence must be reported, not inferred from write acks.
    log = (args.mind / "replica.log").read_text(errors="replace")
    embedding_evidence = [line for line in log.splitlines() if "[backfill] embedded=" in line]
    result = {
        "date": dt.datetime.now(dt.timezone.utc).isoformat(),
        "label": args.label,
        "mind": str(args.mind.resolve()),
        "pid": pid,
        "daemon_binary_sha256": binary_sha256,
        "writers": args.writers,
        "elapsed_s": time.monotonic() - started,
        "embedding_drained_s": drained_at,
        "minimum_settle_s": args.settle_seconds,
        "drain_timeout_s": args.drain_timeout,
        "loadavg": os.getloadavg(),
        "baseline_recall": summary(baseline),
        "loaded_recall": summary(loaded),
        "during_writes_recall": summary(writer_loaded),
        "pending_embeddings_after": pending_after,
        "remember": summary(writes),
        "errors": errors,
        "embedding_log_evidence": embedding_evidence[-20:],
        "backend_log_evidence": [
            line
            for line in log.splitlines()
            if "[llama-embed] contexts=" in line or "[embed]" in line
        ][-8:],
        "target_met": bool(
            loaded
            and not errors
            and len(writes) == args.writers
            and writer_loaded
            and pending_after == 0
            and summary(writer_loaded)["p95_ms"] <= 150
            and summary(loaded)["p95_ms"] <= 150
        ),
        "samples": {"baseline_ms": baseline, "loaded_ms": loaded, "write_ms": writes},
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(
        json.dumps(
            {
                k: v
                for k, v in result.items()
                if k not in ("samples", "embedding_log_evidence", "errors")
            },
            indent=2,
        )
    )
    if errors:
        print(f"{len(errors)} errors; see {args.output}")
        raise SystemExit(1)


if __name__ == "__main__":
    main()
