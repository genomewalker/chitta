"""Daemon connection, RPC normalization, and response formatting.

Implementations receive the server runtime explicitly as ``ctx`` so the public
server facade owns shared state and dependency overrides across all entry points.
"""

from __future__ import annotations

import json
import os
import subprocess
import time
from typing import Any


def ensure_daemon(ctx) -> bool:
    """Ensure daemon is running and connected.

    Uses HTTP if CHITTA_RPC_PORT is set; falls back to Unix socket.
    Uses atomic lock file creation to prevent race conditions.
    Only ONE process should spawn the daemon - others wait.
    """

    rpc_port = os.environ.get("CHITTA_RPC_PORT", "")
    if rpc_port:
        base_url = f"http://127.0.0.1:{rpc_port}"
        if (
            not isinstance(ctx.client, ctx.ChittaHttpClient)
            or getattr(ctx.client, "base_url", "") != base_url
        ):
            ctx.client = ctx.ChittaHttpClient(base_url)
        if ctx.client.connect():
            return True
        # Daemon not up yet — fall through to spawn logic below
        socket_path = ctx.get_socket_path()
    else:
        socket_path = ctx.get_socket_path()
        if isinstance(ctx.client, ctx.ChittaHttpClient):
            ctx.client = None
        if ctx.client and ctx.client.sock:
            return True
        ctx.client = ctx.ChittaClient(socket_path)
        if ctx.client.connect():
            return True

    # Socket doesn't exist or can't connect - wait for daemon
    # First, just wait - subconscious.sh hook usually starts daemon
    for _ in range(30):
        time.sleep(0.1)
        if ctx.client.connect():
            return True

    # Still no daemon - try to start it with atomic lock.
    # NOT ".lock": that path is the daemon's single-writer mutex, an fcntl lock held on the
    # INODE for the daemon's whole life. This here is a create/unlink start-mutex — a totally
    # different protocol. Pointing both at one file meant we deleted a live daemon's mutex as
    # "stale" (it is always >60s old while healthy), after which the next daemon locked a fresh
    # inode and ran alongside it. Four concurrent "exclusive" owners is how the store died on
    # 2026-07-14. Keep the two protocols on separate paths.
    lock_path = socket_path.replace(".sock", ".startlock")
    lock_fd = None
    we_hold_lock = False

    # Clean stale lock files (older than 60 seconds). Best-effort: a racing
    # process may unlink the same path between the stat and the unlink, and an
    # unwritable lock dir is not fatal — the O_EXCL create below decides.
    try:
        if os.path.exists(lock_path):
            lock_age = time.time() - os.path.getmtime(lock_path)
            if lock_age > 60:
                os.unlink(lock_path)
    except OSError as exc:
        ctx.logger.debug("stale start-lock cleanup failed for %s: %s", lock_path, exc)

    try:
        # Try to create lock file atomically (O_EXCL fails if exists)
        lock_fd = os.open(lock_path, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o644)
        we_hold_lock = True
    except FileExistsError:
        # Another process is starting daemon - wait for it
        for _ in range(50):
            time.sleep(0.1)
            if ctx.client.connect():
                return True
        return False

    if we_hold_lock and lock_fd is not None:
        try:
            # Start the daemon. Prefer the systemd unit so there is exactly ONE managed
            # writer with the correct embedder + flags. A raw `chittad daemon` with DEFAULT
            # flags spawns an UNMANAGED SECOND writer: concurrent-writer family churn that
            # corrupts the store, plus hygiene/consolidation ON (overriding the unit's
            # --no-hygiene), which has caused recall stalls + poison snapshot families.
            # Never spawn with default flags.
            home = os.environ.get("HOME", "")
            started = False
            unit = os.path.join(home, ".config", "systemd", "user", "chittad.service")
            if os.path.exists(unit):
                started = os.system("systemctl --user start chittad >/dev/null 2>&1") == 0
            if not started:
                chittad = os.path.join(home, ".claude", "bin", "chittad")
                if os.path.exists(chittad):
                    # Fallback (no systemd unit): match the managed config; at minimum
                    # never enable hygiene/distill, and pin the store path + embedder.
                    mind = os.path.join(home, ".claude", "mind")
                    model = os.path.join(home, ".claude", "bin", "bge-large-en-v1.5.gguf")
                    flags = f"--path {mind} --no-autonomous --no-distill --no-hygiene --no-enrich"
                    if os.path.exists(model):
                        flags += f" --embed-model {model}"
                    os.system(f"{chittad} daemon {flags} >/dev/null 2>&1 &")
            for _ in range(50):
                time.sleep(0.1)
                if ctx.client.connect():
                    return True
        finally:
            # Release lock. The unlink is best-effort: if it fails the file is
            # left behind, and the stale-lock sweep above reclaims it after 60s.
            os.close(lock_fd)
            try:
                os.unlink(lock_path)
            except OSError as exc:
                ctx.logger.debug("could not release start-lock %s: %s", lock_path, exc)

    return False


def _normalize_args(ctx, arguments: dict) -> dict:
    """Coerce Claude Code MCP serialization quirks.

    Claude Code serializes array parameters as JSON-encoded strings
    (e.g. tags='["a","b"]' instead of tags=["a","b"]).
    This normalizer detects and unwraps them before forwarding to the daemon.
    """
    ARRAY_KEYS = {"tags", "shared_realms", "data_paths", "script_paths", "realms"}
    result = {}
    for k, v in arguments.items():
        if k in ARRAY_KEYS and isinstance(v, str):
            stripped = v.strip()
            if stripped.startswith("["):
                try:
                    v = json.loads(stripped)
                except json.JSONDecodeError:
                    pass  # leave as-is; daemon handles comma-separated strings
            elif "," in stripped:
                v = [s.strip() for s in stripped.split(",") if s.strip()]
        result[k] = v
    return result


def daemon_call(ctx, tool_name: str, arguments: dict, structured: bool = False) -> str:
    """Call a tool on the daemon.

    Args:
        tool_name: Name of the tool to call
        arguments: Tool arguments
        structured: If True, return structured JSON data instead of text
    """

    with ctx.profile_stage("daemon_connect", tool=tool_name):
        connected = ctx.ensure_daemon()
    if not connected:
        return "Error: Failed to connect to daemon"

    arguments = ctx._normalize_args(arguments)

    # Choke-point source_session tagging: every singular `remember` (direct or via
    # a composite wrapper like learn_*/remember_typed that builds its own dict and
    # would otherwise drop the caller session) carries the origin session. The
    # daemon's remember handler honors source_session (field_memory_recall.cpp);
    # remember_batch is tagged per-item at the call_tool layer instead. Best-effort:
    # skip silently when the session can't be resolved.
    if tool_name == "remember" and not arguments.get("source_session"):
        _sid = ctx.get_current_session_id(use_cache=True)
        if _sid:
            arguments["source_session"] = _sid

    req = {
        "jsonrpc": "2.0",
        "method": "tools/call",
        "params": {"name": tool_name, "arguments": arguments},
    }

    with ctx.profile_stage("daemon_rpc", tool=tool_name):
        response = ctx.client.call(req)

    # If no response, connection might be stale - try reconnecting once
    if not response:
        ctx.client.close()
        ctx.client = None
        if ctx.ensure_daemon():
            response = ctx.client.call(req)

    if not response:
        return "Error: No response from daemon"

    try:
        data = json.loads(response)
        result = data.get("result", {})

        # Return structured data if requested
        if structured and "structured" in result:
            return json.dumps(result["structured"])

        content = result.get("content", [])

        if content and isinstance(content, list):
            return "\n".join(
                item.get("text", str(item)) if isinstance(item, dict) else str(item)
                for item in content
            )

        return ""
    except (ValueError, AttributeError, TypeError) as e:
        # Daemon sent something that is not the expected JSON-RPC envelope.
        # Surfaced to the caller as text: every handler returns a string, and a
        # malformed response is information the caller needs, not a crash.
        ctx.logger.warning("unparseable daemon response for %s: %s", tool_name, e)
        return f"Error: {e}"


def to_toon(ctx, obj: Any, indent: int = 0) -> str:
    """Convert JSON to TOON format (~40% fewer tokens).

    TOON format:
    - Scalars: key: value
    - Arrays: key[n]{fields}: val1|val2 | val1|val2
    - Objects: key.subkey: value
    """
    if obj is None:
        return ""

    if isinstance(obj, (int, float, bool)):
        return str(obj)

    if isinstance(obj, str):
        # Escape newlines and pipes for TOON rows
        return obj.replace("\n", "\\n").replace("|", "\\|")

    if isinstance(obj, list):
        if not obj:
            return "[]"
        # Check if list of uniform dicts
        if all(isinstance(x, dict) for x in obj):
            # Get common keys
            keys = list(obj[0].keys()) if obj else []
            if keys and all(set(x.keys()) == set(keys) for x in obj):
                # Compact table format with | separator (safer than comma)
                header = f"[{len(obj)}]{{{','.join(keys)}}}:"
                rows = []
                for item in obj:
                    vals = []
                    for k in keys:
                        v = item.get(k, "")
                        # Truncate long strings, escape newlines
                        s = str(v) if v is not None else ""
                        s = s.replace("\n", " ").replace("|", "\\|")[:80]
                        vals.append(s)
                    rows.append(" " + "|".join(vals))
                return header + "\n" + "\n".join(rows)
        # Fallback: one per line
        return "\n".join(f"- {ctx.to_toon(x)}" for x in obj)

    if isinstance(obj, dict):
        lines = []
        for k, v in obj.items():
            if v is None:
                continue
            if isinstance(v, dict):
                # Flatten nested dicts
                for sk, sv in v.items():
                    if sv is not None:
                        lines.append(f"{k}.{sk}: {ctx.to_toon(sv)}")
            elif isinstance(v, list):
                lines.append(f"{k}{ctx.to_toon(v)}")
            else:
                lines.append(f"{k}: {ctx.to_toon(v)}")
        return "\n".join(lines)

    return str(obj)


def _sqz_compress(ctx, text: str, tool_name: str = "mcp") -> str:
    if len(text) < ctx._SQZ_THRESHOLD:
        return text
    sqz = ctx._SQZ_BIN if os.path.isfile(ctx._SQZ_BIN) else None
    if not sqz:
        return text
    try:
        proc = subprocess.run(
            [sqz, "compress", "--cmd", tool_name],
            input=text,
            capture_output=True,
            text=True,
            timeout=5,
        )
        return proc.stdout if proc.returncode == 0 and proc.stdout else text
    except (OSError, subprocess.SubprocessError) as exc:
        # Compression is a token optimization, never a correctness requirement:
        # if sqz is missing, unrunnable, or slower than its 5s timeout, the
        # uncompressed text is still the right answer.
        ctx.logger.debug("sqz compression skipped: %s", exc)
        return text
