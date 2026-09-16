---
name: cc-soul-mcp
description: Configure chitta MCP server for direct tool access (mcp__chitta__*)
execution: inline
---

# Configure chitta MCP

The `cc-soul-mcp` command name is retained for compatibility.
Repository work follows the canonical `CLAUDE.md` (Codex: `codex-plugin/AGENTS.md`);
implementation streams leave installation and service changes to the orchestrator.

Configure the chitta MCP server in Claude Code settings.

## Usage

Run the configure-mcp.sh script:

```bash
# Add MCP server config
${CLAUDE_PLUGIN_ROOT}/scripts/configure-mcp.sh

# Remove MCP server config
${CLAUDE_PLUGIN_ROOT}/scripts/configure-mcp.sh --remove
```

This adds/removes:
- `mcpServers.chitta` in ~/.claude/settings.json
- `mcp__chitta__*` permission

After running, restart Claude Code for changes to take effect.
