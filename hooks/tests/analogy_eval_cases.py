"""Verify scoring, explicit socket routing, and errors without any real RPC."""

from __future__ import annotations

import importlib.util
import json
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("analogy_eval", ROOT / "benchmarks/analogy/run.py")
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)


def main():
    assert runner.matches({"id": 17481394094545567813}, {"id": "17481394094545567813"})
    assert not runner.matches({"id": 17481394094545567812}, {"id": "17481394094545567813"})
    assert runner.matches({"answer": "OPLOG"}, {"answer": "OpLog"})
    assert not runner.matches({"answer": "wrong OpLog"}, {"answer": "OpLog"})
    with tempfile.TemporaryDirectory(prefix="analogy-eval-") as tmp:
        cli = Path(tmp) / "chitta"
        cli.write_text(
            "#!/usr/bin/env python3\n"
            + "import json,os,sys,time\n"
            + "assert sys.argv[1:5] == ['--socket-path','/tmp/eval-only.sock','recall_analogy','--json']\n"
            + "assert os.environ['CHITTA_SOCKET_PATH'] == '/tmp/eval-only.sock'\n"
            + "mode=sys.argv[sys.argv.index('--mode')+1]\n"
            + "if mode == 'timeout': time.sleep(5)\n"
            + "if mode == 'error': sys.exit(3)\n"
            + "if mode == 'invalid': print('bad JSON'); sys.exit()\n"
            + "print(json.dumps({'results':[{'answer':'wrong'},{'answer':'OpLog'}]}))\n"
        )
        cli.chmod(0o700)
        task = dict(
            id="fixture",
            style="proportional",
            params=dict(mode="proportional"),
            expected=[dict(answer="OpLog")],
        )
        row = runner.evaluate(task, str(cli), "/tmp/eval-only.sock")
        assert row["hit_at_3"] and not row["hit_at_1"], row
        rows = [row]
        for mode in ("error", "invalid", "timeout"):
            task["params"]["mode"] = mode
            row = runner.evaluate(task, str(cli), "/tmp/eval-only.sock")
            assert row["error"] and not row["hit_at_3"] and not row["results"], row
            rows.append(row)
        assert rows[-1]["latency_ms"] < 2500
        summary = runner.summarize(rows)
        assert summary["hit_at_3"] == 0.25 and summary["errors"] == 3
    tasks = json.loads((ROOT / "benchmarks/analogy/tasks.json").read_text())["tasks"]
    assert len(tasks) == len({t["id"] for t in tasks}) == 14
    assert all(t["grounding"] and t["expected"] and t["style"] == "proportional" for t in tasks)
    print(
        "PASS: analogy exact scoring, uint64 IDs, explicit socket, error/timeout misses, 14 grounded proportional tasks"
    )


if __name__ == "__main__":
    main()
