"""Stdio and HTTP transport with bounded HTTP sessions.

Implementations receive the server runtime explicitly as ``ctx`` so the public
server facade owns shared state and dependency overrides across all entry points.
"""

from __future__ import annotations

import asyncio
import math
import os
import time
from pathlib import Path


class HttpSessionTable(dict):
    """SDK transport table with monotonic activity, bounded admission and reaping.

    The SDK inserts through __setitem__ and looks up existing sessions through
    __getitem__. Its creation lock is replaced by this async context manager so
    capacity is reserved atomically before a transport (and server task) exists.
    """

    def __init__(self, owners, idle_s=1800.0, max_sessions=64):
        super().__init__()
        self.owners = owners
        self.idle_s = idle_s
        self.max_sessions = max_sessions
        self.last_seen = {}
        self.created = self.expired = self.evicted = 0
        self.lock = asyncio.Lock()

    def __setitem__(self, key, value):
        super().__setitem__(key, value)
        self.last_seen[key] = time.monotonic()
        self.created += 1

    def __getitem__(self, key):
        value = super().__getitem__(key)
        self.last_seen[key] = time.monotonic()
        return value

    def stats(self):
        return {
            "active": len(self),
            "created": self.created,
            "expired": self.expired,
            "evicted": self.evicted,
            "max_sessions": self.max_sessions,
            "idle_s": self.idle_s,
        }

    async def _remove(self, key, reason):
        transport = self.pop(key, None)
        self.last_seen.pop(key, None)
        self.owners.pop(key, None)
        if transport is not None:
            if reason == "expired":
                self.expired += 1
            elif reason == "evicted":
                self.evicted += 1
            await transport.terminate()

    async def _reap(self):
        now = time.monotonic()
        for key in list(self.last_seen):
            if key not in self:
                self.last_seen.pop(key, None)
                self.owners.pop(key, None)
            elif self.get(key).is_terminated:
                await self._remove(key, "closed")
            elif now - self.last_seen[key] > self.idle_s:
                await self._remove(key, "expired")

    async def reap(self):
        async with self.lock:
            await self._reap()

    async def __aenter__(self):
        await self.lock.acquire()
        try:
            await self._reap()
            while len(self) >= self.max_sessions:
                oldest = min(self, key=lambda key: self.last_seen[key])
                await self._remove(oldest, "evicted")
        except BaseException:
            self.lock.release()
            raise
        return self

    async def __aexit__(self, *exc):
        self.lock.release()

    async def monitor(self):
        while True:
            await asyncio.sleep(min(30.0, self.idle_s / 2))
            await self.reap()


def _session_setting(ctx, name, default, kind):
    try:
        value = kind(os.environ.get(name, str(default)))
        if not math.isfinite(value) or value <= 0:
            raise ValueError("must be finite and positive")
        return value
    except ValueError:
        ctx.logger.warning("invalid %s; using %s", name, default)
        return default


def http_session_stats(ctx):
    return ctx._http_sessions.stats() if ctx._http_sessions is not None else {"active": 0}


def _run_stdio(ctx):
    async def run():
        init_options = ctx.InitializationOptions(
            server_name="chitta-mcp",
            server_version="0.1.0",
            capabilities=ctx.ServerCapabilities(tools=ctx.ToolsCapability()),
        )
        lag_task = asyncio.create_task(ctx.monitor_loop_lag())
        try:
            async with ctx.stdio_server() as (read_stream, write_stream):
                await ctx.server.run(read_stream, write_stream, init_options)
        finally:
            await ctx._stop_loop_lag_monitor(lag_task)

    asyncio.run(run())


def _mcp_token(ctx) -> str:
    token_file = Path(os.environ.get("MIND", Path.home() / ".claude" / "mind")) / ".mcp_token"
    try:
        return token_file.read_text().strip()
    except FileNotFoundError:
        return ""


def create_http_session_manager(ctx):
    from mcp.server.streamable_http_manager import StreamableHTTPSessionManager

    class BoundedSessionManager(StreamableHTTPSessionManager):
        async def handle_request(self, scope, receive, send):
            # Reject expired IDs even between periodic sweeps, before the SDK
            # looks them up and refreshes last_seen.
            await self._server_instances.reap()
            await super().handle_request(scope, receive, send)

    session_manager = BoundedSessionManager(
        app=ctx.server,
        json_response=True,
        stateless=False,
    )
    ctx._http_sessions = ctx.HttpSessionTable(
        # Older SDK releases have no credential ownership map. Keep the SDK's
        # actual map when present so expiry still removes ownership metadata.
        getattr(session_manager, "_session_owners", {}),
        idle_s=ctx._session_setting("CHITTA_MCP_SESSION_IDLE_S", 1800.0, float),
        max_sessions=ctx._session_setting("CHITTA_MCP_MAX_SESSIONS", 64, int),
    )
    # Keep SDK transport cleanup and ownership checks; bound its creation path.
    session_manager._server_instances = ctx._http_sessions
    session_manager._session_creation_lock = ctx._http_sessions

    return session_manager


def _run_http(ctx, port: int):
    """Run as streamable HTTP MCP server for Codex, Cursor, Copilot CLI etc."""
    from starlette.applications import Starlette
    from starlette.responses import JSONResponse, PlainTextResponse
    from starlette.routing import Mount

    token = ctx._mcp_token()

    session_manager = ctx.create_http_session_manager()

    from starlette.routing import Route

    async def health(request):
        return JSONResponse(
            {
                "status": "ok",
                "server": "chitta-mcp",
                "transport": "http",
                "mcp_sessions": ctx.http_session_stats(),
            }
        )

    async def auth_middleware(scope, receive, send):
        if scope["type"] == "http" and scope.get("path", "").startswith("/mcp"):
            if token:
                auth = dict(scope.get("headers", [])).get(b"authorization", b"").decode()
                if auth != f"Bearer {token}":
                    response = PlainTextResponse("Unauthorized", status_code=401)
                    await response(scope, receive, send)
                    return
        await app(scope, receive, send)

    app = Starlette(
        routes=[
            Route("/health", health),
            Mount("/mcp", app=session_manager.handle_request),
        ],
    )

    async def run():
        import uvicorn

        config = uvicorn.Config(
            auth_middleware,
            host="127.0.0.1",
            port=port,
            log_level="warning",
        )
        http_server = uvicorn.Server(config)

        ctx.logger.warning(f"chitta-mcp HTTP listening on http://127.0.0.1:{port}/mcp")

        lag_task = asyncio.create_task(ctx.monitor_loop_lag())
        session_task = asyncio.create_task(ctx._http_sessions.monitor())
        try:
            async with session_manager.run():
                await http_server.serve()
        finally:
            await ctx._stop_loop_lag_monitor(session_task)
            await ctx._stop_loop_lag_monitor(lag_task)

    asyncio.run(run())
