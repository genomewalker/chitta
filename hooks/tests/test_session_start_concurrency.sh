#!/bin/bash
# Frozen output, concurrent 300 ms CLI calls, and a whole-process-tree deadline.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PYTHONDONTWRITEBYTECODE=1 python3 - "$ROOT" <<'PY'
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

root = Path(sys.argv[1])
with tempfile.TemporaryDirectory(prefix="chitta-ss-test-") as temp:
    base = Path(temp)
    home = base / "home"
    mind = home / ".claude/mind"
    mind.mkdir(parents=True)
    plugin = base / "plugin/chitta-mcp"
    plugin.mkdir(parents=True)
    cli = base / "chitta"
    cli.write_text('''#!/bin/bash
printf '%s %s\\n' "$BASHPID" "$*" >>"$STUB_CALLS"
if [[ "${STUB_HANG:-}" == cli ]]; then
    trap '' TERM
    sleep 10
fi
sleep "${STUB_DELAY:-0}"
case "$1" in
realm_detect) echo project:latency ;;
ledger_load) cat "$STUB_LEDGER" ;;
soul_context) echo 'Memory: 10, 20 triplets' ;;
sql_query)
    case "$*" in
    *'FROM theme'*) echo '{"rows":[{"memory_count":4,"label":"theme"}]}' ;;
    *'GROUP BY kind'*) echo '{"rows":[{"kind":"wisdom","cnt":3}]}' ;;
    *'COUNT(*) as total'*) echo '{"rows":[{"total":10,"critical":2,"pinned":1}]}' ;;
    *) echo '{"rows":[{"id":7,"kind":"wisdom","content":"recent"}]}' ;;
    esac ;;
recall)
    case "$*" in
    *'--tag correction'*) cat "$STUB_CORRECTIONS" ;;
    *'compliance:auto'*) echo 'missed correction' ;;
    *'cache:break'*) echo 'cache warning' ;;
    *'--realm'*) cat "$STUB_SCOPED" ;;
    *) echo '#8 [80%] [wisdom] fallback answer' ;;
    esac ;;
triplet_history)
    case "$*" in
    *'--id 2 '*|*'--id 3 '*) echo '{"triplets":[{"object":"wontfix"}]}' ;;
    *) echo '{"triplets":[]}' ;;
    esac ;;
query_triplets) printf 'hedging\\nhedging\\nhedging\\n' ;;
esac
''')
    cli.chmod(0o700)
    (plugin / "task_ledger.py").write_text('''import os
import time

def render_inbox(realm, limit):
    assert realm == "project:latency" and limit == 5
    with open(os.environ["STUB_RENDER_PIDS"], "a") as out:
        out.write(str(os.getpid()) + "\\n")
    time.sleep(float(os.environ.get("STUB_DELAY", "0")))
    if os.environ.get("STUB_INBOX_FAIL"):
        raise ValueError("inbox unavailable")
    return "━━━ inbox (project:latency) ━━━\\n• inbox task"

def render_threads(realm, limit):
    assert realm == "project:latency" and limit == 3
    with open(os.environ["STUB_RENDER_PIDS"], "a") as out:
        out.write(str(os.getpid()) + "\\n")
    time.sleep(float(os.environ.get("STUB_DELAY", "0")))
    return "━━━ active threads ━━━\\n  ⟳  active task [12345678]"
''')
    (plugin / "session_registry.py").write_text('''import json
import os
import sys
import time
assert json.load(sys.stdin)["session_id"] == "concurrency-test"
if os.environ.get("STUB_HANG") == "registry":
    time.sleep(10)
else:
    time.sleep(float(os.environ.get("STUB_DELAY", "0")))
with open(os.environ["STUB_REGISTERED"], "w") as out:
    out.write("registered")
''')
    sock = socket.socket(socket.AF_UNIX)
    sock.bind(str(base / "scratch.sock"))
    env = os.environ.copy()
    for name in ("CHITTA_HEADLESS", "CC_SOUL_HEADLESS", "CHITTA_MAX_WAIT", "CC_SOUL_MAX_WAIT",
                 "CHITTA_HOOK_BUDGET_MS", "CC_SOUL_HOOK_BUDGET_MS"):
        env.pop(name, None)
    env.update(HOME=str(home), CHITTA_DB_PATH=str(mind), XDG_RUNTIME_DIR=str(base),
               CHITTA_SOCKET_PATH=str(base / "scratch.sock"), CHITTA_QUEUE=str(base / "queue"),
               CHITTA_BIN=str(cli), CHITTA_PLUGIN_DIR=str(plugin.parent),
               CC_SOUL_PLUGIN_DIR=str(plugin.parent), STUB_CALLS=str(base / "calls"),
               STUB_LEDGER=str(base / "ledger"), STUB_CORRECTIONS=str(base / "corrections"),
               STUB_SCOPED=str(base / "scoped"), STUB_RENDER_PIDS=str(base / "render-pids"),
               STUB_REGISTERED=str(base / "registered"))
    ledger = dict(session_id="previous", mood="idle", snapshot="Goal: preserve order")
    (base / "ledger").write_text(json.dumps(ledger))
    (base / "scoped").write_text("#7 [80%] [wisdom] scoped answer\n")
    corrections = [dict(id=18446744073709551614, text="keep correction α"),
                   dict(id=2, text="skip wontfix"), dict(id=3, text="skip also"),
                   dict(id=4, text="skip verified"), dict(id=5, text="skip applied", correction_state="applied")]
    (base / "corrections").write_text(json.dumps(dict(results=corrections)))
    expected = '''
━━━ inbox (project:latency) ━━━
• inbox task

━━━ active threads ━━━
  ⟳  active task [12345678]
[soul] m=10 t=20
[ledger] previous (idle)

[topology]
Themes: 4m: theme
Active: wisdom:3
Recent (project:latency):
  #7 [wisdom] recent
Total: 10 memories (2 critical, 1 pinned)
[/topology]

[recall:project:latency]
#7 [80%] [wisdom] scoped answer

[/recall:project:latency]
[soul] If context above is sparse for the current task, call mcp__chitta__recall or mcp__chitta__smart_context for deeper retrieval.

[recent-corrections]
keep correction α
[/recent-corrections]

[compliance] missed corrections

[probe] hedging×3 — direct, drop qualifiers


⚠️ BEFORE RUNNING: [cache] Recent cache break detected:
cache warning
'''

    def run(source="startup", **overrides):
        started = time.monotonic()
        result = subprocess.run(["bash", str(root / "hooks/session-start-hook.sh")],
                                input=json.dumps(dict(session_id="concurrency-test", cwd=str(root),
                                                      hook_event_name="SessionStart", source=source)),
                                text=True, capture_output=True, env=dict(env, **overrides), timeout=12)
        elapsed = time.monotonic() - started
        assert result.returncode == 0, result.stderr
        return result.stdout, elapsed

    output, elapsed = run(STUB_DELAY="0.3")
    assert output == expected, repr(output)
    assert elapsed < 1.5, f"300 ms calls serialized: {elapsed:.3f}s"
    pids = (base / "render-pids").read_text().splitlines()
    assert len(pids) == 2 and len(set(pids)) == 1, pids
    assert (base / "registered").read_text() == "registered"
    print(f"ok: 300 ms CLI calls, exact ordered output, one renderer process ({elapsed:.3f}s)")

    (base / "scoped").write_text("Found 0 results:\n")
    output, _ = run()
    assert output == expected.replace("#7 [80%] [wisdom] scoped answer", "#8 [80%] [wisdom] fallback answer")
    (base / "scoped").write_text("#7 [80%] [wisdom] scoped answer\n")
    output, _ = run(STUB_INBOX_FAIL="1")
    assert output == expected.replace("\n━━━ inbox (project:latency) ━━━\n• inbox task\n", "")
    for _ in range(2):
        output, _ = run()
        assert output == expected
    output, _ = run()
    assert output == expected.replace("\n[recent-corrections]\nkeep correction α\n[/recent-corrections]\n", "")
    assert (mind / ".correction_surfaces").read_text() == "18446744073709551614\n" * 5
    print("ok: fallback selection, renderer failure, exact IDs, tags, and five-surface suppression")

    ledger.update(snapshot="Goal: resume the task", active_files=["file.py"], decisions=["keep it"],
                  todos=[dict(status="pending", content="finish")], blockers=["blocked"],
                  discoveries=["found it"], next_steps=["next"], updated_at="2099-01-01T00:00:00Z")
    (base / "ledger").write_text(json.dumps(ledger))
    output, _ = run(source="compact")
    assert output == '''
[session-restored]
Files in context:
  - file.py

Decisions made:
  - keep it

Tasks:
  [pending] finish

Blockers:
  ! blocked

Discoveries:
  * found it

Last context:
Goal: resume the task

[/session-restored]

''', repr(output)
    ledger["mood"] = "in_progress"
    (base / "ledger").write_text(json.dumps(ledger))
    output, _ = run(source="clear")
    assert output == '''[last-session]
Previous task: resume the task
Next step: next
Active files: file.py
Saved: 2099-01-01T00:00:00Z
Run /recap for full context. [/last-session]
''', repr(output)
    print("ok: compact restoration and clear resume text unchanged")

    for hang in ("cli", "registry"):
        (base / "calls").write_text("")
        output, elapsed = run(STUB_HANG=hang, CHITTA_HOOK_BUDGET_MS="350")
        assert elapsed < 0.65, f"deadline exceeded: {hang} {elapsed:.3f}s"
        # All CLI processes, including TERM-ignoring ones, must be dead or reaped.
        for line in (base / "calls").read_text().splitlines():
            stat = Path("/proc", line.split()[0], "stat")
            try:
                assert stat.read_text().split(") ", 1)[1][0] == "Z", line
            except FileNotFoundError:
                pass
    print("ok: 350 ms global deadline bounds stalled CLI/registry and kills lane children")
    sock.close()
PY
