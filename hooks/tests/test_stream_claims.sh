#!/usr/bin/env bash
# Integration only: never fall back to the live daemon.
set -euo pipefail
if [[ ${CHITTA_STREAM_TEST_REPLICA:-0} != 1 || -z ${CHITTA_SOCKET_PATH:-} ]]; then
    echo 'SKIP: stream claims require an explicitly selected replica'; exit 0
fi
PY=$("$(dirname "$0")/../../scripts/python-with-mcp.sh")
"$PY" - <<'PYTEST'
import json, os, subprocess, time, uuid
cli = os.environ["CHITTA_BIN"]
name = "p12-test-" + str(uuid.uuid4())
sid = str(uuid.uuid4())
def rpc(op, **args):
    req = {"jsonrpc":"2.0", "id":1, "method":"tools/call", "params":{"name":"ledger_op", "arguments":{"op":op,"args":args}}}
    p = subprocess.run([cli], input=json.dumps(req), text=True, capture_output=True, check=True)
    value = json.loads(p.stdout)["result"]
    assert not value.get("isError"), value
    return value["structured"]["value"]
a = dict(stream=name,session_id=sid,worktree=os.getcwd(),branch="test",title="fixture")
try:
    claim = rpc("stream_claim", **a)
    assert claim["claimed"] and claim["expires_at"] - time.time() > 21590
    assert not rpc("stream_claim", **dict(a,session_id="competitor"))["claimed"]
    assert not rpc("stream_release",stream=name,session_id="competitor")["released"]
    rpc("session_touch",session_id=sid)
    lease = rpc("stream_list",stream=name)["claims"][0]
    assert lease["expires_at"] - time.time() > 21590
    result = rpc("stream_handoff",stream=name,session_id=sid,content=f"[handoff] stream={name} step=test gates=pass")
    assert result["lease"]["expires_at"] - time.time() > 21590
    assert rpc("stream_release",stream=name,session_id=sid)["released"]
    assert not rpc("stream_list",stream=name)["claims"]
    assert rpc("stream_claim", **a)["claimed"]
    print("PASS: exclusive claim, owner release, six-hour heartbeat/handoff renewal, reclaim")
finally:
    rpc("stream_release",stream=name,session_id=sid)
PYTEST
