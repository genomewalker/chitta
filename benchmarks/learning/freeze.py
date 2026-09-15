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
    sources = {t["object"] for t in trips if t.get("predicate") == "source" and t.get("object")}
    derived = {str(t["object"]) for t in trips if t.get("predicate") == "derived_from"}
    kind = memory.get("kind", memory.get("type"))
    evidence = {
        "id": mid,
        "kind": kind,
        "created_at_ms": memory.get("created_at_ms"),
        "triplets": trips,
        "parents": {p: parents.get(p) for p in sorted(derived)},
        "explicit_signal": memory.get(
            "explicit_signal", bool(re.match(r"^\[(artifact|done)\]", memory.get("content", "")))
        ),
    }

    def result(status, writer, reason):
        return {**evidence, "classification": status, "writer": writer, "reason": reason}

    if kind in {"correction", "episode"}:
        return result("excluded", "preserved", f"{kind} stays in both arms")
    if len(sources) > 1 or (sources and derived and sources != {"distillation"}):
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
            return result("unresolved", "unknown", "derived_from parent unavailable")
        if kind == "operational":
            return result(
                "included",
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
    if evidence["explicit_signal"]:
        return result(
            "excluded", "explicit_signal", "[artifact]/[done] hook signal stays in both arms"
        )
    return result("unlabelled", "unlabelled", "no writer provenance; fix writer before freeze")


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


def cohort_report(socket, realm, *, preview=False, source=None, cut=None):
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
    if cut is None:
        # The cut records state only. Historical writers are never classified.
        return {
            "schema": 2,
            "preview": preview,
            "realm": realm,
            "cut_timestamp_ms": started,
            "cut_store": before,
        }
    cut_ms = cut["cut_timestamp_ms"]
    # Hooks can reach global fallback and graph neighbors outside the pinned realm.
    # Official freezes must inventory the whole store; live diagnostics stay scoped.
    inventory_realm = realm if preview else ""
    memories = enumerate_memories(rpc, inventory_realm)

    def inspect(item):
        mid, memory = item
        provenance = rpc.call("memory_provenance", id=int(mid))
        meta = provenance.get("meta") or {}
        require(str(meta.get("id")) == mid, f"metadata ID mismatch: {mid}")
        ts = meta.get("created_at_ms")
        require(isinstance(ts, int) and ts > 0, f"missing creation timestamp: {mid}")
        memory = {**memory, "created_at_ms": ts}
        if ts <= cut_ms:
            return {"id": mid, "kind": memory.get("kind", memory.get("type")), "created_at_ms": ts}
        trips = rpc.call("query_graph", subject=mid)["triplets"]
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
    stable = stable and set(memories) == set(enumerate_memories(rpc, inventory_realm))
    post = [e for e in evidence if e["created_at_ms"] > cut_ms]
    return {
        "schema": 2,
        "preview": preview,
        "realm": realm,
        "cut_timestamp_ms": cut_ms,
        "cut_store": cut["cut_store"],
        "inspection_started_ms": started,
        "completed_timestamp_ms": now_ms(),
        "socket": str(socket),
        "store": before,
        "enumeration_stable": stable,
        "unresolved": [e["id"] for e in post if e["classification"] == "unresolved"],
        "unlabelled": [
            {"id": e["id"], "kind": e["kind"]} for e in post if e["classification"] == "unlabelled"
        ],
        "evidence": evidence,
        "inventory_scope": inventory_realm or "all",
        "counts": dict(Counter(e["writer"] for e in post)),
        "automatic_by_writer": dict(
            Counter(e["writer"] for e in post if e["classification"] == "included")
        ),
        "baseline_count": len(evidence) - len(post),
    }


def task_membership(task, cohort):
    cut, timestamp = cohort["cut_timestamp_ms"], task["prompt_timestamp_ms"]
    require(timestamp > cut, f"task {task['id']} must be prospective: timestamp after cut")
    return {
        "eligible_cohort_ids": sorted(
            e["id"]
            for e in cohort["evidence"]
            if cut < e["created_at_ms"] <= timestamp and e["classification"] == "included"
        ),
        "future_ids": sorted(e["id"] for e in cohort["evidence"] if e["created_at_ms"] > timestamp),
    }


def task_manifest(task, cohort):
    return {
        "prompt_timestamp_ms": task["prompt_timestamp_ms"],
        "cut_timestamp_ms": cohort["cut_timestamp_ms"],
        "store_sha256": cohort["store"]["sha256"],
        **task_membership(task, cohort),
    }


def task_payload(task):
    return {
        k: v
        for k, v in task.items()
        if k not in {"validation", "eligible_cohort_ids", "future_ids"}
    }


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
    require(cohort.get("schema") == 2, "cohort needs prospective schema 2")
    require(
        fixture or cohort.get("inventory_scope") == "all",
        "official freeze needs all-store inventory for global fallback",
    )
    require(not cohort.get("unresolved"), "cohort has unresolved IDs")
    require(
        not cohort.get("unlabelled"),
        "post-cut records lack writer provenance (id/kind): "
        + json.dumps(cohort.get("unlabelled")),
    )
    require(fixture or not cohort.get("fixture"), "fixture cohort cannot certify an official panel")
    require(cohort["cut_store"]["fully_hashed"], "cut manifest needs full hashes")
    evidence = cohort["evidence"]
    require(len(evidence) == len({e["id"] for e in evidence}), "duplicate cohort evidence")
    for entry in evidence:
        require(
            isinstance(entry.get("created_at_ms"), int) and entry["created_at_ms"] > 0,
            f"missing creation timestamp: {entry['id']}",
        )
        if entry["created_at_ms"] <= cohort["cut_timestamp_ms"]:
            require("classification" not in entry, "pre-cut records must not be classified")
            continue
        computed = classify(entry, entry["triplets"], entry["parents"])
        require(
            computed["classification"] != "unlabelled",
            f"post-cut record lacks writer provenance: {entry['id']} ({entry['kind']})",
        )
        require(
            computed["classification"] == entry["classification"]
            and computed["classification"] != "unresolved"
            and computed["writer"] == entry["writer"],
            "cohort classification is unresolved or inconsistent",
        )
    for task in tasks:
        for key, expected in task_membership(task, cohort).items():
            require(key not in task or task[key] == expected, f"task {task['id']} {key} mismatch")
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


def freeze(
    tasks_path,
    cohort_path,
    config_path,
    out,
    *,
    fixture=False,
    isolation=None,
    socket=None,
    source=None,
):
    tasks, cohort, config = map(read_json, (tasks_path, cohort_path, config_path))
    if not fixture:
        require(
            socket and source, "freeze requires --socket probe and --source immutable task snapshot"
        )
        require(not cohort.get("preview"), "preview cut cannot be frozen")
        cohort = cohort_report(socket, cohort["realm"], source=source, cut=cohort)
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
        "source replica changed after task inspection",
    )
    out = Path(out)
    require(not out.exists(), "freeze output already exists; use a new path")
    tasks = deepcopy(tasks)
    for task in tasks:
        task.update(task_membership(task, cohort))
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
        "schema": 2,
        "task_cohorts": {t["id"]: task_manifest(t, cohort) for t in tasks},
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
        "--cut-timestamp-ms", type=int, help="diagnostic only: inspect writes since this cut"
    )
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
    f.add_argument("--socket")
    f.add_argument("--source")
    f.add_argument("--isolation", choices=("strict", "home-audit"))
    args = parser.parse_args()
    if args.action == "cohort":
        require(args.cut_timestamp_ms is None or args.dry_run, "backdated cuts are diagnostic only")
        cut = (
            None
            if args.cut_timestamp_ms is None
            else {"cut_timestamp_ms": args.cut_timestamp_ms, "cut_store": None}
        )
        result = cohort_report(
            args.socket, args.realm, preview=args.dry_run, source=args.source, cut=cut
        )
        write_json(args.out, result)
        print(
            json.dumps(
                {
                    "counts": result.get("counts", {}),
                    "unresolved": len(result.get("unresolved", [])),
                    "unlabelled": len(result.get("unlabelled", [])),
                    "cut_timestamp_ms": result["cut_timestamp_ms"],
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
            socket=args.socket,
            source=args.source,
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
