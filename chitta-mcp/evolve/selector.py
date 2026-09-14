"""Deterministic UCB ranking with a cumulative hypothesis exploration quota."""

from __future__ import annotations

import argparse
import json
import math
from dataclasses import replace
from pathlib import Path

from .proposals import PRIOR_EFFORT, VERIFIABILITY, Proposal, dedupe, gather, normalize, number
from .store import MemoryStore, body


def history(store: MemoryStore) -> list[dict]:
    # Keep skipped surveys too; one cycle is one event, even after write retries.
    unique = {}
    for record in store.recall("verdict"):
        value = body(record)
        verdict = value.get("verdict", "")
        if isinstance(verdict, str) and verdict.split(":")[0] in (
            "accept",
            "reject",
            "inconclusive",
            "refuted",
            "skipped",
        ):
            if value.get("proposal_id") or value.get("survey"):
                key = value.get("cycle_id") or str(record.get("id"))
                unique[key] = value
    return list(unique.values())


def mechanism_key(value: str) -> str:
    return " ".join(value.split()).casefold()


def effort_from_history(proposal: Proposal, verdicts: list[dict]) -> str:
    levels = list(PRIOR_EFFORT)
    failures = 0
    floor = levels.index(proposal.prior_effort)

    def matches(value):
        return (
            mechanism_key(value["mechanism"]) == mechanism_key(proposal.mechanism)
            if isinstance(value.get("mechanism"), str)
            else value.get("proposal_id", value.get("id")) == proposal.id
        )

    for verdict in verdicts:
        if matches(verdict):
            resolution = verdict.get("resolution") or {}
            if verdict.get("verdict") == "refuted" or resolution.get("outcome") == "refuted":
                failures += 1
        if verdict.get("verdict") == "skipped:no_tractable_candidate":
            updates = [u for u in verdict.get("prior_effort_updates", []) if matches(u)]
            if updates:
                failures += 1
                floor = max(floor, *(levels.index(u["prior_effort"]) for u in updates))
    return levels[min(2, max(floor, failures))]


def factors(proposal: Proposal) -> tuple[float, float, float]:
    return (
        VERIFIABILITY[proposal.verifiability],
        PRIOR_EFFORT[proposal.prior_effort],
        1.2 if proposal.internal_evidence else 1.0,
    )


def attempts(verdicts: list[dict]) -> list[dict]:
    return [v for v in verdicts if v.get("proposal_id")]


def rank(
    proposals: list[Proposal], verdicts: list[dict], c: float = 1.0
) -> list[tuple[Proposal, float, int]]:
    if number(c) < 0:
        raise ValueError("UCB c must be nonnegative")
    counts: dict[str, int] = {}
    tried = attempts(verdicts)
    for verdict in tried:
        ident = verdict["proposal_id"]
        counts[ident] = counts.get(ident, 0) + 1
    ranked = []
    for proposal in dedupe(proposals):
        proposal = replace(proposal, prior_effort=effort_from_history(proposal, verdicts))
        tries = counts.get(proposal.id, 0)
        gain = proposal.expected_gain
        # Deltas are fractional metric changes, with sign encoding direction.
        utility = (
            abs(gain["delta"])
            * gain["confidence"]
            / (proposal.cost["effort_h"] * proposal.cost["blast_radius"])
        )
        v, p, i = factors(proposal)
        score = utility * v * p * i + c * math.sqrt(math.log(len(tried) + 1) / (tries + 1))
        ranked.append((proposal, score, tries))
    return sorted(ranked, key=lambda row: (-row[1], row[0].id))


def shortlist(
    proposals: list[Proposal],
    verdicts: list[dict],
    quota: float = 0.3,
    c: float = 1.0,
    k: int = 3,
) -> list[Proposal]:
    if not 0 <= number(quota) <= 1:
        raise ValueError("explore quota must be [0,1]")
    if k < 1:
        raise ValueError("top-k must be positive")
    ranked = rank(proposals, verdicts, c)
    hypotheses = {p.id for p in proposals if p.source == "hypothesis"}
    tried = attempts(verdicts)
    explored = sum(v.get("source") == "hypothesis" or v["proposal_id"] in hypotheses for v in tried)
    due = explored < math.ceil(quota * (len(tried) + 1) - 1e-12)
    # A survey cannot undo the quota or reintroduce exhausted exploitation.
    return [
        p
        for p, _, _ in ranked
        if (p.id in hypotheses if due and hypotheses else p.prior_effort != "exhausted")
    ][:k]


def choose(
    proposals: list[Proposal], verdicts: list[dict], quota: float = 0.3, c: float = 1.0
) -> Proposal:
    candidates = shortlist(proposals, verdicts, quota, c, k=1)
    if not candidates:
        raise ValueError("no eligible proposals; exhausted ideas require exploration quota")
    return candidates[0]


def table(ranked: list[tuple[Proposal, float, int]]) -> str:
    lines = [
        "rank  id                source      score     tries  V     P     I    prior_effort  title"
    ]
    for n, (p, score, tries) in enumerate(ranked, 1):
        v, effort, i = factors(p)
        lines.append(
            f"{n:>4}  {p.id}  {p.source:10}  {score:8.5f}  {tries:5}  "
            f"{v:.2f}  {effort:.2f}  {i:.1f}  {p.prior_effort:12}  {p.title}"
        )
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
    if args.dry_run:
        print(table(rank(proposals, verdicts, args.c)))
    else:
        print(json.dumps(choose(proposals, verdicts, args.explore_quota, args.c).to_dict()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
