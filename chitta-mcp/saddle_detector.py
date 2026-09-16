#!/usr/bin/env python3
"""Saddle detector over the outcome ledger.

A "saddle" is the observable analogue of a near-correct attempt the agent keeps
returning to (Lai et al. 2026, arXiv:2609.04963, for looped latent reasoners):
a run of failing shell commands whose heads are near-identical, within a short
window of one session. The agent is oscillating around a dead end. That is the
moment a memory (a gotcha, a correction) has the highest expected utility, so
it is the natural trigger for recall instead of injecting on every prompt.

Stdlib only. Read-only unless --notice-file is supplied. Usage:
    python3 saddle_detector.py report [--ledger PATH] [--min-fails 3]
                                      [--window 8] [--similarity 0.8] [--json]
    python3 saddle_detector.py check  --session ID [--ledger PATH] ...
        exit 0 and print the episode if the session's tail is currently
        inside a saddle, exit 1 otherwise (hook-side trigger).
"""

from __future__ import annotations

import json
import os
import re
import sys
import time

DEFAULT_LEDGER = os.path.join(
    os.environ.get("CHITTA_DB_PATH", os.path.expanduser("~/.claude/mind")),
    "outcome_ledger.jsonl",
)

_NUM = re.compile(r"\d+")
_WS = re.compile(r"\s+")


def failed(r: dict) -> bool:
    """Codex PostToolUse payloads carry no exit code: the hook records
    exit_code=null plus a likely_fail text heuristic. Unknown means unknown."""
    code = r.get("exit_code")
    if code is None:
        return bool(r.get("likely_fail"))
    try:
        return int(code) != 0
    except (ValueError, TypeError):
        return False


def normalize(head: str) -> str:
    """Collapse the volatile parts of a command head so retries compare equal."""
    h = _NUM.sub("#", head.strip().lower())
    return _WS.sub(" ", h)[:60]


def similar(a: str, b: str, threshold: float) -> bool:
    if a == b:
        return True
    from difflib import SequenceMatcher

    return SequenceMatcher(None, a, b).ratio() >= threshold


def load(ledger: str | os.PathLike) -> dict[str, list[dict]]:
    from collections import defaultdict

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
        for _j, (i2, r2) in enumerate(bash[k + 1 : k + 1 + window], start=1):
            if i2 in used:
                break
            if not similar(anchor, normalize(r2.get("cmd_head", "")), threshold):
                continue
            end_idx = i2
            if r2.get("exit_code") in (0, "0"):
                escaped = True
                break
            if failed(r2):
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
        "params": {
            "min_fails": args.min_fails,
            "window": args.window,
            "similarity": args.similarity,
        },
        "top_episodes": sorted(all_eps, key=lambda e: -e["n_fails"])[:8],
    }


def load_tail(ledger: str | os.PathLike, session: str, minutes: float) -> list[dict]:
    """Bound hook IO to 1 MiB / 4096 complete lines; discard partial records."""
    now = time.time() * 1000
    with open(ledger, "rb") as handle:
        size = handle.seek(0, 2)
        offset = max(0, size - 1024 * 1024)
        handle.seek(offset)
        data = handle.read()
    lines = data.split(b"\n")
    if offset:
        lines = lines[1:]
    events = []
    session_bytes = session.encode()
    for line in lines[:-1][-4096:]:
        if b"bash_outcome" not in line or session_bytes not in line:
            continue
        try:
            row = json.loads(line)
            if (
                isinstance(row, dict)
                and row.get("session_id") == session
                and row.get("event") == "bash_outcome"
                and isinstance(row.get("ts"), (int, float))
                and now - minutes * 60000 <= row["ts"] <= now
                and isinstance(row.get("cmd_head"), str)
                and row["cmd_head"].strip()
            ):
                events.append(row)
        except (ValueError, TypeError):
            continue
    return sorted(events, key=lambda row: row["ts"])


def open_saddle(events: list[dict], args) -> dict | None:
    # Build episodes anchored on the first failure, so another retry does not
    # change the saddle ID. Other command shapes can be interleaved.
    groups = []
    for row in events:
        shape = normalize(row["cmd_head"])
        matches = [g for g in groups if similar(g["shape"], shape, args.similarity)]
        if failed(row):
            if matches:
                matches[0]["fails"].append(row)
            else:
                groups.append({"shape": shape, "fails": [row]})
        elif row.get("exit_code") in (0, "0"):
            groups = [g for g in groups if g not in matches]
    candidates = [
        g
        for g in groups
        if len(g["fails"]) >= args.min_fails
        and (args.cmd is None or similar(g["shape"], normalize(args.cmd), args.similarity))
    ]
    if not candidates:
        return None
    group = max(candidates, key=lambda g: g["fails"][-1]["ts"])
    first, last = group["fails"][0], group["fails"][-1]
    identity = f"{args.session}:{first['ts']}:{group['shape']}"
    excerpt = " ".join(str(last.get("stderr_head") or "error unavailable").split())[:160]
    n = len(group["fails"])
    message = (
        f"[saddle] this command shape failed {n}× in {args.minutes:g} min "
        f"(last: {excerpt}). Change approach or read the error before retrying."
    )
    return {
        "saddle_id": identity,
        "start_ts": first["ts"],
        "end_ts": last["ts"],
        "n_fails": n,
        "escaped": False,
        "cmd_head": first["cmd_head"][:80],
        "stderr_head": excerpt,
        "window_minutes": args.minutes,
        "message": message,
    }


def check(events: list[dict], args) -> int:
    episode = open_saddle(events, args)
    if episode is None:
        return 1
    if args.notice_file:
        # Serialize concurrent retries without waiting on another hook process.
        import fcntl

        with open(args.notice_file, "a+") as handle:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
            handle.seek(0)
            if episode["saddle_id"] in handle.read().splitlines():
                return 1
            handle.write(episode["saddle_id"] + "\n")
        print(
            json.dumps(
                {
                    "hookSpecificOutput": {
                        "hookEventName": "PreToolUse",
                        "additionalContext": episode["message"],
                    }
                }
            )
        )
    else:
        print(json.dumps(episode))
    return 0


def main(argv: list[str] | None = None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    # argparse/pathlib pull in much of the stdlib on PyPy. Keep the hook's
    # small fixed option set independent of the offline report parser.
    if argv and argv[0] == "check" and "--help" not in argv:
        from types import SimpleNamespace

        values = dict(
            session="",
            cmd=None,
            ledger=DEFAULT_LEDGER,
            min_fails=3,
            minutes=7.0,
            similarity=0.8,
            notice_file=None,
        )
        tokens = iter(argv[1:])
        try:
            for token in tokens:
                if token == "--json":
                    continue
                key, sep, value = token.partition("=")
                key = key.removeprefix("--").replace("-", "_")
                if not token.startswith("--") or key not in values:
                    return 2
                values[key] = value if sep else next(tokens)
            values["minutes"] = float(values["minutes"])
            values["similarity"] = float(values["similarity"])
            values["min_fails"] = int(values["min_fails"])
            args = SimpleNamespace(**values)
            if not args.session or not 0 < args.minutes < float("inf") or args.min_fails < 1:
                return 2
            return check(load_tail(args.ledger, args.session, args.minutes), args)
        except (OSError, ValueError, TypeError, StopIteration):
            return 2
    import argparse
    from pathlib import Path

    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("mode", choices=["report", "check"])
    ap.add_argument("--ledger", type=Path, default=DEFAULT_LEDGER)
    ap.add_argument("--min-fails", type=int, default=3)
    ap.add_argument("--window", type=int, default=8, help="bash events after the anchor to scan")
    ap.add_argument("--similarity", type=float, default=0.8)
    ap.add_argument("--session", default="")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--cmd", default=None, help="check: proposed command shape")
    ap.add_argument("--minutes", type=float, default=7, help="check: recent time window")
    ap.add_argument("--notice-file", type=Path, help="check: dedupe and emit hook JSON")
    args = ap.parse_args(argv)
    if not args.ledger.exists():
        print(f"no ledger at {args.ledger}", file=sys.stderr)
        return 2
    if args.mode == "check":
        if not args.session or args.minutes <= 0 or args.min_fails < 1:
            return 2
        try:
            return check(load_tail(args.ledger, args.session, args.minutes), args)
        except (OSError, ValueError, TypeError):
            return 2
    by_session = load(args.ledger)
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
        print(
            f"  {e['n_fails']:2d} fails  escaped={str(e['escaped']):5s} inj={e['injected_during']}  {e['session_id'][:8]}  {e['cmd_head']}"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
