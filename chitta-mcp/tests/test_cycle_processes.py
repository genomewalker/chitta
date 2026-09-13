"""Real subprocess regression tests; every Codex executable here is a local stub."""

from __future__ import annotations

import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from types import SimpleNamespace

from evolve.cycle import Budget, implement_command


class CycleProcessTests(unittest.TestCase):
    def make_stub(self, root, body):
        stub = root / "codex"
        stub.write_text(f"#!{sys.executable}\n" + body)
        stub.chmod(0o755)
        return dict(os.environ, PATH=str(root) + os.pathsep + os.environ["PATH"])

    def assert_stopped(self, pid):
        status = Path(f"/proc/{pid}/stat")
        for _ in range(100):
            try:
                if status.read_text().split()[2] == "Z":
                    return
            except FileNotFoundError:
                return
            time.sleep(0.01)
        self.fail(f"process {pid} still running")

    def test_final_message_then_sleep_reaps_group_and_closes_stdin(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root / "final.txt"
            env = self.make_stub(
                root,
                """import os, signal, subprocess, sys, time
from pathlib import Path
assert sys.stdin.read() == ''
child = subprocess.Popen([sys.executable, '-c', 'import signal,time; signal.signal(signal.SIGTERM, signal.SIG_IGN); time.sleep(30)'])
Path('pids').write_text(f'{os.getpid()} {child.pid}')
Path(sys.argv[sys.argv.index('-o') + 1]).write_text('final answer')
signal.signal(signal.SIGTERM, signal.SIG_IGN)
time.sleep(30)
""",
            )
            command = implement_command(
                SimpleNamespace(implementer="codex", model="gpt-6-astra"), root, "stub task", output
            )
            started = time.monotonic()
            result = Budget(0.1).run(command, root, env=env, final_output=output)
            self.assertEqual(result.returncode, 0)
            self.assertLess(time.monotonic() - started, 3)
            self.assertEqual(output.read_text(), "final answer")
            for pid in (root / "pids").read_text().split():
                self.assert_stopped(int(pid))

    def test_budget_timeout_cleans_group_and_ignores_stale_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root / "final.txt"
            output.write_text("stale")
            env = self.make_stub(
                root,
                """import os, signal, time
from pathlib import Path
Path('pid').write_text(str(os.getpid()))
signal.signal(signal.SIGTERM, signal.SIG_IGN)
time.sleep(30)
""",
            )
            started = time.monotonic()
            with self.assertRaisesRegex(TimeoutError, "budget exhausted"):
                Budget(0.01).run(["codex"], root, env=env, final_output=output)
            self.assertLess(time.monotonic() - started, 2)
            self.assert_stopped(int((root / "pid").read_text()))
            self.assertFalse(output.exists())

    def test_parent_exit_still_kills_descendant_and_preserves_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            env = self.make_stub(
                root,
                """import subprocess, sys
from pathlib import Path
child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'])
Path('pid').write_text(str(child.pid))
sys.exit(7)
""",
            )
            with self.assertRaises(subprocess.CalledProcessError) as caught:
                Budget(0.1).run(["codex"], root, env=env)
            self.assertEqual(caught.exception.returncode, 7)
            self.assert_stopped(int((root / "pid").read_text()))

    def test_cleanup_when_polling_raises(self):
        # Signal behavior is exercised above; exceptions must also reach finally.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaises(subprocess.CalledProcessError):
                Budget(0.1).run(
                    [sys.executable, "-c", f"import os; os.kill(os.getpid(), {signal.SIGTERM})"],
                    root,
                )
