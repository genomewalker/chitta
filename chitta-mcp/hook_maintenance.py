"""Dream sweep file discovery and distill_now transport; selection is native."""

import fcntl
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from hook_ancillary import realm
from hook_client import read_text


def run(client):
    ledger = Path(__file__).resolve().parents[1] / "scripts/token-ledger.py"
    if ledger.is_file():
        subprocess.Popen(
            [
                sys.executable,
                str(ledger),
                "--json",
                "--cache-daily",
                str(client.state / "token-ledger.json"),
            ],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
    client.mind = Path(os.environ.get("MIND", str(client.mind)))
    client.mind.mkdir(parents=True, exist_ok=True)
    with (client.mind / ".dream_sweep.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return
        processed = client.mind / ".dream_sweep_processed"
        cutoff = processed.stat().st_mtime if processed.exists() else 0
        entries = []
        for path in sorted((Path.home() / ".claude/projects").rglob("*.jsonl")):
            if path.stat().st_mtime <= cutoff:
                continue
            message_rows = 0
            with path.open() as transcript:
                for line in transcript:
                    try:
                        row = json.loads(line)
                        if (
                            isinstance(row, dict)
                            and row.get("type") in ("user", "assistant")
                            and isinstance(row.get("message"), dict)
                        ):
                            message_rows += 1
                    except ValueError:
                        continue
            entries.append(
                {"session_id": path.stem, "path": str(path), "message_rows": message_rows}
            )
        plan = client.policy(
            "hook_ancillary",
            family="dream-select",
            entries=entries,
            processed=read_text(processed).splitlines(),
        )
        client.apply(plan)
        with processed.open("a") as acknowledgements:
            for sid in plan["short_sessions"]:
                acknowledgements.write(sid + "\n")
            count = 0
            for candidate in plan["candidates"]:
                path = Path(candidate["path"])
                project = Path(path.parent.name.replace("-", "/"))
                target = realm(client, project) if project.is_dir() else "brahman"
                client.rpc(
                    "distill_now",
                    {
                        "session_id": candidate["session_id"],
                        "transcript_path": str(path),
                        "realm": target,
                    },
                )
                acknowledgements.write(candidate["session_id"] + "\n")
                acknowledgements.flush()
                count += 1
        if count:
            plan = client.policy("hook_ancillary", family="dream-synthesis", processed_count=count)
            client.apply(plan)
            with tempfile.NamedTemporaryFile(mode="w", suffix=".jsonl") as transcript:
                for row in plan["rows"]:
                    transcript.write(json.dumps(row) + "\n")
                transcript.flush()
                client.rpc(
                    "distill_now",
                    {
                        "session_id": "dream-sweep-synthesis-" + time.strftime("%Y%m%d"),
                        "transcript_path": transcript.name,
                        "realm": "brahman",
                    },
                )
