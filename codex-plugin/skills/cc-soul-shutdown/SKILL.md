---
name: cc-soul-shutdown
description: Gracefully stop the chitta daemon
execution: inline
---

# Stop the chitta daemon

The `cc-soul-shutdown` command name is retained for compatibility.
Repository work follows the canonical `CLAUDE.md` (Codex: `codex-plugin/AGENTS.md`);
implementation streams leave installation and service changes to the orchestrator.

Stop the chittad daemon gracefully.

## Usage

When the systemd user service is installed:

```bash
systemctl --user stop chittad
```

Otherwise use the daemon lifecycle helper:

```bash
${CLAUDE_PLUGIN_ROOT}/hooks/subconscious.sh stop
```

The helper delegates to systemd when available. Its standalone fallback sends
SIGTERM, waits for the configured shutdown grace period so the snapshot can
finish, and only then force-kills remaining daemon processes and cleans up
PID/socket files. Never kill the separate `--http` MCP transport.
