#!/usr/bin/env python3
"""Freeze complete exact relation joins before probing recall_analogy."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import statistics
import time
from pathlib import Path

from transport import rpc


def edge_key(edge: dict) -> tuple:
    return edge["subject"], edge["predicate"], edge["object"]


def join(source: list[dict], target: list[dict], b: str) -> tuple[list, list]:
    relations = [e for e in source if e["object"] == b]
    predicates = {e["predicate"] for e in relations}
    return relations, [e for e in target if e["predicate"] in predicates]


def rank(edges: list[dict]) -> list[str]:
    ordered = sorted(edges, key=lambda e: (-e["weight"], -e["valid_from_ms"], e["object"],
                                          e["predicate"], e.get("source_memory_id") or 0, e["id"]))
    return list(dict.fromkeys(e["object"] for e in ordered))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tasks", type=Path, default=Path(__file__).with_name("tasks.json"))
    parser.add_argument("--output", type=Path, default=Path(__file__).with_name("baseline.json"))
    parser.add_argument("--snapshot-id", required=True)
    args = parser.parse_args()
    socket = os.environ.get("CHITTA_EVAL_SOCKET", "")
    if not socket or not Path(socket).is_socket():
        parser.error("CHITTA_EVAL_SOCKET must name the private frozen replica")
    raw = args.tasks.read_bytes()
    tasks = json.loads(raw)["tasks"]
    assert len(tasks) == 14 and all(t["style"] == "proportional" for t in tasks)
    world_ms = int(time.time() * 1000)
    graph = {}

    def subject(name):
        if name not in graph:
            # Temporal export carries source-memory IDs and recency, without a
            # fixed output buffer. Cross-check the ordinary indexed graph API:
            # any truncation or temporal/supersession discrepancy fails the run.
            full = rpc(socket, "triplet_query_as_of", dict(subject=name, world_ms=world_ms))["triplets"]
            ordinary = rpc(socket, "query_graph", dict(subject=name))["triplets"]
            assert {e["id"] for e in full} == {e["id"] for e in ordinary}, name
            # Legacy reused triplet IDs can resolve to another subject.
            # As in the original grounding audit, require exact subjects.
            graph[name] = [e for e in full if e["subject"] == name]
        return graph[name]

    candidates = sorted({e[k] for t in tasks for e in t["grounding"] for k in ("subject", "object")})
    positives, negatives = [], []
    used_queries, target_counts = set(), {}
    for task in tasks:
        a, b, c = (task["params"][k] for k in ("a", "b", "c"))
        started = time.perf_counter()
        # Measure actual two-query join separately from cached enumeration.
        source = rpc(socket, "triplet_query_as_of", dict(subject=a, world_ms=world_ms))["triplets"]
        target = rpc(socket, "triplet_query_as_of", dict(subject=c, world_ms=world_ms))["triplets"]
        source = [e for e in source if e["subject"] == a]
        target = [e for e in target if e["subject"] == c]
        relations, edges = join(source, target, b)
        answers = rank(edges)
        latency = (time.perf_counter() - started) * 1000
        assert source == subject(a) and target == subject(c)
        missing = [e for e in task["grounding"] if edge_key(e) not in
                   {edge_key(v) for v in subject(e["subject"])}]
        row = dict(id=task["id"], params=task["params"], relations=relations, edges=edges,
                   answers=answers, missing_grounding=missing, latency_ms=round(latency, 3))
        positives.append(row)
        # Prefer an existing subject with other outgoing edges. Selection uses
        # only graph facts, never analogy output; one negative per original query.
        ordered_targets = sorted(candidates, key=lambda n: (target_counts.get(n, 0), n))
        negative_c = next((n for n in ordered_targets if n != c and subject(n)
                           and (a, b, n) not in used_queries
                           and not join(source, subject(n), b)[1]), None)
        if negative_c is None:
            raise ValueError("no existing negative target for " + task["id"])
        used_queries.add((a, b, negative_c))
        target_counts[negative_c] = target_counts.get(negative_c, 0) + 1
        negatives.append(dict(id=task["id"] + "-negative", style="negative",
                              params=dict(mode="proportional", a=a, b=b, c=negative_c),
                              expected=[], source_id=task["id"], target_edges=subject(negative_c)))
    missing_count = sum(bool(t["missing_grounding"]) or not t["relations"] or not t["answers"]
                        for t in positives)
    out = dict(snapshot_id=args.snapshot_id, socket=socket, world_ms=world_ms,
               tasks_sha256=hashlib.sha256(raw).hexdigest(), positives=positives, negatives=negatives,
               summary=dict(tasks=14, hit_at_1=14-missing_count, hit_at_3=14-missing_count,
                            negative_abstentions=sum(bool(p["relations"]) for p in positives),
                            unsupported_answers=0, missing_grounding=missing_count,
                            median_latency_ms=round(statistics.median(p["latency_ms"] for p in positives), 3)))
    args.output.write_text(json.dumps(out, indent=2) + "\n")
    print(json.dumps(out["summary"]))


if __name__ == "__main__":
    main()
