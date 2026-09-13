#!/usr/bin/env python3
"""Reparse Claude transcripts to backfill conversation_turn table."""

import json
import os
import sys
from pathlib import Path

_MIND = os.environ.get("MIND_PATH") or os.environ.get("CHITTA_DB_PATH") or str(Path.home() / ".claude" / "mind")
QUEUE_FILE = os.environ.get("CHITTA_QUEUE") or os.environ.get("CHITTA_QUEUE_PATH") or os.path.join(_MIND, "queue.jsonl")


def parse_transcript(filepath: Path) -> tuple[int, int]:
    """Parse a single transcript file, return (user_count, assistant_count)."""
    session_id = filepath.stem

    # Validate UUID format
    import re

    if not re.match(r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$", session_id):
        return 0, 0

    user_count = 0
    assistant_count = 0
    turn_index = 0

    with open(filepath) as f, open(QUEUE_FILE, "a") as queue:
        for line in f:
            if not line.strip():
                continue

            try:
                data = json.loads(line)
            except json.JSONDecodeError:
                continue

            msg_type = data.get("type")
            if msg_type not in ("user", "assistant"):
                continue

            turn_index += 1
            message = data.get("message", {})
            content_raw = message.get("content", "")

            # Extract text content
            if isinstance(content_raw, str):
                content = content_raw
            elif isinstance(content_raw, list):
                texts = []
                for item in content_raw:
                    if isinstance(item, dict) and item.get("type") == "text":
                        texts.append(item.get("text", ""))
                    elif isinstance(item, str):
                        texts.append(item)
                content = "\n".join(texts)
            else:
                content = ""

            # Skip empty
            if len(content) < 3:
                continue

            if msg_type == "user":
                user_count += 1
            else:
                assistant_count += 1

            # Write to queue
            entry = {
                "tool": "store_turn",
                "args": {
                    "session_id": session_id,
                    "role": msg_type,
                    "content": content,
                    "turn_index": turn_index,
                },
            }
            queue.write(json.dumps(entry) + "\n")

    return user_count, assistant_count


def main():
    print("=== Transcript Reparser ===", file=sys.stderr)
    print(f"Queue file: {QUEUE_FILE}", file=sys.stderr)

    # Clear queue
    open(QUEUE_FILE, "w").close()

    projects_dir = Path.home() / ".claude" / "projects"
    if not projects_dir.exists():
        print("No projects directory found", file=sys.stderr)
        return

    total_files = 0
    total_user = 0
    total_assistant = 0

    for project_dir in projects_dir.iterdir():
        if not project_dir.is_dir():
            continue

        for jsonl_file in project_dir.glob("*.jsonl"):
            user_count, assistant_count = parse_transcript(jsonl_file)
            if user_count > 0 or assistant_count > 0:
                print(
                    f"  {jsonl_file.stem}: {user_count} user + {assistant_count} assistant",
                    file=sys.stderr,
                )
                total_files += 1
                total_user += user_count
                total_assistant += assistant_count

    print(file=sys.stderr)
    print(f"Processed {total_files} transcripts", file=sys.stderr)
    print(f"Total: {total_user} user + {total_assistant} assistant turns", file=sys.stderr)

    queue_size = sum(1 for _ in open(QUEUE_FILE)) if os.path.exists(QUEUE_FILE) else 0
    print(f"Queue entries: {queue_size}", file=sys.stderr)
    print("\nDaemon will process queue automatically.", file=sys.stderr)


if __name__ == "__main__":
    main()
