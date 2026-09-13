"""Small, synchronous streamable-HTTP MCP client (stdlib, Python 3.9+)."""

from __future__ import annotations

import argparse
import json
import os
import re
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

MAX_RESPONSE = 8 * 1024 * 1024


class BridgeError(RuntimeError):
    """Transport, protocol, or tool failure; credentials are never included."""


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def tool_text(result: dict) -> str:
    """Unwrap MCP text blocks, or a structured result when provided."""
    if result.get("structuredContent") is not None:
        return json.dumps(result["structuredContent"], ensure_ascii=False)
    return "\n".join(
        item["text"] for item in result.get("content", []) if item.get("type") == "text"
    )


def json_object(text: str) -> dict:
    """Accept JSON, a fenced JSON object, or a bridge session footer, never prose guesses."""
    value = text.strip()
    if value.startswith("```"):
        value = value.split("\n", 1)[1]
    obj, end = json.JSONDecoder().raw_decode(value)
    suffix = value[end:].strip()
    if suffix.startswith("```"):
        suffix = suffix[3:].strip()
    if suffix and not re.fullmatch(r"\(Codex session: [^()]+\)", suffix):
        raise ValueError("Unexpected text after JSON object")
    if not isinstance(obj, dict):
        raise ValueError("Expected a JSON object")
    return obj


class BridgeClient:
    def __init__(
        self,
        url: str = "http://127.0.0.1:7681/mcp",
        *,
        token: str | None = None,
        token_file: Path | None = None,
        timeout: float = 120,
    ):
        parsed = urllib.parse.urlsplit(url)
        if parsed.scheme not in ("http", "https") or not parsed.hostname or parsed.username:
            raise ValueError("Expected an HTTP(S) MCP endpoint without embedded credentials")
        if timeout <= 0:
            raise ValueError("timeout must be positive")
        self.url = url
        self.timeout = timeout
        if token is None:
            token = os.environ.get("CHITTA_BRIDGE_TOKEN", "").strip()
            if not token:
                path = token_file or Path.home() / ".chitta-bridge" / "token"
                try:
                    token = path.read_text().strip()
                except OSError:
                    raise BridgeError(
                        "No bridge token; set CHITTA_BRIDGE_TOKEN or --token-file"
                    ) from None
        self._token = token
        self._opener = urllib.request.build_opener(_NoRedirect)
        self._next_id = 0
        self.session_id = None
        self.protocol = "2024-11-05"
        self.initialized = False

    def _request(self, method: str, params: dict, *, notification: bool = False):
        self._next_id += 1
        request_id = self._next_id
        payload = {"jsonrpc": "2.0", "method": method, "params": params}
        if not notification:
            payload["id"] = request_id
        body = json.dumps(payload).encode()
        headers = {
            "Content-Type": "application/json",
            "Accept": "application/json, text/event-stream",
            "Authorization": "Bearer " + self._token,
        }
        if self.session_id:
            headers["Mcp-Session-Id"] = self.session_id
        if self.initialized:
            headers["MCP-Protocol-Version"] = self.protocol
        deadline = time.monotonic() + self.timeout
        try:
            for attempt in range(2):
                req = urllib.request.Request(self.url, data=body, headers=headers, method="POST")
                try:
                    response = self._opener.open(req, timeout=self.timeout)
                    break
                except urllib.error.HTTPError as exc:
                    target = urllib.parse.urljoin(self.url, exc.headers.get("Location", ""))
                    code = exc.code
                    exc.close()
                    # Only the bridge's slash redirect is permitted; preserve POST and auth.
                    if code in (307, 308) and attempt == 0 and target == self.url.rstrip("/") + "/":
                        self.url = target
                        continue
                    raise BridgeError(f"Bridge HTTP status {code}") from None
            with response:
                self.session_id = response.headers.get("Mcp-Session-Id", self.session_id)
                if notification:
                    return None
                if "text/event-stream" in response.headers.get("Content-Type", ""):
                    message = self._sse(response, request_id, deadline)
                else:
                    raw = response.read(MAX_RESPONSE + 1)
                    if len(raw) > MAX_RESPONSE:
                        raise BridgeError("Bridge response exceeds size limit")
                    message = json.loads(raw)
        except (OSError, ValueError, urllib.error.URLError):
            raise BridgeError("Bridge transport failure, timeout, or invalid JSON") from None
        if not isinstance(message, dict) or message.get("id") != request_id:
            raise BridgeError("MCP response id mismatch")
        if "error" in message:
            raise BridgeError("MCP RPC error")
        result = message.get("result")
        if not isinstance(result, dict):
            raise BridgeError("MCP result is not an object")
        return result

    @staticmethod
    def _sse(response, request_id: int, deadline: float) -> dict:
        data = []
        size = 0
        while True:
            if time.monotonic() >= deadline:
                raise BridgeError("Bridge SSE deadline exceeded")
            raw = response.readline(MAX_RESPONSE - size + 1)
            size += len(raw)
            if size > MAX_RESPONSE:
                raise BridgeError("Bridge SSE response exceeds size limit")
            line = raw.decode("utf-8").rstrip("\r\n")
            if line.startswith("data:"):
                data.append(line[5:].lstrip(" "))
            if not line and data:
                message = json.loads("\n".join(data))
                data = []
                if isinstance(message, dict) and message.get("id") == request_id:
                    return message
            if not raw:
                raise BridgeError("SSE ended before matching MCP response")

    def initialize(self) -> dict:
        result = self._request(
            "initialize",
            {
                "protocolVersion": self.protocol,
                "capabilities": {},
                "clientInfo": {"name": "chitta-evolve", "version": "1.0"},
            },
        )
        self.protocol = result.get("protocolVersion", self.protocol)
        self.initialized = True
        self._request("notifications/initialized", {}, notification=True)
        return result

    def list_tools(self) -> list:
        if not self.initialized:
            self.initialize()
        items = []
        params = {}
        cursors = set()
        while True:
            result = self._request("tools/list", params)
            items.extend(result.get("tools", []))
            cursor = result.get("nextCursor")
            if not cursor:
                return items
            if cursor in cursors:
                raise BridgeError("Repeated tools/list cursor")
            cursors.add(cursor)
            params = {"cursor": cursor}

    def call_tool(self, name: str, arguments: dict) -> dict:
        if not self.initialized:
            self.initialize()
        result = self._request("tools/call", {"name": name, "arguments": arguments})
        if result.get("isError"):
            raise BridgeError("Bridge tool failed: " + name)
        return result


def add_client_args(parser: argparse.ArgumentParser, timeout: float = 120) -> None:
    parser.add_argument("--url", default="http://127.0.0.1:7681/mcp")
    parser.add_argument("--token-file", type=Path)
    parser.add_argument("--timeout", type=float, default=timeout)


def client_from_args(args) -> BridgeClient:
    return BridgeClient(args.url, token_file=args.token_file, timeout=args.timeout)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    add_client_args(parser)
    parser.add_argument("--probe", action="store_true", required=True)
    parser.add_argument("--fetch", help="Also run read-only web_fetch for this URL")
    args = parser.parse_args()
    client = client_from_args(args)
    try:
        print(json.dumps({"tool_count": len(client.list_tools())}), flush=True)
        if args.fetch:
            fetched = tool_text(
                client.call_tool("web_fetch", {"url": args.fetch, "max_chars": 2000})
            )
            print(fetched)
            if fetched.startswith(("Error:", "[error", "(curl fallback: HTTP")):
                return 1
    except BridgeError as exc:
        parser.exit(1, str(exc) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
