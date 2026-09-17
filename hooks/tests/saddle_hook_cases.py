"""Isolated integration cases; commands are hook input and never executed."""

from __future__ import annotations

import importlib.util
import json
import os
import statistics
import subprocess
import tempfile
import time
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("saddle", ROOT / "chitta-mcp/saddle_detector.py")
saddle = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(saddle)


def main():
    with tempfile.TemporaryDirectory(prefix="saddle-hook-") as tmp:
        base = Path(tmp)
        mind = base / "home/.claude/mind"
        mind.mkdir(parents=True)
        cli = base / "event-response"
        subprocess.run(
            [
                os.environ.get("CXX", os.environ.get("CXX", "g++")),
                "-std=c++17",
                "-O2",
                "-pthread",
                "-I" + str(ROOT / "chitta/include"),
                str(ROOT / "hooks/tests/event-response.cpp"),
                "-lcrypto",
                "-o",
                str(cli),
            ],
            check=True,
        )
        env = dict(
            os.environ,
            HOME=str(base / "home"),
            CHITTA_DB_PATH=str(mind),
            MIND_PATH=str(mind),
            XDG_RUNTIME_DIR=str(base / "run"),
            CHITTA_QUEUE=str(base / "queue"),
            CHITTA_SOCKET_PATH=str(base / "absent.sock"),
            CHITTA_PLUGIN_DIR=str(ROOT),
            CHITTA_OUTPUT_CAP="0",  # This fixture checks saddle advisories only.
            CHITTA_BIN=str(cli),
        )
        for key in ("CHITTA_HEADLESS", "CC_SOUL_HEADLESS"):
            env.pop(key, None)
        ledger = mind / "outcome_ledger.jsonl"
        now = int(time.time() * 1000)
        rows = [
            dict(
                ts=now - (3 - i) * 60000,
                session_id="test-saddle",
                event="bash_outcome",
                cmd_head=f"curl https://broken.test/retry/{i}",
                exit_code=1,
                stderr_head='curl: "connection refused"',
            )
            for i in range(3)
        ]

        def write(events):
            ledger.write_text("".join(json.dumps(e) + "\n" for e in events))

        def pre(cmd="curl https://broken.test/retry/9", session="test-saddle", extra_env=None):
            result = subprocess.run(
                ["bash", str(ROOT / "hooks/pre-tool-hook.sh"), "Bash"],
                input=json.dumps({"session_id": session, "tool_input": {"command": cmd}}),
                text=True,
                capture_output=True,
                env=extra_env or env,
                timeout=3,
            )
            assert result.returncode == 0, result.stderr
            return result.stdout

        write(rows)
        notice = json.loads(pre())["hookSpecificOutput"]
        assert "3× in 7 min" in notice["additionalContext"]
        assert "connection refused" in notice["additionalContext"]
        assert "permissionDecision" not in notice
        assert not pre().strip(), "same saddle must not repeat"
        assert not pre("git status --short").strip(), "unrelated command"
        assert not pre(session="another-session").strip(), "session isolation"
        rows.append(dict(rows[-1], ts=now - 1000, cmd_head="curl https://broken.test/retry/4"))
        write(rows)
        assert not pre().strip(), "fourth failure must retain saddle ID"

        args = SimpleNamespace(
            cmd="curl https://broken.test/retry/9",
            min_fails=3,
            similarity=0.8,
            session="test-saddle",
            minutes=7,
        )
        unknown = dict(rows[-1], ts=now, exit_code=None, likely_fail=False)
        assert saddle.open_saddle(rows + [unknown], args)
        success = dict(unknown, exit_code=0)
        assert saddle.open_saddle(rows + [success], args) is None
        codex = [dict(r, exit_code=None, likely_fail=True) for r in rows[:3]]
        assert saddle.open_saddle(codex, args)["n_fails"] == 3
        assert saddle.open_saddle([dict(r, exit_code=None) for r in rows[:3]], args) is None
        write([dict(r, ts=now - 8 * 60000) for r in rows])
        assert not pre().strip(), "expired failures"
        ledger.write_text('not JSON\n[]\n{"event":"bash_outcome"}\n')
        assert not pre().strip(), "malformed rows fail open"
        ledger.unlink()
        assert not pre().strip(), "missing ledger fails open"

        # A large irrelevant prefix must not turn each check into a full scan.
        ledger.write_bytes(
            b"x" * (2 * 1024 * 1024)
            + b"\n"
            + b"".join((json.dumps(r) + "\n").encode() for r in rows)
        )
        assert len(saddle.load_tail(ledger, "test-saddle", 7)) == 4

        # Existing code-intel advice and the saddle must form one JSON output.
        inline = 'python -c \'open("x","w")\''
        write([dict(r, cmd_head=inline) for r in rows[:3]])
        combined = json.loads(pre(inline))["hookSpecificOutput"]["additionalContext"]
        assert "[saddle]" in combined and "[code-intel]" in combined

        # Exercise Stop before its transcript early exit; no process controls/RPCs.
        # The hook's pre-existing notification cleanup is replaced by a shell stub.
        write(rows[:3])
        (mind / ".hb_test-saddle").touch()
        result = subprocess.run(
            [
                "bash",
                "-c",
                'pkill() { :; }; export -f pkill; source "$1"',
                "stop-test",
                str(ROOT / "hooks/stop-core.sh"),
            ],
            input=json.dumps({"session_id": "test-saddle"}),
            env=env,
            capture_output=True,
            text=True,
            timeout=3,
        )
        assert result.returncode == 0, result.stderr
        assert "[saddle]" in result.stderr
        events = [json.loads(line) for line in ledger.read_text().splitlines()]
        assert sum(e["event"] == "saddle" for e in events) == 1
        assert events[-1]["n_fails"] == 3

        # Native policy is mandatory. A failed/stalled RPC must not run the
        # deleted shell detector or acknowledge a notice that was never emitted.
        (mind / ".saddle_test-saddle").unlink()
        missing = dict(env, CHITTA_BIN="/bin/false")
        assert pre(extra_env=missing).strip() == "[chitta] daemon unavailable; context not loaded."
        assert not (mind / ".saddle_test-saddle").exists()
        stalled = base / "stalled-cli"
        stalled.write_text(
            '#!/bin/sh\nif [ "$1" = realm_detect ]; then echo project:test; else exec sleep 10; fi\n'
        )
        stalled.chmod(0o755)
        started = time.monotonic()
        assert (
            pre(extra_env=dict(env, CHITTA_BIN=str(stalled), CHITTA_MAX_WAIT="0.3")).strip()
            == "[chitta] daemon unavailable; context not loaded."
        )
        assert 0.28 < time.monotonic() - started < 0.8
        assert not (mind / ".saddle_test-saddle").exists()

        # Interleave baseline (missing ledger) and a populated no-saddle ledger.
        # Neither headless alias is set; measure the complete PreToolUse process.
        timings = {"baseline": [], "no_saddle": []}
        for _ in range(25):
            for arm in timings:
                if arm == "baseline":
                    ledger.unlink(missing_ok=True)
                else:
                    write([dict(r, exit_code=0) for r in rows])
                started = time.perf_counter()
                assert not pre().strip()
                timings[arm].append((time.perf_counter() - started) * 1000)
        stats = {
            arm: {
                "median_ms": round(statistics.median(values), 2),
                "p95_ms": round(sorted(values)[23], 2),
            }
            for arm, values in timings.items()
        }
        stats["incremental_median_ms"] = round(
            stats["no_saddle"]["median_ms"] - stats["baseline"]["median_ms"], 2
        )
        # The task budgets added overhead; report full-hook tails separately.
        added = [on - off for on, off in zip(timings["no_saddle"], timings["baseline"])]
        stats["incremental_p95_ms"] = round(sorted(added)[23], 2)
        # Relative bounds only: absolute hook wall time tracks shared-node load
        # (170 ms median at load 105 on 2026-09-15 with a 59 ms baseline).
        assert stats["incremental_p95_ms"] < 150, stats
        assert stats["incremental_median_ms"] < 120, stats
        print("PreToolUse 25 runs/arm: " + json.dumps(stats))
        print(
            "PASS: saddle notices, dedupe, time/session/shape gates, Codex unknowns, "
            "Stop telemetry, malformed/bounded ledger, native policy transport, timeout, latency"
        )


if __name__ == "__main__":
    main()
