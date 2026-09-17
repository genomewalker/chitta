"""Local content-addressed Bash output storage and bounded hook context."""

import hashlib
import json
import os
import tempfile

from hook_client import setting


def output_text(payload):
    if payload.get("tool_name", "Bash") not in ("Bash", "bash", "exec_command"):
        return ""
    result = payload.get("tool_response", payload.get("tool_result", payload))
    if isinstance(result, str):
        return result
    if isinstance(result, dict):
        return "".join(
            result.get(k, "") for k in ("stdout", "stderr") if isinstance(result.get(k), str)
        )
    return ""


def summarize(payload, state):
    text = output_text(payload)
    try:
        cap = max(256, int(setting("OUTPUT_CAP_CHARS", "6000")))
    except ValueError:
        cap = 6000
    if len(text) <= cap:
        return ""
    digest = hashlib.sha256(text.encode()).hexdigest()
    directory = state / "outputs"
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    fd, temporary = tempfile.mkstemp(dir=directory)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(text.encode())
        os.replace(temporary, directory / digest)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    header = f"§ref:{digest[:12]}§ {len(text)} chars; first/last 20 lines follow\n"
    lines = text.splitlines(keepends=True)
    first = "".join(lines[:20])
    last = "".join(lines[max(20, len(lines) - 20) :])
    preview = first + ("\n…\n" if len(lines) > 40 else "") + last
    room = cap - len(header) - 5
    if len(preview) > room:
        preview = text[: room // 2] + "\n…\n" + text[-room // 2 :]
    return header + preview


def add_context(output, summary):
    if not summary:
        return output
    try:
        envelope = json.loads(output) if output.strip() else {}
        if not isinstance(envelope, dict):
            raise ValueError("not an envelope")
    except ValueError:
        envelope = {}
        summary = output + summary
    hook = envelope.setdefault("hookSpecificOutput", {})
    hook["hookEventName"] = "PostToolUse"
    previous = hook.get("additionalContext", "")
    hook["additionalContext"] = previous + ("\n" if previous else "") + summary
    return json.dumps(envelope, ensure_ascii=False) + "\n"
