"""SessionStart production policy against frozen cards and envelope boundaries."""

import json
import os
import subprocess
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="chitta-session-native-") as temporary:
    base = Path(temporary)
    binary = base / "policy"
    subprocess.run(
        [
            os.environ.get("CXX", "g++"),
            "-std=c++17",
            "-O2",
            "-pthread",
            "-I" + str(ROOT / "chitta/include"),
            str(ROOT / "hooks/tests/event-response.cpp"),
            "-lcrypto",
            "-o",
            str(binary),
        ],
        check=True,
    )
    home = base / "home"
    mind = home / ".claude/mind"
    mind.mkdir(parents=True)
    plugin = base / "plugin/chitta-mcp"
    plugin.mkdir(parents=True)
    fixture = base / "fixture.json"
    cli = base / "cli"
    cli.write_text(
        '#!/bin/bash\nif [[ "${1:-}" == realm_detect ]]; then echo project:latency; else exec "$NATIVE_FIXTURE"; fi\n'
    )
    cli.chmod(0o700)
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("CHITTA_", "CC_SOUL_", "HOOK_"))
    }
    env.update(
        HOME=str(home),
        CHITTA_DB_PATH=str(mind),
        CHITTA_BIN=str(cli),
        CHITTA_QUEUE=str(base / "queue"),
        CHITTA_PLUGIN_DIR=str(plugin.parent),
        NATIVE_FIXTURE=str(binary),
        HOOK_POLICY_FIXTURE=str(fixture),
    )

    def result(text="", **structured):
        return dict(
            text=text,
            structured=dict(
                structured, **({"text": text} if text and "text" not in structured else {})
            ),
        )

    data = {
        "_ledger": dict(session_id="previous", mood="idle", snapshot="Goal: preserve order"),
        "_inbox": [dict(digest="inbox task")],
        "_threads": [dict(title="active task", thread_id="12345678-long")],
        "soul_context": result("Memory: 10, 20 triplets"),
        "sql_query.themes": result(rows=[dict(memory_count=4, label="theme")]),
        "sql_query.kinds": result(rows=[dict(kind="wisdom", cnt=3)]),
        "sql_query.counts": result(rows=[dict(total=10, critical=2, pinned=1)]),
        "sql_query.recent": result(rows=[dict(id=7, kind="wisdom", content="recent")]),
        "recall.scoped": result("#7 [80%] [wisdom] scoped answer\n"),
        "recall": result("#8 [80%] [wisdom] fallback answer"),
        "recall.correction": result(
            results=[
                dict(id=18446744073709551614, text="keep correction α"),
                dict(id=2, text="skip wontfix"),
                dict(id=3, text="skip also"),
                dict(id=4, text="skip verified"),
                dict(id=5, text="skip applied", correction_state="applied"),
            ]
        ),
        "triplet_history.2": result(history=[dict(object="wontfix")]),
        "triplet_history.3": result(history=[dict(object="wontfix")]),
        "recall.compliance:auto user correction": result("missed correction"),
        "recall.cache:break session cache_hit_ratio": result("cache warning"),
        "query_triplets": result("hedging\nhedging\nhedging\n"),
        "ledger_op.session_get": result(value=dict(thread_id="prior-thread")),
    }
    expected = (ROOT / "hooks/tests/session-start-expected.txt").read_text()
    # The old synthetic transport advertised SQL/probe RPCs that do not exist
    # in the shipped daemon. Keep the historical bytes visible, and remove
    # exactly those dead blocks; real-replica output is the parity authority.
    start = expected.index("\n[topology]\n")
    end = expected.index("[/topology]\n", start) + len("[/topology]\n")
    expected = expected[:start] + expected[end:]
    expected = expected.replace("\n[probe] hedging×3 — direct, drop qualifiers\n\n", "")

    def run(source="startup", **overrides):
        fixture.write_text(json.dumps(data))
        start = time.monotonic()
        proc = subprocess.run(
            ["bash", str(ROOT / "hooks/session-start-hook.sh")],
            input=json.dumps(
                dict(
                    session_id="native-session",
                    cwd=str(ROOT),
                    source=source,
                    model="model-α",
                    transcript_path="/tmp/transcript with spaces-α.jsonl",
                )
            ),
            text=True,
            capture_output=True,
            env=env | overrides,
            timeout=10,
            check=True,
        )
        return proc, time.monotonic() - start

    output, elapsed = run()
    assert output.stdout == expected, repr(output.stdout)
    records = [json.loads(line) for line in (base / "queue").read_text().splitlines()]
    registered = next(row["args"] for row in records if row["tool"] == "session_register")
    assert registered["project_dir"] == str(ROOT) and registered["realm"] == "project:latency"
    assert (
        registered["metadata"]["thread_id"] == "prior-thread"
        and registered["metadata"]["model"] == "model-α"
    )
    assert registered["transcript_path"] == "/tmp/transcript with spaces-α.jsonl"
    assert any(
        row["tool"] == "ledger_op"
        and row["args"]
        == dict(op="lease_claim", args=dict(session_id="native-session", thread_id="prior-thread"))
        for row in records
    )
    print("ok: frozen SessionStart bytes, u64 corrections, queued registration/lease/transcript")
    for raw in (
        "Found 2 results:\nFound 1 results:\n",
        "[weak: no strong matches]\nFound 0 results:\n",
        "#7 [80%] [wisdom]   \n",
        "#7 [80%] [episode] ignored body\n",
    ):
        data["recall.scoped"] = result(raw)
        output, _ = run()
        assert output.stdout == expected.replace(
            "#7 [80%] [wisdom] scoped answer", "#8 [80%] [wisdom] fallback answer"
        )
    assert (mind / ".correction_surfaces").read_text() == "18446744073709551614\n" * 5
    output, _ = run()
    assert "[recent-corrections]" not in output.stdout
    print("ok: empty/header/episode fallbacks, five-surface suppression")
    for source in ("compact", "clear"):
        data["_ledger"].update(
            snapshot="Goal: resume the task",
            active_files=["file.py"],
            decisions=["keep it"],
            todos=[dict(status="pending", content="finish")],
            blockers=["blocked"],
            discoveries=["found it"],
            next_steps=["next"],
            updated_at="2099-01-01T00:00:00Z",
            mood="in_progress",
        )
        output, _ = run(source)
        assert ("[session-restored]" if source == "compact" else "[last-session]") in output.stdout
        assert "file.py" in output.stdout and "[topology]" not in output.stdout
    print("ok: compact/clear cards retained")
    output, elapsed = run("compact", HOOK_FIXTURE_DELAY_MS="200")
    assert "[session-restored]" in output.stdout and elapsed < 1.5, (output, elapsed)
    print("ok: independent delayed reads stay concurrent")
    # A native transport that ignores TERM and forks cannot outlive the hook budget.
    cli.write_text(
        '#!/bin/bash\nif [[ "${1:-}" == realm_detect ]]; then echo project:latency; else trap "" TERM; sleep 10; fi\n'
    )
    output, elapsed = run(CHITTA_HOOK_BUDGET_MS="250")
    assert (
        output.stdout == "[chitta] daemon unavailable; context not loaded.\n" and elapsed < 0.65
    ), (output, elapsed)
    print("ok: timeout emits one line and bounds the CLI process group")
