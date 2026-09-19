"""Exercise release through the actual handler on an explicitly selected replica."""
import json
import os
import subprocess
import uuid

assert os.environ.get("CHITTA_STREAM_TEST_REPLICA") == "1"
assert os.environ.get("CHITTA_SOCKET_PATH")
cli = os.environ["CHITTA_BIN"]


def rpc(op, **args):
    request = {"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {
        "name": "ledger_op", "arguments": {"op": op, "args": args}}}
    result = json.loads(subprocess.run([cli], input=json.dumps(request), text=True,
                                      capture_output=True, check=True, timeout=30).stdout)["result"]
    assert not result.get("isError"), result
    return result["structured"]["value"]


for prefixed in (False, True):
    name, sid = "p20-" + str(uuid.uuid4()), str(uuid.uuid4())
    args = dict(stream=name, session_id=sid, worktree=os.getcwd(), branch="test", title="release fixture")
    release_name = "stream:" + name if prefixed else name
    try:
        assert rpc("stream_claim", **args)["claimed"]
        assert not rpc("stream_release", stream=release_name, session_id="competitor")["released"]
        assert rpc("stream_list", stream=name)["claims"]
        assert rpc("stream_release", stream=release_name, session_id=sid)["released"]
        assert not rpc("stream_list", stream=name)["claims"]
        rpc("session_touch", session_id=sid)
        assert not rpc("stream_list", stream=name)["claims"]
        assert not rpc("stream_release", stream=release_name, session_id=sid)["released"]
        assert rpc("stream_claim", **args)["claimed"]
        assert rpc("lease_release", thread_id="stream:" + name, session_id=sid)
    finally:
        rpc("lease_release", thread_id="stream:" + name, session_id=sid)
print("PASS: bare/prefixed owner release, wrong owner, repeated release, no heartbeat resurrection, raw lease parity")
