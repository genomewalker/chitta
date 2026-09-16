#!/usr/bin/env python3
"""Summarize runtime incidents without treating undated logs as a clean soak."""

from __future__ import annotations

import argparse
import collections
import datetime as dt
import re
from pathlib import Path


def incidents(lines):
    counts = collections.defaultdict(collections.Counter)
    starts = collections.defaultdict(lambda: collections.defaultdict(list))
    for line in lines:
        stamp = re.search(r"\b(20\d\d-\d\d-\d\d)[T ](\d\d:\d\d:\d\d)", line)
        day = stamp[1] if stamp else "undated"
        counts[day]["lines"] += 1
        if re.search(
            r"(?:open(?:ing)? .*fail|fail(?:ed|ure)? .*open|cannot open|could not open)", line, re.I
        ):
            counts[day]["open failures"] += 1
        if re.search(
            r"stale.*lock.*(?:replac|remov|clear|recover)|(?:replac|remov|clear).*stale.*lock",
            line,
            re.I,
        ):
            counts[day]["stale-lock replacements"] += 1
        held = re.search(r"\[lockprof\].*\bheld=(\d+(?:\.\d+)?)ms", line)
        if held and float(held[1]) > 150:
            counts[day]["holds >150ms"] += 1
        event = None
        if "[socket_server] Listening (warming up)" in line:
            event = "native"  # Includes startups that fail before ready.
        elif "Scheduled restart job" in line:
            event = "unit"
        elif re.search(r"\[daemon\] (?:ready ms=|Starting daemon)", line):
            event = "ready"
        if event:
            starts[day][event].append(
                dt.datetime.fromisoformat(f"{stamp[1]}T{stamp[2]}") if stamp else None
            )
        if "Start request repeated too quickly" in line:
            counts[day]["restart loops"] += 1
    # Each third (or subsequent) start within five minutes is a loop signal.
    for day, sources in starts.items():
        # Prefer attempt markers; never count both warm-up and ready as restarts.
        selected = sources.get("native") or sources.get("unit") or sources["ready"]
        counts[day]["starts/restarts"] = len(selected)
        times = sorted(value for value in selected if value is not None)
        counts[day]["restart loops"] += sum(
            (times[i] - times[i - 2]).total_seconds() <= 300 for i in range(2, len(times))
        )
    return counts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", type=Path, nargs="*")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        rows = incidents(
            [
                "2026-09-16 12:00:00 [daemon] ready ms=9000",
                "2026-09-16 12:01:00 [daemon] ready ms=9000",
                "2026-09-16 12:02:00 [daemon] ready ms=9000",
                "2026-09-16 12:02:01 [chitta-field] open failed: I/O error",
                "2026-09-16 12:02:02 stale instance lock /tmp/x; replacing it",
                "2026-09-16 12:02:03 [lockprof] EXCLUSIVE observe held=150ms",
                "2026-09-16 12:02:04 [lockprof] EXCLUSIVE observe held=151ms",
                "[daemon] Failed to open chitta-field store",
            ]
        )
        row = rows["2026-09-16"]
        assert row["restart loops"] == row["open failures"] == 1
        assert row["stale-lock replacements"] == row["holds >150ms"] == 1
        assert rows["undated"]["open failures"] == 1
        attempts = incidents(
            [
                f"2026-09-16 12:0{i}:00 [socket_server] Listening (warming up) on /tmp/test"
                for i in range(3)
            ]
            + ["2026-09-16 12:02:01 [daemon] ready ms=9000"]
        )
        assert attempts["2026-09-16"]["starts/restarts"] == 3
        assert attempts["2026-09-16"]["restart loops"] == 1
        print("incident date, thresholds, restart loops and undated coverage: passed")
        return
    if not args.logs:
        parser.error("provide logs or --self-test")

    def log_lines():
        for path in args.logs:
            with path.open(errors="replace") as stream:
                yield from stream

    counts = incidents(log_lines())
    columns = [
        "open failures",
        "stale-lock replacements",
        "starts/restarts",
        "restart loops",
        "holds >150ms",
    ]
    print(f"Runtime incident report — {dt.datetime.now(dt.timezone.utc).date()} UTC")
    print("| Log date | " + " | ".join(columns) + " |")
    print("| --- | " + " | ".join(["---:"] * len(columns)) + " |")
    for day, row in sorted(counts.items()):
        values = [
            ("unknown" if day == "undated" and c == "restart loops" and not row[c] else str(row[c]))
            for c in columns
        ]
        print("| " + day + " | " + " | ".join(values) + " |")
    print(
        "Coverage is only the supplied log lines; zero matches do not prove a fortnight without incidents."
    )
    if "undated" in counts:
        print(
            "Undated lines cannot establish dates or restart-loop timing; collect timestamped logs for the soak."
        )


if __name__ == "__main__":
    main()
