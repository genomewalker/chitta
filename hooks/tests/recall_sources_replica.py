#!/usr/bin/env python3
"""Source-toggle and confidence checks against an explicitly verified private replica."""

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "benchmarks/current_truth"))
from run import endpoint  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--calibrated", action="store_true")
    args = parser.parse_args()
    socket = endpoint()

    def recall(sources=None):
        cmd = [
            os.environ["CHITTA_BIN"],
            "--socket-path",
            socket,
            "recall",
            "--query",
            "which env var controls the daemon store lock wait",
            "--realm",
            "project:cc-soul",
            "--strategy",
            "hybrid",
            "--limit",
            "10",
            "--no-learn",
            "--json",
        ]
        if sources is not None:
            cmd += ["--sources", str(sources).lower()]
        return json.loads(
            subprocess.run(cmd, capture_output=True, text=True, check=True, timeout=60).stdout
        )

    default, explicit, memories = recall(), recall(True), recall(False)

    def ids(result):
        return [r["id"] for r in result["results"]]

    assert ids(default) == ids(explicit), "default must include sources"
    source_rows = [r for r in default["results"] if "source_identity" in r]
    assert source_rows, "fixture query must exercise source merging"
    assert all("source_identity" not in r for r in memories["results"])
    retained = [r["id"] for r in default["results"] if "source_identity" not in r]
    assert retained == ids(memories)[: len(retained)], "source toggle changed memory ordering"
    if args.calibrated:
        assert default["source_hits"] == len(source_rows)
        assert memories["source_hits"] == 0
        assert default["max_relevance"] == memories["max_relevance"]
        assert default["abstain"] == memories["abstain"]
    print(
        json.dumps(
            {
                "sources": len(source_rows),
                "memory_order_unchanged": True,
                "source_max_relevance": default["max_relevance"],
                "memory_max_relevance": memories["max_relevance"],
                "calibrated": args.calibrated,
            }
        )
    )


if __name__ == "__main__":
    main()
