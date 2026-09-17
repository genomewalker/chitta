#!/usr/bin/env python3
"""Audit transcript requests, event-time windows and modeled token costs.

Reads transcripts only. Prices are configurable list estimates, not invoices.
Claude cache writes default to 1.25 times input (5m) and twice input (1h).
Codex output already includes reasoning; reasoning is never charged twice.
"""

from __future__ import annotations

import argparse
import json
import os
import time
from collections import defaultdict
from datetime import datetime
from pathlib import Path

HOME = Path.home()
COUNTERS = ("input", "cached", "cache_write", "cache_write_1h", "output", "reasoning")


def price(name, default):
    return float(os.environ.get(name, default))


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


def timestamp(value):
    try:
        return (
            float(value)
            if isinstance(value, (int, float))
            else datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()
        )
    except (ValueError, TypeError, AttributeError):
        return None


def records(paths, provider):
    """Deduplicate across files too, including copied/forked transcripts.

    Codex transcripts often omit provider IDs. In that case the session ID and
    cumulative usage vector identify a usage event; repeated snapshots are not
    requests. This fallback is explicitly reported, never inferred from tools.
    """
    requests, outputs = {}, []
    diagnostics = defaultdict(int)
    for path in paths:
        model, session, previous, seen_totals = "unknown", path.stem, {}, set()
        try:
            source = path.open(errors="replace")
        except OSError:
            diagnostics["unreadable_files"] += 1
            continue
        with source:
            for index, line in enumerate(source):
                try:
                    event = json.loads(line)
                except ValueError:
                    diagnostics["malformed_events"] += 1
                    continue
                if not isinstance(event, dict):
                    continue
                ts = timestamp(event.get("timestamp"))
                p, m = event.get("payload"), event.get("message")
                p = p if isinstance(p, dict) else {}
                m = m if isinstance(m, dict) else {}
                if event.get("type") == "session_meta":
                    session = p.get("id", session)
                session = event.get("sessionId", session)
                model = m.get("model") or p.get("model") or model
                contents = m.get("content", [])
                bodies = (
                    [
                        part.get("content")
                        for part in contents
                        if isinstance(part, dict) and part.get("type") == "tool_result"
                    ]
                    if isinstance(contents, list)
                    else []
                )
                if p.get("type") in ("function_call_output", "custom_tool_call_output"):
                    bodies.append(p.get("output"))
                if ts is not None:
                    outputs.extend(
                        (ts, len(body if isinstance(body, str) else json.dumps(body)))
                        for body in bodies
                    )
                usage = m.get("usage") if provider == "fable" else None
                fallback = False
                if provider == "astra":
                    info = p.get("info")
                    if not isinstance(info, dict) or not isinstance(
                        info.get("total_token_usage"), dict
                    ):
                        continue
                    total = info["total_token_usage"]
                    fingerprint = json.dumps(total, sort_keys=True)
                    # Update the baseline even outside the requested time window.
                    delta = {
                        k: max(0, v - previous.get(k, 0))
                        for k, v in total.items()
                        if isinstance(v, (int, float))
                    }
                    if any(
                        total.get(k, 0) < v
                        for k, v in previous.items()
                        if isinstance(v, (int, float))
                    ):
                        delta = total.copy()
                    previous = total
                    if fingerprint in seen_totals:
                        continue
                    seen_totals.add(fingerprint)
                    if not any(delta.values()):
                        continue
                    # Prefer the provider's request usage: an initial cumulative
                    # snapshot can include history predating this transcript.
                    last = info.get("last_token_usage")
                    usage = {**delta, **last} if isinstance(last, dict) else delta
                    request_id = (
                        p.get("request_id") or info.get("request_id") or event.get("request_id")
                    )
                    fallback = not bool(request_id)
                    key = str(request_id) if request_id else f"{session}:{fingerprint}"
                    context = (
                        last.get("input_tokens", delta.get("input_tokens", 0))
                        if isinstance(last, dict)
                        else delta.get("input_tokens", 0)
                    )
                    values = dict(
                        input=max(
                            0, usage.get("input_tokens", 0) - usage.get("cached_input_tokens", 0)
                        ),
                        cached=usage.get("cached_input_tokens", 0),
                        output=usage.get("output_tokens", 0),
                        reasoning=usage.get("reasoning_output_tokens", 0),
                    )
                else:
                    if not isinstance(usage, dict):
                        continue
                    request_id = event.get("requestId") or event.get("request_id") or m.get("id")
                    fallback = not bool(request_id)
                    key = str(request_id) if request_id else f"{path}:{index}"
                    creation = usage.get("cache_creation") or {}
                    values = dict(
                        input=usage.get("input_tokens", 0),
                        cached=usage.get("cache_read_input_tokens", 0),
                        cache_write=usage.get("cache_creation_input_tokens", 0),
                        cache_write_1h=creation.get("ephemeral_1h_input_tokens", 0),
                        output=usage.get("output_tokens", 0),
                    )
                    context = sum(values[k] for k in ("input", "cached", "cache_write"))
                if ts is None:
                    diagnostics["usage_without_timestamp"] += 1
                    continue
                row = dict(
                    session=session,
                    model=model,
                    timestamp=ts,
                    context=context,
                    fallback=fallback,
                    **{k: values.get(k, 0) for k in COUNTERS},
                )
                if key in requests:
                    old = requests[key]
                    diagnostics["duplicate_requests"] += 1
                    # Streaming usage snapshots can grow before the final block.
                    for k in (*COUNTERS, "context"):
                        old[k] = max(old[k], row[k])
                    old["timestamp"] = min(old["timestamp"], ts)
                else:
                    requests[key] = row
    return list(requests.values()), outputs, dict(diagnostics)


def cost(s, prices):
    i, c, o = prices
    write = price("CHITTA_PRICE_CACHE_WRITE", i * 1.25)
    write_1h = price("CHITTA_PRICE_CACHE_WRITE_1H", i * 2)
    return (
        s["input"] * i
        + s["cached"] * c
        + (s["cache_write"] - s["cache_write_1h"]) * write
        + s["cache_write_1h"] * write_1h
        + s["output"] * o
    ) / 1e6


def summarize(rows, prices):
    result = {k: sum(row[k] for row in rows) for k in COUNTERS}
    result["turns"] = len(rows)
    result["requests"] = len(rows)
    result["fallback_request_ids"] = sum(row["fallback"] for row in rows)
    result["avg_context_per_request"] = (
        int(sum(row["context"] for row in rows) / len(rows)) if rows else 0
    )
    last = max(rows, key=lambda row: row["timestamp"]) if rows else {}
    result["last_request_context"] = last.get("context", 0)
    result["last_request_at"] = last.get("timestamp")
    result["cost"] = cost(result, prices)
    return result


def report_agent(paths, provider, prices, since, until, top):
    lifetime, outputs, diagnostics = records(paths, provider)
    rows = [r for r in lifetime if since <= r["timestamp"] <= until]
    sessions, models, lifetime_sessions = defaultdict(list), defaultdict(list), defaultdict(list)
    for row in lifetime:
        if row["timestamp"] <= until:
            lifetime_sessions[row["session"]].append(row)
    for row in rows:
        sessions[row["session"]].append(row)
        models[row["model"]].append(row)
    summaries = []
    for session, values in sessions.items():
        s = summarize(values, prices)
        life = summarize(lifetime_sessions[session], prices)
        s.update(
            session=session,
            lifetime_mean_context=life["avg_context_per_request"],
            last_request_context=life["last_request_context"],
        )
        summaries.append(s)
    summaries.sort(key=lambda s: -s["cost"])
    total = summarize(rows, prices)
    lengths = [n for ts, n in outputs if since <= ts <= until]
    total.update(
        tool_outputs=len(lengths),
        tool_chars=sum(lengths),
        tool_over_6k=sum(n > 6000 for n in lengths),
    )
    return dict(
        sessions=len(sessions),
        total=total,
        total_cost=total["cost"],
        avg_context_per_turn=total["avg_context_per_request"],
        per_model={m: summarize(rs, prices) for m, rs in models.items()},
        diagnostics=diagnostics,
        top=summaries[:top],
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--days", type=float, default=7)
    ap.add_argument("--top", type=int, default=5)
    ap.add_argument("--json", action="store_true")
    ap.add_argument(
        "--now", type=float, default=None, help="Pin the event-time window (Unix seconds)"
    )
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        self_test()
        return 0
    until = args.now if args.now is not None else time.time()
    report = dict(since_days=args.days, since=until - args.days * 86400, until=until, agents={})
    for provider, root, prices in [
        ("fable", HOME / ".claude/projects", CLAUDE),
        ("astra", HOME / ".codex/sessions", CODEX),
    ]:
        report["agents"][provider] = report_agent(
            sorted(root.glob("**/*.jsonl")), provider, prices, report["since"], until, args.top
        )
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        for provider, r in report["agents"].items():
            t = r["total"]
            print(
                f"{provider}: sessions={r['sessions']} requests={t['requests']} mean_context={t['avg_context_per_request']} last_context={t['last_request_context']} cost=${r['total_cost']:.2f}"
            )
            for model, usage in r["per_model"].items():
                print(
                    f"  model={model} requests={usage['requests']} input={usage['input']} cached={usage['cached']} cache_write={usage['cache_write']} output={usage['output']}"
                )
            for s in r["top"]:
                print(
                    f"  {s['session']} requests={s['requests']} lifetime_mean={s['lifetime_mean_context']} last_context={s['last_request_context']} cost=${s['cost']:.2f}"
                )
    return 0


def self_test():
    import tempfile

    with tempfile.TemporaryDirectory() as root:
        path = Path(root) / "events.jsonl"

        def write(events):
            path.write_text("".join(json.dumps(e) + "\n" for e in events))

        def claude(ts, rid, output=2, model="m1"):
            return dict(
                timestamp=ts,
                requestId=rid,
                sessionId="s",
                message=dict(
                    model=model,
                    usage=dict(
                        input_tokens=10,
                        cache_read_input_tokens=20,
                        cache_creation_input_tokens=30,
                        output_tokens=output,
                    ),
                ),
            )

        write([claude(1, "old"), claude(11, "a"), claude(11, "a", 5), claude(12, "b", model="m2")])
        r = report_agent([path, path], "fable", (1, 0.1, 2), 10, 20, 5)
        assert r["total"]["requests"] == 2 and r["total"]["output"] == 7
        assert r["total"]["cache_write"] == 60 and len(r["per_model"]) == 2
        assert r["top"][0]["lifetime_mean_context"] == 60
        assert abs(r["total_cost"] - 113 / 1e6) < 1e-12

        def codex(ts, inp, out, last):
            return dict(
                timestamp=ts,
                payload=dict(
                    type="token_count",
                    info=dict(
                        total_token_usage=dict(
                            input_tokens=inp, cached_input_tokens=inp // 2, output_tokens=out
                        ),
                        last_token_usage=dict(input_tokens=last),
                    ),
                ),
            )

        write(
            [
                dict(timestamp=0, type="session_meta", payload=dict(id="s")),
                dict(timestamp=0, type="turn_context", payload=dict(model="codex-test")),
                codex(1, 100, 10, 100),
                codex(11, 300, 30, 200),
                codex(12, 300, 30, 200),
                codex(21, 600, 60, 300),
            ]
        )
        r = report_agent([path, path], "astra", (1, 0.1, 2), 10, 20, 5)
        assert r["total"]["requests"] == 1 and r["total"]["input"] == 100
        assert r["total"]["output"] == 20 and r["total"]["fallback_request_ids"] == 1
        assert (
            r["top"][0]["last_request_context"] == 200
            and r["top"][0]["lifetime_mean_context"] == 150
        )
        assert "codex-test" in r["per_model"]
        inherited = codex(11, 10000, 1000, 200)
        inherited["payload"]["info"]["last_token_usage"].update(
            cached_input_tokens=100, output_tokens=20
        )
        write([inherited])
        isolated = report_agent([path], "astra", CODEX, 10, 20, 0)
        assert isolated["total"]["input"] == 100 and isolated["total"]["output"] == 20
        os.utime(path, (0, 0))
        assert report_agent([path], "astra", CODEX, 10, 20, 0)["sessions"] == 1
        write([claude(11, "hour")])
        events = json.loads(path.read_text())
        events["message"]["usage"]["cache_creation"] = {"ephemeral_1h_input_tokens": 10}
        write([events])
        r = report_agent([path], "fable", (1, 0.1, 2), 10, 20, 0)
        assert abs(r["total_cost"] - 61 / 1e6) < 1e-12
    print("token-ledger self-test: PASS (dedup, timestamps, cache cost, models, context, mtime)")


if __name__ == "__main__":
    raise SystemExit(main())
