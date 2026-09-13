#!/usr/bin/env python3
"""SMRITI-Bench scorer: reads a results JSONL and computes headline metrics.

Per condition: success rate with a Wilson 95% interval, mean tokens.
Headline: ΔSR (on - off), ΔT (on - off tokens), MUI (ledger-grounded
memory-utility credit — see mui_credit), echo_rate (the old surface-echo
MUI proxy, kept for reference), per-lane ablation deltas when
ablate:<lane> conditions are present, and ablate_all_vs_off_sr_gap as a
sanity check when an ablate:all condition is present.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from collections import defaultdict

import split as task_split


def wilson_interval(successes: int, n: int, z: float = 1.96):
    if n == 0:
        return (0.0, 0.0, 0.0)
    p = successes / n
    denom = 1 + z * z / n
    center = (p + z * z / (2 * n)) / denom
    half = (z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n))) / denom
    return (p, max(0.0, center - half), min(1.0, center + half))


def load_records(path):
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]


def memory_was_used(record) -> bool:
    """Old surface-echo proxy for "the agent drew on a planted memory": does
    a prefix of the memory's content appear verbatim in the transcript?
    Reported as `echo_rate` — kept for reference, not the headline MUI.
    Reads 0.0 across results/a1aa8c0f.jsonl's whole 27-win on-condition:
    planted text is never literally echoed back by a real agent (README
    "MUI: what 'used' means and why it's weak"). See mui_credit for the
    ledger-grounded replacement."""
    transcript = record.get("transcript", "").lower()
    for content in record.get("planted_memory_contents", []):
        needle = content.lower()[:40]
        if needle and needle in transcript:
            return True
    return False


def mui_credit(on_record, paired_off_records):
    """Ledger-grounded MUI credit for one memory-on run: does this run give
    demonstrable, outcome-level evidence that the planted memory changed
    something, rather than evidence it was merely quoted?

    Credit requires all of:
      1. injected_confirmed — the memory actually reached the child session
         (not just planted and never delivered — README "Unconfirmed
         injection").
      2. the run passed.
      3. at least one paired memory-off trial for the same task (same
         trial number when present, else every off trial recorded for
         that task_id — see score()) either FAILED (sr_lift: memory
         changed the outcome) or used >=1.5x this run's tokens (cost:
         memory changed the cost, even though off still passed the same
         way exploration-solvable tasks did in the first pilot).

    Returns (sr_lift: bool, cost: bool) rather than a single bool so
    score() can report the two components separately — sr_lift and cost
    are mutually exclusive here (cost is only checked when no paired off
    trial failed), so sr_lift_credit + cost_credit == mui exactly."""
    if not on_record.get("injected_confirmed") or not on_record["passed"]:
        return False, False
    if not paired_off_records:
        return False, False
    if any(not o["passed"] for o in paired_off_records):
        return True, False
    on_tokens = on_record["tokens_used"]
    cost_credit = any(o["tokens_used"] >= 1.5 * on_tokens for o in paired_off_records)
    return False, cost_credit


def score(records, include_splits=True):
    by_condition = defaultdict(list)
    for r in records:
        by_condition[r["condition"]].append(r)

    per_condition = {}
    for condition, rs in sorted(by_condition.items()):
        n = len(rs)
        successes = sum(1 for r in rs if r["passed"])
        p, lo, hi = wilson_interval(successes, n)
        tokens = [r["tokens_used"] for r in rs]
        # injected_confirmed is None for records where nothing was planted (off);
        # the confirmation rate is only over records where it's True/False.
        confirmable = [r for r in rs if r.get("injected_confirmed") is not None]
        confirmed = sum(1 for r in confirmable if r["injected_confirmed"])
        per_condition[condition] = {
            "n": n,
            "success_rate": p,
            "wilson_95ci": [lo, hi],
            # mean_tokens is uncached input+output (README "Token accounting" /
            # ClaudeCodeAdapter docstring) -- delta_tokens below is computed from it.
            "mean_tokens": sum(tokens) / n if n else 0.0,
            "mean_input_tokens": sum(r.get("input_tokens", 0) for r in rs) / n if n else 0.0,
            "mean_output_tokens": sum(r.get("output_tokens", 0) for r in rs) / n if n else 0.0,
            "mean_cache_read_tokens": sum(r.get("cache_read_tokens", 0) for r in rs) / n
            if n
            else 0.0,
            # Fraction of this condition's records where an outcome-ledger
            # 'injected' event during the agent's call actually named one of
            # the planted ids (runner.read_injected_ids_in_window). None when
            # nothing was planted in this condition (e.g. off). A record with
            # injected_confirmed=False is NOT a valid memory-on sample -- the
            # memory never reached the child session -- see README "Threats
            # to validity" > "Unconfirmed injection".
            "injection_confirmation_rate": (confirmed / len(confirmable)) if confirmable else None,
        }

    headline = {}
    if "off" in per_condition and "on" in per_condition:
        headline["delta_sr"] = (
            per_condition["on"]["success_rate"] - per_condition["off"]["success_rate"]
        )
        headline["delta_tokens"] = (
            per_condition["on"]["mean_tokens"] - per_condition["off"]["mean_tokens"]
        )

        # MUI v2 (ledger-grounded, replaces the echo proxy as the headline
        # number — see mui_credit). Pair each on-run with the off-run(s)
        # for the same task: prefer the exact same trial number (that's
        # how run_all interleaves conditions, so it's a true 1:1 pair for
        # a normal run); fall back to every off-run recorded for that
        # task_id when the exact trial is missing (a --resume'd or
        # partially-skipped run can leave that gap).
        off_by_task_trial = defaultdict(list)
        off_by_task = defaultdict(list)
        for r in by_condition.get("off", []):
            off_by_task_trial[(r["task_id"], r["trial"])].append(r)
            off_by_task[r["task_id"]].append(r)

        on_records = by_condition["on"]
        sr_lift_n = 0
        cost_n = 0
        for r in on_records:
            paired = off_by_task_trial.get((r["task_id"], r["trial"])) or off_by_task.get(
                r["task_id"], []
            )
            sr_lift, cost = mui_credit(r, paired)
            sr_lift_n += sr_lift
            cost_n += cost
        headline["mui"] = (sr_lift_n + cost_n) / len(on_records) if on_records else None
        headline["sr_lift_credit"] = sr_lift_n / len(on_records) if on_records else None
        headline["cost_credit"] = cost_n / len(on_records) if on_records else None
        headline["mui_n"] = len(on_records)

        # Old surface-echo proxy, kept for reference (README "MUI: what
        # 'used' means and why it's weak") — not the headline number.
        on_wins = [r for r in on_records if r["passed"]]
        used = sum(1 for r in on_wins if memory_was_used(r))
        headline["echo_rate"] = used / len(on_wins) if on_wins else None
        headline["echo_rate_n_wins"] = len(on_wins)

        # Surface this at headline level too: a low rate here calls delta_sr
        # itself into question, not just the per-condition detail.
        headline["on_injection_confirmation_rate"] = per_condition["on"][
            "injection_confirmation_rate"
        ]

    for label in by_condition:
        if label.startswith("ablate:") and "on" in per_condition:
            lane = label.split(":", 1)[1]
            headline.setdefault("ablation_delta_sr", {})[lane] = (
                per_condition["on"]["success_rate"] - per_condition[label]["success_rate"]
            )

    # Sanity check on the ablation wiring itself (runner.ABLATE_ALL_LANES):
    # disabling every lane should behave like memory=off. A gap far from 0
    # here means CHITTA_ABLATE_LANES isn't actually reaching/working in
    # every hook, not that ablation "found" a memory effect.
    if "ablate:all" in per_condition and "off" in per_condition:
        headline["ablate_all_vs_off_sr_gap"] = (
            per_condition["ablate:all"]["success_rate"] - per_condition["off"]["success_rate"]
        )

    report = {"per_condition": per_condition, "headline": headline}
    if include_splits:
        assignments = task_split.load()
        grouped = defaultdict(list)
        for record in records:
            grouped[record.get("split", assignments.get(record["task_id"], "unknown"))].append(
                record
            )
        report["per_split"] = {
            name: score(grouped[name], include_splits=False)
            for name in sorted({"visible", "holdout"} | set(grouped))
        }
        report["split_assignment_source"] = (
            "recorded"
            if all("split" in r for r in records)
            else "current manifest fallback for legacy rows"
        )
        report["split_hashes"] = sorted({r["split_hash"] for r in records if "split_hash" in r})
    return report


def main():
    parser = argparse.ArgumentParser(description="SMRITI-Bench scorer")
    parser.add_argument("results", help="path to results/<run_id>.jsonl")
    args = parser.parse_args()

    records = load_records(args.results)
    if not records:
        print("no records", file=sys.stderr)
        sys.exit(1)

    print(json.dumps(score(records), indent=2))


if __name__ == "__main__":
    main()
