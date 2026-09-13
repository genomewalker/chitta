#!/usr/bin/env python3
"""Install/uninstall chitta-mcp for Claude Code and/or Codex CLI."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

PLUGIN_NAME = "chitta"
# Pre-rename (2026-09-02, see docs/RENAME.md) plugin/cache name. Uninstall and
# hook-command detection still recognize it so an upgrade-in-place install
# doesn't leave orphaned entries behind.
LEGACY_PLUGIN_NAME = "cc-soul"
MARKETPLACE = "local"
HOOKS_DIR = Path(__file__).resolve().parent.parent / "hooks"


# ── helpers ──────────────────────────────────────────────────────────


def _chitta_mcp_path() -> str:
    return shutil.which("chitta-mcp") or "chitta-mcp"


def _chitta_cli_path() -> str | None:
    return shutil.which("chitta") or str(Path.home() / ".claude" / "bin" / "chitta")


def _codex_home() -> Path:
    return Path(os.environ.get("CODEX_HOME", Path.home() / ".codex"))


def _plugin_dir() -> Path:
    return _codex_home() / "plugins" / "cache" / MARKETPLACE / PLUGIN_NAME / "local"


def _codex_config() -> Path:
    return _codex_home() / "config.toml"


def _codex_hooks_file() -> Path:
    return _codex_home() / "hooks.json"


def _plugin_source_dir() -> Path:
    """Find the codex-plugin source directory.

    Checks in order:
      1. Alongside this package (editable / source install)
      2. Wheel shared-data location (pip install)
    """
    src = Path(__file__).resolve().parent.parent / "codex-plugin"
    if src.is_dir():
        return src
    import sysconfig

    data = Path(sysconfig.get_path("data")) / "share" / PLUGIN_NAME / "codex-plugin"
    if data.is_dir():
        return data
    return src


def _hooks_source() -> Path:
    """Find the hooks source directory."""
    src = Path(__file__).resolve().parent.parent / "hooks"
    if src.is_dir():
        return src
    import sysconfig

    data = Path(sysconfig.get_path("data")) / "share" / PLUGIN_NAME / "hooks"
    if data.is_dir():
        return data
    return src


# ── Claude Code ──────────────────────────────────────────────────────


def _install_claude_code():
    try:
        result = subprocess.run(
            [
                "claude",
                "mcp",
                "add",
                "--transport",
                "stdio",
                "--scope",
                "user",
                "chitta",
                "--",
                "chitta-mcp",
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode == 0:
            print("  Claude Code: registered")
        elif "already exists" in result.stderr.lower():
            print("  Claude Code: already registered")
        else:
            print(f"  Claude Code: failed — {result.stderr.strip()}")
            return False
    except FileNotFoundError:
        print("  Claude Code: 'claude' CLI not found — skipping")
        return False
    return True


def _uninstall_claude_code():
    try:
        result = subprocess.run(
            ["claude", "mcp", "remove", "chitta"],
            capture_output=True,
            text=True,
        )
        if result.returncode == 0:
            print("  Claude Code: removed")
        elif "not found" in result.stderr.lower():
            print("  Claude Code: not registered")
        else:
            print(f"  Claude Code: failed — {result.stderr.strip()}")
            return False
    except FileNotFoundError:
        print("  Claude Code: 'claude' CLI not found — skipping")
        return False
    return True


# ── Codex CLI ────────────────────────────────────────────────────────


def _generate_codex_hooks(hooks_dir: Path) -> dict:
    """Translate chitta hooks.json to Codex hooks format.

    Codex supports: SessionStart, UserPromptSubmit, PreToolUse, PostToolUse, Stop.
    Matchers in Codex are regex strings ("startup", "resume", ".*", "Bash").
    """
    source = hooks_dir / "hooks.json"
    if not source.is_file():
        return {}

    cc_hooks = json.loads(source.read_text())["hooks"]
    codex_hooks: dict[str, list] = {}

    # Event mapping: which chitta events map to Codex
    # Codex hook schema compatibility:
    # PreToolUse payload contracts differ from Claude's and currently reject
    # some chitta outputs (e.g. additionalContext), so we do not install that
    # event for Codex until a dedicated Codex-safe pre-tool hook is provided.
    supported_events = {"SessionStart", "UserPromptSubmit", "PostToolUse", "Stop"}

    for event, matchers in cc_hooks.items():
        if event not in supported_events:
            continue

        codex_matchers = []
        for matcher_block in matchers:
            cc_matcher = matcher_block["matcher"]
            # Convert chitta glob "*" to Codex regex ".*"
            codex_matcher = ".*" if cc_matcher == "*" else cc_matcher

            codex_hook_list = []
            for hook in matcher_block["hooks"]:
                # Resolve ${CLAUDE_PLUGIN_ROOT} to absolute hooks directory
                cmd = hook["command"].replace("${CLAUDE_PLUGIN_ROOT}/hooks/", str(hooks_dir) + "/")

                # SessionStart: use Codex-specific JSON adapters. Claude's
                # compact hook emits watchPaths and its resume helper emits
                # initialUserMessage; neither field exists in Codex's schema.
                if event == "SessionStart" and cmd.endswith("session-start-hook.sh"):
                    cmd = str(hooks_dir / "codex-session-start-wrapper.sh")
                elif event == "SessionStart" and cmd.endswith("compact-restore-hook.sh"):
                    cmd = str(hooks_dir / "codex-compact-restore-wrapper.sh")
                elif event == "SessionStart" and cmd.endswith("resume-inject-hook.sh"):
                    continue
                elif event == "UserPromptSubmit" and cmd.endswith("prompt-hook.sh"):
                    cmd = str(hooks_dir / "codex-prompt-hook.sh")
                elif event == "Stop" and cmd.endswith("stop-hook.sh"):
                    cmd = str(hooks_dir / "codex-stop-hook.sh")

                # SessionStart: skip standalone subconscious hook in Codex.
                # The wrapper runs subconscious and then emits valid JSON.
                if event == "SessionStart" and cmd.endswith("subconscious.sh start"):
                    continue

                # SessionStart: subconscious.sh needs more time in Codex (daemon startup)
                timeout = hook.get("timeout")
                if event == "SessionStart" and cmd.endswith("codex-compact-restore-wrapper.sh"):
                    timeout = 12
                if event == "SessionStart" and "subconscious.sh" in cmd:
                    timeout = 15

                codex_hook = {"type": "command", "command": cmd}
                if timeout is not None:
                    codex_hook["timeout"] = timeout
                codex_hook_list.append(codex_hook)

            if codex_hook_list:
                codex_matchers.append(
                    {
                        "matcher": codex_matcher,
                        "hooks": codex_hook_list,
                    }
                )

        codex_hooks[event] = codex_matchers

    return {"hooks": codex_hooks}


def _is_cc_soul_hook_command(cmd: str, hooks_dir: Path) -> bool:
    """Return True when a hook command belongs to chitta.

    Matches current install path and legacy Claude plugin-cache paths that may
    have been merged into Codex hooks.json by older installers.
    """
    if str(hooks_dir) in cmd:
        return True
    return (
        "/.claude/plugins/cache/genomewalker-cc-soul/cc-soul/" in cmd
        or "/.claude/plugins/cache/genomewalker-chitta/chitta/" in cmd
    )



def _strip_plugin_entry(text: str, name: str) -> str:
    """Drop the [plugins."<name>@<marketplace>"] table: its header and the
    `key = value` lines directly under it. Stops at the next header, a blank
    line, or a comment so foreign blocks (cc-clip's notify marker) survive."""
    header = f'[plugins."{name}@{MARKETPLACE}"]'
    if header not in text:
        return text
    out, skip = [], False
    for line in text.split("\n"):
        if line.strip() == header:
            skip = True
            continue
        if skip and (line.startswith("[") or line.startswith("#") or not line.strip()):
            skip = False
        if skip:
            continue
        out.append(line)
    return "\n".join(out)

def _install_codex():
    source = _plugin_source_dir()
    if not source.is_dir():
        print(f"  Codex: plugin source not found at {source}")
        return False

    hooks_dir = _hooks_source()
    if not hooks_dir.is_dir():
        print(f"  Codex: hooks directory not found at {hooks_dir}")
        return False

    chitta_cli = Path(_chitta_cli_path())
    if not chitta_cli.is_file():
        print("  Codex: 'chitta' CLI not found in PATH — required for hooks")
        return False

    dest = _plugin_dir()
    dest.mkdir(parents=True, exist_ok=True)

    # Copy plugin manifest
    codex_plugin = source / ".codex-plugin"
    if codex_plugin.is_dir():
        shutil.copytree(codex_plugin, dest / ".codex-plugin", dirs_exist_ok=True)

    # Copy skills
    skills = source / "skills"
    if skills.is_dir():
        shutil.copytree(skills, dest / "skills", dirs_exist_ok=True)

    # Write .mcp.json with resolved absolute path
    mcp_path = _chitta_mcp_path()
    mcp_json = {"mcpServers": {"chitta": {"command": mcp_path, "args": []}}}
    (dest / ".mcp.json").write_text(json.dumps(mcp_json, indent=2) + "\n")

    # Enable plugin in config.toml
    config = _codex_config()
    config.parent.mkdir(parents=True, exist_ok=True)
    config.touch(exist_ok=True)
    text = config.read_text()

    if "[features]" not in text:
        text += "\n[features]\nplugins = true\nhooks = true\n"
    else:
        features_section = text.split("[features]", 1)[1].split("\n[", 1)[0]
        if "plugins" not in features_section:
            text = text.replace("[features]", "[features]\nplugins = true")
        if "hooks" not in features_section:
            text = text.replace("[features]", "[features]\nhooks = true")

    text = _strip_plugin_entry(text, LEGACY_PLUGIN_NAME)
    if f"{PLUGIN_NAME}@{MARKETPLACE}" not in text:
        text += f'\n[plugins."{PLUGIN_NAME}@{MARKETPLACE}"]\nenabled = true\n'

    config.write_text(text)
    legacy_dest = _codex_home() / "plugins" / "cache" / MARKETPLACE / LEGACY_PLUGIN_NAME / "local"
    if legacy_dest.is_dir():
        shutil.rmtree(legacy_dest)
        print(f"  Codex: removed legacy install {legacy_dest}")

    # Generate hooks.json
    codex_hooks = _generate_codex_hooks(hooks_dir)
    if codex_hooks:
        hooks_file = _codex_hooks_file()
        # Merge with existing hooks if present
        if hooks_file.is_file():
            existing = json.loads(hooks_file.read_text())
            # First remove all existing chitta hook matchers across all events
            # (including events not regenerated in this install pass).
            for event in list(existing.get("hooks", {}).keys()):
                kept = [
                    m
                    for m in existing["hooks"][event]
                    if not any(
                        _is_cc_soul_hook_command(h.get("command", ""), hooks_dir)
                        for h in m.get("hooks", [])
                    )
                ]
                if kept:
                    existing["hooks"][event] = kept
                else:
                    del existing["hooks"][event]

            for event, matchers in codex_hooks["hooks"].items():
                if event not in existing.get("hooks", {}):
                    existing.setdefault("hooks", {})[event] = matchers
                else:
                    # Remove existing chitta matchers (current + legacy cache paths),
                    # then append fresh ones — preserves third-party hooks.
                    kept = [
                        m
                        for m in existing["hooks"][event]
                        if not any(
                            _is_cc_soul_hook_command(h.get("command", ""), hooks_dir)
                            for h in m.get("hooks", [])
                        )
                    ]
                    existing["hooks"][event] = kept + matchers
            codex_hooks = existing
        hooks_file.write_text(json.dumps(codex_hooks, indent=2) + "\n")
        print(f"  Codex: hooks written to {hooks_file}")

    skill_names = sorted(
        d.name for d in (dest / "skills").iterdir() if d.is_dir() and not d.name.startswith("_")
    )
    print(f"  Codex: installed to {dest}")
    print(f"  Codex: MCP server → {mcp_path}")
    print(f"  Codex: {len(skill_names)} skills available")
    return True


def _uninstall_codex():
    dest = _plugin_dir()
    if dest.is_dir():
        shutil.rmtree(dest)
        print(f"  Codex: removed {dest}")
    else:
        print("  Codex: not installed")

    legacy_dest = _codex_home() / "plugins" / "cache" / MARKETPLACE / LEGACY_PLUGIN_NAME / "local"
    if legacy_dest.is_dir():
        shutil.rmtree(legacy_dest)
        print(f"  Codex: removed legacy install {legacy_dest}")

    # Remove hooks that reference our hook scripts
    hooks_file = _codex_hooks_file()
    if hooks_file.is_file():
        hooks_dir = _hooks_source()
        data = json.loads(hooks_file.read_text())
        changed = False
        for event in list(data.get("hooks", {}).keys()):
            matchers = data["hooks"][event]
            filtered = []
            for m in matchers:
                remaining = [
                    h
                    for h in m.get("hooks", [])
                    if not _is_cc_soul_hook_command(h.get("command", ""), hooks_dir)
                ]
                if remaining:
                    m["hooks"] = remaining
                    filtered.append(m)
                else:
                    changed = True
            if filtered:
                data["hooks"][event] = filtered
            else:
                del data["hooks"][event]
                changed = True
        if changed:
            if data.get("hooks"):
                hooks_file.write_text(json.dumps(data, indent=2) + "\n")
            else:
                hooks_file.unlink()
            print("  Codex: removed hook entries")

    config = _codex_config()
    if config.is_file():
        text = config.read_text()
        for name in (PLUGIN_NAME, LEGACY_PLUGIN_NAME):
            stripped = _strip_plugin_entry(text, name)
            if stripped != text:
                text = stripped
                print("  Codex: removed config entry")
        config.write_text(text)
    return True


# ── CLI ──────────────────────────────────────────────────────────────

TARGETS = {
    "claude-code": (_install_claude_code, _uninstall_claude_code),
    "codex": (_install_codex, _uninstall_codex),
}

USAGE = """usage: chitta-mcp-install [claude-code|codex|all]
       chitta-mcp-uninstall [claude-code|codex|all]

Targets:
  claude-code   Register MCP server with Claude Code (default)
  codex         Install Codex CLI plugin (skills + MCP + hooks)
  all           Both"""


def _parse_target(args: list[str]) -> list[str]:
    if not args:
        return ["claude-code"]
    if args[0] == "all":
        return list(TARGETS.keys())
    if args[0] in ("--help", "-h"):
        print(USAGE)
        sys.exit(0)
    if args[0] in TARGETS:
        return [args[0]]
    print(f"Unknown target: {args[0]}\n")
    print(USAGE)
    sys.exit(1)


def install():
    targets = _parse_target(sys.argv[1:])
    print("chitta-mcp install:")
    ok = True
    for t in targets:
        if not TARGETS[t][0]():
            ok = False
    if not ok:
        sys.exit(1)
    print("\ndone.")


def uninstall():
    targets = _parse_target(sys.argv[1:])
    print("chitta-mcp uninstall:")
    for t in targets:
        TARGETS[t][1]()
    print("\ndone.")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "uninstall":
        sys.argv.pop(1)
        uninstall()
    else:
        install()
