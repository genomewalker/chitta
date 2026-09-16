---
name: cc-soul-setup
description: Set up chitta with a source build when a compiler is available, or pre-built binaries
execution: inline
---

# Set up chitta

The `cc-soul-setup` command name is retained for compatibility.
Repository work follows the canonical `CLAUDE.md` (Codex: `codex-plugin/AGENTS.md`);
implementation streams leave installation and service changes to the orchestrator.

Install chitta through the shared installer.

## Source-build requirements
- cmake
- make
- C++20 compiler (g++ or clang++)

Without a compiler, the installer tries compatible pre-built binaries.

## Usage

```bash
# Run the shared installer from the plugin checkout
${CLAUDE_PLUGIN_ROOT}/scripts/smart-install.sh
```

The installer resolves the current release, builds from source with llama.cpp
when a compiler is available, and falls back to compatible pre-built binaries
if needed. It installs binaries, the embedding model, hooks, MCP dependencies
and service configuration, then stops the daemon for migrations and restarts it.
It preserves an existing embedding-model identity.

If neither the source build nor a compatible binary is available, report the
installer error; `/cc-soul-update` uses this same installer.

After setup, the daemon is managed by systemd (`systemctl --user status chittad`). It starts automatically and restarts on crash.
