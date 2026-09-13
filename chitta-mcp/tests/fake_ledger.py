"""In-process RPC fake for inference/selection tests; no filesystem or daemon."""

from __future__ import annotations

import copy
import json
import time
from contextlib import contextmanager
from unittest.mock import patch


class FakeLedger:
    def __init__(self):
        self.threads = {}
        self.sessions = {}
        self.leases = {}

    def __call__(self, tool, params):
        assert tool == "ledger_op"
        return {"value": copy.deepcopy(self.run(params["op"], params["args"]))}

    def run(self, op, a):
        now = time.time()
        if op == "thread_create":
            tid = a["id"]
            self.threads[tid] = dict(
                thread_id=tid,
                title=a["title"],
                realm=a["realm"],
                status="active",
                topic_fingerprint=a["fingerprint"],
                created_at=now,
                last_active_at=now,
                sealed_at=None,
                parent_thread_id=a["parent"],
                metadata_json="{}",
            )
            return tid
        if op == "thread_get":
            return self.threads.get(a["thread_id"])
        if op == "thread_update":
            row = self.threads.get(a["thread_id"])
            if row is None:
                return False
            row.update(a["fields"])
            return True
        if op == "session_bind":
            sid = a["session_id"]
            if not sid:
                return False
            row = self.sessions.setdefault(
                sid,
                dict(
                    session_id=sid,
                    thread_id=None,
                    client="",
                    project_dir="",
                    transcript_path="",
                    status="active",
                    started_at=now,
                    last_active_at=now,
                    ended_at=None,
                    metadata_json="{}",
                ),
            )
            for key in ("thread_id", "client", "project_dir", "transcript_path"):
                if a.get(key):
                    row[key] = a[key]
            metadata = json.loads(row["metadata_json"])
            metadata.update(a.get("metadata") or {})
            row.update(
                status=a["status"],
                last_active_at=now,
                ended_at=None,
                metadata_json=json.dumps(metadata),
            )
            return True
        if op == "session_get":
            return self.sessions.get(a["session_id"])
        if op in ("session_close", "session_touch"):
            row = self.sessions.get(a["session_id"])
            close = op == "session_close"
            if row:
                row.update(
                    status=a.get("status", "ended") if close else "active",
                    last_active_at=now,
                    ended_at=now if close else None,
                )
            for tid, lease in list(self.leases.items()):
                if lease["session_id"] == a["session_id"]:
                    if close:
                        del self.leases[tid]
                    else:
                        lease.update(expires_at=now + 900, last_heartbeat_at=now)
            return row is not None
        if op == "lease_claim":
            tid, sid = a["thread_id"], a["session_id"]
            old = self.leases.get(tid)
            if old and old["session_id"] != sid and old["expires_at"] > now and not a["force"]:
                return dict(
                    claimed=False,
                    reason="live_owner",
                    thread_id=tid,
                    owner_session_id=old["session_id"],
                    expires_at=old["expires_at"],
                    generation=old["generation"],
                )
            same = old and old["session_id"] == sid
            gen = old["generation"] + (0 if same else 1) if old else 1
            lease = dict(
                thread_id=tid,
                session_id=sid,
                generation=gen,
                acquired_at=old["acquired_at"] if same else now,
                last_heartbeat_at=now,
                expires_at=now + max(30, a["ttl"]),
            )
            self.leases[tid] = lease
            self.sessions[sid].update(thread_id=tid, status="active", last_active_at=now)
            return dict(
                claimed=True,
                thread_id=tid,
                session_id=sid,
                expires_at=lease["expires_at"],
                generation=gen,
            )
        if op == "lease_release":
            ids = [
                tid
                for tid, lease in self.leases.items()
                if lease["session_id"] == a["session_id"]
                and (not a["thread_id"] or tid == a["thread_id"])
            ]
            for tid in ids:
                del self.leases[tid]
            return bool(ids)
        if op in ("thread_list", "session_list", "lease_list"):
            table = {
                "thread_list": self.threads,
                "session_list": self.sessions,
                "lease_list": self.leases,
            }[op]
            rows = [
                r
                for r in table.values()
                if all(
                    a.get(k) is None or r.get(k) == a[k]
                    for k in ("realm", "status", "project_dir", "thread_id")
                )
            ]
            if op == "lease_list" and a["active_only"]:
                rows = [r for r in rows if r["expires_at"] > now]
            rows.sort(
                key=lambda r: r.get("last_active_at", r.get("last_heartbeat_at", 0)), reverse=True
            )
            return {"rows": rows[: a["limit"]], "after": None}
        raise AssertionError(f"fake does not implement {op}")


@contextmanager
def fake_ledger():
    with patch("task_ledger.daemon_call", FakeLedger()):
        yield
