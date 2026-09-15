"""Conservative tool-target audit for the explicitly weaker home-audit mode.

This is evidence checking, not a kernel sandbox. Opaque Bash programs are
unverifiable and void a trial; only a small inspectable command grammar passes.
"""

from __future__ import annotations

import glob
import json
import re
import shlex
from pathlib import Path

TOOLS = ["Bash", "Read", "Edit", "Write", "Glob", "Grep"]
CAVEAT = "home-audit screening only; permissions and post-hoc audit, no OS isolation"


def inside(path, roots, cwd, home):
    value = str(path)
    if value == "~" or value.startswith("~/"):
        value = home + value[1:]
    p = Path(value)
    p = (Path(cwd) / p).resolve() if not p.is_absolute() else p.resolve()
    return any(p.is_relative_to(Path(r).resolve()) for r in roots)


def audit_access(payload, policy):
    tool, args = payload.get("tool_name"), payload.get("tool_input", {})
    cwd = payload.get("cwd", policy["work"])
    roots, home = policy["roots"], policy["home"]
    if not inside(cwd, roots, policy["work"], home):
        return f"out-of-root cwd: {cwd}"
    if tool not in TOOLS:
        return f"unverifiable tool: {tool}"
    if tool != "Bash":
        targets = [args[k] for k in ("file_path", "path") if k in args]
        if tool == "Glob":
            targets.append(args.get("pattern", "."))
        if not targets:
            targets = [cwd] if tool == "Grep" else []
        if not targets:
            return f"missing target: {tool}"
        for target in targets:
            if not isinstance(target, str) or not inside(target, roots, cwd, home):
                return f"out-of-root {tool}: {target}"
            path = Path(target) if Path(target).is_absolute() else Path(cwd) / target
            if tool in {"Edit", "Write"} and any(
                path.resolve().is_relative_to(Path(p).resolve())
                for p in policy.get("protected", [])
            ):
                return f"audit control modification: {target}"
            for match in glob.iglob(str(path), recursive=True):
                if not inside(match, roots, cwd, home):
                    return f"out-of-root {tool} expansion: {match}"
                if Path(match).is_dir() and tool in {"Glob", "Grep"}:
                    for child in Path(match).rglob("*"):
                        if child.is_symlink() and not inside(child, roots, cwd, home):
                            return f"out-of-root {tool} symlink: {child}"
        return None
    text = args.get("command", "")
    for target in policy["live_targets"]:
        if target and target in text:
            return f"live connection/access attempt: {target}"
    if re.search(r"(?:localhost|127\.\d+\.\d+\.\d+|\[?::1\]?|0\.0\.0\.0)", text):
        return "local network connection attempt"
    # Do not pretend to resolve arbitrary shell, interpreters, scripts, aliases,
    # substitutions, redirections or environment-dependent paths by regex.
    if re.search(r"[$`;&|<>\n\\{}]", text):
        return "unverifiable Bash syntax"
    try:
        words = shlex.split(text)
    except ValueError:
        return "unparseable Bash command"
    if not words:
        return "empty Bash command"
    executable = words[0]
    if executable not in {"pwd", "ls", "cat", "head", "tail", "wc", "true", "false"}:
        return f"unverifiable Bash executable: {executable}"
    for word in words[1:]:
        if word.startswith("-"):
            if "=" in word or "/" in word:
                return f"unverifiable Bash option: {word}"
            continue
        if not inside(word, roots, cwd, home):
            return f"out-of-root Bash: {word}"
        target = Path(word) if Path(word).is_absolute() else Path(cwd) / word
        for match in glob.iglob(str(target)):
            if not inside(match, roots, cwd, home):
                return f"out-of-root Bash expansion: {match}"
    return None


def audit_trial(hooks, history, policy):
    for hook in hooks:
        if hook.get("audit_reason"):
            return hook["audit_reason"]
    attempts = [h["payload"] for h in hooks if h.get("event") == "PreToolUse"]
    for h in hooks:
        if h.get("event") in {"PostToolUse", "PostToolUseFailure"}:
            payload = h.get("payload")
            if not payload:
                return "missing tool audit payload"
            if not any(
                p.get("tool_name") == payload.get("tool_name")
                and p.get("tool_input") == payload.get("tool_input")
                for p in attempts
            ):
                return "missing PreToolUse shadow event"
    for payload in attempts:
        reason = audit_access(payload, policy)
        if reason:
            return reason
    bash_commands = [p["tool_input"]["command"] for p in attempts if p["tool_name"] == "Bash"]
    if history != bash_commands:
        return "bash history/shadow log mismatch"
    return None


def deny_rules(roots, live):
    """Deny existing sibling subtrees along allowed-root ancestors; guard handles
    future names, relative paths, symlinks and shell syntax at tool time.
    Claude absolute file patterns start with //, not /.
    """
    roots = [Path(r).absolute() for r in roots]
    denied = set(live)
    frontier = [Path("/")]
    while frontier:
        parent = frontier.pop()
        try:
            children = list(parent.iterdir())
        except OSError:
            denied.add(str(parent))
            continue
        for child in children:
            if child in roots:
                continue
            if any(r.is_relative_to(child) for r in roots):
                frontier.append(child)
            else:
                denied.add(str(child))
    rules = []
    for path in sorted(denied):
        for tool in TOOLS[1:]:
            rules.extend([f"{tool}(/{path})", f"{tool}(/{path}/**)"])
        rules.append(f"Bash(*{path}*)")
    for tool in TOOLS[1:]:
        rules.append(f"{tool}(~/.claude/projects/**)")
    rules += [f"Bash({name} *)" for name in ("curl", "wget", "nc", "socat", "ssh")]
    return rules


def read_history(path):
    return [json.loads(line) for line in Path(path).read_text().splitlines() if line.strip()]
