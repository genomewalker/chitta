"""Native ancillary decisions, write schemas, and local memory transport."""

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

root, cli = Path(sys.argv[1]), Path(sys.argv[2])


def policy(family, **args):
    request = {
        "params": {
            "name": "ledger_op",
            "arguments": {"op": "hook_ancillary", "args": {"family": family, **args}},
        }
    }
    return json.loads(subprocess.check_output([str(cli)], input=json.dumps(request), text=True))[
        "result"
    ]["value"]


# Existing threshold (four turns), completed-set filtering and ten-session cap.
entries = [{"session_id": "done", "message_rows": 4}, {"session_id": "short", "message_rows": 3}]
entries += [{"session_id": str(i), "message_rows": 4} for i in range(12)]
plan = policy("dream-select", entries=entries, processed=["done"])
assert plan["short_sessions"] == ["short"]
assert [row["session_id"] for row in plan["candidates"]] == list(map(str, range(10)))
plan = policy("dream-synthesis", processed_count=3)
assert [row["type"] for row in plan["rows"]] == ["user", "assistant", "user", "assistant"]
assert "3 recent Claude sessions" in plan["rows"][0]["message"]["content"]

plan = policy(
    "run-ledger",
    input={"session_id": "fixture"},
    transcript="cmake --build build",
    timestamp="pinned",
)
assert "task_type:coding" in plan["queue"][0]["args"]["content"]
assert "n_wisdoms:0" in plan["queue"][0]["args"]["content"]

# Deferred writes must have the types consumed by the actual long_task_event RPC.
plan = policy("shepherd", panes=[{"task_id": "shepherd-test", "text": "Error in rule fixture"}])
event = plan["queue"][0]["args"]
assert event["task_id"] == "shepherd-test"
assert json.loads(event["payload"])["severity"] == "critical"
assert event["tags"] == ["shepherd", "poll", "snakemake_error"]
assert len(plan["alerts"]) == 1
assert not policy("bash-history", command="ls -la").get("history")
assert policy("bash-history", command="python3 run.py")["history"] == "python3 run.py"
assert not policy(
    "memory-intercept", input={"tool_input": {"file_path": "/tmp/other.md", "content": "x"}}
).get("memory_sync")
plan = policy(
    "memory-intercept",
    input={
        "tool_input": {
            "file_path": "/tmp/memory/MEMORY.md",
            "content": '# Project Notes\nA quoted "note"',
        }
    },
    timestamp="2026-09-16 00:00",
)
assert plan["memory_sync"] and "import" in plan
assert 'quoted "note"' in plan["import"]["content"]
assert plan["imported_notice"] == "[memory-intercept] Imported to chitta\n"
# Policy timeout is a single safe line, without writing the requested memory file.
with tempfile.TemporaryDirectory() as tmp:
    home = Path(tmp)
    (home / ".claude/mind").mkdir(parents=True)
    env = dict(
        os.environ, HOME=tmp, CHITTA_DB_PATH=str(home / ".claude/mind"), CHITTA_BIN="/bin/false"
    )
    target = home / "memory/MEMORY.md"
    out = subprocess.check_output(
        ["bash", str(root / "hooks/memory-intercept.sh")],
        input=json.dumps({"tool_input": {"file_path": str(target), "content": "notes"}}),
        text=True,
        env=env,
    )
    assert out == "[chitta] daemon unavailable; context not loaded.\n"
    assert not target.exists()
print(
    "ok: native sweep eligibility/synthesis, Shepherd event types, history, memory import, unavailable envelope"
)
