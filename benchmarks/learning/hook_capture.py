#!/usr/bin/env python3
"""Run a hook without suppressing its output, with a private audit copy."""

from __future__ import annotations

import json
import os
import subprocess
import sys
import time
from pathlib import Path


def main():
    event, hook = sys.argv[1:3]
    payload = sys.stdin.read()
    started = time.time_ns() // 1_000_000
    p = subprocess.run(["bash", hook], input=payload, text=True, capture_output=True)
    row = {
        "event": event,
        "started_ms": started,
        "ended_ms": time.time_ns() // 1_000_000,
        "exit_code": p.returncode,
        "stdout": p.stdout,
        "stderr": p.stderr,
        "session_id": json.loads(payload).get("session_id"),
    }
    path = Path(os.environ["LEARNING_HOOK_LOG"])
    # One O_APPEND write keeps concurrent hook events from interleaving.
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
    try:
        os.write(fd, (json.dumps(row) + "\n").encode())
    finally:
        os.close(fd)
    sys.stdout.write(p.stdout)
    sys.stderr.write(p.stderr)
    return p.returncode


if __name__ == "__main__":
    sys.exit(main())
