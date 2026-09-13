"""Normalize telemetry, remembered problems and external mechanism cards."""

from __future__ import annotations

import hashlib
import json
import math
import re
import subprocess
import sys
from collections import defaultdict
from dataclasses import asdict, dataclass
from pathlib import Path

from .store import MemoryStore, body


@dataclass(frozen=True)
class Proposal:
    id: str
    title: str
    mechanism: str
    expected_gain: dict
    cost: dict
    evidence: list
    source: str

    def to_dict(self) -> dict:
        return asdict(self)


def stable_id(title: str, mechanism: str) -> str:
    canonical = "\n".join(" ".join(s.split()).casefold() for s in (title, mechanism))
    return hashlib.sha256(canonical.encode()).hexdigest()[:16]


def number(value: object) -> float:
    if isinstance(value, bool):
        raise ValueError("boolean is not a numeric estimate")
    try:
        result = float(value)
    except (TypeError, ValueError):
        raise ValueError("expected a finite number") from None
    if not math.isfinite(result):
        raise ValueError("expected a finite number")
    return result


def normalize(card: dict) -> Proposal:
    required = ("title", "mechanism", "expected_gain", "cost", "evidence", "source")
    if not isinstance(card, dict) or any(key not in card for key in required):
        raise ValueError("proposal is missing required fields")
    if not all(isinstance(card[key], str) for key in ("title", "mechanism", "source")):
        raise ValueError("title, mechanism and source must be strings")
    for name, keys in (
        ("expected_gain", ("metric", "delta", "confidence")),
        ("cost", ("effort_h", "blast_radius")),
    ):
        if not isinstance(card[name], dict) or any(key not in card[name] for key in keys):
            raise ValueError("invalid " + name)
    if not isinstance(card["expected_gain"]["metric"], str):
        raise ValueError("metric must be a string")
    title, mechanism = card["title"].strip(), card["mechanism"].strip()
    if not title or not mechanism:
        raise ValueError("title and mechanism must be nonempty")
    gain, cost = card["expected_gain"], card["cost"]
    delta, confidence = number(gain["delta"]), number(gain["confidence"])
    effort, blast = number(cost["effort_h"]), number(cost["blast_radius"])
    if not 0 <= confidence <= 1 or effort <= 0 or blast < 1:
        raise ValueError("confidence must be [0,1], effort > 0, blast_radius >= 1")
    if not isinstance(card["evidence"], list) or not str(gain["metric"]).strip():
        raise ValueError("evidence must be a list and metric must be nonempty")
    source = str(card["source"])
    if source not in ("telemetry", "memory", "hypothesis"):
        raise ValueError("source must be telemetry, memory or hypothesis")
    return Proposal(
        stable_id(title, mechanism),
        title,
        mechanism,
        {"metric": str(gain["metric"]), "delta": delta, "confidence": confidence},
        {"effort_h": effort, "blast_radius": blast},
        card["evidence"],
        source,
    )


def dedupe(proposals: list[Proposal]) -> list[Proposal]:
    merged = {}
    for proposal in proposals:
        prior = merged.get(proposal.id)
        if prior:
            evidence = prior.evidence + [e for e in proposal.evidence if e not in prior.evidence]
            card = prior.to_dict()
            card["evidence"] = evidence
            proposal = normalize(card)
        merged[proposal.id] = proposal
    return sorted(merged.values(), key=lambda p: p.id)


def candidate(
    title: str,
    mechanism: str,
    metric: str,
    delta: float,
    evidence: list,
    source: str = "telemetry",
    confidence: float = 0.5,
) -> Proposal:
    return normalize(
        dict(
            title=title,
            mechanism=mechanism,
            expected_gain=dict(metric=metric, delta=delta, confidence=confidence),
            cost=dict(effort_h=2, blast_radius=1),
            evidence=evidence,
            source=source,
        )
    )


def jsonl(path: Path):
    if not path.exists():
        return
    with path.open(encoding="utf-8", errors="replace") as stream:
        for line in stream:
            try:
                value = json.loads(line)
                if isinstance(value, dict):
                    yield value
            except ValueError:
                continue


def telemetry(
    repo: Path, ledger: Path, shadow: Path, store: MemoryStore, warnings: list[str]
) -> list[Proposal]:
    counts = defaultdict(int)
    lanes = defaultdict(lambda: [0, 0])
    for row in jsonl(ledger):
        event = row.get("event")
        counts[str(event)] += 1
        if event == "bash_outcome" and row.get("exit_code", 0) != 0:
            counts["bash_failed"] += 1
        if event == "injected":
            for lane, timed_out in (row.get("lane_timeout") or {}).items():
                lanes[lane][0] += int(bool(timed_out))
                lanes[lane][1] += 1
    proposals = []

    def rate(title, mechanism, metric, numerator, denominator):
        if denominator:
            proposals.append(
                candidate(
                    title,
                    mechanism,
                    metric,
                    -numerator / denominator * 0.2,
                    [
                        dict(
                            numerator=numerator,
                            denominator=denominator,
                            rate=numerator / denominator,
                            path=str(ledger),
                        )
                    ],
                )
            )

    rate(
        "Reduce empty recall",
        "Diagnose empty recall lanes and recover relevant candidates without relaxing relevance contracts.",
        "recall_empty_rate",
        counts["recall_empty"],
        counts["recall_empty"] + counts["injected"],
    )
    rate(
        "Reduce bash failures",
        "Use failure-specific recall before repeated shell attempts, preserving successful command behavior.",
        "bash_failure_rate",
        counts["bash_failed"],
        counts["bash_outcome"],
    )
    for lane, (timeouts, total) in sorted(lanes.items()):
        rate(
            "Reduce " + lane + " lane timeouts",
            "Bound work in the " + lane + " recall lane without lowering recall quality.",
            "lane_timeout." + lane,
            timeouts,
            total,
        )
    buckets = defaultdict(lambda: [0, 0])
    for row in jsonl(shadow):
        size = row.get("evidence_bytes", row.get("evidence_size", row.get("c_e")))
        bucket = "unknown" if size is None else ("small" if number(size) < 32768 else "large")
        buckets[bucket][0] += int(row.get("accept") is True)
        buckets[bucket][1] += 1
    for bucket, (accepted, total) in sorted(buckets.items()):
        proposals.append(
            candidate(
                "Improve MDL evidence coverage (" + bucket + ")",
                "Collect representative evidence across independent chunks before MDL consolidation; preserve the acceptance margin.",
                "mdl_accept_rate." + bucket,
                0.2 * (1 - accepted / total),
                [dict(size_bucket=bucket, accepted=accepted, total=total, rate=accepted / total)],
            )
        )
    if not shadow.exists():
        warnings.append("MDL shadow ledger absent: " + str(shadow))
    if not ledger.exists():
        warnings.append("outcome ledger absent: " + str(ledger))
    try:
        health = store.call("health_check")
        if "rpc_over_budget" in health:
            proposals.append(
                candidate(
                    "Reduce RPC budget overruns",
                    "Find blocking RPC work and bound it while preserving response semantics.",
                    "rpc_over_budget",
                    -0.1,
                    [health],
                )
            )
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
        warnings.append("health_check unavailable: " + str(exc))
    saddle = repo / "chitta-mcp/saddle_detector.py"
    if saddle.exists() and ledger.exists():
        try:
            report = json.loads(
                subprocess.run(
                    [sys.executable, str(saddle), "report", "--json", "--ledger", str(ledger)],
                    check=True,
                    capture_output=True,
                    text=True,
                    timeout=store.timeout,
                ).stdout
            )
            proposals.append(
                candidate(
                    "Escape repeated failing command saddles",
                    "Trigger targeted corrective recall when similar commands repeatedly fail within a session.",
                    "saddle_escape_rate",
                    0.1,
                    [report],
                )
            )
        except (OSError, ValueError, subprocess.SubprocessError) as exc:
            warnings.append("saddle report unavailable: " + str(exc))
    return proposals


def gather(
    repo: Path, store: MemoryStore, ledger: Path | None = None, shadow: Path | None = None
) -> tuple[list[Proposal], list[str]]:
    warnings: list[str] = []
    mind = Path.home() / ".claude/mind"
    proposals = telemetry(
        repo,
        ledger or mind / "outcome_ledger.jsonl",
        shadow or mind / "mdl_gate_shadow.jsonl",
        store,
        warnings,
    )
    for tag in ("ceiling", "todo", "incident", "correction"):
        try:
            for record in store.recall(tag, "project:cc-soul"):
                text = record.get("content", record.get("text", ""))
                if not text:
                    continue
                proposals.append(
                    candidate(
                        f"Address {tag}: " + str(text).splitlines()[0][:100],
                        str(text),
                        "mean_nDCG",
                        0.02,
                        [dict(memory_id=record.get("id"), tag=tag)],
                        "memory",
                        0.3,
                    )
                )
        except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as exc:
            warnings.append(f"memory {tag} unavailable: {exc}")
    paths = subprocess.run(
        ["git", "-C", str(repo), "ls-files", "--recurse-submodules", "-z"],
        capture_output=True,
        check=True,
    ).stdout
    for raw in paths.split(b"\0"):
        if not raw:
            continue
        relative = raw.decode("utf-8", "replace")
        path = repo / relative
        if path.is_symlink() or not path.is_file() or path.stat().st_size > 1_000_000:
            continue
        lines = path.read_text(errors="replace").splitlines()
        for n, line in enumerate(lines, 1):
            match = re.match(r"\s*(?://|#)\s*ceiling:\s*(\S.*)", line, re.I)
            if match:
                context = [match[1]]
                for following in lines[n : n + 8]:
                    continuation = re.match(r"\s*(?://|#)\s*(\S.*)", following)
                    if not continuation or continuation[1].lower().startswith("ceiling:"):
                        break
                    context.append(continuation[1])
                proposals.append(
                    candidate(
                        "Ceiling: " + match[1][:100],
                        "\n".join(context),
                        "mean_nDCG",
                        0.02,
                        [dict(path=relative, line=n)],
                        "memory",
                        0.3,
                    )
                )
    for path in sorted((repo / "chitta-mcp/evolve/proposals.d").glob("*.json")):
        try:
            card = json.loads(path.read_text())
            if not card.get("id"):
                raise ValueError("external cards require an id")
            proposals.append(normalize(card))
        except (OSError, ValueError, KeyError, TypeError) as exc:
            warnings.append(f"invalid card {path}: {exc}")
    return dedupe(proposals), warnings


def persist(proposals: list[Proposal], store: MemoryStore) -> dict[str, str]:
    existing = {body(r).get("id"): str(r.get("id", "")) for r in store.recall("proposal")}
    for proposal in proposals:
        if proposal.id not in existing:
            # Recheck by canonical ID; the store may provide a refreshed view.
            found = [
                r
                for r in store.recall("proposal", query=proposal.id)
                if body(r).get("id") == proposal.id
            ]
            existing[proposal.id] = (
                str(found[0].get("id", ""))
                if found
                else store.remember("proposal", proposal.to_dict())
            )
    return existing
