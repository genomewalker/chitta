#!/usr/bin/env python3
"""Frozen, deterministic repository-truth retrieval panel; never learns or seeds answers."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

PANEL = Path(__file__).with_name("questions.json")
CONFIG = {"realm": "project:cc-soul", "limit": 3, "strategy": "fused", "no_learn": True}


def match(matcher: dict, hit: dict) -> bool:
    text = hit.get("content", hit.get("text", ""))
    kind, value = matcher["type"], matcher["value"]
    if kind == "ids":
        return str(hit["id"]) in value
    if kind == "exact":
        # Exact literal substring, case-sensitive; not a bag of answer tokens.
        return value in text
    if kind == "regex":
        return re.search(value, text) is not None
    raise ValueError(f"unknown matcher: {kind}")


def load_panel(path: Path = PANEL) -> dict:
    panel = json.loads(path.read_text())
    questions = panel["questions"]
    if len(questions) != 50 or len({q["id"] for q in questions}) != 50:
        raise ValueError("panel requires 50 distinct questions")
    if [sum(q["split"] == s for q in questions) for s in ("visible", "holdout")] != [30, 20]:
        raise ValueError("panel split must be 30 visible / 20 holdout")
    if sum(q["expected_answer"] == "not recorded" for q in questions) != 10:
        raise ValueError("panel requires 10 abstention questions")
    if len(panel["regression_fixtures"]) != 5:
        raise ValueError("five named regression fixtures required")
    if not re.fullmatch(r"[0-9a-f]{40}", panel["source_commit"]):
        raise ValueError("source_commit must be a full SHA")
    for q in questions + panel["regression_fixtures"]:
        if not all(q.get(k) for k in ("question", "expected_answer", "citation", "id")):
            raise ValueError("missing question evidence")
        if q["expected_answer"] == "not recorded" and not q["wrong_answer_traps"]:
            raise ValueError("abstention needs predeclared wrong-answer traps")
        for matcher in [q["matcher"], *q["wrong_answer_traps"]]:
            match(matcher, {"id": "", "content": ""})
    return panel


def endpoint(live: bool = False) -> str:
    raw = os.environ.get("CHITTA_EVAL_SOCKET", "")
    if not raw:
        raise ValueError("CHITTA_EVAL_SOCKET is required, including with --live")
    socket = Path(raw).resolve()
    if not socket.is_socket():
        raise ValueError(f"not a Unix socket: {socket}")
    if not live:
        # Fail closed: only a socket inside the private eval mind is eligible.
        # realpath prevents a symlink in the replica from aliasing a live socket.
        mind_raw = os.environ.get("CHITTA_EVAL_MIND")
        if not mind_raw:
            raise ValueError("CHITTA_EVAL_MIND is required without --live")
        mind = Path(mind_raw).resolve()
        live_minds = {Path.home() / ".claude/mind"}
        for key in ("CHITTA_LIVE_MIND", "CHITTA_DB_PATH", "CHITTA_MIND", "MIND_PATH"):
            if os.environ.get(key):
                live_minds.add(Path(os.environ[key]))
        if mind == Path("/") or any(mind == p.resolve() for p in live_minds):
            raise ValueError("refusing live or unsafe eval mind; use --live deliberately")
        if mind not in socket.parents:
            raise ValueError("socket must resolve inside CHITTA_EVAL_MIND; use --live deliberately")
        live_socket = os.environ.get("CHITTA_SOCKET_PATH")
        if live_socket and socket == Path(live_socket).resolve():
            raise ValueError("socket aliases CHITTA_SOCKET_PATH; use --live deliberately")
    return str(socket)


def recall(question: str, socket: str) -> list:
    env = dict(os.environ, CHITTA_CLI_AUTOSTART="0", CC_SOUL_CLI_AUTOSTART="0")
    result = subprocess.run(
        [
            os.environ.get("CHITTA_BIN", "chitta"),
            "recall",
            "--json",
            "--realm",
            CONFIG["realm"],
            "--limit",
            "3",
            "--no-learn",
            "--strategy",
            CONFIG["strategy"],
            "--socket-path",
            socket,
            "--query",
            question,
        ],
        env=env,
        capture_output=True,
        text=True,
        check=True,
        timeout=60,
    )
    data = json.loads(result.stdout)
    if not isinstance(data, dict) or data.get("error") or not isinstance(data.get("results"), list):
        raise ValueError("invalid recall envelope; refusing to score transport failure")
    hits = data["results"]
    if len(hits) > 3:
        raise ValueError("recall returned more than requested top three")
    for hit in hits:
        if (
            not isinstance(hit, dict)
            or "id" not in hit
            or not isinstance(hit.get("content", hit.get("text")), str)
        ):
            raise ValueError("invalid recall hit; refusing to score missing content")
    return hits


def score(question: dict, hits: list) -> dict:
    hits = hits[:3]
    traps = [str(h["id"]) for h in hits if any(match(t, h) for t in question["wrong_answer_traps"])]
    matched = [str(h["id"]) for h in hits if match(question["matcher"], h)]
    abstention = question["expected_answer"] == "not recorded"
    outcome = (
        "wrong-confident"
        if traps
        else "abstain-correct"
        if abstention
        else "correct"
        if matched
        else "miss"
    )
    return {
        "id": question["id"],
        "split": question["split"],
        "expects_abstention": abstention,
        "outcome": outcome,
        "correct": outcome in {"correct", "abstain-correct"},
        "matched_ids": matched,
        "trap_ids": traps,
        "top3_ids": [str(h["id"]) for h in hits],
    }


def aggregate(rows: list) -> dict:
    answerable = [r for r in rows if not r["expects_abstention"]]
    unknown = [r for r in rows if r["expects_abstention"]]
    correct = sum(r["correct"] for r in answerable)
    abstained = sum(r["correct"] for r in unknown)
    wrong = sum(r["outcome"] == "wrong-confident" for r in rows)
    return {
        "n": len(rows),
        "answerable": len(answerable),
        "hits": correct,
        "p3": correct / len(answerable) if answerable else None,
        "abstention_questions": len(unknown),
        "abstain_correct": abstained,
        "abstain": abstained / len(unknown) if unknown else None,
        "correct_total": correct + abstained,
        "wrong_confident": wrong,
        "utility": correct + abstained - 2 * wrong,
    }


def evaluate(panel: dict, socket: str, split: str = "all") -> dict:
    questions = [q for q in panel["questions"] if split == "all" or q["split"] == split]
    rows = [
        score(q, recall(q["question"], socket)) for q in sorted(questions, key=lambda q: q["id"])
    ]
    fixtures = [score(q, recall(q["question"], socket)) for q in panel["regression_fixtures"]]
    return {
        "schema_version": 1,
        "source_commit": panel["source_commit"],
        "panel_sha256": hashlib.sha256(PANEL.read_bytes()).hexdigest(),
        "config": CONFIG,
        "snapshot_id": os.environ.get("CHITTA_EVAL_SNAPSHOT_ID"),
        "socket": socket,
        "fixture_status": panel["fixture_status"],
        "overall": aggregate(rows),
        "splits": {
            s: aggregate([r for r in rows if r["split"] == s]) for s in ("visible", "holdout")
        },
        "rows": rows,
        "regression_fixtures": fixtures,
    }


def markdown(report: dict) -> str:
    lines = [
        "| Panel | Hits / answerable (p@3) | Abstention | Total correct | Wrong-confident |",
        "|---|---:|---:|---:|---:|",
    ]
    for name, row in [("all", report["overall"]), *report["splits"].items()]:
        lines.append(
            f"| {name} | {row['hits']}/{row['answerable']} | "
            f"{row['abstain_correct']}/{row['abstention_questions']} | "
            f"{row['correct_total']}/{row['n']} | {row['wrong_confident']} |"
        )
    lines += ["", report["fixture_status"], "", "| Fixture | Outcome |", "|---|---|"]
    lines += [f"| {r['id']} | {r['outcome']} |" for r in report["regression_fixtures"]]
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--live", action="store_true")
    parser.add_argument("--split", choices=["visible", "holdout", "all"], default="all")
    parser.add_argument("--output", type=Path, help="JSON path; adjacent .md table also written")
    args = parser.parse_args()
    try:
        panel = load_panel()
        if args.dry_run:
            print(
                json.dumps(
                    {
                        "valid": True,
                        "source_commit": panel["source_commit"],
                        "questions": 50,
                        "visible": 30,
                        "holdout": 20,
                        "abstentions": 10,
                        "fixture_status": panel["fixture_status"],
                    },
                    indent=2,
                )
            )
            return
        report = evaluate(panel, endpoint(args.live), args.split)
        report["mode"] = "live-smoke" if args.live else "replica"
        if args.output:
            args.output.write_text(json.dumps(report, indent=2) + "\n")
            args.output.with_suffix(".md").write_text(markdown(report))
        else:
            print(json.dumps(report, indent=2))
        print(markdown(report), file=sys.stderr)
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as exc:
        parser.exit(2, f"current-truth: {exc}\n")


if __name__ == "__main__":
    main()
