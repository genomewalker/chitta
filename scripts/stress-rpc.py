#!/usr/bin/env python3
"""Exercise RPC concurrency on a caller-owned eval-replica COPY, never a default endpoint.

Start with scripts/eval-replica.sh and private CHITTA_EVAL_MIND/CHITTA_EVAL_PORT,
CHITTA_GLOBAL_LOCK=0. Each request has a 30-second total deadline. The identity
panel runs separately, before mutation; replay verification SIGKILLs only the
validated replica PID. Reports and daemon logs are evidence, not repository files.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import contextlib
import importlib.util
import json
import math
import re
import socket
import threading
import time
import uuid
from pathlib import Path

HELPER_PATH = Path(__file__).with_name("stress-embed-recall.py")
spec = importlib.util.spec_from_file_location("replica_helpers", HELPER_PATH)
helpers = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helpers)


RPC_STATS_LOCK = threading.Lock()
RPC_LOCAL = threading.local()
rpc_max_s = 0.0


def rpc(endpoint, tool, arguments, timeout=30):
    global rpc_max_s
    started = time.monotonic()
    try:
        result = request(endpoint, tool, arguments, timeout)
        if time.monotonic() - started > min(timeout, 30):
            raise TimeoutError(f"{tool} exceeded its total deadline")
        return result
    finally:
        with RPC_STATS_LOCK:
            rpc_max_s = max(rpc_max_s, time.monotonic() - started)


def request(endpoint, tool, arguments, timeout=30):
    timeout = min(timeout, 30)
    deadline = time.monotonic() + timeout
    request = {
        "jsonrpc": "2.0",
        "id": 1,
        "method": "tools/call",
        "params": {"name": tool, "arguments": arguments},
    }
    existing = getattr(RPC_LOCAL, "connection", None)
    connection_scope = (
        contextlib.nullcontext(existing) if existing else socket.socket(socket.AF_UNIX)
    )
    with connection_scope as connection:
        connection.settimeout(timeout)
        if existing is None:
            connection.connect(str(endpoint))
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError(f"{tool} exceeded its deadline during connect")
        connection.settimeout(remaining)
        connection.sendall((json.dumps(request) + "\n").encode())
        chunks = bytearray()
        while b"\n" not in chunks:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"{tool} exceeded 30 seconds")
            connection.settimeout(remaining)
            chunk = connection.recv(65536)
            if not chunk:
                raise RuntimeError(f"{tool}: EOF before reply")
            chunks.extend(chunk)
        reply = json.loads(chunks.split(b"\n", 1)[0])
    if "error" in reply or reply.get("result", {}).get("isError"):
        raise RuntimeError(f"{tool}: {json.dumps(reply)[:500]}")
    result = reply["result"]["structured"]
    if result.get("status") == "warming_up":
        raise RuntimeError("daemon is still warming up")
    return result


def helper_rpc(endpoint, tool, arguments, timeout=30):
    start = time.monotonic()
    value = rpc(endpoint, tool, arguments, timeout=timeout)
    return (time.monotonic() - start) * 1000, {"structured": value}


# The shared restart/identity helpers use the same strict per-request deadline.
helpers.rpc = helper_rpc


def memory(endpoint, identity):
    value = rpc(endpoint, "get", {"id": str(identity)})
    # Both payload and state must be present. expand_memory exposes metadata
    # without get's default-filled state fields, which could conceal a torn hit.
    expanded = rpc(endpoint, "expand_memory", {"id": str(identity)})
    metadata = expanded.get("metadata", {})
    if not value.get("content") or not all(
        key in metadata for key in ("confidence", "strength", "status", "ts_ms")
    ):
        raise AssertionError(f"torn payload/state for {identity}: {expanded}")
    if expanded.get("content") != value["content"]:
        raise AssertionError(f"inconsistent payload for {identity}")
    value["metadata"] = {key: item for key, item in metadata.items() if key != "strength"}
    return value


def replay_differences(before, after):
    """Reads enqueue access deltas; compare authored state and monotonic access state.

    get/expand_memory themselves touch the row, and the Rust touch worker can
    persist those deltas while either verification pass is still running.
    Strength is evaluated at wall time. Neither is an authored-state mutation.
    """
    differences = []
    for key in before.keys() | after.keys():
        if key not in ("strength", "metadata") and before.get(key) != after.get(key):
            differences.append(key)
    old, new = before["metadata"], after["metadata"]
    advanced = False
    for key in ("access_count", "last_accessed_ms"):
        if key not in old or key not in new or new[key] < old[key]:
            differences.append("metadata." + key)
        elif new[key] > old[key]:
            advanced = True
    for key in old.keys() | new.keys():
        if key in ("access_count", "last_accessed_ms"):
            continue
        if key == "decay_rate" and advanced and key in old and key in new:
            if not math.isfinite(new[key]) or new[key] < 0:
                differences.append("metadata." + key)
        elif old.get(key) != new.get(key):
            differences.append("metadata." + key)
    return differences


def profiling(text):
    rows = []
    malformed = 0
    for line in text.splitlines():
        if "[lockprof] RUST " not in line:
            continue
        values = dict(
            re.findall(r"(component|held_us|wait_us|max_hold_us|max_wait_us)=([^\s]+)", line)
        )
        if all(key in values for key in ("component", "held_us", "wait_us")):
            rows.append(values)
        else:
            malformed += 1
    return {
        "observations": len(rows),
        "malformed_lines": malformed,
        "components": sorted({row["component"] for row in rows}),
        "max_observed_hold_ms": max((int(row["held_us"]) / 1000 for row in rows), default=None),
        "max_observed_wait_ms": max((int(row["wait_us"]) / 1000 for row in rows), default=None),
        "lifetime_max_hold_ms": max(
            (int(row.get("max_hold_us", row["held_us"])) / 1000 for row in rows), default=None
        ),
        "lifetime_max_wait_ms": max(
            (int(row.get("max_wait_us", row["wait_us"])) / 1000 for row in rows), default=None
        ),
        "holds_over_50ms": sum(int(row["held_us"]) > 50000 for row in rows),
        "basis": "interval observations; lifetime maxima may include startup; all >50ms holds logged",
    }


def run(args, report):
    global rpc_max_s
    mind = args.mind.resolve(strict=True)
    endpoint, pid = helpers.scratch(mind)
    binary = Path(__file__).resolve().parents[1] / "bin/chittad"
    if Path(f"/proc/{pid}/exe").resolve() != binary.resolve():
        raise ValueError("scratch daemon must be this worktree's binary")
    environ = Path(f"/proc/{pid}/environ").read_bytes().split(b"\0")
    if b"CHITTA_GLOBAL_LOCK=0" not in environ:
        raise ValueError("scratch daemon must run with CHITTA_GLOBAL_LOCK=0")
    if b"CHITTA_NO_QUEUE=1" not in environ or not (mind / ".quiesce").is_file():
        raise ValueError("use the quiesced eval-replica copy so only these clients create memories")
    report.update(mind=str(mind), daemon_binary=str(binary), initial_pid=pid)
    helpers.wait_ready(endpoint)
    if not args.skip_identity:
        report["identity"] = helpers.restart_measurement(mind)
        measured = report["identity"]
        before = 20
        if args.identity_baseline:
            baseline_identity = json.loads(args.identity_baseline.read_text())
            if "mind" in baseline_identity and Path(baseline_identity["mind"]).resolve() != mind:
                raise ValueError("baseline names a different replica copy")
            if measured["queries"] != baseline_identity["queries"]:
                raise ValueError("baseline must use the same ordered 20 queries")
            if any(
                baseline_identity.get(key) != "20/20"
                for key in ("fixed_query_response_identity", "fixed_query_ordered_ids_identity")
            ):
                raise ValueError("baseline fixed-query controls must both be 20/20")
            before = int(baseline_identity["ordered_recall_identity"].split("/")[0])
        after = int(measured["ordered_recall_identity"].split("/")[0])
        report["identity_gate"] = {
            "baseline_distinct": f"{before}/20",
            "after_distinct": f"{after}/20",
            "passed": after >= before
            and measured["fixed_query_response_identity"] == "20/20"
            and measured["fixed_query_ordered_ids_identity"] == "20/20",
        }
    endpoint, _ = helpers.scratch(mind)
    log = mind / "replica.log"
    offset = log.stat().st_size
    nonce = "locking-" + uuid.uuid4().hex
    baseline = rpc(endpoint, "health_check", {})["memory_count"]
    ledger_before = rpc(endpoint, "ledger_op", {"op": "counts"})["value"]
    acknowledged = {}
    samples = []
    read_checks = {"hits": 0, "new_realm_hits": 0}
    errors = []
    stop = threading.Event()
    barrier = threading.Barrier(args.writers + args.readers + 1)
    result_lock = threading.Lock()
    deadline = 0.0
    with RPC_STATS_LOCK:
        rpc_max_s = 0.0

    def client(index, writer):
        nonlocal deadline
        count = 0
        try:
            if args.connection_mode == "persistent":
                RPC_LOCAL.connection = socket.socket(socket.AF_UNIX)
                RPC_LOCAL.connection.settimeout(30)
                RPC_LOCAL.connection.connect(str(endpoint))
            barrier.wait(timeout=30)
            while time.monotonic() < deadline and not stop.is_set():
                start = time.monotonic()
                key = f"{nonce}-{index}-{count}"
                iteration_hits = iteration_new_hits = 0
                if writer:
                    if args.handler_class == "ledger":
                        ack = rpc(
                            endpoint,
                            "ledger_op",
                            {"op": "thread_create", "args": {"id": key, "title": key}},
                        )
                        if ack["value"] != key:
                            raise AssertionError("ledger acknowledged wrong identity")
                        identity, expected = key, key
                    else:
                        ack = rpc(
                            endpoint,
                            args.handler_class,
                            {
                                "content": key + " memory recall locking concurrency",
                                "realm": nonce,
                                "type": "episode",
                                "confidence": 1.0,
                                "source_session": nonce,
                            },
                        )
                        identity = str(ack["id"])
                        expected = memory(endpoint, identity)
                    with result_lock:
                        if identity in acknowledged:
                            raise AssertionError("duplicate acknowledged identity")
                        acknowledged[identity] = expected
                else:
                    if args.handler_class == "ledger":
                        rpc(endpoint, "ledger_op", {"op": "thread_list", "args": {"limit": 20}})
                    hits = rpc(
                        endpoint,
                        "recall",
                        {
                            "query": "memory recall locking concurrency",
                            "limit": 5,
                            "no_learn": True,
                            "realm": nonce
                            if args.handler_class != "ledger" and index % 2 == 0
                            else "",
                        },
                    )["results"]
                    for hit in hits:
                        if not hit.get("text"):
                            raise AssertionError(f"recall hit lacks payload: {hit}")
                        checked = memory(endpoint, hit["id"])
                        iteration_hits += 1
                        iteration_new_hits += checked.get("realm") == nonce
                with result_lock:
                    samples.append((writer, time.monotonic() - start))
                    read_checks["hits"] += iteration_hits
                    read_checks["new_realm_hits"] += iteration_new_hits
                count += 1
        except (OSError, ValueError, RuntimeError, AssertionError, KeyError, TypeError) as exc:
            with result_lock:
                errors.append(f"{'writer' if writer else 'reader'} {index}: {exc}")
            stop.set()
        finally:
            connection = getattr(RPC_LOCAL, "connection", None)
            if connection is not None:
                connection.close()
                del RPC_LOCAL.connection

    started = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.writers + args.readers) as pool:
        futures = [pool.submit(client, i, True) for i in range(args.writers)]
        futures += [pool.submit(client, i, False) for i in range(args.readers)]
        deadline = time.monotonic() + args.seconds
        barrier.wait(timeout=30)
        for future in futures:
            future.result(timeout=args.seconds + 180)
    report.update(
        elapsed_s=time.monotonic() - started,
        errors=errors,
        acknowledged=len(acknowledged),
        writer_iterations=sum(writer for writer, _ in samples),
        reader_iterations=sum(not writer for writer, _ in samples),
        max_iteration_ms=max((elapsed * 1000 for _, elapsed in samples), default=0),
        max_call_ms=rpc_max_s * 1000,
        read_checks=read_checks,
    )
    with log.open("rb") as stream:
        stream.seek(offset)
        report["rust_locks"] = profiling(stream.read().decode(errors="replace"))
    if report["max_call_ms"] > 30000:
        raise TimeoutError("a call exceeded the 30-second deadlock deadline")
    if errors:
        raise AssertionError("concurrent clients failed")
    if not acknowledged or not report["reader_iterations"]:
        raise AssertionError("stress did not exercise both writers and readers")
    if args.handler_class != "ledger" and not read_checks["new_realm_hits"]:
        raise AssertionError("recall never exercised concurrently published memories")
    after_count = rpc(endpoint, "health_check", {})["memory_count"]
    expected_count = baseline + (0 if args.handler_class == "ledger" else len(acknowledged))
    report["memory_count"] = {"before": baseline, "expected": expected_count, "after": after_count}
    if after_count != expected_count:
        raise AssertionError("memory count inconsistent with acknowledged writes")
    if args.handler_class == "ledger":
        counts = rpc(endpoint, "ledger_op", {"op": "counts"})["value"]
        if counts["threads"] != ledger_before["threads"] + len(acknowledged):
            raise AssertionError("ledger thread count inconsistent")
    else:
        # Reads can legitimately update access metadata during the workload.
        # Compare replay with the state after clients have drained, while also
        # requiring every acknowledged payload to remain intact.
        for identity, expected in acknowledged.items():
            before_replay = memory(endpoint, identity)
            if before_replay["content"] != expected["content"]:
                raise AssertionError(f"acknowledged payload changed for {identity}")
            acknowledged[identity] = before_replay
    # Kill without snapshotting, so acknowledged records must replay from WAL.
    helpers.restart_copy(mind, kill_after_ack=True)
    endpoint, _ = helpers.scratch(mind)
    replayed_count = rpc(endpoint, "health_check", {})["memory_count"]
    if replayed_count != expected_count:
        raise AssertionError("WAL replay changed memory count")
    for identity, expected in acknowledged.items():
        if args.handler_class == "ledger":
            restored = rpc(
                endpoint, "ledger_op", {"op": "thread_get", "args": {"thread_id": identity}}
            )["value"]
            if restored["title"] != expected:
                raise AssertionError(f"WAL replay lost ledger {identity}")
        else:
            restored = memory(endpoint, identity)
            differences = replay_differences(expected, restored)
            if differences:
                report["wal_replay_mismatch"] = {
                    "id": identity,
                    "fields": differences,
                    "before": {k: v for k, v in expected.items() if k != "strength"},
                    "after": {k: v for k, v in restored.items() if k != "strength"},
                }
                raise AssertionError(f"WAL replay changed payload/state {identity}")
    report["wal_replay"] = {
        "verified": len(acknowledged),
        "state_basis": "after all stress clients drained",
        "excluded": ["wall_time_decayed_strength"],
        "access_state": "counts/timestamps cannot regress; decay rate may change with an access advance",
    }
    locks = report["rust_locks"]
    report["invariants_passed"] = True
    report["exit_gate_met"] = bool(
        args.seconds >= 300
        and args.writers == args.readers == 12
        and locks["observations"]
        and not locks["malformed_lines"]
        and locks["holds_over_50ms"] == 0
        and report.get("identity_gate", {}).get("passed", False)
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mind", type=Path, required=True)
    parser.add_argument("--seconds", type=float, default=300)
    parser.add_argument("--writers", type=int, default=12)
    parser.add_argument("--readers", type=int, default=12)
    parser.add_argument(
        "--connection-mode",
        choices=("persistent", "per-call"),
        default="persistent",
        help="one socket per client, or additional connection-churn diagnostics",
    )
    parser.add_argument(
        "--identity-baseline",
        type=Path,
        help="before-change restart measurement from this same copy; gate is no worse",
    )
    parser.add_argument(
        "--handler-class", choices=("ledger", "observe", "remember"), default="remember"
    )
    parser.add_argument(
        "--skip-identity", action="store_true", help="diagnostic only; cannot satisfy exit gate"
    )
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    if not math.isfinite(args.seconds) or args.seconds <= 0 or min(args.writers, args.readers) <= 0:
        parser.error("duration and client counts must be positive")
    report = {
        "handler_class": args.handler_class,
        "writers": args.writers,
        "readers": args.readers,
        "requested_seconds": args.seconds,
        "connection_mode": args.connection_mode,
        "exit_gate_met": False,
    }
    try:
        run(args, report)
    except (OSError, ValueError, RuntimeError, AssertionError, KeyError, TypeError) as exc:
        report["failure"] = str(exc)
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0 if report["exit_gate_met"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
