# chitta

## Hard constraints — read these first

These are first on the page deliberately. Codex truncates silently once the
combined `AGENTS.md` bytes pass `project_doc_max_bytes`, and what gets dropped is
the end of the file (#13386, #37956). Anything that must survive lives up here.

1. **Never `pkill` the `--http` MCP process.** It is your own transport on port
   9481. SIGTERM reads as a clean exit, so `Restart=on-failure` won't revive it —
   port 9481 stayed dead 2026-09-02 to 09-08. Use `bash scripts/dev-install.sh`.
2. **Work in your own git worktree, never on `main`.** One worktree per stream.
3. **`install`, never `cp`, over a running binary** — `cp` gives ETXTBSY.
4. **The worktree files are the state, not your context.** Do not rely on Astra's
   history-notes or context management to carry state across a headless run
   (#43194, #43335, #42449). The spec plus `Plan.md` and `Documentation.md` in the
   worktree are the source of truth; if it isn't written down, it is lost.
5. **Keep tool output short.** `head`/`tail`/`--quiet`, never `cat` a large log.
   Astra tool loops can re-inject accumulated output — one run reached 3.5M tokens
   in 11 minutes (#44305).
6. **Redirect stdin.** `codex exec` blocks on an open stdin ("Reading additional
   input from stdin…"), so `</dev/null` or detach.

> Status as of 2026-09-13: **canonical for Codex.** `CLAUDE.md` is canonical for
> Claude Code; the git-root `AGENTS.md` is a short pointer here. Codex loads
> global config, then git-root, then cwd, closer files overriding. Sources at the
> end of this file and in `docs/HOOKS.md`.

## Your job here

You are Codex on `gpt-6-astra`, running an implementation stream from a written
spec. A Claude Fable 5.1 session orchestrates, reviews your diff, and decides what
merges. Leave the branch reviewable: small commits, clear message, no unrelated
churn.

```bash
codex exec -C /projects/caeg/scratch/kbd606/tmp/codex-wt-<name> \
  --approve-for-me --skip-git-repo-check \
  -m gpt-6-astra -c model_reasoning_effort=high \
  -o /path/to/last-message.txt "<spec>" </dev/null
```

- **Effort:** `high` for implementation. Reach for `xhigh` or `max` only when an
  eval shows it helps. Never `ultra` unattended — it silently spawns sub-agents on
  other models, which is a cost and audit black box. Astra itself has no published
  effort recommendation.
- **Approvals:** `--approve-for-me` works in **this** dev build (verified in
  `codex exec --help`) but is absent from the public docs; fall back to `-a never`.
  It implies the workspace-write sandbox and cannot be combined with `-s/--sandbox`
  — the parser rejects the pair. `approval_policy = "untrusted"` hangs without a
  TTY, so never use it headless. If you pass `-a/--ask-for-approval` it must come
  **before** `exec` (#26602). Sandbox modes: `workspace-write`, `read-only`,
  `danger-full-access`.
- **Getting the result out:** `-o/--output-last-message FILE` is the robust way to
  capture the final message; `--json` emits events as JSONL for tracking. Parsing
  the terminal transcript is not reliable.
- `~/.codex/config.toml` defaults to `gpt-5.6-sol` at `xhigh`, so pass `-m`
  explicitly. Also local: `gpt-5.6-luna`, `gpt-5.6-terra`, `gpt-5.5`,
  `gpt-5.3-codex-spark`. The CLI cannot list models or effort names
  non-interactively — check `codex --help` or the picker before using another.

## Reaching chitta

Your transport is the **HTTP** MCP unit: `chitta-mcp-http.service` on port 9481,
`[mcp_servers.chitta]` url `http://127.0.0.1:9481/mcp/`. chitta-bridge is on 7681.
Tools are namespaced `mcp__chitta__*`; `grow` and `connect` sit behind the
`advanced` gateway. Hooks arrive via `scripts/configure-codex-hooks.sh` and the
Codex plugin cache, owned by `scripts/sync-installed-hooks.sh`.

Two known MCP defects worth checking before blaming chitta: `bearer_token_env_var`
does not always propagate (#41378) — verify the token actually reached the server —
and streamable-HTTP sessions leak (#41600), which an idle-session timeout on
`chitta-mcp-http` will address.

**Your Bash exit codes log as unknown, not success.** Your `PostToolUse` carries no
exit code and `PostToolUseFailure` never fires (#34289), so
`hooks/post-bash-hook.sh:44` detects your shape (`tool_response` is a string, not
an object) and records `"exit_code": null` plus `"likely_fail": true` when the
output reads like a failure. Consumers never treat null as success. That flag is
text matching, not authority — state failures explicitly in your own report.

## Build & deploy

```bash
cd chitta && cmake --build build --parallel
install -m 0755 ../bin/chittad ~/.claude/bin/chittad
install -m 0755 ../bin/chitta  ~/.claude/bin/chitta
[ -f ../bin/chitta_hintd ] && install -m 0755 ../bin/chitta_hintd ~/.claude/bin/chitta_hintd
systemctl --user restart chittad
systemctl --user try-restart chitta-hintd 2>/dev/null || true
bash scripts/dev-install.sh
```

`chitta_hintd` exists only in a `CHITTA_WITH_LLAMA_CPP=ON` build. The MCP process
name uses a hyphen, `chitta-mcp`; a pattern with a space matches nothing.

Hooks and MCP Python run from `~/.claude/hooks/*` and the plugin cache, not from a
checkout — `dev-install.sh` symlinks both back here, and a plugin update can
replace those symlinks with a stale clone, so re-run it afterwards. Binaries stay
on the build-and-install flow above. Release: `./scripts/release.sh patch|minor|major -y`.

## Orientation

| | Path |
|---|---|
| Daemon | `chitta/src/simple_cli.cpp` |
| RPC | `chitta/include/chitta/rpc/field_handler.hpp` |
| Store | `chitta-field/src/store.rs` |
| MCP | `chitta-mcp/server.py` |
| Hooks | `hooks/*.sh` |

`hooks/pre-tool-hook.sh` is the enforcement layer: its behaviour, the `CHITTA_*`
table, bypass flags, and the `CC_SOUL_*` aliases are in `docs/HOOKS.md` and
`docs/RENAME.md`. Its Haiku routing applies to Claude Code, not to you. A
`CHITTA_*` command prefix does not reach a nested bash — `export` it first.

**Sources.** Codex [AGENTS.md](https://learn.chatgpt.com/docs/agent-configuration/agents-md)
and the approvals reference; `gpt-6-astra` (openai.com/index/gpt-6-astra,
2026-09-04); OpenAI long-horizon guide (the `Prompt.md` / `Plan.md` /
`Implement.md` / `Documentation.md` pattern). Community-verified defects, GitHub
issues Sept 2026: #13386 and #37956 silent doc truncation; #34289 no PostToolUse
exit code; #26602 flag ordering; #41378 bearer-token propagation; #41600
streamable-HTTP session leak; #43194, #43335, #42449 context-management state loss;
#44305 tool-output amplification. Claude-side sources are in `docs/HOOKS.md`.

<!-- BEGIN sqz-claude-guidance (auto-installed by sqz init; remove this block to disable) -->

## sqz — context compression

`sqz` compresses verbose tool output. A PreToolUse hook already pipes `Bash`
output through it, so do not add `| sqz compress` by hand; compound and
interactive commands are skipped automatically.

The `sqz-mcp` server exposes `sqz_read_file`, `sqz_grep`, and `sqz_list_dir` —
prefer them over `Read`, `Grep`, and `ls` for anything over a few KB. There are no
write tools by design.

A `§ref:HASH§` token is a dedup reference to content already seen. Resolve one
with `sqz expand <prefix>`, or the `expand` MCP tool. To opt out for one command,
prefix `SQZ_NO_DEDUP=1`; `passthrough` returns raw text if compression is making a
task harder.

<!-- END sqz-claude-guidance -->
