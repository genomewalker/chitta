"""Read the daily ledger cache without scanning transcripts at SessionStart."""

import json
import math


def weekly_line(state):
    try:
        agents = json.loads((state / "token-ledger.json").read_text())["agents"]
        fable, astra = agents["fable"], agents["astra"]
        average = fable.get("total", {}).get(
            "avg_context_per_request", fable.get("avg_context_per_turn")
        )
        values = (
            fable["sessions"],
            average,
            fable["total_cost"],
            astra["sessions"],
            astra["total_cost"],
        )
        if any(not isinstance(v, (int, float)) or not math.isfinite(v) or v < 0 for v in values):
            return ""
        return (
            f"[tokens] this week: fable {fable['sessions']}s "
            f"{average / 1000:.0f}k/req ${fable['total_cost']:.2f}; "
            f"astra {astra['sessions']}s ${astra['total_cost']:.2f}\n"
        )
    except (OSError, ValueError, KeyError, TypeError):
        return ""
