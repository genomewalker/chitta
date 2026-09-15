#!/usr/bin/env python3
"""Prospective task/cohort preparation; imports never freeze or query anything."""

from __future__ import annotations

import argparse
import json
import re
import sys
import tempfile
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from copy import deepcopy
from datetime import datetime, timezone
from pathlib import Path

from audit import TOOLS
from common import (
    REALM,
    ROOT,
    RPC,
    command,
    digest,
    family,
    git,
    initial_hashes,
    live_paths,
    now_ms,
    read_json,
    require,
    seal,
    task_worktree,
    tree_identity,
    validate_runtime_roots,
    write_json,
)

NATIVE_KINDS = {"wisdom", "belief", "preference", "milestone"}


def classify(memory, triplets, parents):
    """Kind only disambiguates an output AFTER writer provenance is established."""
    mid = str(memory["id"])
    trips = [t for t in triplets if str(t.get("subject")) == mid]
    sources = {t["object"] for t in trips if t.get("predicate") == "source"}
    derived = {str(t["object"]) for t in trips if t.get("predicate") == "derived_from"}
    kind = memory.get("kind", memory.get("type"))
    evidence = {
        "id": mid,
        "kind": kind,
        "created_at_ms": memory.get("created_at_ms"),
        "triplets": trips,
        "parents": {p: parents.get(p) for p in sorted(derived)},
    }

    def result(status, writer, reason):
        return {**evidence, "classification": status, "writer": writer, "reason": reason}

    if kind in {"correction", "episode"}:
        return result("excluded", "preserved", f"{kind} stays in both arms")
    if len(sources) > 1 or (sources and derived):
        return result("unresolved", "unknown", "conflicting writer provenance")
    if sources == {"distillation"}:
        return result("included", "queue_distillation", "source=distillation")
    if sources:
        return result("excluded", "other_source", "source=" + ",".join(sorted(sources)))
    if any(t.get("predicate") == "ingested_from" for t in trips):
        return result(
            "excluded", "ingester", "ingested_from distinguishes ingester.cpp from native distiller"
        )
    if derived:
        if any(
            parents.get(p) and parents[p].get("type", parents[p].get("kind")) != "episode"
            for p in derived
        ):
            return result("unresolved", "unknown", "derived_from contradicts episode provenance")
        if any(not parents.get(p) for p in derived):
            return result("ambiguous", "ambiguous", "derived_from parent unavailable")
        if kind == "operational":
            return result(
                "excluded",
                "native_value_fact",
                "episode-derived operational: deterministic value-fact writer",
            )
        if kind in NATIVE_KINDS:
            return result(
                "included", "native_learning", "episode-derived native learning; no ingested_from"
            )
        return result(
            "included", "native_learning_unexpected_kind", "episode-derived native output"
        )
    return result("ambiguous", "ambiguous", "no writer provenance; excluded from both arms")


def enumerate_memories(rpc, realm):
    rows, offset = {}, 0
    while True:
        page = rpc.call("list_memories_brief", realm=realm, limit=100, offset=offset)["memories"]
        for row in page:
            mid = str(row["id"])
            require(mid not in rows, "unstable pagination: repeated memory ID")
            rows[mid] = row
        offset += len(page)
        if not page:
            return rows


def cohort_report(socket, realm, *, preview=False, source=None):
    require(realm == REALM, f"realm must be {REALM}")
    rpc = RPC(socket)
    health = rpc.call("health_check")
    argv = Path(f"/proc/{health['pid']}/cmdline").read_bytes().decode().split("\0")
    require("--path" in argv and "daemon" in argv, "cannot bind socket PID to a store path")
    mind = argv[argv.index("--path") + 1]
    if not preview:
        require(
            (Path(mind) / ".quiesce").is_file() and "--no-distill" in argv,
            "official cohort inspection requires a quiescent scratch replica",
        )
    before = family(source or mind, hash_files=not preview)
    if source:
        loaded = family(mind, hash_files=False)
        require(
            loaded["selection"] == before["selection"],
            "probe and immutable source select different families",
        )
    started = now_ms()
    memories = enumerate_memories(rpc, realm)
    queue_rows = rpc.call("query_graph", object="distillation")["triplets"]

    def inspect(item):
        mid, memory = item
        trips = rpc.call("query_graph", subject=mid)["triplets"]
        try:
            provenance = rpc.call("memory_provenance", id=int(mid))
        except ValueError as exc:
            provenance = {"unavailable": str(exc)}
        parents = {}
        for t in trips:
            if t.get("predicate") == "derived_from":
                parent = str(t["object"])
                parents[parent] = rpc.call("get", id=parent) if parent.isdecimal() else None
        result = classify(memory, trips, parents)
        result["provenance"] = provenance
        meta = provenance.get("meta") or {}
        require(not meta or str(meta.get("id")) == mid, "metadata ID mismatch")
        result["created_at_ms"] = meta.get("created_at_ms")
        result["access_count"] = meta.get("access_count")
        result["content_sha256"] = seal(memory["content"])
        return result

    with ThreadPoolExecutor(max_workers=4) as pool:
        evidence = list(pool.map(inspect, sorted(memories.items())))
    after = family(source or mind, hash_files=False)
    stable = before["selection"] == after["selection"] and all(
        before["files"].get(n) == v for n, v in after["files"].items() if n.startswith("MANIFEST.")
    )
    stable = stable and {n: v["size"] for n, v in before["files"].items()} == {
        n: v["size"] for n, v in after["files"].items()
    }
    stable = stable and set(memories) == set(enumerate_memories(rpc, realm))
    unresolved = [e["id"] for e in evidence if e["classification"] == "unresolved"]
    return {
        "schema": 1,
        "preview": preview,
        "realm": realm,
        "cut_timestamp_ms": now_ms(),
        "inspection_started_ms": started,
        "completed_timestamp_ms": now_ms(),
        "socket": str(socket),
        "store": before,
        "enumeration_stable": stable,
        "queue_object_query": queue_rows,
        "ids": [e["id"] for e in evidence if e["classification"] == "included"],
        "unresolved": unresolved,
        "ambiguous_ids": [e["id"] for e in evidence if e["classification"] == "ambiguous"],
        "ambiguous_population": ambiguous_population(evidence),
        "unexpected_native_kinds": dict(
            Counter(e["kind"] for e in evidence if e["writer"] == "native_learning_unexpected_kind")
        ),
        "evidence": evidence,
        "counts": dict(Counter(e["writer"] for e in evidence)),
        "classification_counts": dict(Counter(e["classification"] for e in evidence)),
    }


def ambiguous_population(evidence):
    rows = [e for e in evidence if e["classification"] == "ambiguous"]
    cutoff = int(datetime(2026, 3, 26, tzinfo=timezone.utc).timestamp() * 1000)
    hist = {"before_2026-03-26": 0, "on_or_after_2026-03-26": 0, "unknown": 0}
    kinds = Counter()
    for row in rows:
        kinds[row["kind"]] += 1
        ts = row.get("created_at_ms")
        hist[
            "unknown"
            if not ts
            else ("before_2026-03-26" if ts < cutoff else "on_or_after_2026-03-26")
        ] += 1
    total = sum(e.get("access_count") or 0 for e in evidence)
    ambiguous = sum(e.get("access_count") or 0 for e in rows)
    return {
        "count": len(rows),
        "kinds": dict(kinds),
        "creation_date_histogram": hist,
        "historical_access_count": ambiguous,
        "historical_total_access_count": total,
        "historical_access_share": ambiguous / total if total else None,
        "access_count_missing": sum(e.get("access_count") is None for e in evidence),
        "arm_a_recall_exposure": {
            "share": None,
            "status": "not measured: diagnostic has no arm A",
            "required_after_exclusion": 0,
        },
    }


def task_payload(task):
    return {k: v for k, v in task.items() if k not in {"validation", "eligible_cohort_ids"}}


def install_grader(task, work):
    target = Path(work) / ".learning-grader"
    require(not target.exists() and not target.is_symlink(), "grader path already exists")
    target.mkdir()
    for name, content in task["grader"]["files"].items():
        relative = Path(name)
        require(not relative.is_absolute() and ".." not in relative.parts, "unsafe grader path")
        path = target / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)


def check_task(task):
    for key in (
        "id",
        "prompt",
        "repo",
        "cwd_sha",
        "grader",
        "initial_state_hashes",
        "transcript_sha256",
        "prompt_timestamp_ms",
        "selection_note",
        "dependencies",
    ):
        require(key in task, f"missing task {key}")
    require(re.fullmatch(r"[a-zA-Z0-9_-]+", task["id"]), "unsafe task id")
    require(
        task["prompt"].strip() and task["selection_note"].strip(), "empty prompt/selection note"
    )
    require(re.fullmatch(r"[a-f0-9]{64}", task["transcript_sha256"]), "invalid transcript SHA256")
    require(isinstance(task["prompt_timestamp_ms"], int), "timestamp must be epoch milliseconds")
    require(task["grader"]["files"], "grader must contain hidden checks")
    require(
        isinstance(task["grader"]["command"], list) and task["grader"]["command"],
        "grader command must be argv",
    )
    require(task.get("known_good"), "known_good ref is required for pre-fail/post-pass validation")
    require(task["dependencies"], "pin task dependencies (paths and hashes)")
    for dep in task["dependencies"]:
        require(digest(dep["path"]) == dep["sha256"], "dependency changed")


def add_task(entry, tasks_path):
    task = read_json(entry)
    task["repo"] = str(Path(task["repo"]).resolve())
    task["cwd_sha"] = git(task["repo"], "rev-parse", task["cwd_sha"] + "^{commit}")
    if task.get("known_good"):
        task["known_good"] = git(task["repo"], "rev-parse", task["known_good"] + "^{commit}")
    task["initial_state_hashes"] = initial_hashes(task["repo"], task["cwd_sha"])
    if "transcript" in task:
        task["transcript_sha256"] = digest(task.pop("transcript"))
    task.pop("validation", None)
    check_task(task)
    tasks = read_json(tasks_path) if Path(tasks_path).exists() else []
    require(task["id"] not in {t["id"] for t in tasks}, "duplicate task")
    tasks.append(task)
    write_json(tasks_path, tasks)


def validate_task(task):
    check_task(task)
    results = {}
    for label, ref in (("initial", task["cwd_sha"]), ("known_good", task["known_good"])):
        state = {**task, "cwd_sha": ref, "initial_state_hashes": initial_hashes(task["repo"], ref)}
        with tempfile.TemporaryDirectory(prefix="learning-validate-") as td:
            with task_worktree(state, td) as work:
                install_grader(task, work)
                env = {
                    "PATH": "/usr/bin:/bin",
                    "HOME": str(Path(td) / "home"),
                    "CHITTA_SOCKET_PATH": str(Path(td) / "absent.sock"),
                    "CHITTA_DB_PATH": str(Path(td) / "mind"),
                    "CHITTA_NO_QUEUE": "1",
                    "CHITTA_BIN": "/bin/true",
                    "CHITTA_QUEUE": str(Path(td) / "queue"),
                    "XDG_RUNTIME_DIR": str(Path(td) / "run"),
                    "LANG": "C.UTF-8",
                }
                for directory in ("home", "mind", "run"):
                    (Path(td) / directory).mkdir()
                result = command(
                    task["grader"]["command"],
                    env=env,
                    cwd=work,
                    check=False,
                    timeout=task["grader"].get("timeout_s", 60),
                )
                results[label] = {
                    "exit_code": result.returncode,
                    "stdout": result.stdout,
                    "stderr": result.stderr,
                }
    require(results["initial"]["exit_code"] > 0, "grader must fail normally on initial state")
    require(results["known_good"]["exit_code"] == 0, "grader must pass on known_good")
    return {"payload_sha256": seal(task_payload(task)), "timestamp_ms": now_ms(), **results}


def validate_freeze(tasks, cohort, config, *, fixture=False):
    require(not cohort.get("unresolved"), "cohort has unresolved IDs")
    require(fixture or not cohort.get("fixture"), "fixture cohort cannot certify an official panel")
    ids = cohort.get("ids", [])
    require(len(ids) == len(set(ids)), "duplicate cohort IDs")
    evidence = cohort.get("evidence", [])
    require(len(evidence) == len({e["id"] for e in evidence}), "duplicate cohort evidence")
    require(
        set(ids) == {e["id"] for e in evidence if e["classification"] == "included"},
        "cohort membership/evidence mismatch",
    )
    require(
        set(cohort.get("ambiguous_ids", []))
        == {e["id"] for e in evidence if e["classification"] == "ambiguous"},
        "ambiguous membership/evidence mismatch",
    )
    for entry in evidence:
        computed = classify(entry, entry["triplets"], entry["parents"])
        require(
            computed["classification"] == entry["classification"]
            and computed["classification"] != "unresolved",
            "cohort classification is unresolved or inconsistent",
        )
    require(not cohort.get("preview"), "preview cohort cannot be frozen")
    require(cohort.get("enumeration_stable"), "unstable cohort enumeration")
    require(cohort["store"]["fully_hashed"], "source family needs full hashes")
    require(cohort["realm"] == REALM, "wrong cohort realm")
    require(
        len(tasks) == (2 if fixture else 20),
        "expected two fixture tasks or twenty prospective tasks",
    )
    require(len({t["id"] for t in tasks}) == len(tasks), "duplicate task IDs")
    times = [t["prompt_timestamp_ms"] for t in tasks]
    require(
        times == sorted(times) and min(times) > cohort["cut_timestamp_ms"],
        "tasks must be prospective and ordered",
    )
    for task in tasks:
        check_task(task)
        validation = task.get("validation", {})
        require(
            validation.get("payload_sha256") == seal(task_payload(task)),
            "task lacks current validation",
        )
        require(
            validation.get("initial", {}).get("exit_code", 0) > 0
            and validation.get("known_good", {}).get("exit_code") == 0,
            "missing pre-fail/post-pass evidence",
        )
        require(
            not task.get("needs_context") and not task.get("unreconstructed_state"),
            "non-self-contained task",
        )
        require(
            task.get("selection_rule") == "next_eligible_graded_prompt",
            "incorrect prospective selection rule",
        )
        require(
            task.get("recall_inspected") is False, "tasks must be selected before recall inspection"
        )
        require(
            initial_hashes(task["repo"], task["cwd_sha"]) == task["initial_state_hashes"],
            "initial state changed",
        )
    for key in (
        "model",
        "claude_version",
        "max_turns",
        "budget_usd",
        "timeout_s",
        "claude_bin",
        "chitta_bin",
        "chittad_bin",
        "embed_model",
        "seed",
    ):
        require(config.get(key) is not None, f"missing runner pin: {key}")
    require(config.get("isolation", "strict") in {"strict", "home-audit"}, "invalid isolation")
    require(config.get("allowed_tools") == TOOLS, "allowedTools must be pinned")
    require(config.get("permission_mode") == "dontAsk", "permission mode must be dontAsk")
    require(
        config["max_turns"] > 0 and config["budget_usd"] > 0 and config["timeout_s"] > 0,
        "invalid execution budget",
    )


def freeze(tasks_path, cohort_path, config_path, out, *, fixture=False, isolation=None):
    tasks, cohort, config = map(read_json, (tasks_path, cohort_path, config_path))
    if isolation:
        config["isolation"] = isolation
    config.setdefault("isolation", "strict")
    config.setdefault("allowed_tools", TOOLS)
    config.setdefault("permission_mode", "dontAsk")
    validate_freeze(tasks, cohort, config, fixture=fixture)
    protected = [*live_paths().values(), ROOT, cohort["store"]["mind"], *[t["repo"] for t in tasks]]
    roots = config.get("runtime_roots", [])
    validate_runtime_roots(roots, protected)
    if not fixture:
        executable = Path(sys.executable).resolve()
        require(
            executable.is_relative_to(Path("/usr"))
            or any(executable.is_relative_to(Path(r).resolve()) for r in roots),
            "declare the Python runtime directory in runtime_roots for the model sandbox",
        )
    runtime_pins = {str(Path(r).resolve()): tree_identity(r) for r in roots}
    require(
        family(cohort["store"]["mind"]) == cohort["store"],
        "source replica changed after cohort cut",
    )
    out = Path(out)
    require(not out.exists(), "freeze output already exists; use a new path")
    tasks = deepcopy(tasks)
    for task in tasks:
        task["eligible_cohort_ids"] = cohort["ids"]
    out.mkdir(parents=True)
    write_json(out / "tasks.json", tasks)
    write_json(out / "cohort.json", cohort)
    pins = {
        str(p.relative_to(ROOT)): digest(p)
        for base in ("hooks", "benchmarks/learning")
        for p in sorted((ROOT / base).rglob("*"))
        if p.is_file() and p.suffix in {".sh", ".py"} and "__pycache__" not in p.parts
    }
    for p in (
        ROOT / "scripts/eval-replica.sh",
        ROOT / "scripts/eval-replica-select.py",
        ROOT / "scripts/eval-learning.sh",
        ROOT / "benchmarks/learning/protocol.md",
    ):
        pins[str(p.relative_to(ROOT))] = digest(p)
    binaries = {
        key: {"path": str(Path(config[key]).resolve()), "sha256": digest(config[key])}
        for key in ("claude_bin", "chitta_bin", "chittad_bin", "embed_model")
    }
    manifest = {
        "schema": 1,
        "isolation": config.get("isolation", "strict"),
        "runtime_sha256": runtime_pins,
        "python": {"path": sys.executable, "sha256": digest(sys.executable)},
        "fixture": fixture,
        "frozen_timestamp_ms": now_ms(),
        "tasks_sha256": digest(out / "tasks.json"),
        "cohort_sha256": digest(out / "cohort.json"),
        "config": config,
        "binaries": binaries,
        "code_sha256": pins,
        "trials": 3,
        "realm": REALM,
        "utility_recall": False,
        "selection_exclusions": config.get("selection_exclusions", []),
    }
    write_json(out / "manifest.json", manifest)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    c = sub.add_parser("cohort")
    c.add_argument("--socket", required=True)
    c.add_argument("--realm", default=REALM)
    c.add_argument("--source", help="immutable source COPY used to start the probe replica")
    c.add_argument("--out", required=True)
    c.add_argument(
        "--dry-run", action="store_true", help="read-only diagnostic; never eligible for freeze"
    )
    t = sub.add_parser("task").add_subparsers(dest="task_action", required=True)
    a = t.add_parser("add")
    a.add_argument("--entry", required=True)
    a.add_argument("--tasks", required=True)
    v = t.add_parser("validate")
    v.add_argument("--tasks", required=True)
    v.add_argument("--id")
    f = sub.add_parser("freeze")
    for name in ("tasks", "cohort", "config", "out"):
        f.add_argument("--" + name, required=True)
    f.add_argument("--fixture", action="store_true")
    f.add_argument("--isolation", choices=("strict", "home-audit"))
    args = parser.parse_args()
    if args.action == "cohort":
        result = cohort_report(args.socket, args.realm, preview=args.dry_run, source=args.source)
        write_json(args.out, result)
        print(
            json.dumps(
                {
                    "counts": result["counts"],
                    "unresolved": len(result["unresolved"]),
                    "included": len(result["ids"]),
                    "preview": result["preview"],
                }
            )
        )
    elif args.action == "task":
        if args.task_action == "add":
            add_task(args.entry, args.tasks)
        else:
            tasks = read_json(args.tasks)
            require(not args.id or args.id in {t["id"] for t in tasks}, "unknown task id")
            for task in tasks:
                if not args.id or task["id"] == args.id:
                    task["validation"] = validate_task(task)
            write_json(args.tasks, tasks)
    else:
        result = freeze(
            args.tasks,
            args.cohort,
            args.config,
            args.out,
            fixture=args.fixture,
            isolation=args.isolation,
        )
        print(
            json.dumps(
                {
                    "manifest": str(Path(args.out) / "manifest.json"),
                    "tasks_sha256": result["tasks_sha256"],
                    "cohort_sha256": result["cohort_sha256"],
                }
            )
        )


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError) as exc:
        raise SystemExit(f"freeze: {exc}") from exc
