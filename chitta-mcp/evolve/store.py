"""Small JSON CLI adapter; all writes are confined to the evolution realm."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
from typing import Any

REALM = "project:chitta-evolve"


def records(value: Any) -> list[dict]:
    if isinstance(value, list):
        return [v for v in value if isinstance(v, dict)]
    if isinstance(value, dict):
        for key in ("results", "memories", "items", "structured", "result"):
            if key in value:
                return records(value[key])
        return [value]
    return []


def body(record: dict) -> dict:
    value = record.get("content", record.get("text", record))
    if isinstance(value, str):
        try:
            value = json.loads(value)
        except ValueError:
            return {}
    return value if isinstance(value, dict) else {}


class MemoryStore:
    def __init__(self, executable: str | None = None, timeout: float = 30):
        self.executable = (
            executable
            or os.environ.get("CHITTA_BIN")
            or shutil.which("chitta")
            or os.path.expanduser("~/.claude/bin/chitta")
        )
        self.timeout = timeout
        self.remaining = None
        self._realm_records = None

    def call(self, *args: str) -> Any:
        result = subprocess.run(
            [self.executable, *args, "--json"],
            capture_output=True,
            text=True,
            timeout=min(self.timeout, self.remaining()) if self.remaining else self.timeout,
            check=True,
        )
        value = json.loads(result.stdout)
        if isinstance(value, dict) and (value.get("error") or value.get("status") == "error"):
            raise RuntimeError(str(value))
        return value

    def recall(self, tag: str, realm: str = REALM, query: str | None = None) -> list[dict]:
        if realm == REALM:
            # Top-k semantic recall is not an exhaustive experiment ledger.
            # Page the small evolution realm, retaining full JSON bodies and tags.
            if self._realm_records is None:
                self._realm_records = []
                seen = set()
                offset = 0
                while True:
                    response = self.call(
                        "list_memories_brief",
                        "--realm",
                        REALM,
                        "--limit",
                        "100",
                        "--offset",
                        str(offset),
                    )
                    if not isinstance(response, dict) or "memories" not in response:
                        self._realm_records = None
                        raise RuntimeError(
                            "evolution memory listing returned an unexpected response"
                        )
                    page = records(response)
                    for record in page:
                        if record.get("realm") != REALM or record.get("id") in seen:
                            self._realm_records = None
                            raise RuntimeError(
                                "evolution memory pagination returned a wrong realm or repeated id"
                            )
                        seen.add(record.get("id"))
                    self._realm_records.extend(page)
                    if len(page) < 100:
                        break
                    offset += len(page)
            return [r for r in self._realm_records if tag in r.get("tags", [])]
        value = self.call(
            "recall",
            "--realm",
            realm,
            "--tag",
            tag,
            "--query",
            query or tag,
            "--limit",
            "100",
            "--strategy",
            "keyword",
            "--no-learn",
        )
        # Reject an explicitly misrouted response rather than importing another realm.
        if isinstance(value, dict) and value.get("realm") not in (None, realm):
            raise RuntimeError("recall returned the wrong realm: " + str(value.get("realm")))
        if (
            not isinstance(value, (dict, list))
            or isinstance(value, dict)
            and not any(key in value for key in ("results", "memories", "items"))
        ):
            raise RuntimeError("recall returned an unexpected response shape")
        return [r for r in records(value) if r.get("realm") in (None, realm)]

    def remember(self, tag: str, value: dict) -> str:
        result = self.call(
            "remember",
            "--realm",
            REALM,
            "--type",
            "signal",
            "--tags",
            json.dumps([tag]),
            "--content",
            " " + json.dumps(value, sort_keys=True),
        )
        for record in records(result):
            ident = record.get("id", record.get("memory_id"))
            if ident is not None:
                if self._realm_records is not None:
                    self._realm_records.append(
                        dict(id=str(ident), content=json.dumps(value), realm=REALM, tags=[tag])
                    )
                return str(ident)
        raise RuntimeError("remember did not return a memory id")
