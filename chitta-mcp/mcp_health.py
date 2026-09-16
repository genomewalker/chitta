"""MCP scheduling and health diagnostics.

Implementations receive the server runtime explicitly as ``ctx`` so the public
server facade owns shared state and dependency overrides across all entry points.
"""

from __future__ import annotations

import asyncio
import json
import os


def _lag_setting_ms(ctx, name: str, legacy_name: str, default: float) -> float:
    raw = os.environ.get(name, os.environ.get(legacy_name, str(default)))
    try:
        return max(1.0, float(raw))
    except ValueError:
        ctx.logger.warning("invalid %s=%r; using %.0fms", name, raw, default)
        return default


def loop_lag_stats(ctx) -> dict[str, float | int]:
    """Return a stable snapshot for health output and diagnostics."""
    return {
        "max_lag_ms": round(ctx._loop_lag_max_ms, 1),
        "over_count": ctx._loop_lag_over_count,
    }


async def monitor_loop_lag(
    ctx, interval_ms: float | None = None, warn_ms: float | None = None
) -> None:
    """Measure asyncio scheduling delay and warn when it crosses the threshold."""
    if interval_ms is None:
        interval_ms = ctx._lag_setting_ms(
            "CHITTA_MCP_LAG_INTERVAL_MS", "CC_SOUL_MCP_LAG_INTERVAL_MS", 250.0
        )
    if warn_ms is None:
        warn_ms = ctx._lag_setting_ms("CHITTA_MCP_LAG_WARN_MS", "CC_SOUL_MCP_LAG_WARN_MS", 200.0)
    interval_s = interval_ms / 1000.0
    loop = asyncio.get_running_loop()
    while True:
        expected = loop.time() + interval_s
        await asyncio.sleep(interval_s)
        lag_ms = max(0.0, (loop.time() - expected) * 1000.0)
        ctx._loop_lag_max_ms = max(ctx._loop_lag_max_ms, lag_ms)
        if lag_ms >= warn_ms:
            ctx._loop_lag_over_count += 1
            ctx.logger.warning(
                "asyncio scheduling delay %.1fms exceeds %.1fms threshold",
                lag_ms,
                warn_ms,
            )


async def _stop_loop_lag_monitor(ctx, task: asyncio.Task) -> None:
    task.cancel()
    try:
        await task
    except asyncio.CancelledError:
        pass


def _with_loop_lag_health(ctx, result: str) -> str:
    """Attach MCP loop counters without assuming the daemon health format."""
    stats = ctx.loop_lag_stats()
    sessions = ctx.http_session_stats()
    try:
        payload = json.loads(result)
    except (json.JSONDecodeError, TypeError):
        suffix = json.dumps(
            {"mcp_loop_lag": stats, "mcp_sessions": sessions}, separators=(",", ":")
        )
        return f"{result.rstrip()}\n{suffix}" if result else suffix
    if isinstance(payload, dict):
        payload["mcp_loop_lag"] = stats
        payload["mcp_sessions"] = sessions
        return json.dumps(payload)
    suffix = json.dumps({"mcp_loop_lag": stats, "mcp_sessions": sessions}, separators=(",", ":"))
    return f"{result.rstrip()}\n{suffix}"
