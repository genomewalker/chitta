"""Session, realm, transcript, and ledger session context.

Implementations receive the server runtime explicitly as ``ctx`` so the public
server facade owns shared state and dependency overrides across all entry points.
"""

from __future__ import annotations

import json
import os
import subprocess


def inject_message_session(ctx, tool: str, args: dict) -> str | None:
    """Stamp the calling session onto a messaging call, mutating `args` in place.

    Returns an error string when the session cannot be resolved, otherwise None.
    Fail closed rather than forwarding: the daemon would fall back to its own
    get_session_id(), which reads chittad's environment and not the caller's, and
    would silently store and read messages under session_id="".

    Both entry points to the daemon — the `advanced` gateway and call_tool —
    need this, so it lives in one place.
    """
    if tool not in ctx.MSG_TOOLS or args.get("session_id"):
        return None
    # use_cache=False forces a fresh PPID lookup, which is what makes this
    # correct across a session resume.
    sid = ctx.get_current_session_id(use_cache=False)
    if not sid:
        return (
            "Error: could not determine this session's ID for messaging. "
            "Pass session_id explicitly, or call session_register first."
        )
    args["session_id"] = sid
    # msg_send names the same value sender_session_id.
    if tool == "msg_send" and not args.get("sender_session_id"):
        args["sender_session_id"] = sid
        if not args.get("sender_realm"):
            realm = ctx.get_current_realm()
            if realm:
                args["sender_realm"] = realm
    return None


def get_current_session_id(ctx, use_cache: bool = True) -> str | None:
    """
    Get current session ID using multiple detection strategies.

    Order of precedence:
    1. CLAUDE_SESSION_ID environment variable (most reliable if set)
    2. PPID lookup in session_registry (find session where pid = our parent)
    3. Cached current_session_id (from session_register or transcript_register)
    4. Single active session fallback (if exactly one exists)

    Note: Sidecar file removed - unreliable with multiple concurrent sessions.
    Note: use_cache=False forces fresh PPID lookup (use for messaging tools).
    """

    # 1. Check environment variable (most reliable)
    env_session = os.environ.get("CLAUDE_SESSION_ID")

    # 2. PPID lookup - query session_registry for our parent process
    # Always perform this lookup to enable validation against env_session
    ppid = os.getppid()
    ppid_session = None
    sessions = []
    try:
        result = ctx.daemon_call("session_list", {"active_only": True}, structured=True)
        data = json.loads(result)
        sessions = data.get("sessions", [])

        # Look for session matching our PPID
        for s in sessions:
            if s.get("pid") == ppid:
                ppid_session = s.get("session_id")
                break
    except (json.JSONDecodeError, KeyError, TypeError):
        pass

    # Validate: warn if env_session != ppid_session (potential stale env or wrong context)
    if env_session and ppid_session and env_session != ppid_session:
        ctx.logger.warning(
            f"Session mismatch: env={env_session} ppid={ppid_session} (pid={ppid}). "
            f"Using env_session. This may indicate stale CLAUDE_SESSION_ID."
        )

    # Return env_session if set (primary source of truth)
    if env_session:
        ctx.current_session_id = env_session
        return ctx.current_session_id

    # Return PPID-detected session if found
    if ppid_session:
        ctx.current_session_id = ppid_session
        return ctx.current_session_id

    # 3. Return cached session if set (and cache allowed)
    if use_cache and ctx.current_session_id:
        return ctx.current_session_id

    # 4. Single active session fallback
    try:
        if not sessions:
            result = ctx.daemon_call("session_list", {"active_only": True}, structured=True)
            data = json.loads(result)
            sessions = data.get("sessions", [])
        if len(sessions) == 1:
            ctx.current_session_id = sessions[0].get("session_id")
            return ctx.current_session_id
    except (json.JSONDecodeError, KeyError, TypeError):
        pass

    return None


def get_current_realm(ctx) -> str | None:
    """
    Get current realm using multiple detection strategies.

    Order of precedence:
    1. Cached current_realm (from previous detection)
    2. CHITTA_REALM environment variable
    3. .cc-soul-realm file in current directory
    4. Git repository name (becomes project:<repo-name>)
    """

    # 1. Return cached realm if set
    if ctx.current_realm:
        return ctx.current_realm

    # 2. CHITTA_REALM env var
    env_realm = os.environ.get("CHITTA_REALM")
    if env_realm:
        ctx.current_realm = env_realm
        return ctx.current_realm

    # 3. .cc-soul-realm file
    try:
        realm_file = os.path.join(os.getcwd(), ".cc-soul-realm")
        if os.path.exists(realm_file):
            with open(realm_file) as f:
                realm = f.read().strip()
                if realm:
                    ctx.current_realm = realm
                    return ctx.current_realm
    except OSError as exc:
        # Deleted cwd, unreadable file, or undecodable bytes: fall through to
        # the git-repo strategy rather than failing realm detection outright.
        ctx.logger.debug(".cc-soul-realm unreadable: %s", exc)

    # 4. Git repo name
    try:
        result = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True, timeout=2
        )
        if result.returncode == 0 and result.stdout.strip():
            repo_name = os.path.basename(result.stdout.strip())
            ctx.current_realm = f"project:{repo_name}"
            return ctx.current_realm
    except (OSError, subprocess.SubprocessError) as exc:
        # git missing, not a repo, or slower than the 2s timeout. No realm is a
        # valid answer — callers only inject a realm when one is detected.
        ctx.logger.debug("git realm detection failed: %s", exc)

    return None


def handle_transcript_search(ctx, arguments: dict) -> str:
    """
    Search transcript content directly in Python (fast, no daemon).
    Keyword-based search - ranks by keyword density.
    """
    query = arguments.get("query", "")
    session_id = arguments.get("session_id", "")
    limit = int(arguments.get("limit", 10))

    if not query:
        return "Error: query parameter required"

    # Extract keywords (3+ chars)
    keywords = [w.lower() for w in query.split() if len(w) >= 3]
    if not keywords:
        return "Error: query must contain words with 3+ characters"

    # Get transcript path(s)
    transcript_paths = []
    if session_id:
        # Get specific session's transcript
        result = ctx.daemon_call("transcript_get", {"session_id": session_id}, structured=True)
        try:
            data = json.loads(result)
            if "transcript_path" in data:
                transcript_paths.append((session_id, data["transcript_path"]))
        except (json.JSONDecodeError, AttributeError, TypeError) as exc:
            # Daemon returned an error string rather than structured JSON.
            ctx.logger.debug("transcript_get gave no usable path for %s: %s", session_id, exc)
    else:
        # Get all pending transcripts
        result = ctx.daemon_call("transcript_list", {}, structured=True)
        try:
            data = json.loads(result)
            for t in data.get("transcripts", []):
                transcript_paths.append((t.get("session_id", ""), t.get("transcript_path", "")))
        except (json.JSONDecodeError, AttributeError, TypeError) as exc:
            ctx.logger.debug("transcript_list gave no usable paths: %s", exc)

    if not transcript_paths:
        return "No transcripts found"

    # Search transcripts
    results = []
    for sid, path in transcript_paths:
        if not path or not os.path.exists(path):
            continue

        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                for line_num, line in enumerate(f, 1):
                    if not line.strip():
                        continue
                    try:
                        entry = json.loads(line)
                        msg_type = entry.get("type", "")
                        if msg_type not in ("user", "assistant"):
                            continue

                        content = ""
                        msg = entry.get("message", {})
                        msgcontent = msg.get("content", "")
                        if isinstance(msgcontent, str):
                            content = msgcontent
                        elif isinstance(msgcontent, list):
                            for block in msgcontent:
                                if isinstance(block, dict) and "text" in block:
                                    content += block["text"] + "\n"

                        if len(content) < 20:
                            continue

                        # Count keyword matches
                        content_lower = content.lower()
                        match_count = sum(1 for kw in keywords if kw in content_lower)
                        if match_count == 0:
                            continue

                        # Score by keyword density
                        score = match_count / len(keywords)
                        results.append(
                            {
                                "session_id": sid,
                                "line": line_num,
                                "role": msg_type,
                                "content": content[:500] + ("..." if len(content) > 500 else ""),
                                "score": score,
                                "matches": match_count,
                            }
                        )
                    except (json.JSONDecodeError, AttributeError, TypeError):
                        # Transcripts are append-only JSONL written by a live
                        # process: a torn final line, or an entry whose
                        # "message" is not an object, is expected. Skip the
                        # line, keep scanning the file.
                        continue
        except OSError as exc:
            # Transcript deleted or permissions changed between the exists()
            # check and the open. Skip this file, keep scanning the rest.
            ctx.logger.debug("skipping unreadable transcript %s: %s", path, exc)
            continue

    # Sort by score descending
    results.sort(key=lambda x: x["score"], reverse=True)
    results = results[:limit]

    if not results:
        return f"No matches found for: {query}"

    # Format output
    output = f"Found {len(results)} matches for: {query}\n"
    output += "=" * 50 + "\n\n"
    for i, r in enumerate(results, 1):
        output += f"{i}. [{r['role']}] (line {r['line']}, score: {r['score']:.2f})\n"
        output += f"   {r['content'][:200]}...\n\n"

    return output
