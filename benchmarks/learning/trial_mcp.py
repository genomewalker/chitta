"""Private stdio MCP identity bridge. Recall stays in the instrumented hooks.

No model-callable memory tools: exposing uninstrumented recall would invalidate
injection counts. The sole configured server connects only to the trial broker.
"""

from __future__ import annotations

import json
import os
import sys

from common import RPC


def main():
    RPC(os.environ["CHITTA_SOCKET_PATH"]).call("health_check")
    for line in sys.stdin:
        request = json.loads(line)
        if "id" not in request:
            continue
        method = request.get("method")
        if method == "initialize":
            result = {
                "protocolVersion": request["params"]["protocolVersion"],
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "chitta-trial-hooks", "version": "1"},
            }
        elif method == "tools/list":
            result = {"tools": []}
        elif method == "ping":
            result = {}
        else:
            print(
                json.dumps(
                    {
                        "jsonrpc": "2.0",
                        "id": request["id"],
                        "error": {"code": -32601, "message": "trial hooks only"},
                    }
                ),
                flush=True,
            )
            continue
        print(json.dumps({"jsonrpc": "2.0", "id": request["id"], "result": result}), flush=True)


if __name__ == "__main__":
    main()
