"""Read-only per-cycle best-versus-first candidate measurements."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from .selector import history
from .store import MemoryStore


def table(verdicts: list[dict]) -> str:
    lines = ["cycle  metric  selected  best_delta  first_delta  candidates  fallback"]
    cycles = {v["cycle_id"]: v for v in verdicts if v.get("cycle_id")}
    for cycle_id, value in sorted(cycles.items()):
        candidates = value.get("candidates", [])
        if not candidates:
            continue
        metric = value.get("metric", "")
        first = next((r for r in candidates if r["index"] == 1), {})
        best = next((r for r in candidates if r["index"] == value.get("selected_candidate")), {})

        def delta(row, metric=metric):
            measured = row.get("delta", {}).get(metric)
            return "n/a" if measured is None else f"{measured:+.6g}"

        lines.append(
            f"{cycle_id}  {metric}  {value.get('selected_candidate') or 'n/a'}  "
            f"{delta(best)}  {delta(first)}  {len(candidates)}  "
            f"{value.get('fanout', {}).get('fallback') or '-'}"
        )
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cycles", type=Path, help="read local cycle artifacts instead of memory")
    args = parser.parse_args(argv)
    verdicts = (
        [json.loads(path.read_text()) for path in args.cycles.glob("*/verdict.json")]
        if args.cycles
        else history(MemoryStore())
    )
    print(table(verdicts))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
