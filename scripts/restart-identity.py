#!/usr/bin/env python3
"""Ordered recall identity on a private, frozen replica (never the live daemon).

The gate compares ordered IDs exactly. Scores and all exposed components are
recorded and diffed, but clock-dependent score drift alone is not an ID failure.
The default report and private store live in a new temporary directory.
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import socket
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FROZEN = Path("/projects/caeg/scratch/kbd606/tmp/learning-cut-20260915-frozen")


def run(cmd, env):
    result = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=900)
    if result.returncode:
        raise RuntimeError(
            f"{cmd[0]} exited {result.returncode}: {result.stderr[-3000:]} {result.stdout[-3000:]}"
        )
    return result.stdout


def numeric_fields(value, prefix=""):
    out = {}
    for key, item in value.items():
        name = f"{prefix}{key}"
        if isinstance(item, dict):
            out.update(numeric_fields(item, name + "."))
        elif isinstance(item, (int, float)) and not isinstance(item, bool):
            out[name] = item
    return out


def compare(before, after):
    rows = []
    for left, right in zip(before, after, strict=True):
        old = {hit["id"]: hit for hit in left["results"]}
        new = {hit["id"]: hit for hit in right["results"]}
        deltas = {}
        for mid in old.keys() & new.keys():
            a, b = numeric_fields(old[mid]), numeric_fields(new[mid])
            changes = {key: b[key] - a[key] for key in a.keys() & b.keys() if b[key] != a[key]}
            if changes:
                deltas[mid] = changes
        rows.append(
            {
                "query": left["query"],
                "identical": list(old) == list(new),
                "before_ids": list(old),
                "after_ids": list(new),
                "score_deltas": deltas,
                "removed": sorted(old.keys() - new.keys()),
                "added": sorted(new.keys() - old.keys()),
            }
        )
    return rows


def read_socket(mind):
    for line in (mind / "replica.env").read_text().splitlines():
        if line.startswith("CHITTA_EVAL_SOCKET="):
            return shlex.split(line.split("=", 1)[1])[0]
    raise RuntimeError("replica.env has no socket")


def collect(args, env, queries):
    rows = []
    for query in queries:
        raw = run(
            [
                str(args.cli),
                "--socket-path",
                read_socket(args.mind),
                "recall",
                "--query",
                query,
                "--limit",
                str(args.limit),
                "--strategy",
                "hybrid",
                "--realm",
                args.realm,
                "--no-learn",
                "--explain",
                "--json",
            ],
            env,
        )
        response = json.loads(raw)
        hits = response.get("results")
        if not isinstance(hits, list) or not hits:
            raise RuntimeError(f"missing/nonempty recall results for {query!r}: {raw[:500]}")
        if len({hit["id"] for hit in hits}) != len(hits):
            raise RuntimeError(f"duplicate IDs for {query!r}")
        if any("explain" not in hit for hit in hits):
            raise RuntimeError("recall --explain did not return score components")
        rows.append({"query": query, **response})
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=FROZEN)
    parser.add_argument("--mind", type=Path, default=os.environ.get("CHITTA_EVAL_MIND"))
    parser.add_argument("--port", type=int, default=os.environ.get("CHITTA_EVAL_PORT"))
    parser.add_argument("--daemon", type=Path, default=ROOT / "bin/chittad")
    parser.add_argument("--cli", type=Path, default=ROOT / "bin/chitta")
    parser.add_argument("--report", type=Path)
    parser.add_argument("--limit", type=int, default=20)
    parser.add_argument("--realm", default="", help="default: unscoped production recall")
    parser.add_argument("--within-process", action="store_true")
    parser.add_argument("--restarts", type=int, default=1)
    args = parser.parse_args()
    if args.restarts < 1 or args.limit < 1:
        parser.error("restarts and limit must be positive")
    args.mind = (
        args.mind or Path(tempfile.mkdtemp(prefix="restart-identity-")) / "mind"
    ).absolute()
    # Refuse all pre-existing stores, including another stream's scratch daemon.
    if args.mind.exists():
        parser.error("--mind must not exist; the gate owns a fresh private copy")
    if (
        args.source.resolve() == args.mind.resolve()
        or args.source.resolve() in args.mind.resolve().parents
    ):
        parser.error("scratch mind must be outside the source")
    if args.port is None:
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            args.port = sock.getsockname()[1]
    args.report = args.report or args.mind.parent / "restart-identity.json"
    queries = json.loads(Path(__file__).with_name("restart-identity-queries.json").read_text())
    if len(queries) != 20 or len(set(queries)) != 20:
        raise RuntimeError("gate requires exactly 20 distinct frozen queries")
    env = dict(os.environ)
    env.update(
        CHITTA_LIVE_MIND=str(args.source.resolve()),
        CHITTA_EVAL_MIND=str(args.mind),
        CHITTA_EVAL_PORT=str(args.port),
        CHITTAD_BIN=str(args.daemon.resolve()),
        CHITTA_BIN=str(args.cli.resolve()),
        OPENBLAS_NUM_THREADS="1",
        OMP_NUM_THREADS="1",
        RAYON_NUM_THREADS="1",
        PATH=str(Path(sys.executable).parent) + ":" + env["PATH"],
    )
    helper = ["bash", str(ROOT / "scripts/eval-replica.sh")]
    report = {
        "source": str(args.source),
        "mind": str(args.mind),
        "port": args.port,
        "within_process": args.within_process,
        "queries": queries,
        "runs": [],
        "comparisons": [],
    }
    failed = False
    try:
        run(helper + ["start"], env)
        report["runs"].append(collect(args, env, queries))
        for index in range(args.restarts):
            if not args.within_process:
                run(helper + ["restart"], env)
            report["runs"].append(collect(args, env, queries))
            diff = compare(report["runs"][-2], report["runs"][-1])
            report["comparisons"].append(diff)
            count = sum(row["identical"] for row in diff)
            print(
                f"{'control' if args.within_process else 'restart'} {index + 1}: {count}/20",
                flush=True,
            )
            for row in diff:
                print(
                    f"{'OK' if row['identical'] else 'DIFF'} {row['query']}: "
                    f"{json.dumps(row if not row['identical'] else {'score_deltas': row['score_deltas']}, sort_keys=True)}",
                    flush=True,
                )
            failed |= count != 20
    except (RuntimeError, ValueError, OSError, subprocess.SubprocessError) as exc:
        report["error"] = str(exc)
        failed = True
        print(str(exc), file=sys.stderr)
    finally:
        try:
            run(helper + ["stop"], env)
        except (RuntimeError, OSError, subprocess.SubprocessError) as exc:
            report["stop_error"] = str(exc)
            failed = True
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
        print(f"report: {args.report}")
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
