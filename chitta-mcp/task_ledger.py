#!/usr/bin/env python3
"""
task-ledger.db manager: Threads, Inbox, Artifacts.
Pure stdlib — no external dependencies.

CLI: python3 -m task_ledger <command> [--<field> <value> ...]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sqlite3
import time
from pathlib import Path
from uuid import uuid4

DB_PATH = Path(os.environ.get("CHITTA_TASK_LEDGER", Path.home() / ".claude" / "task-ledger.db"))

_SCHEMA = """
PRAGMA journal_mode=DELETE;
PRAGMA foreign_keys=ON;
PRAGMA busy_timeout=5000;

CREATE TABLE IF NOT EXISTS threads (
    thread_id        TEXT PRIMARY KEY,
    title            TEXT NOT NULL,
    realm            TEXT NOT NULL DEFAULT '',
    status           TEXT NOT NULL DEFAULT 'active'
                         CHECK(status IN ('active','sealed','dormant')),
    topic_fingerprint TEXT,
    created_at       REAL NOT NULL,
    last_active_at   REAL NOT NULL,
    sealed_at        REAL,
    parent_thread_id TEXT REFERENCES threads(thread_id),
    metadata_json    TEXT NOT NULL DEFAULT '{}'
);

CREATE TABLE IF NOT EXISTS inbox (
    item_id         TEXT PRIMARY KEY,
    task_id         TEXT NOT NULL DEFAULT '',
    thread_id       TEXT REFERENCES threads(thread_id),
    event_type      TEXT NOT NULL,
    digest          TEXT NOT NULL DEFAULT '',
    payload_json    TEXT NOT NULL DEFAULT '{}',
    target_realm    TEXT NOT NULL DEFAULT '',
    created_at      REAL NOT NULL,
    delivery_state  TEXT NOT NULL DEFAULT 'pending'
                        CHECK(delivery_state IN ('pending','delivered','acked','suppressed')),
    delivered_at    REAL,
    acked_at        REAL
);

CREATE TABLE IF NOT EXISTS artifacts (
    artifact_id        TEXT PRIMARY KEY,
    task_id            TEXT NOT NULL DEFAULT '',
    thread_id          TEXT REFERENCES threads(thread_id),
    path               TEXT NOT NULL,
    kind               TEXT NOT NULL DEFAULT 'file',
    mtime              REAL,
    size               INTEGER,
    md5                TEXT,
    created_at         REAL NOT NULL,
    parent_artifact_id TEXT REFERENCES artifacts(artifact_id),
    metadata_json      TEXT NOT NULL DEFAULT '{}'
);

CREATE TABLE IF NOT EXISTS thread_sessions (
    session_id       TEXT PRIMARY KEY,
    thread_id        TEXT REFERENCES threads(thread_id),
    client           TEXT NOT NULL DEFAULT '',
    project_dir      TEXT NOT NULL DEFAULT '',
    transcript_path  TEXT NOT NULL DEFAULT '',
    status           TEXT NOT NULL DEFAULT 'active'
                         CHECK(status IN ('active','ended','interrupted','completed')),
    started_at       REAL NOT NULL,
    last_active_at   REAL NOT NULL,
    ended_at         REAL,
    metadata_json    TEXT NOT NULL DEFAULT '{}'
);

CREATE TABLE IF NOT EXISTS thread_leases (
    thread_id          TEXT PRIMARY KEY REFERENCES threads(thread_id),
    session_id         TEXT NOT NULL REFERENCES thread_sessions(session_id),
    generation         INTEGER NOT NULL DEFAULT 1,
    acquired_at        REAL NOT NULL,
    last_heartbeat_at  REAL NOT NULL,
    expires_at         REAL NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_inbox_state_realm  ON inbox(delivery_state, target_realm);
CREATE INDEX IF NOT EXISTS idx_inbox_task          ON inbox(task_id);
CREATE INDEX IF NOT EXISTS idx_artifacts_task      ON artifacts(task_id);
CREATE INDEX IF NOT EXISTS idx_artifacts_path      ON artifacts(path);
CREATE INDEX IF NOT EXISTS idx_threads_realm_status ON threads(realm, status);
CREATE INDEX IF NOT EXISTS idx_thread_sessions_thread ON thread_sessions(thread_id, status);
CREATE INDEX IF NOT EXISTS idx_thread_sessions_project ON thread_sessions(project_dir, last_active_at);
CREATE INDEX IF NOT EXISTS idx_thread_leases_session ON thread_leases(session_id);
"""


# Rollback journal, not WAL: WAL needs a mmap-shared -shm file, which fails on
# the NFS home with "locking protocol" (every registry call timed out 2026-08-15
# to 09-13).
def connect() -> sqlite3.Connection:
    DB_PATH.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(str(DB_PATH), timeout=10)
    conn.row_factory = sqlite3.Row
    conn.executescript(_SCHEMA)
    return conn


def _row(r) -> dict | None:
    return dict(r) if r else None


def _rows(rs) -> list[dict]:
    return [dict(r) for r in rs]


# ─── Threads ──────────────────────────────────────────────────────────────────


def thread_create(
    title: str, realm: str = "", fingerprint: str | None = None, parent: str | None = None
) -> str:
    tid = str(uuid4())
    now = time.time()
    with connect() as conn:
        conn.execute(
            "INSERT INTO threads(thread_id,title,realm,topic_fingerprint,created_at,last_active_at,parent_thread_id)"
            " VALUES(?,?,?,?,?,?,?)",
            (tid, title, realm, fingerprint, now, now, parent),
        )
    return tid


def thread_list(realm: str | None = None, status: str | None = None, limit: int = 20) -> list[dict]:
    sql = "SELECT * FROM threads WHERE 1=1"
    params: list = []
    if realm is not None:
        sql += " AND realm=?"
        params.append(realm)
    if status is not None:
        sql += " AND status=?"
        params.append(status)
    sql += " ORDER BY last_active_at DESC LIMIT ?"
    params.append(limit)
    with connect() as conn:
        return _rows(conn.execute(sql, params).fetchall())


def thread_get(thread_id: str) -> dict | None:
    with connect() as conn:
        return _row(
            conn.execute("SELECT * FROM threads WHERE thread_id=?", (thread_id,)).fetchone()
        )


def thread_seal(thread_id: str, reason: str | None = None) -> bool:
    now = time.time()
    with connect() as conn:
        cur = conn.execute(
            "UPDATE threads SET status='sealed', sealed_at=?, last_active_at=? WHERE thread_id=?",
            (now, now, thread_id),
        )
        return cur.rowcount > 0


def thread_update(thread_id: str, **fields) -> bool:
    allowed = {
        "title",
        "realm",
        "status",
        "topic_fingerprint",
        "last_active_at",
        "metadata_json",
        "parent_thread_id",
    }
    cols = {k: v for k, v in fields.items() if k in allowed}
    if not cols:
        return False
    set_clause = ", ".join(f"{k}=?" for k in cols)
    with connect() as conn:
        cur = conn.execute(
            f"UPDATE threads SET {set_clause} WHERE thread_id=?",
            [*cols.values(), thread_id],
        )
        return cur.rowcount > 0


# ─── Session/thread ownership ────────────────────────────────────────────────


def session_bind(
    session_id: str,
    thread_id: str | None = None,
    client: str = "",
    project_dir: str = "",
    transcript_path: str = "",
    status: str = "active",
    metadata: dict | None = None,
) -> bool:
    """Upsert the durable session→thread association used by both frontends."""
    if not session_id:
        return False
    now = time.time()
    with connect() as conn:
        # BEGIN IMMEDIATE (same as lease_claim): the metadata read-merge-write
        # below must not interleave with a concurrent bind for this session.
        conn.execute("BEGIN IMMEDIATE")
        existing = conn.execute(
            "SELECT metadata_json FROM thread_sessions WHERE session_id=?",
            (session_id,),
        ).fetchone()
        merged_metadata: dict = {}
        if existing:
            try:
                merged_metadata = json.loads(existing["metadata_json"] or "{}")
            except (json.JSONDecodeError, TypeError):
                merged_metadata = {}
        if metadata is not None:
            merged_metadata.update(metadata)
        conn.execute(
            "INSERT INTO thread_sessions"
            "(session_id,thread_id,client,project_dir,transcript_path,status,"
            " started_at,last_active_at,metadata_json)"
            " VALUES(?,?,?,?,?,?,?,?,?)"
            " ON CONFLICT(session_id) DO UPDATE SET"
            " thread_id=COALESCE(excluded.thread_id,thread_sessions.thread_id),"
            " client=CASE WHEN excluded.client<>'' THEN excluded.client ELSE thread_sessions.client END,"
            " project_dir=CASE WHEN excluded.project_dir<>'' THEN excluded.project_dir ELSE thread_sessions.project_dir END,"
            " transcript_path=CASE WHEN excluded.transcript_path<>'' THEN excluded.transcript_path ELSE thread_sessions.transcript_path END,"
            " status=excluded.status,last_active_at=excluded.last_active_at,"
            " ended_at=NULL,metadata_json=excluded.metadata_json",
            (
                session_id,
                thread_id,
                client,
                project_dir,
                transcript_path,
                status,
                now,
                now,
                json.dumps(merged_metadata),
            ),
        )
    return True


def session_touch(session_id: str) -> bool:
    now = time.time()
    with connect() as conn:
        cur = conn.execute(
            "UPDATE thread_sessions SET last_active_at=?,status='active',ended_at=NULL"
            " WHERE session_id=?",
            (now, session_id),
        )
        conn.execute(
            "UPDATE thread_leases SET last_heartbeat_at=?,expires_at=? WHERE session_id=?",
            (now, now + 900.0, session_id),
        )
        return cur.rowcount > 0


def session_close(session_id: str, status: str = "ended") -> bool:
    if status not in ("ended", "interrupted", "completed"):
        status = "ended"
    now = time.time()
    with connect() as conn:
        cur = conn.execute(
            "UPDATE thread_sessions SET status=?,last_active_at=?,ended_at=? WHERE session_id=?",
            (status, now, now, session_id),
        )
        conn.execute("DELETE FROM thread_leases WHERE session_id=?", (session_id,))
        return cur.rowcount > 0


def session_get(session_id: str) -> dict | None:
    with connect() as conn:
        return _row(
            conn.execute(
                "SELECT * FROM thread_sessions WHERE session_id=?", (session_id,)
            ).fetchone()
        )


def session_list(
    project_dir: str | None = None,
    status: str | None = None,
    thread_id: str | None = None,
    limit: int = 100,
) -> list[dict]:
    sql = "SELECT * FROM thread_sessions WHERE 1=1"
    params: list = []
    if project_dir is not None:
        sql += " AND project_dir=?"
        params.append(project_dir)
    if status is not None:
        sql += " AND status=?"
        params.append(status)
    if thread_id is not None:
        sql += " AND thread_id=?"
        params.append(thread_id)
    sql += " ORDER BY last_active_at DESC LIMIT ?"
    params.append(limit)
    with connect() as conn:
        return _rows(conn.execute(sql, params).fetchall())


def lease_claim(thread_id: str, session_id: str, ttl: int = 900, force: bool = False) -> dict:
    """Atomically acquire/renew exclusive ownership of a resumable thread."""
    now = time.time()
    expires = now + max(30, ttl)
    with connect() as conn:
        conn.execute("BEGIN IMMEDIATE")
        existing = conn.execute(
            "SELECT * FROM thread_leases WHERE thread_id=?", (thread_id,)
        ).fetchone()
        if (
            existing
            and existing["session_id"] != session_id
            and existing["expires_at"] > now
            and not force
        ):
            return {
                "claimed": False,
                "reason": "live_owner",
                "thread_id": thread_id,
                "owner_session_id": existing["session_id"],
                "expires_at": existing["expires_at"],
                "generation": existing["generation"],
            }
        same_owner = bool(existing and existing["session_id"] == session_id)
        generation = (
            existing["generation"]
            if same_owner
            else (existing["generation"] + 1 if existing else 1)
        )
        acquired_at = existing["acquired_at"] if same_owner else now
        conn.execute(
            "INSERT INTO thread_leases"
            "(thread_id,session_id,generation,acquired_at,last_heartbeat_at,expires_at)"
            " VALUES(?,?,?,?,?,?)"
            " ON CONFLICT(thread_id) DO UPDATE SET"
            " session_id=excluded.session_id,generation=excluded.generation,"
            " acquired_at=excluded.acquired_at,last_heartbeat_at=excluded.last_heartbeat_at,"
            " expires_at=excluded.expires_at",
            (thread_id, session_id, generation, acquired_at, now, expires),
        )
        conn.execute(
            "UPDATE thread_sessions SET thread_id=?,status='active',last_active_at=?"
            " WHERE session_id=?",
            (thread_id, now, session_id),
        )
        return {
            "claimed": True,
            "thread_id": thread_id,
            "session_id": session_id,
            "expires_at": expires,
            "generation": generation,
        }


def lease_release(session_id: str, thread_id: str | None = None) -> bool:
    sql = "DELETE FROM thread_leases WHERE session_id=?"
    params: list = [session_id]
    if thread_id:
        sql += " AND thread_id=?"
        params.append(thread_id)
    with connect() as conn:
        cur = conn.execute(sql, params)
        return cur.rowcount > 0


def lease_list(active_only: bool = True) -> list[dict]:
    now = time.time()
    sql = "SELECT * FROM thread_leases"
    params: list = []
    if active_only:
        sql += " WHERE expires_at>?"
        params.append(now)
    sql += " ORDER BY last_heartbeat_at DESC"
    with connect() as conn:
        return _rows(conn.execute(sql, params).fetchall())


# ─── Inbox ────────────────────────────────────────────────────────────────────


def inbox_push(
    task_id: str,
    event_type: str,
    digest: str,
    target_realm: str,
    thread_id: str | None = None,
    payload: dict | None = None,
) -> str:
    iid = str(uuid4())
    now = time.time()
    with connect() as conn:
        conn.execute(
            "INSERT INTO inbox(item_id,task_id,thread_id,event_type,digest,"
            "payload_json,target_realm,created_at) VALUES(?,?,?,?,?,?,?,?)",
            (
                iid,
                task_id,
                thread_id,
                event_type,
                digest,
                json.dumps(payload or {}),
                target_realm,
                now,
            ),
        )
    return iid


def inbox_list(
    target_realm: str | None = None, state: str = "pending", limit: int = 50
) -> list[dict]:
    sql = "SELECT * FROM inbox WHERE delivery_state=?"
    params: list = [state]
    if target_realm is not None:
        sql += " AND (target_realm=? OR target_realm='')"
        params.append(target_realm)
    sql += " ORDER BY created_at DESC LIMIT ?"
    params.append(limit)
    with connect() as conn:
        return _rows(conn.execute(sql, params).fetchall())


def inbox_ack(item_id: str, new_state: str = "acked") -> bool:
    now = time.time()
    col = "acked_at" if new_state == "acked" else "delivered_at"
    with connect() as conn:
        cur = conn.execute(
            f"UPDATE inbox SET delivery_state=?, {col}=? WHERE item_id=?",
            (new_state, now, item_id),
        )
        return cur.rowcount > 0


# ─── Artifacts ────────────────────────────────────────────────────────────────


def artifact_register(
    task_id: str,
    path: str,
    kind: str = "file",
    thread_id: str | None = None,
    parent_artifact_id: str | None = None,
) -> str:
    aid = str(uuid4())
    now = time.time()
    p = Path(path)
    mtime = size = md5 = None
    if p.exists():
        try:
            st = p.stat()
            mtime = st.st_mtime
            if p.is_file():
                size = st.st_size
                if size <= 16 * 1024 * 1024:
                    md5 = hashlib.md5(p.read_bytes()).hexdigest()
        except OSError:
            pass
    with connect() as conn:
        conn.execute(
            "INSERT OR REPLACE INTO artifacts"
            "(artifact_id,task_id,thread_id,path,kind,mtime,size,md5,"
            "created_at,parent_artifact_id) VALUES(?,?,?,?,?,?,?,?,?,?)",
            (aid, task_id, thread_id, str(path), kind, mtime, size, md5, now, parent_artifact_id),
        )
    return aid


def artifact_list(
    task_id: str | None = None, path_glob: str | None = None, thread_id: str | None = None
) -> list[dict]:
    sql = "SELECT * FROM artifacts WHERE 1=1"
    params: list = []
    if task_id:
        sql += " AND task_id=?"
        params.append(task_id)
    if thread_id:
        sql += " AND thread_id=?"
        params.append(thread_id)
    if path_glob:
        sql += " AND path GLOB ?"
        params.append(path_glob)
    sql += " ORDER BY created_at DESC"
    with connect() as conn:
        return _rows(conn.execute(sql, params).fetchall())


def artifact_link(child_id: str, parent_id: str) -> bool:
    with connect() as conn:
        cur = conn.execute(
            "UPDATE artifacts SET parent_artifact_id=? WHERE artifact_id=?",
            (parent_id, child_id),
        )
        return cur.rowcount > 0


def artifact_lineage(artifact_id: str) -> list[dict]:
    chain: list[dict] = []
    current: str | None = artifact_id
    seen: set[str] = set()
    with connect() as conn:
        while current and current not in seen:
            seen.add(current)
            row = conn.execute("SELECT * FROM artifacts WHERE artifact_id=?", (current,)).fetchone()
            if not row:
                break
            d = dict(row)
            chain.append(d)
            current = d.get("parent_artifact_id")
    return chain


# ─── CLI ──────────────────────────────────────────────────────────────────────


def render_inbox(realm: str = "", limit: int = 5) -> str:
    items = inbox_list(target_realm=realm, state="pending", limit=limit)
    if not items:
        return ""
    lines = [f"━━━ inbox ({realm or 'all'}) ━━━"]
    for i in items[:limit]:
        et = i.get("event_type", "")
        icon = "✓" if et == "completed" else "✗" if et in ("failed", "failure") else "•"
        lines.append(f"{icon} {i.get('digest', '')[:110]}")
    return "\n".join(lines)


def render_threads(realm: str = "", limit: int = 3) -> str:
    threads = thread_list(realm=realm, status="active", limit=limit)
    if not threads:
        return ""
    lines = ["━━━ active threads ━━━"]
    for t in threads[:limit]:
        tid = (t.get("thread_id") or "")[:8]
        lines.append(f"  ⟳  {t.get('title', '?')} [{tid}]")
    return "\n".join(lines)


def _cli() -> None:
    p = argparse.ArgumentParser(prog="task_ledger")
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("thread_create")
    s.add_argument("--title", required=True)
    s.add_argument("--realm", default="")
    s.add_argument("--fingerprint")
    s.add_argument("--parent")

    s = sub.add_parser("thread_list")
    s.add_argument("--realm")
    s.add_argument("--status")
    s.add_argument("--limit", type=int, default=20)

    s = sub.add_parser("thread_get")
    s.add_argument("--thread-id", required=True, dest="thread_id")

    s = sub.add_parser("thread_seal")
    s.add_argument("--thread-id", required=True, dest="thread_id")
    s.add_argument("--reason")

    s = sub.add_parser("thread_update")
    s.add_argument("--thread-id", required=True, dest="thread_id")
    s.add_argument("--title")
    s.add_argument("--status")
    s.add_argument("--last-active-at", type=float, dest="last_active_at")
    s.add_argument("--fingerprint")

    s = sub.add_parser("session_bind")
    s.add_argument("--session-id", required=True, dest="session_id")
    s.add_argument("--thread-id", dest="thread_id")
    s.add_argument("--client", default="")
    s.add_argument("--project-dir", default="", dest="project_dir")
    s.add_argument("--transcript-path", default="", dest="transcript_path")
    s.add_argument("--status", default="active")
    s.add_argument("--metadata")

    s = sub.add_parser("session_touch")
    s.add_argument("--session-id", required=True, dest="session_id")

    s = sub.add_parser("session_close")
    s.add_argument("--session-id", required=True, dest="session_id")
    s.add_argument("--status", default="ended")

    s = sub.add_parser("session_get")
    s.add_argument("--session-id", required=True, dest="session_id")

    s = sub.add_parser("session_list")
    s.add_argument("--project-dir", dest="project_dir")
    s.add_argument("--status")
    s.add_argument("--thread-id", dest="thread_id")
    s.add_argument("--limit", type=int, default=100)

    s = sub.add_parser("lease_claim")
    s.add_argument("--thread-id", required=True, dest="thread_id")
    s.add_argument("--session-id", required=True, dest="session_id")
    s.add_argument("--ttl", type=int, default=900)
    s.add_argument("--force", action="store_true")

    s = sub.add_parser("lease_release")
    s.add_argument("--session-id", required=True, dest="session_id")
    s.add_argument("--thread-id", dest="thread_id")

    s = sub.add_parser("lease_list")
    s.add_argument("--all", action="store_true", dest="include_expired")

    s = sub.add_parser("inbox_push")
    s.add_argument("--task-id", default="", dest="task_id")
    s.add_argument("--event-type", required=True, dest="event_type")
    s.add_argument("--digest", default="")
    s.add_argument("--target-realm", default="", dest="target_realm")
    s.add_argument("--thread-id", dest="thread_id")
    s.add_argument("--payload")

    s = sub.add_parser("inbox_list")
    s.add_argument("--target-realm", dest="target_realm")
    s.add_argument("--state", default="pending")
    s.add_argument("--limit", type=int, default=50)

    s = sub.add_parser("inbox_ack")
    s.add_argument("--item-id", required=True, dest="item_id")
    s.add_argument("--state", default="acked")

    s = sub.add_parser("artifact_register")
    s.add_argument("--task-id", default="", dest="task_id")
    s.add_argument("--path", required=True)
    s.add_argument("--kind", default="file")
    s.add_argument("--thread-id", dest="thread_id")
    s.add_argument("--parent-artifact-id", dest="parent_artifact_id")

    s = sub.add_parser("artifact_list")
    s.add_argument("--task-id", dest="task_id")
    s.add_argument("--thread-id", dest="thread_id")
    s.add_argument("--path-glob", dest="path_glob")

    s = sub.add_parser("artifact_link")
    s.add_argument("--child-id", required=True, dest="child_id")
    s.add_argument("--parent-id", required=True, dest="parent_id")

    s = sub.add_parser("artifact_lineage")
    s.add_argument("--artifact-id", required=True, dest="artifact_id")

    s = sub.add_parser("render_inbox")
    s.add_argument("--realm", default="")
    s.add_argument("--limit", type=int, default=5)

    s = sub.add_parser("render_threads")
    s.add_argument("--realm", default="")
    s.add_argument("--limit", type=int, default=3)

    args = p.parse_args()
    result: object = None

    if args.cmd == "thread_create":
        result = thread_create(args.title, args.realm, args.fingerprint, args.parent)
    elif args.cmd == "thread_list":
        result = thread_list(args.realm, args.status, args.limit)
    elif args.cmd == "thread_get":
        result = thread_get(args.thread_id)
    elif args.cmd == "thread_seal":
        result = thread_seal(args.thread_id, args.reason)
    elif args.cmd == "thread_update":
        fields: dict = {}
        if args.title:
            fields["title"] = args.title
        if args.status:
            fields["status"] = args.status
        if args.last_active_at:
            fields["last_active_at"] = args.last_active_at
        if args.fingerprint:
            fields["topic_fingerprint"] = args.fingerprint
        result = thread_update(args.thread_id, **fields)
    elif args.cmd == "session_bind":
        metadata = json.loads(args.metadata) if args.metadata else None
        result = session_bind(
            args.session_id,
            args.thread_id,
            args.client,
            args.project_dir,
            args.transcript_path,
            args.status,
            metadata,
        )
    elif args.cmd == "session_touch":
        result = session_touch(args.session_id)
    elif args.cmd == "session_close":
        result = session_close(args.session_id, args.status)
    elif args.cmd == "session_get":
        result = session_get(args.session_id)
    elif args.cmd == "session_list":
        result = session_list(args.project_dir, args.status, args.thread_id, args.limit)
    elif args.cmd == "lease_claim":
        result = lease_claim(args.thread_id, args.session_id, args.ttl, args.force)
    elif args.cmd == "lease_release":
        result = lease_release(args.session_id, args.thread_id)
    elif args.cmd == "lease_list":
        result = lease_list(not args.include_expired)
    elif args.cmd == "inbox_push":
        payload = json.loads(args.payload) if args.payload else None
        result = inbox_push(
            args.task_id, args.event_type, args.digest, args.target_realm, args.thread_id, payload
        )
    elif args.cmd == "inbox_list":
        result = inbox_list(args.target_realm, args.state, args.limit)
    elif args.cmd == "inbox_ack":
        result = inbox_ack(args.item_id, args.state)
    elif args.cmd == "artifact_register":
        result = artifact_register(
            args.task_id, args.path, args.kind, args.thread_id, args.parent_artifact_id
        )
    elif args.cmd == "artifact_list":
        result = artifact_list(args.task_id, args.path_glob, args.thread_id)
    elif args.cmd == "artifact_link":
        result = artifact_link(args.child_id, args.parent_id)
    elif args.cmd == "artifact_lineage":
        result = artifact_lineage(args.artifact_id)
    elif args.cmd == "render_inbox":
        print(render_inbox(args.realm, args.limit))
        return
    elif args.cmd == "render_threads":
        print(render_threads(args.realm, args.limit))
        return

    print(json.dumps(result, default=str))


if __name__ == "__main__":
    _cli()
