"""A frontend must be able to reap every helper launched by its hook."""

import os
import signal
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class HookProcessLifetime(unittest.TestCase):
    def test_background_helper_stays_in_frontend_process_group(self):
        code = (
            "import os,sys; sys.path.insert(0,sys.argv[1]); "
            "from hook_session import detached; "
            "child=detached([sys.executable,'-c','import time; time.sleep(30)']); "
            "print(child.pid,os.getpgrp(),flush=True)"
        )
        carrier = subprocess.Popen(
            [sys.executable, "-c", code, str(ROOT / "chitta-mcp")],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,
        )
        child = None
        try:
            output, error = carrier.communicate(timeout=5)
            self.assertEqual(carrier.returncode, 0, error)
            child, group = map(int, output.split())
            self.assertEqual(group, carrier.pid)
            self.assertEqual(os.getpgid(child), group)
        finally:
            # Only this freshly spawned, single-process fixture is terminated.
            if child is not None:
                try:
                    os.kill(child, signal.SIGTERM)
                except ProcessLookupError:
                    pass
            if carrier.poll() is None:
                carrier.kill()
                carrier.wait(timeout=5)


if __name__ == "__main__":
    unittest.main()
