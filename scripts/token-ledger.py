#!/usr/bin/env python3
"""Token ledger: what Fable (Claude Code) and Astra (Codex) sessions cost, from
their own transcripts. No API calls; reads ~/.claude/projects/*/*.jsonl and
~/.codex/sessions/**/*.jsonl.

  scripts/token-ledger.py                # last 7 days, both agents, text table
  scripts/token-ledger.py --days 1 --json
  scripts/token-ledger.py --top 10       # the sessions that cost most

The number that matters is turns x context: every assistant turn re-sends the
whole thread (cached, but billed). Tool-output bytes and turn counts are the
levers; output tokens are the other line. Prices are per million tokens and
come from CHITTA_PRICE_INPUT, CHITTA_PRICE_CACHED, CHITTA_PRICE_OUTPUT
(defaults 15, 1.5, 75, the Claude Code Fable list prices as of 2026-09) and
CHITTA_CODEX_PRICE_INPUT/_CACHED/_OUTPUT (defaults 2, 0.5, 8); adjust to the
contract in force, the ratios are what the report is for.
"""

from __future__ import annotations

import argparse
import fcntl
import json
import os
import tempfile
import time
from collections import defaultdict
from pathlib import Path

HOME = Path.home()


def price(name, default):
    try:
        return float(os.environ.get(name, default))
    except ValueError:
        return default


CLAUDE = (
    price("CHITTA_PRICE_INPUT", 15),
    price("CHITTA_PRICE_CACHED", 1.5),
    price("CHITTA_PRICE_OUTPUT", 75),
)
CODEX = (
    price("CHITTA_CODEX_PRICE_INPUT", 2),
    price("CHITTA_CODEX_PRICE_CACHED", 0.5),
    price("CHITTA_CODEX_PRICE_OUTPUT", 8),
)


def empty():
    return {
        "turns": 0,
        "input": 0,
        "cached": 0,
        "cache_write": 0,
        "output": 0,
        "reasoning": 0,
        "tool_outputs": 0,
        "tool_chars": 0,
        "tool_over_6k": 0,
    }


def claude_session(path):
    s = empty()
    for line in path.open(errors="replace"):
        try:
            d = json.loads(line)
        except ValueError:
            continue
        m = d.get("message")
        if not isinstance(m, dict):
            continue
        u = m.get("usage")
        if isinstance(u, dict):
            s["turns"] += 1
            s["input"] += u.get("input_tokens", 0)
            s["cached"] += u.get("cache_read_input_tokens", 0)
            s["cache_write"] += u.get("cache_creation_input_tokens", 0)
            s["output"] += u.get("output_tokens", 0)
        c = m.get("content")
        if isinstance(c, list):
            for part in c:
                if isinstance(part, dict) and part.get("type") == "tool_result":
                    body = part.get("content")
                    text = body if isinstance(body, str) else json.dumps(body)
                    s["tool_outputs"] += 1
                    s["tool_chars"] += len(text)
                    s["tool_over_6k"] += len(text) > 6000
    return s


def codex_session(path):
    s = empty()
    last = None
    for line in path.open(errors="replace"):
        try:
            d = json.loads(line)
        except ValueError:
            continue
        p = d.get("payload") if isinstance(d.get("payload"), dict) else {}
        kind = p.get("type")
        if kind in ("function_call_output", "custom_tool_call_output"):
            out = p.get("output")
            text = out if isinstance(out, str) else json.dumps(out)
            s["tool_outputs"] += 1
            s["tool_chars"] += len(text)
            s["tool_over_6k"] += len(text) > 6000
        elif kind == "function_call" or kind == "custom_tool_call" or kind == "message":
            if p.get("role") != "user":
                s["turns"] += kind != "message" or 1
        info = p.get("info") if isinstance(p.get("info"), dict) else None
        if info and isinstance(info.get("total_token_usage"), dict):
            last = info["total_token_usage"]
    if last:
        s["input"] = last.get("input_tokens", 0) - last.get("cached_input_tokens", 0)
        s["cached"] = last.get("cached_input_tokens", 0)
        s["output"] = last.get("output_tokens", 0)
        s["reasoning"] = last.get("reasoning_output_tokens", 0)
    return s


def cost(s, prices):
    i, c, o = prices
    return (s["input"] * i + s["cached"] * c + s["output"] * o) / 1e6


def add(total, s):
    for k, v in s.items():
        total[k] += v


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("--days", type=float, default=7)
    ap.add_argument("--top", type=int, default=5)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--cache-daily", type=Path, help="Atomically refresh this cache at most daily")
    args = ap.parse_args()
    lock = None
    if args.cache_daily:
        args.cache_daily.parent.mkdir(parents=True, exist_ok=True)
        lock = args.cache_daily.with_suffix(".lock").open("a")
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return 0
        if args.cache_daily.exists() and time.time() - args.cache_daily.stat().st_mtime < 86400:
            return 0
    since = time.time() - args.days * 86400
    report = {"since_days": args.days, "agents": {}}
    for agent, root, pattern, reader, prices in (
        ("fable", HOME / ".claude/projects", "*/*.jsonl", claude_session, CLAUDE),
        ("astra", HOME / ".codex/sessions", "**/*.jsonl", codex_session, CODEX),
    ):
        sessions = []
        total = defaultdict(int)
        for path in root.glob(pattern):
            try:
                if path.stat().st_mtime < since:
                    continue
                s = reader(path)
            except OSError:
                continue
            if s["turns"] == 0 and s["output"] == 0:
                continue
            s["session"] = path.stem[:24]
            s["cost"] = round(cost(s, prices), 2)
            sessions.append(s)
            add(total, {k: v for k, v in s.items() if isinstance(v, int)})
        sessions.sort(key=lambda s: -s["cost"])
        report["agents"][agent] = {
            "sessions": len(sessions),
            "total": dict(total),
            "total_cost": round(sum(s["cost"] for s in sessions), 2),
            "avg_context_per_turn": int((total["cached"] + total["input"]) / total["turns"])
            if total["turns"]
            else 0,
            "top": sessions[: args.top],
        }
    if args.cache_daily:
        fd, temporary = tempfile.mkstemp(dir=args.cache_daily.parent)
        try:
            with os.fdopen(fd, "w") as stream:
                json.dump(report, stream)
            os.replace(temporary, args.cache_daily)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)
    if args.json:
        print(json.dumps(report, indent=2))
        return 0
    for agent, r in report["agents"].items():
        t = r["total"]
        print(
            f"\n== {agent}: {r['sessions']} sessions in {args.days:g} days, cost ≈ ${r['total_cost']:,.0f} "
            f"(prices per MTok: {CLAUDE if agent == 'fable' else CODEX})"
        )
        print(
            f"   turns={t['turns']:,} context/turn={r['avg_context_per_turn']:,} cached={t['cached'] / 1e6:,.0f}M "
            f"input={t['input'] / 1e6:,.1f}M output={t['output'] / 1e6:,.2f}M tool-outputs={t['tool_outputs']:,} "
            f"({t['tool_chars'] / 1e6:,.1f}M chars, {t['tool_over_6k']} over 6k)"
        )
        for s in r["top"]:
            print(
                f"   ${s['cost']:>8,.0f}  turns={s['turns']:>5}  cached={s['cached'] / 1e6:>7,.0f}M  out={s['output'] / 1e3:>6,.0f}k  "
                f"tool-chars={s['tool_chars'] / 1e3:>6,.0f}k  {s['session']}"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
