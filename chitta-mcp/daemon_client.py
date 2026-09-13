"""Shared stdlib daemon transports; safe to import from PyPy hooks without MCP."""

from __future__ import annotations

import _thread
import contextvars
import functools
import itertools
import json
import os
import socket
import time


def _debug(message, *args):
    # Successful hook calls need neither logging nor threading's higher-level
    # machinery. MCP callers still use the same named logger on failures.
    import logging

    logging.getLogger("chitta-mcp").debug(message, *args)


def djb2_hash(s: str) -> int:
    """DJB2 hash algorithm matching C++ implementation."""
    h = 5381
    for c in s:
        h = ((h << 5) + h + ord(c)) & 0xFFFFFFFF
    return h


def get_socket_dir() -> str:
    """Get persistent socket directory (matches C++ daemon logic)."""
    # XDG_RUNTIME_DIR is session-scoped and managed by systemd
    xdg_runtime = os.environ.get("XDG_RUNTIME_DIR")
    if xdg_runtime and os.access(xdg_runtime, os.W_OK):
        socket_dir = os.path.join(xdg_runtime, "chitta")
        os.makedirs(socket_dir, mode=0o700, exist_ok=True)
        return socket_dir
    run_user = f"/run/user/{os.getuid()}"
    if os.access(run_user, os.W_OK):
        socket_dir = os.path.join(run_user, "chitta")
        os.makedirs(socket_dir, mode=0o700, exist_ok=True)
        return socket_dir
    # Fall back to ~/.cache/chitta (persistent, user-owned)
    home = os.environ.get("HOME")
    if home:
        cache_dir = os.path.join(home, ".cache")
        os.makedirs(cache_dir, mode=0o755, exist_ok=True)
        socket_dir = os.path.join(cache_dir, "chitta")
        os.makedirs(socket_dir, mode=0o700, exist_ok=True)
        return socket_dir
    # Last resort: /tmp
    return "/tmp"


def get_socket_path() -> str:
    """Get the daemon socket path."""
    explicit = os.environ.get("CHITTA_SOCKET_PATH")
    if explicit:
        return explicit
    home = os.environ.get("HOME", "")
    mind_path = (
        os.environ.get("MIND_PATH")
        or os.environ.get("CHITTA_DB_PATH")
        or os.environ.get("CHITTA_MIND")
        or os.path.join(home, ".claude", "mind")
    )
    hash_val = djb2_hash(mind_path)
    return os.path.join(get_socket_dir(), f"chitta-{hash_val}.sock")


class ChittaClient:
    """Client for communicating with chittad daemon."""

    def __init__(self, socket_path: str, timeout: float = 30.0):
        self.timeout = timeout
        self.socket_path = socket_path
        self.sock: socket.socket | None = None
        # One socket shared by the 4-worker executor: the lock keeps two
        # threads from interleaving sendall/recv and reading each other's
        # responses; monotonic ids let us detect a desynced connection.
        self.lock = _thread.allocate_lock()
        self._ids = itertools.count(1)

    def connect(self) -> bool:
        """Connect to daemon socket."""
        try:
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.sock.settimeout(self.timeout)
            self.sock.connect(self.socket_path)
            return True
        except OSError as exc:
            # No daemon listening yet, stale socket file, or wrong permissions.
            # ensure_daemon() treats False as "spawn or wait", so this is a
            # normal startup state, not an error worth surfacing.
            _debug("daemon socket connect failed (%s): %s", self.socket_path, exc)
            self.close()
            return False

    def call(self, req: dict) -> str | None:
        """Send one JSON-RPC request and return the matching response line.

        Assigns a fresh monotonic id, serializes socket access, and verifies
        the response id. A mismatched id means a stale response from a prior
        timed-out request is still in the pipe — the connection is desynced,
        so it is dropped and the caller's reconnect-once logic takes over.
        """
        with self.lock:
            if not self.sock:
                return None
            req_id = next(self._ids)
            req["id"] = req_id
            try:
                deadline = time.monotonic() + self.timeout
                self.sock.settimeout(self.timeout)
                self.sock.sendall((json.dumps(req) + "\n").encode())
                buf = b""
                while b"\n" not in buf:
                    self.sock.settimeout(max(0.001, deadline - time.monotonic()))
                    if time.monotonic() >= deadline:
                        return None
                    chunk = self.sock.recv(65536)
                    if not chunk:
                        return None
                    buf += chunk
                line, _, _ = buf.partition(b"\n")
                response = line.decode().strip()
                try:
                    resp_id = json.loads(response).get("id")
                    # id None is legal (e.g. parse-error responses); anything
                    # else must echo our id.
                    if resp_id is not None and resp_id != req_id:
                        self.sock.close()
                        self.sock = None
                        return None
                except (ValueError, AttributeError):
                    pass
                return response
            except (OSError, UnicodeDecodeError) as exc:
                # Broken pipe, timeout, or an undecodable frame. Returning None
                # is the caller's signal to drop the connection and retry once
                # (see daemon_call), so do not raise through the lock.
                _debug("daemon socket call failed: %s", exc)
                return None

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                # Already closed or reset by the daemon; the handle is being
                # discarded either way.
                pass
            self.sock = None


class ChittaHttpClient:
    """HTTP client for chittad — survives daemon restarts (no persistent connection)."""

    def __init__(self, base_url: str, timeout: float = 60.0):
        self.timeout = timeout
        self.base_url = base_url
        self._ids = itertools.count(1)
        self.lock = _thread.allocate_lock()

    def connect(self) -> bool:
        import urllib.request
        from http.client import HTTPException

        try:
            urllib.request.urlopen(
                urllib.request.Request(
                    self.base_url + "/",
                    data=b'{"jsonrpc":"2.0","id":0,"method":"tools/call","params":{"name":"health_check","arguments":{}}}',
                    headers={"Content-Type": "application/json"},
                ),
                timeout=5,
            )
            return True
        except (OSError, HTTPException) as exc:
            # Daemon not listening on the RPC port yet. ensure_daemon() falls
            # through to its spawn-and-wait path on False.
            _debug("daemon HTTP health probe failed (%s): %s", self.base_url, exc)
            return False

    def call(self, req: dict) -> str | None:
        import urllib.request
        from http.client import HTTPException

        with self.lock:
            req_id = next(self._ids)
            req["id"] = req_id
            try:
                r = urllib.request.urlopen(
                    urllib.request.Request(
                        self.base_url + "/",
                        data=json.dumps(req).encode(),
                        headers={"Content-Type": "application/json"},
                    ),
                    timeout=self.timeout,
                )
                return r.read().decode()
            except (OSError, HTTPException, UnicodeDecodeError) as exc:
                # Same contract as the Unix-socket client: None means "retry
                # once with a fresh connection".
                _debug("daemon HTTP call failed: %s", exc)
                return None

    def close(self):
        pass  # stateless — nothing to close


# Hooks make one attempt, never start a daemon, and suppress follow-on waits
# after a failure. Long-lived MCP clients retain their own connection policy.
_unavailable_until = 0.0
# Remaining daemon-wait budget for one hook operation, in seconds. Only time
# spent waiting on the daemon counts: a wall-clock deadline let the transcript
# glob on NFS (~70 ms) plus PyPy warm-up exhaust the budget before the first
# RPC, so every SessionStart registration failed.
_budget = contextvars.ContextVar("chitta_hook_budget", default=None)
HOOK_BUDGET_S = float(os.environ.get("CHITTA_HOOK_RPC_BUDGET_S", "0.100"))


def hook_budget(fn):
    """Bound cumulative daemon waits within one hook lifecycle operation."""

    @functools.wraps(fn)
    def wrapped(*args, **kwargs):
        token = _budget.set([HOOK_BUDGET_S])
        try:
            return fn(*args, **kwargs)
        finally:
            _budget.reset(token)

    return wrapped


def daemon_call(tool_name: str, arguments: dict, timeout: float = 0.075):
    """Return structured data or None, with a bounded hook transport budget."""
    global _unavailable_until
    if time.monotonic() < _unavailable_until:
        return None
    budget = _budget.get()
    if budget is not None:
        timeout = min(timeout, budget[0])
        if timeout <= 0:
            return None
    started = time.monotonic()
    try:
        return _daemon_call(tool_name, arguments, timeout, started)
    finally:
        if budget is not None:
            budget[0] -= time.monotonic() - started


def _daemon_call(tool_name: str, arguments: dict, timeout: float, started: float):
    global _unavailable_until
    transport = None
    try:
        port = os.environ.get("CHITTA_RPC_PORT")
        if port and not os.environ.get("CHITTA_SOCKET_PATH"):
            host = os.environ.get("CHITTA_RPC_HOST", "127.0.0.1")
            transport = ChittaHttpClient(f"http://{host}:{port}", timeout=timeout)
        else:
            transport = ChittaClient(get_socket_path(), timeout=timeout)
        if isinstance(transport, ChittaClient) and not transport.connect():
            _unavailable_until = time.monotonic() + 0.5
            return None
        transport.timeout = max(0.001, timeout - (time.monotonic() - started))
        request = {
            "jsonrpc": "2.0",
            "method": "tools/call",
            "params": {"name": tool_name, "arguments": arguments},
        }
        if isinstance(transport, ChittaHttpClient):
            import threading

            # urllib's timeout is per socket operation, not a total deadline;
            # DNS and a trickling body may outlive it. A daemon worker bounds
            # the hook's wait and cannot keep the hook process alive at exit.
            done = threading.Event()
            replies = []

            def request_http():
                try:
                    replies.append(transport.call(request))
                finally:
                    done.set()

            threading.Thread(target=request_http, daemon=True).start()
            response = replies[0] if done.wait(timeout) and replies else None
        else:
            response = transport.call(request)
        if not response:
            _unavailable_until = time.monotonic() + 0.5
            return None
        data = json.loads(response)
        result = data.get("result", {})
        if data.get("error") or result.get("isError"):
            _debug("daemon RPC rejected %s: %s", tool_name, data)
            return None
        structured = result.get("structured")
        if isinstance(structured, dict) and structured.get("status") == "warming_up":
            return None
        return structured
    except (OSError, ValueError, TypeError, AttributeError):
        _unavailable_until = time.monotonic() + 0.5
        return None
    finally:
        if transport is not None:
            transport.close()
