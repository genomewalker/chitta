"""Manual real-stdio recall timing; read-only RPC and isolated caches/state.

Run from chitta-mcp with the MCP interpreter: python tests/profile_recall.py PREFIX.
Only timings and response sizes are persisted, never memories or credentials.
"""

from __future__ import annotations

import asyncio
import json
import os
import sys
import tempfile
import time
from pathlib import Path

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import server  # noqa: E402
from recall_gateway import resolve_onnx_dir  # noqa: E402

BOOTSTRAP = """
import os, server
server._SQZ_BIN = os.environ['PROFILE_SQZ']
server.client = server.ChittaClient(os.environ['PROFILE_SOCKET'])
# Never enter the service/start-lock fallback, even on failure.
def connect_only():
    return bool(server.client.sock) or server.client.connect()
server.ensure_daemon = connect_only
original_call = server.daemon_call
def readonly_call(name, *args, **kwargs):
    assert name in {'recall', 'hybrid_recall', 'health_check', 'soul_context'}, name
    return original_call(name, *args, **kwargs)
server.daemon_call = readonly_call
server._run_stdio()
"""


async def measure(prefix: Path):
    socket_path = (
        Path(f"/run/user/{os.getuid()}/chitta")
        / f"chitta-{server.djb2_hash(str(Path.home() / '.claude/mind'))}.sock"
    )
    if not socket_path.is_socket():
        raise RuntimeError("Existing live socket not found; refusing to start a daemon")
    rows = []
    with tempfile.TemporaryDirectory(prefix="mcp-profile-") as directory:
        env = dict(
            os.environ,
            HOME=directory,
            XDG_CACHE_HOME=directory,
            XDG_RUNTIME_DIR=directory,
            CHITTA_MCP_PROFILE="1",
            CHITTA_RERANK_ONNX_DIR=resolve_onnx_dir(),
            HF_HUB_OFFLINE="1",
            TRANSFORMERS_OFFLINE="1",
            PROFILE_SOCKET=str(socket_path),
            PROFILE_SQZ=server._SQZ_BIN,
            PYTHONPATH=os.pathsep.join(sys.path),
        )
        env.pop("CHITTA_RPC_PORT", None)
        with prefix.with_suffix(".log").open("w") as log:
            started = time.monotonic()
            params = StdioServerParameters(
                command=sys.executable, args=["-c", BOOTSTRAP], env=env, cwd=str(ROOT)
            )
            async with stdio_client(params, errlog=log) as (read, write):
                async with ClientSession(read, write) as session:
                    await session.initialize()
                    rows.append(dict(stage="initialize", seconds=time.monotonic() - started))
                    for n in range(4):
                        started = time.monotonic()
                        response = await session.call_tool(
                            "recall",
                            dict(
                                query="SMRITI benchmark result memory-on vs memory-off",
                                realm="project:cc-soul",
                                limit=10,
                            ),
                        )
                        elapsed = time.monotonic() - started
                        texts = [item.text for item in response.content if item.type == "text"]
                        row = dict(
                            stage="cold" if n == 0 else f"warm{n}",
                            seconds=elapsed,
                            error=response.isError,
                            bytes=sum(len(t) for t in texts),
                        )
                        rows.append(row)
                        print(json.dumps(row), flush=True)
                        if (
                            response.isError
                            or not texts
                            or any(t.startswith("Error:") for t in texts)
                        ):
                            raise RuntimeError("MCP recall failed")
    prefix.with_suffix(".json").write_text(json.dumps(rows, indent=2) + "\n")


if __name__ == "__main__":
    asyncio.run(measure(Path(sys.argv[1]).resolve()))
