#!/usr/bin/env python3
"""Saddle detector over the outcome ledger.

A "saddle" is the observable analogue of a near-correct attempt the agent keeps
returning to (Lai et al. 2026, arXiv:2609.04963, for looped latent reasoners):
a run of failing shell commands whose heads are near-identical, within a short
window of one session. The agent is oscillating around a dead end. That is the
moment a memory (a gotcha, a correction) has the highest expected utility, so
it is the natural trigger for recall instead of injecting on every prompt.

Read-only. Stdlib only. Usage:
    python3 saddle_detector.py report [--ledger PATH] [--min-fails 3]
                                      [--window 8] [--similarity 0.8] [--json]
    python3 saddle_detector.py check  --session ID [--ledger PATH] ...
        exit 0 and print the episode if the session's tail is currently
        inside a saddle, exit 1 otherwise (hook-side trigger).
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from collections import defaultdict
from difflib import SequenceMatcher
from pathlib import Path

DEFAULT_LEDGER = Path(os.environ.get("CHITTA_DB_PATH", os.path.expanduser("~/.claude/mind"))) / "outcome_ledger.jsonl"

_NUM = re.compile(r"\d+")
_WS = re.compile(r"\s+")


def failed(r: dict) -> bool:
    """Codex PostToolUse payloads carry no exit code: the hook records
    exit_code=null plus a likely_fail text heuristic. Unknown means unknown."""
    code = r.get("exit_code", 0)
    if code is None:
        return bool(r.get("likely_fail"))
    return int(code) != 0


def normalize(head: str) -> str:
    """Collapse the volatile parts of a command head so retries compare equal."""
    h = _NUM.sub("#", head.strip().lower())
    return _WS.sub(" ", h)[:60]


def similar(a: str, b: str, threshold: float) -> bool:
    if a == b:
        return True
    return SequenceMatcher(None, a, b).ratio() >= threshold


def load(ledger: Path) -> dict[str, list[dict]]:
    by_session: dict[str, list[dict]] = defaultdict(list)
    with open(ledger, encoding="utf-8", errors="replace") as f:
        for line in f:
            try:
                r = json.loads(line)
            except ValueError:
                continue
            if "ts" in r and "session_id" in r:
                by_session[r["session_id"]].append(r)
    for evs in by_session.values():
        evs.sort(key=lambda r: r["ts"])
    return by_session


def find_episodes(events: list[dict], min_fails: int, window: int, threshold: float) -> list[dict]:
    """Scan one session's events; return saddle episodes.

    An episode opens on a failing command and grows while subsequent commands
    (within `window` bash events) have a similar head. It qualifies once it holds
    `min_fails` failures. It closes on the first similar command that succeeds
    (escaped=True), or when the window runs out / session ends (escaped=False).
    """
    bash = [(i, r) for i, r in enumerate(events) if r.get("event") == "bash_outcome"]
    episodes: list[dict] = []
    used: set[int] = set()
    for k, (i, r) in enumerate(bash):
        if i in used or not failed(r):
            continue
        anchor = normalize(r.get("cmd_head", ""))
        fails = [i]
        escaped = False
        end_idx = i
        for j, (i2, r2) in enumerate(bash[k + 1 : k + 1 + window], start=1):
            if i2 in used:
                break
            if not similar(anchor, normalize(r2.get("cmd_head", "")), threshold):
                continue
            end_idx = i2
            if not failed(r2):
                escaped = True
                break
            fails.append(i2)
        if len(fails) < min_fails:
            continue
        used.update(fails)
        injected_during = [
            e for e in events[fails[0] : end_idx + 1] if e.get("event") == "injected"
        ]
        episodes.append(
            {
                "start_ts": events[fails[0]]["ts"],
                "end_ts": events[end_idx]["ts"],
                "n_fails": len(fails),
                "escaped": escaped,
                "cmd_head": events[fails[0]].get("cmd_head", "")[:80],
                "injected_during": len(injected_during),
                "injected_ids": sorted({i for e in injected_during for i in e.get("ids", [])}),
            }
        )
    return episodes


def report(by_session: dict[str, list[dict]], args) -> dict:
    total_bash = total_fail = 0
    fails_in_saddle = 0
    sessions_with = 0
    all_eps: list[dict] = []
    for sid, evs in by_session.items():
        eps = find_episodes(evs, args.min_fails, args.window, args.similarity)
        n_bash = sum(1 for r in evs if r.get("event") == "bash_outcome")
        n_fail = sum(1 for r in evs if r.get("event") == "bash_outcome" and failed(r))
        total_bash += n_bash
        total_fail += n_fail
        if eps:
            sessions_with += 1
            fails_in_saddle += sum(e["n_fails"] for e in eps)
            for e in eps:
                e["session_id"] = sid
            all_eps.extend(eps)
    escaped = sum(1 for e in all_eps if e["escaped"])
    with_inj = sum(1 for e in all_eps if e["injected_during"])
    esc_with_inj = sum(1 for e in all_eps if e["injected_during"] and e["escaped"])
    esc_without = sum(1 for e in all_eps if not e["injected_during"] and e["escaped"])
    lengths = sorted(e["n_fails"] for e in all_eps)
    return {
        "sessions": len(by_session),
        "sessions_with_saddle": sessions_with,
        "bash_events": total_bash,
        "failing_events": total_fail,
        "episodes": len(all_eps),
        "fails_inside_episodes": fails_in_saddle,
        "fail_share_in_saddles": round(fails_in_saddle / total_fail, 3) if total_fail else None,
        "median_episode_fails": lengths[len(lengths) // 2] if lengths else None,
        "max_episode_fails": lengths[-1] if lengths else None,
        "escaped": escaped,
        "escape_rate": round(escaped / len(all_eps), 3) if all_eps else None,
        "episodes_with_injection_during": with_inj,
        "escape_rate_with_injection": round(esc_with_inj / with_inj, 3) if with_inj else None,
        "escape_rate_without_injection": round(esc_without / (len(all_eps) - with_inj), 3)
        if len(all_eps) - with_inj
        else None,
        "params": {"min_fails": args.min_fails, "window": args.window, "similarity": args.similarity},
        "top_episodes": sorted(all_eps, key=lambda e: -e["n_fails"])[:8],
    }


def check(by_session: dict[str, list[dict]], args) -> int:
    evs = by_session.get(args.session, [])
    eps = find_episodes(evs, args.min_fails, args.window, args.similarity)
    if not eps:
        return 1
    last = eps[-1]
    # The session is "in" the saddle only if the episode is still open (not
    # escaped) and its last failure is the session's most recent bash event.
    tail = [r for r in evs if r.get("event") == "bash_outcome"]
    if last["escaped"] or not tail or tail[-1]["ts"] > last["end_ts"]:
        return 1
    print(json.dumps(last))
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["report", "check"])
    ap.add_argument("--ledger", type=Path, default=DEFAULT_LEDGER)
    ap.add_argument("--min-fails", type=int, default=3)
    ap.add_argument("--window", type=int, default=8, help="bash events after the anchor to scan")
    ap.add_argument("--similarity", type=float, default=0.8)
    ap.add_argument("--session", default="")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args(argv)
    if not args.ledger.exists():
        print(f"no ledger at {args.ledger}", file=sys.stderr)
        return 2
    by_session = load(args.ledger)
    if args.mode == "check":
        return check(by_session, args)
    out = report(by_session, args)
    if args.json:
        print(json.dumps(out, indent=1))
        return 0
    for k, v in out.items():
        if k == "top_episodes":
            continue
        print(f"{k:34s} {v}")
    print("top episodes:")
    for e in out["top_episodes"]:
        print(f"  {e['n_fails']:2d} fails  escaped={str(e['escaped']):5s} inj={e['injected_during']}  {e['session_id'][:8]}  {e['cmd_head']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
