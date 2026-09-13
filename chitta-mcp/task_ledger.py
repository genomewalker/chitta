#!/usr/bin/env python3
"""Daemon-owned threads, inbox, artifacts, session bindings and leases.

Pure stdlib; SQLite is imported only by the explicit read-only migration command.
"""

from __future__ import annotations

import json
from pathlib import Path

from daemon_client import daemon_call


def _new_id() -> str:
    # UUID support pulls in hashing/SSL on PyPy; only create operations need it.
    from uuid import uuid4

    return str(uuid4())


def _rpc(op: str, args: dict, default=None):
    result = daemon_call("ledger_op", {"op": op, "args": args})
    return result.get("value", default) if isinstance(result, dict) else default


def _list(op: str, args: dict, limit: int | None = None) -> list[dict]:
    if limit == 0:
        return []
    rows = []
    while True:
        page_args = dict(
            args, limit=100 if limit is None or limit < 0 else min(100, limit - len(rows))
        )
        page = _rpc(op, page_args)
        if not isinstance(page, dict):
            return []  # Never present a partial list as a complete result.
        rows.extend(page["rows"])
        if not page.get("after") or (limit is not None and limit >= 0 and len(rows) >= limit):
            return rows
        args = dict(args, after=page["after"])


def thread_create(
    title: str, realm: str = "", fingerprint: str | None = None, parent: str | None = None
) -> str:
    return _rpc("thread_create", dict(locals(), id=_new_id()))


def thread_list(realm: str | None = None, status: str | None = None, limit: int = 20) -> list[dict]:
    return _list("thread_list", locals(), limit)


def thread_get(thread_id: str) -> dict | None:
    return _rpc("thread_get", locals(), None)


def thread_seal(thread_id: str, reason: str | None = None) -> bool:
    return _rpc("thread_seal", locals(), False)


def thread_update(thread_id: str, **fields) -> bool:
    return _rpc("thread_update", locals(), False)


def session_bind(
    session_id: str,
    thread_id: str | None = None,
    client: str = "",
    project_dir: str = "",
    transcript_path: str = "",
    status: str = "active",
    metadata: dict | None = None,
) -> bool:
    return _rpc("session_bind", locals(), False)


def session_touch(session_id: str) -> bool:
    return _rpc("session_touch", locals(), False)


def session_close(session_id: str, status: str = "ended") -> bool:
    return _rpc("session_close", locals(), False)


def session_get(session_id: str) -> dict | None:
    return _rpc("session_get", locals(), None)


def session_list(
    project_dir: str | None = None,
    status: str | None = None,
    thread_id: str | None = None,
    limit: int = 100,
) -> list[dict]:
    return _list("session_list", locals(), limit)


def lease_claim(thread_id: str, session_id: str, ttl: int = 900, force: bool = False) -> dict:
    return _rpc("lease_claim", locals(), {})


def lease_release(session_id: str, thread_id: str | None = None) -> bool:
    return _rpc("lease_release", locals(), False)


def lease_list(active_only: bool = True) -> list[dict]:
    return _list("lease_list", locals(), None)


def inbox_push(
    task_id: str,
    event_type: str,
    digest: str,
    target_realm: str,
    thread_id: str | None = None,
    payload: dict | None = None,
) -> str:
    return _rpc("inbox_push", dict(locals(), id=_new_id()))


def inbox_list(
    target_realm: str | None = None, state: str = "pending", limit: int = 50
) -> list[dict]:
    return _list("inbox_list", locals(), limit)


def inbox_ack(item_id: str, new_state: str = "acked") -> bool:
    return _rpc("inbox_ack", locals(), False)


def artifact_register(
    task_id: str,
    path: str,
    kind: str = "file",
    thread_id: str | None = None,
    parent_artifact_id: str | None = None,
) -> str:
    args = dict(locals(), id=_new_id())
    import hashlib

    p = Path(path)
    args.update(path=str(path), mtime=None, size=None, md5=None)
    try:
        st = p.stat()
        args["mtime"] = st.st_mtime
        if p.is_file():
            args["size"] = st.st_size
            if st.st_size <= 16 * 1024 * 1024:
                args["md5"] = hashlib.md5(p.read_bytes()).hexdigest()
    except OSError:
        pass
    return _rpc("artifact_register", args)


def artifact_list(
    task_id: str | None = None, path_glob: str | None = None, thread_id: str | None = None
) -> list[dict]:
    return _list("artifact_list", locals(), None)


def artifact_link(child_id: str, parent_id: str) -> bool:
    return _rpc("artifact_link", locals(), False)


def artifact_lineage(artifact_id: str) -> list[dict]:
    rows, seen = [], set()
    while artifact_id and artifact_id not in seen:
        page = _rpc("artifact_lineage", {"artifact_id": artifact_id})
        if not isinstance(page, dict):
            return []
        for row in page["rows"]:
            if row["artifact_id"] in seen:
                return rows
            seen.add(row["artifact_id"])
            rows.append(row)
        artifact_id = page.get("next_id")
    return rows


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


def migrate(source: str) -> dict:
    """Import all legacy rows read-only; existing daemon keys always win."""
    import sqlite3
    from contextlib import closing

    counts = {}
    inserted = {}
    uri = Path(source).expanduser().resolve().as_uri() + "?mode=ro"
    with closing(sqlite3.connect(uri, uri=True)) as conn:
        conn.row_factory = sqlite3.Row
        conn.execute("PRAGMA query_only=ON")
        conn.execute("BEGIN")
        for table in ("threads", "thread_sessions", "thread_leases", "inbox", "artifacts"):
            counts[table] = inserted[table] = 0
            exists = conn.execute(
                "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", (table,)
            ).fetchone()
            if not exists:
                continue
            for row in conn.execute(f"SELECT * FROM {table}"):
                result = _rpc("import", {"table": table, "row": dict(row)})
                if result is None:
                    raise RuntimeError(
                        f"daemon import failed at {table} row {counts[table] + 1}; safe to rerun"
                    )
                counts[table] += 1
                inserted[table] += bool(result)
    return {"rows": counts, "inserted": inserted}


def _cli() -> None:
    import argparse

    p = argparse.ArgumentParser(prog="task_ledger")
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("migrate")
    s.add_argument("--from", required=True, dest="source")

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

    if args.cmd == "migrate":
        result = migrate(args.source)
    elif args.cmd == "thread_create":
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
