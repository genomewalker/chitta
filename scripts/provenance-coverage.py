#!/usr/bin/env python3
"""Read-only live CLI audit: identifiable session/source coverage, by writer and age."""

from __future__ import annotations

import argparse
import json
import os
import select
import subprocess
import sys
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path

READS = {"list_memories_brief", "memory_provenance", "query_graph"}
SESSION_PREDICATES = {"source_session", "replication_session", "session", "session_id"}
UNKNOWN = {"", "unknown", "none", "null", "n/a", "unspecified"}


class CLI:
    """One thin-client process, sequential read-only JSON-RPC, no store-file access."""

    def __init__(self, binary: str, socket: str):
        self.process = subprocess.Popen(
            [binary, "--socket-path", socket],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            env=dict(os.environ, CHITTA_CLI_AUTOSTART="0", CC_SOUL_CLI_AUTOSTART="0"),
        )
        self.sequence = 0

    def call(self, name: str, **arguments) -> dict:
        if name not in READS:
            raise ValueError(f"read-only audit rejects {name}")
        self.sequence += 1
        request = {
            "jsonrpc": "2.0",
            "id": self.sequence,
            "method": "tools/call",
            "params": {"name": name, "arguments": arguments},
        }
        self.process.stdin.write(json.dumps(request).encode() + b"\n")
        self.process.stdin.flush()
        if not select.select([self.process.stdout], [], [], 120)[0]:
            raise TimeoutError(f"CLI timeout: {name}")
        response = json.loads(self.process.stdout.readline())
        if response.get("id") != self.sequence or response.get("error"):
            raise ValueError(f"RPC failed: {name}")
        result = response.get("result", {})
        if result.get("isError"):
            raise ValueError(f"tool failed: {name}: {result.get('content')}")
        data = result.get("structured", result.get("structuredContent"))
        if not isinstance(data, dict):
            raise ValueError(f"missing structured response: {name}")
        return data

    def close(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.terminate()  # Only this owned thin-client child, never the daemon.
            self.process.wait(timeout=5)
        self.process.stdout.close()


def identifiable(value) -> bool:
    return isinstance(value, str) and value.strip().lower() not in UNKNOWN


def evidence(triplets: list, memory_id: str, existing_ids: set) -> dict:
    sessions, sources, writers = set(), set(), set()
    for triplet in triplets:
        if str(triplet.get("subject")) != memory_id:
            continue
        if triplet.get("invalidated_at") or triplet.get("expired") or triplet.get("superseded_by"):
            continue
        predicate, target = triplet.get("predicate"), str(triplet.get("object") or "").strip()
        if not identifiable(target):
            continue
        if predicate in SESSION_PREDICATES:
            session = target.removeprefix("session:")
            if identifiable(session):
                sessions.add(session)
        elif predicate == "source":
            # Source labels identify a writer but are NOT invented session IDs.
            if target.startswith("session:"):
                if identifiable(target[8:]):
                    sessions.add(target[8:])
            elif target.isdigit():
                if target in existing_ids:
                    sources.add(target)
            else:
                sources.add(target)
                if "://" not in target and "/" not in target:
                    writers.add(target)
        elif predicate in {"source_file", "source_url", "source_path", "source_uri"}:
            sources.add(target)
        elif predicate == "derived_from":
            writers.add("native_distiller")
            if target.isdigit():
                if target in existing_ids:
                    sources.add(target)
            else:
                session = target.removeprefix("session:")
                if identifiable(session):
                    sessions.add(session)
        elif predicate in {"writer", "source_writer"}:
            writers.add(target)
    return {
        "session": bool(sessions),
        "source": bool(sources),
        "covered": bool(sessions or sources),
        "writer": "+".join(sorted(writers)) if writers else "unknown",
    }


def summarize(rows: list, as_of_ms: int) -> dict:
    def counts(group):
        n = len(group)
        covered = sum(r["covered"] for r in group)
        return {
            "total": n,
            "covered": covered,
            "share": covered / n if n else None,
            "with_session": sum(r["session"] for r in group),
            "with_source": sum(r["source"] for r in group),
        }

    result = {}
    for label, group in (
        ("overall", rows),
        (
            "last_7_days",
            [
                r
                for r in rows
                if isinstance(r["created_at_ms"], int)
                and as_of_ms - 7 * 86400000 <= r["created_at_ms"] <= as_of_ms
            ],
        ),
    ):
        writers = defaultdict(list)
        for row in group:
            writers[row["writer"]].append(row)
        result[label] = {
            **counts(group),
            "by_writer": {w: counts(g) for w, g in sorted(writers.items())},
        }
    result["unknown_creation_time"] = sum(
        not isinstance(r["created_at_ms"], int) or r["created_at_ms"] <= 0 for r in rows
    )
    result["future_creation_time"] = sum(
        isinstance(r["created_at_ms"], int) and r["created_at_ms"] > as_of_ms for r in rows
    )
    return result


def audit(cli: CLI, as_of_ms: int, page_size: int) -> dict:
    # Freeze the observed ID set before per-memory reads; concurrent live writes
    # make this an interval census, not an atomic store snapshot.
    ids, offset = set(), 0
    while True:
        page = cli.call("list_memories_brief", limit=page_size, offset=offset).get("memories")
        if not isinstance(page, list):
            raise ValueError("invalid memory listing")
        incoming = {str(m["id"]) for m in page}
        if page and not incoming - ids:
            raise ValueError("pagination made no progress")
        ids.update(incoming)
        offset += len(page)
        # The store caps pages below the requested limit; only empty is EOF.
        if not page:
            break
    print(f"provenance: enumerated {len(ids)} IDs", file=sys.stderr, flush=True)
    rows = []
    exposed_sessions = 0
    for index, mid in enumerate(sorted(ids, key=int), 1):
        meta = cli.call("memory_provenance", id=int(mid)).get("meta")
        if not isinstance(meta, dict):
            raise ValueError(f"metadata disappeared during audit: {mid}")
        trips = cli.call("query_graph", subject=mid).get("triplets")
        if not isinstance(trips, list):
            raise ValueError("invalid triplet response")
        if str(meta.get("id")) != mid:
            raise ValueError(f"metadata ID mismatch: {mid}")
        ts = meta.get("created_at_ms")
        item = evidence(trips, mid, ids)
        if "source_session" in meta:
            exposed_sessions += 1
            if identifiable(meta["source_session"]):
                item.update(session=True, covered=True)
        rows.append(
            {
                **item,
                "created_at_ms": ts if isinstance(ts, int) and ts > 0 else None,
            }
        )
        if index % 5000 == 0:
            print(f"provenance: {index}/{len(ids)}", file=sys.stderr, flush=True)
    report = summarize(rows, as_of_ms)
    report["memories_exposing_session_field"] = exposed_sessions
    report["session_coverage_complete"] = exposed_sessions == len(ids)
    report["coverage_is_lower_bound"] = exposed_sessions != len(ids)
    return report


def markdown(report: dict) -> str:
    lines = [
        "| Window / writer | Covered / total | Share | Observable session | Source |",
        "|---|---:|---:|---:|---:|",
    ]
    for window in ("overall", "last_7_days"):
        summary = report[window]
        for label, row in [(window, summary), *summary["by_writer"].items()]:
            share = f"{100 * row['share']:.3f}%" if row["share"] is not None else "n/a"
            lines.append(
                f"| {label} | {row['covered']}/{row['total']} | {share} | {row['with_session']} | {row['with_source']} |"
            )
    if report.get("coverage_is_lower_bound"):
        lines += [
            "",
            "Coverage is a lower bound: the public metadata API omits the direct source_session field. Observable-session counts cover triplets only; zero is not evidence of no stored sessions.",
        ]
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", required=True, help="explicit live or replica Unix socket")
    parser.add_argument("--binary", default=os.environ.get("CHITTA_BIN", "chitta"))
    parser.add_argument("--page-size", type=int, default=1000)
    parser.add_argument("--as-of", help="ISO UTC timestamp; default audit start")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    cli = None
    try:
        if not Path(args.socket).is_socket() or args.page_size < 1:
            raise ValueError("existing socket and positive page size required")
        now = datetime.fromisoformat(args.as_of) if args.as_of else datetime.now(timezone.utc)
        if now.tzinfo is None:
            raise ValueError("--as-of must include timezone")
        cli = CLI(args.binary, args.socket)
        report = audit(cli, int(now.timestamp() * 1000), args.page_size)
        report.update(
            schema_version=1,
            as_of=now.isoformat(),
            finished_at=datetime.now(timezone.utc).isoformat(),
            socket=args.socket,
            scope="all listed memories across realms; sequential live interval census",
            definition="identifiable direct session or nonempty source triplet; source writer labels qualify as source, never as session; numeric sources require an existing memory",
            limitations="not atomic; concurrent inserts/deletes can affect pagination; no inference from memory text, no transitive session expansion; public metadata may omit direct source_session, so report coverage completeness explicitly",
        )
        args.output.write_text(json.dumps(report, indent=2) + "\n")
        args.output.with_suffix(".md").write_text(markdown(report))
        print(markdown(report))
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
        parser.exit(2, f"provenance-coverage: {exc}\n")
    finally:
        if cli:
            cli.close()


if __name__ == "__main__":
    main()
