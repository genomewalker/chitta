"""Byte-for-byte Bash/jq vs Python reference saddle checks on isolated ledgers."""

from __future__ import annotations

import contextlib
import fcntl
import importlib.util
import io
import json
import os
import random
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
    now = int(time.time() * 1000) - 10000
    cmd = "curl https://broken.test/retry/9"
    rows = [
        dict(ts=now - (3 - i) * 1000, session_id="parity", event="bash_outcome",
             cmd_head=f"curl https://broken.test/retry/{i}", exit_code=1,
             stderr_head='curl: "refused"\ntry again')
        for i in range(3)
    ]

    def encode(events):
        return "".join(json.dumps(r, ensure_ascii=False) + "\n" for r in events).encode()

    cases = [
        ("open", rows, cmd, {}, True),
        ("closed", rows + [dict(rows[-1], ts=now, exit_code=0)], cmd, {}, False),
        ("string-zero", rows + [dict(rows[-1], ts=now, exit_code="0")], cmd, {}, False),
        ("dedup-suppressed", rows, cmd, {"dedup": True}, False),
        ("codex-failing", [dict(r, exit_code=None, likely_fail=True) for r in rows], cmd, {}, True),
        ("codex-unknown", [dict(r, exit_code=None) for r in rows], cmd, {}, False),
        ("unknown-does-not-close", rows + [dict(rows[-1], ts=now, exit_code=None)], cmd, {}, True),
        ("unrelated", rows, "git status --short", {}, False),
        ("wrong-session", [dict(r, session_id="other") for r in rows], cmd, {}, False),
        ("no-session", rows, cmd, {"session": ""}, False),
        ("invalid-session", rows, cmd, {"session": "../parity"}, False),
        ("old", [dict(r, ts=now - 8 * 60000) for r in rows], cmd, {}, False),
        ("future", [dict(r, ts=now + 60000) for r in rows], cmd, {}, False),
        ("shape-gates", [dict(r, cmd_head=None) for r in rows] + [dict(r, cmd_head="\t ") for r in rows], cmd, {}, False),
        ("malformed", b'not JSON\n[]\nnull\n42\n{"event":"bash_outcome"}\n' + encode(rows), cmd, {}, True),
        ("partial-last", encode(rows)[:-1], cmd, {}, False),
        ("bounded-bytes", encode(rows) + b"x" * (1024 * 1024) + b"\n", cmd, {}, False),
        ("large-prefix", b"x" * (2 * 1024 * 1024) + b"\n" + encode(rows), cmd, {}, True),
        ("bounded-lines", encode(rows) + b"{}\n" * 4096, cmd, {}, False),
        ("last-lines", b"{}\n" * 5000 + encode(rows), cmd, {}, True),
        ("sort-timestamps", list(reversed(rows)), cmd, {}, True),
        ("interleaved", [r for row in rows for r in (row, dict(row, cmd_head="git status", exit_code=0))], cmd, {}, True),
        ("normalization", [dict(r, cmd_head="  CURL\tHTTPS://broken.test/retry/123\n") for r in rows], cmd, {}, True),
        ("sixty-characters", [dict(r, cmd_head="x" * 60 + str(i)) for i, r in enumerate(rows)], "x" * 60 + "different", {}, True),
        ("near-similar", [dict(r, cmd_head="abcdefghij" + x) for r, x in zip(rows, ("aa", "ab", "ac"))], "abcdefghijad", {}, True),
        ("threshold-equal", [dict(r, cmd_head="abcde") for r in rows], "abcdf", {}, True),
        ("threshold-below", [dict(r, cmd_head="abcde") for r in rows], "abcxy", {}, False),
        ("sequence-not-edit-distance", [dict(r, cmd_head="tide") for r in rows], "diet", {"similarity": 0.4}, False),
        ("sequence-asymmetric", [dict(r, cmd_head="diet") for r in rows], "tide", {"similarity": 0.4}, True),
        ("unicode", [dict(r, cmd_head="ÉCHO\u001cΩΣ\t１２３", stderr_head="é\t💥\n" * 80) for r in rows], "écho ως ٩", {}, True),
        ("unicode-expansion", [dict(r, cmd_head="İΣ") for r in rows], "i\u0307ς", {}, True),
        ("fallback-excerpt", [dict(r, stderr_head=None) for r in rows], cmd, {}, True),
        ("invalid-exits", [dict(r, exit_code="bad") for r in rows], cmd, {}, False),
        ("numeric-string", [dict(r, exit_code=" -1_0 ") for r in rows], cmd, {}, True),
        ("false-closes", rows + [dict(rows[-1], ts=now, exit_code=False)], cmd, {}, False),
        ("custom-window-threshold", rows, cmd, {"minutes": 2.5, "min_fails": 4}, False),
        ("locked-marker", rows, cmd, {"locked": True}, False),
    ]
    for size in (1048576, 1048577):
        prefix = b"\n" if size > 1048576 else b""
        payload = prefix + encode(rows)
        payload += b"x" * (size - len(payload) - 1) + b"\n"
        cases.append((f"exact-byte-boundary-{size}", payload, cmd, {}, size == 1048576))
    tied = [dict(r, cmd_head=c) for c in ("abcde", "abxyz") for r in rows]
    cases.append(("candidate-tie", tied, "abcxy", {"similarity": 0.6}, True))
    cases.append(("close-multiple", tied + [dict(rows[-1], ts=now, cmd_head="abcxy", exit_code=0)],
                  "abcxy", {"similarity": 0.6}, False))
    # Differential cases stress SequenceMatcher tie-breaking and anchored groups.
    rng = random.Random(9182)
    for i in range(40):
        events = [dict(r, cmd_head="".join(rng.choices("abcde ", k=12))) for r in rows]
        cases.append((f"sequence-{i}", events, events[-1]["cmd_head"], {"similarity": 0.3}, None))

    with tempfile.TemporaryDirectory(prefix="saddle-parity-") as tmp:
        base = Path(tmp)
        env = dict(os.environ, HOME=str(base), CHITTA_BIN="/bin/true",
                   CHITTA_SOCKET_PATH=str(base / "absent.sock"))
        for key in ("CHITTA_HEADLESS", "CC_SOUL_HEADLESS"):
            env.pop(key, None)
        for name, events, command, options, expected in cases:
            case = base / name
            py = case / "python"
            sh = case / "shell"
            py.mkdir(parents=True)
            sh.mkdir()
            ledger = case / "ledger"
            ledger.write_bytes(events if isinstance(events, bytes) else encode(events))
            sid = options.get("session", "parity")
            args = SimpleNamespace(session=sid, cmd=command, min_fails=options.get("min_fails", 3),
                                   minutes=options.get("minutes", 7), similarity=options.get("similarity", 0.8),
                                   notice_file=py / ".saddle_parity")
            filtered = saddle.load_tail(ledger, sid, args.minutes)
            if options.get("dedup"):
                marker = saddle.open_saddle(filtered, args)["saddle_id"] + "\n"
                args.notice_file.write_text(marker)
                (sh / ".saddle_parity").write_text(marker)
            with contextlib.ExitStack() as stack:
                if options.get("locked"):
                    for marker in (args.notice_file, sh / ".saddle_parity"):
                        handle = stack.enter_context(marker.open("a+"))
                        fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
                out = io.StringIO()
                with contextlib.redirect_stdout(out):
                    try:
                        status = saddle.check(filtered, args) if sid else 2
                    except OSError:
                        status = 2
                result = subprocess.run(
                    ["bash", "-c", 'source "$1"; shift; saddle_check "$@"', "parity",
                     str(ROOT / "hooks/lib.sh"),
                     json.dumps({"session_id": sid, "tool_input": {"command": command}}),
                     str(ledger), str(sh), str(args.minutes), str(args.min_fails), str(args.similarity)],
                    env=env, capture_output=True, timeout=3,
                )
            assert result.stdout == out.getvalue().encode(), (name, result.stdout, out.getvalue(), result.stderr)
            assert (result.returncode == 0) == (status == 0), (name, result.returncode, status, result.stderr)
            if expected is not None:
                assert bool(result.stdout) == expected, name
            for marker in py.iterdir():
                assert (sh / marker.name).read_bytes() == marker.read_bytes(), name
        print(f"PASS: {len(cases)} saddle parity ledgers (notice bytes, status, dedupe marker)")


if __name__ == "__main__":
    main()
