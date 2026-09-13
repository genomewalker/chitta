#!/usr/bin/env python3
"""
Outcome ledger (SMRITI Phase 1): joins `injected` memory-id events to the
`bash_outcome` events that follow them within a time window, and reports
per-memory success/failure credit (Wilson lower bound). Pure stdlib.
Read-only join + report — does not touch recall scoring.

CLI:
  python3 outcome_ledger.py report [--ledger PATH] [--window-s 600]
  python3 outcome_ledger.py credit [--ledger PATH] [--window-s 600] [--apply]
"""

from __future__ import annotations

import argparse
import json
import math
import os
import subprocess
from collections import defaultdict
from pathlib import Path

DEFAULT_LEDGER = (
    Path(os.environ.get("CHITTA_DB_PATH", str(Path.home() / ".claude" / "mind")))
    / "outcome_ledger.jsonl"
)
CHITTA_BIN = os.environ.get("CHITTA_BIN", str(Path.home() / ".claude" / "bin" / "chitta"))


def load_events(ledger_path: Path) -> list:
    events = []
    if not ledger_path.exists():
        return events
    with ledger_path.open() as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                events.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    return events


def wilson_lower_bound(successes: int, n: int, z: float = 1.96) -> float:
    if n == 0:
        return 0.0
    phat = successes / n
    denom = 1 + z * z / n
    center = phat + z * z / (2 * n)
    margin = z * math.sqrt((phat * (1 - phat) + z * z / (4 * n)) / n)
    return max(0.0, (center - margin) / denom)


def compute_credit(events: list, window_s: int) -> dict:
    """Per-session join: for each `injected` event, look at `bash_outcome`
    events in the same session with ts in [ts0, ts0 + window_s*1000]. All exit
    codes 0 -> +1 success per injected id; any nonzero -> +1 failure. Zero
    bash_outcome events in the window -> no success/failure credit (the
    injection is still counted)."""
    by_session = defaultdict(list)
    for e in events:
        by_session[e.get("session_id", "unknown")].append(e)

    stats = defaultdict(lambda: {"injections": 0, "successes": 0, "failures": 0})

    for sess_events in by_session.values():
        sess_events.sort(key=lambda e: e.get("ts", 0))
        outcomes = [e for e in sess_events if e.get("event") == "bash_outcome"]
        for e in sess_events:
            if e.get("event") != "injected":
                continue
            ts0 = e.get("ts", 0)
            ids = e.get("ids") or []
            for mid in ids:
                stats[mid]["injections"] += 1
            window = [o for o in outcomes if ts0 <= o.get("ts", 0) <= ts0 + window_s * 1000]
            if not window:
                continue
            # Codex payloads carry no exit code (recorded as null) — only a
            # likely_fail heuristic. Unknown outcomes never count as successes;
            # a window with nothing known yields no verdict.
            known = [o for o in window if o.get("exit_code") is not None]
            flagged = any(o.get("likely_fail") for o in window)
            if not known and not flagged:
                continue
            all_zero = all(o.get("exit_code") == 0 for o in known) and not flagged
            for mid in ids:
                stats[mid]["successes" if all_zero else "failures"] += 1
    return stats


def report(ledger_path: Path, window_s: int) -> str:
    stats = compute_credit(load_events(ledger_path), window_s)
    rows = []
    for mid, s in stats.items():
        n = s["successes"] + s["failures"]
        rows.append(
            (
                mid,
                s["injections"],
                s["successes"],
                s["failures"],
                wilson_lower_bound(s["successes"], n),
            )
        )
    rows.sort(key=lambda r: r[1], reverse=True)

    lines = [f"{'id':<24}{'injections':>12}{'successes':>12}{'failures':>12}{'wilson_lb':>12}"]
    for mid, inj, suc, fail, wlb in rows:
        lines.append(f"{mid:<24}{inj:>12}{suc:>12}{fail:>12}{wlb:>12.3f}")
    lines.append("")
    lines.append(
        f"totals: memories={len(rows)} injections={sum(r[1] for r in rows)} "
        f"successes={sum(r[2] for r in rows)} failures={sum(r[3] for r in rows)}"
    )
    return "\n".join(lines)


def call_ack_nack(tool: str, mid: str) -> tuple:
    """Invoke ack_memory/nack_memory via `chitta mcp`'s JSON-RPC bridge. The
    plain `chitta ack_memory --id X` CLI form does NOT work — ack_memory and
    nack_memory are daemon tools not listed in the client's TOOL_SPECS, so the
    CLI rejects them as an unknown option (verified). `chitta mcp` forwards a
    tools/call request to the daemon and returns the real result synchronously."""
    req = json.dumps(
        {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {"name": tool, "arguments": {"id": mid}},
        }
    )
    try:
        proc = subprocess.run(
            [CHITTA_BIN, "mcp"], input=req, capture_output=True, text=True, timeout=5
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        return False, str(exc)
    if proc.returncode != 0:
        return False, proc.stderr.strip() or f"exit {proc.returncode}"
    try:
        resp = json.loads(proc.stdout.strip().splitlines()[-1])
    except (json.JSONDecodeError, IndexError):
        return False, proc.stdout.strip()
    if "error" in resp:
        return False, resp["error"].get("message", str(resp["error"]))
    # ack_memory/nack_memory report "not found" as ordinary content text, not a
    # JSON-RPC error (verified) — inspect it so credit --apply doesn't claim
    # success for a memory id that no longer exists.
    content = resp.get("result", {}).get("content", [])
    text = content[0].get("text", "") if content else ""
    if "not found" in text.lower():
        return False, text
    return True, ""


def credit(
    ledger_path: Path,
    window_s: int,
    apply: bool,
    min_observations: int = 3,
    ack_threshold: float = 0.6,
    nack_threshold: float = 0.3,
) -> str:
    stats = compute_credit(load_events(ledger_path), window_s)
    lines = []
    for mid, s in sorted(stats.items(), key=lambda kv: -kv[1]["injections"]):
        n = s["successes"] + s["failures"]
        if n < min_observations:
            continue
        wlb = wilson_lower_bound(s["successes"], n)
        if wlb > ack_threshold:
            action = "ack_memory"
        elif wlb < nack_threshold:
            action = "nack_memory"
        else:
            continue
        if apply:
            ok, err = call_ack_nack(action, mid)
            status = "ok" if ok else f"FAILED: {err}"
            lines.append(f"{action} --id {mid}  (n={n} wlb={wlb:.3f})  [{status}]")
        else:
            lines.append(f"[dry-run] {action} --id {mid}  (n={n} wlb={wlb:.3f})")
    if not lines:
        lines.append(
            f"no memories crossed the credit threshold (min_observations={min_observations})"
        )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = parser.add_subparsers(dest="command", required=True)

    p_report = sub.add_parser("report", help="print per-memory injection/outcome table")
    p_report.add_argument("--ledger", type=Path, default=DEFAULT_LEDGER)
    p_report.add_argument("--window-s", type=int, default=600)

    p_credit = sub.add_parser("credit", help="ack/nack memories crossing the credit threshold")
    p_credit.add_argument("--ledger", type=Path, default=DEFAULT_LEDGER)
    p_credit.add_argument("--window-s", type=int, default=600)
    p_credit.add_argument(
        "--apply",
        action="store_true",
        help="actually call ack_memory/nack_memory (default: dry-run, prints intended calls)",
    )

    args = parser.parse_args()

    if args.command == "report":
        print(report(args.ledger, args.window_s))
    elif args.command == "credit":
        print(credit(args.ledger, args.window_s, args.apply))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
