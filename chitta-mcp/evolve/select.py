"""Deterministic UCB ranking with a cumulative hypothesis exploration quota."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

from .proposals import Proposal, dedupe, gather, normalize, number
from .store import MemoryStore, body


def history(store: MemoryStore) -> list[dict]:
    # Cycle IDs make retrying a verdict write idempotent for selection statistics.
    unique = {}
    for record in store.recall("verdict"):
        value = body(record)
        if value.get("verdict") in ("accept", "reject", "inconclusive") and value.get(
            "proposal_id"
        ):
            unique[value.get("cycle_id", str(record.get("id")))] = value
    return list(unique.values())


def rank(
    proposals: list[Proposal], verdicts: list[dict], c: float = 1.0
) -> list[tuple[Proposal, float, int]]:
    if number(c) < 0:
        raise ValueError("UCB c must be nonnegative")
    counts: dict[str, int] = {}
    for verdict in verdicts:
        ident = verdict["proposal_id"]
        counts[ident] = counts.get(ident, 0) + 1
    ranked = []
    for proposal in dedupe(proposals):
        tries = counts.get(proposal.id, 0)
        gain = proposal.expected_gain
        # Deltas are fractional metric changes, with sign encoding direction.
        utility = (
            abs(gain["delta"])
            * gain["confidence"]
            / (proposal.cost["effort_h"] * proposal.cost["blast_radius"])
        )
        score = utility + c * math.sqrt(math.log(len(verdicts) + 1) / (tries + 1))
        ranked.append((proposal, score, tries))
    return sorted(ranked, key=lambda row: (-row[1], row[0].id))


def choose(
    proposals: list[Proposal], verdicts: list[dict], quota: float = 0.3, c: float = 1.0
) -> Proposal:
    if not 0 <= number(quota) <= 1:
        raise ValueError("explore quota must be [0,1]")
    ranked = rank(proposals, verdicts, c)
    if not ranked:
        raise ValueError("no proposals available")
    hypotheses = {p.id for p in proposals if p.source == "hypothesis"}
    explored = sum(
        v.get("source") == "hypothesis" or v["proposal_id"] in hypotheses for v in verdicts
    )
    due = explored < math.ceil(quota * (len(verdicts) + 1) - 1e-12)
    if due and hypotheses:
        return next(p for p, _, _ in ranked if p.id in hypotheses)
    return ranked[0][0]


def table(ranked: list[tuple[Proposal, float, int]]) -> str:
    lines = ["rank  id                source      score     tries  title"]
    for n, (p, score, tries) in enumerate(ranked, 1):
        lines.append(f"{n:>4}  {p.id}  {p.source:10}  {score:8.5f}  {tries:5}  {p.title}")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--backlog", type=Path)
    parser.add_argument("--explore-quota", type=float, default=0.3)
    parser.add_argument("--c", type=float, default=1)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    store = MemoryStore()
    proposals = (
        [normalize(p) for p in json.loads(args.backlog.read_text())]
        if args.backlog
        else gather(args.repo, store)[0]
    )
    verdicts = history(store)
    selected = choose(proposals, verdicts, args.explore_quota, args.c)
    print(
        table(rank(proposals, verdicts, args.c)) if args.dry_run else json.dumps(selected.to_dict())
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
