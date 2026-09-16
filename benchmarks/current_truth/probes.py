#!/usr/bin/env python3
"""Run the original five September 16 probes without modifying the frozen panel."""

from __future__ import annotations

import json
import re
from pathlib import Path

import run


def main():
    panel = json.loads(Path(__file__).with_name("original_probes.json").read_text())
    socket = run.endpoint()
    rows = []
    for probe in panel["probes"]:
        hits = run.recall(probe["question"], socket)
        useful = [
            str(hit["id"])
            for hit in hits
            if all(
                re.search(pattern, hit.get("content", hit.get("text", "")))
                for pattern in probe["all"]
            )
        ]
        rows.append(
            {"id": probe["id"], "question": probe["question"], "useful": useful, "hits": hits}
        )
    print(
        json.dumps(
            {"rule": panel["rule"], "passed": sum(bool(r["useful"]) for r in rows), "rows": rows},
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
