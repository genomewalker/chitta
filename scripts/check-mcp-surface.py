#!/usr/bin/env python3
"""Measure the actual MCP tools/list result without connecting to a daemon.

Calls server.list_tools itself, including composite replacement and recursive
null stripping. Payload is the compact UTF-8 JSON MCP result {"tools": [...]},
serialized with the SDK's exclude_none convention (no JSON-RPC envelope).

Token estimate for deployed Anthropic models: ceil(Unicode characters / 4).
No Anthropic tokenizer is shipped locally; the embedding tokenizer is not an
Anthropic tokenizer. This is an approximation, not an exact model token count.
Both thresholds are inclusive and enforced by default. --max-core and
--max-tokens allow stricter budgets; --json emits a machine-readable report.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "chitta-mcp"))


def measure() -> dict:
    import server
    import tool_policy

    # Use the production path, not a second implementation of discovery policy.
    listed = asyncio.run(server.list_tools())
    payload = json.dumps(
        {"tools": [tool.model_dump(mode="json", exclude_none=True) for tool in listed]},
        ensure_ascii=False,
        separators=(",", ":"),
    )
    names = [tool.name for tool in listed]
    known = tool_policy.ALL_TOOLS
    return {
        "core": len(names),
        "advanced": len(known & tool_policy.ADVANCED_TOOLS),
        "hidden": len(known & tool_policy.INTERNAL_TOOLS),
        "total": len(known),
        "characters": len(payload),
        "bytes": len(payload.encode("utf-8")),
        "tokens": (len(payload) + 3) // 4,
        "tokenizer": "Anthropic approximation: ceil(characters / 4); no local tokenizer",
        "core_names": names,
        "unclassified": sorted(known - set(names) - server.HIDDEN_TOOLS),
        "invalid_core": sorted(tool_policy.CORE_TOOLS - set(names)),
        "duplicate_names": sorted({name for name in names if names.count(name) > 1}),
        "overlapping_tiers": sorted(
            (tool_policy.CORE_TOOLS & server.HIDDEN_TOOLS)
            | (tool_policy.INTERNAL_TOOLS & tool_policy.ADVANCED_TOOLS)
        ),
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--max-core", type=int, default=80)
    parser.add_argument("--max-tokens", type=int, default=8000)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    if args.max_core < 0 or args.max_tokens < 0:
        parser.error("thresholds must be nonnegative")
    report = measure()
    report["max_core"] = args.max_core
    report["max_tokens"] = args.max_tokens
    report["ok"] = (
        report["core"] <= args.max_core
        and report["tokens"] <= args.max_tokens
        and not any(
            report[key]
            for key in ("unclassified", "invalid_core", "duplicate_names", "overlapping_tiers")
        )
    )
    if args.json:
        print(json.dumps(report, indent=2))
    else:
        print(
            f"MCP surface: core={report['core']} advanced={report['advanced']} "
            f"hidden={report['hidden']} total={report['total']}"
        )
        print(
            f"tools/list: {report['characters']} chars, {report['bytes']} bytes, "
            f"~{report['tokens']} tokens ({report['tokenizer']})"
        )
        print(
            f"{'PASS' if report['ok'] else 'FAIL'}: core <= {args.max_core}; "
            f"payload <= {args.max_tokens} tokens"
        )
        for key in ("unclassified", "invalid_core", "duplicate_names", "overlapping_tiers"):
            if report[key]:
                print(f"{key}: {', '.join(report[key])}")
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
