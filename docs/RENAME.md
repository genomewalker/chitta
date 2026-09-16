# Rename: cc-soul → chitta

Status as of 2026-09-02: the project was renamed from `cc-soul` to `chitta`.
The GitHub repository moved to `github.com/genomewalker/chitta` (the old
`github.com/genomewalker/cc-soul` URL redirects). Every `CC_SOUL_*` env var
still works via the alias shim described below — nothing that already works
today needs to change.

## Why

The project's real identity was already "chitta" — the binaries
(`chitta`/`chittad`/`chitta_hintd`), the backing store (`chitta-field`), and
the MCP server (`chitta-mcp`) were never named `cc-soul`. "cc-soul" survived
only in packaging (plugin/marketplace names, install-script paths), and "cc"
(Claude Code) undersold the project once Codex and OpenCode were also
supported as frontends.

## What changed

- Plugin identity: `.claude-plugin/plugin.json` name is now `chitta`;
  `.claude-plugin/marketplace.json` name is now `genomewalker-chitta` with a
  `chitta` plugin entry. Skills are namespaced by plugin name, so
  `/cc-soul:recap` becomes `/chitta:recap` for a fresh install of the
  renamed marketplace.
- The GitHub repository: `genomewalker/cc-soul` → `genomewalker/chitta`
  (GitHub redirects the old URL).
- `CC_SOUL_*` environment variables all have `CHITTA_*` twins (table below).
- Install scripts (`scripts/dev-install.sh`, `scripts/sync-installed-hooks.sh`,
  `scripts/smart-install.sh`, `hooks/smart-install.sh`,
  `scripts/configure-codex-hooks.sh`, `chitta-mcp/install.py`) recognize both
  the pre-rename and post-rename marketplace directory, `installed_plugins.json`
  key, `enabledPlugins` key, and Codex plugin cache directory — they prefer
  the new name and fall back to the old one, and never error on either.

## What did NOT change

Scope discipline: only packaging/identity and env-var names moved. These stay
exactly as they were and are not part of this rename:

- File/directory names, the `chitta-field` submodule, binaries, systemd unit
  names, `$MIND` paths.
- Stored memory realm names — `project:cc-soul` remains a valid realm name
  for existing memories; realms are not retroactively renamed.
- `[cc-soul]` log-line prefixes in install/sync scripts (cosmetic, matched by
  nothing else).
- The GitHub Pages custom domain in `docs/CNAME` (`cc-soul.vaklab.dev`) — that
  requires a DNS change outside this repo; update it (and the DNS record)
  separately if the docs site should move to a `chitta.*` domain.
- Historical commit messages, incident references, and dated log entries —
  these are left verbatim as a record of what happened at the time.

## `CC_SOUL_*` → `CHITTA_*` alias table

Every variable below works under either name. `hooks/lib.sh` (sourced by
every Claude/Codex hook) exports whichever name is unset from whichever is
set; hook entrypoints that run before `lib.sh` is sourced (the `HEADLESS`
fast-exit checks, `MAX_WAIT`) read both names inline for the same effect.
Python read sites (`chitta-mcp/*.py`, `benchmarks/smriti/runner.py`,
`hooks/stop-transcript-snapshot.py`) check `CHITTA_*` first, then
`CC_SOUL_*`. If both names are set for the same variable, the `CHITTA_*`
value wins.

| Old (`CC_SOUL_*`) | New (`CHITTA_*`) |
|---|---|
| `CC_SOUL_ABLATE_LANES` | `CHITTA_ABLATE_LANES` |
| `CC_SOUL_ADMIT_DEBUG` | `CHITTA_ADMIT_DEBUG` |
| `CC_SOUL_AGENT_LIMIT` | `CHITTA_AGENT_LIMIT` |
| `CC_SOUL_AGENT_NO_FORCE` | `CHITTA_AGENT_NO_FORCE` |
| `CC_SOUL_AGENT_WARN` | `CHITTA_AGENT_WARN` |
| `CC_SOUL_ALLOW_EDIT` | `CHITTA_ALLOW_EDIT` |
| `CC_SOUL_ALLOW_GLOB_RM` | `CHITTA_ALLOW_GLOB_RM` |
| `CC_SOUL_ALLOW_READ` | `CHITTA_ALLOW_READ` |
| `CC_SOUL_ANCHOR_ENFORCE` | `CHITTA_ANCHOR_ENFORCE` |
| `CC_SOUL_AUTO_RECAP` | `CHITTA_AUTO_RECAP` |
| `CC_SOUL_BOOT_GRACE` | `CHITTA_BOOT_GRACE` |
| `CC_SOUL_C2_SMALL_REALM` | `CHITTA_C2_SMALL_REALM` |
| `CC_SOUL_C2_SMALL_REALM_MAXN` | `CHITTA_C2_SMALL_REALM_MAXN` |
| `CC_SOUL_C2_SMALL_REALM_MINPCT` | `CHITTA_C2_SMALL_REALM_MINPCT` |
| `CC_SOUL_CHECKPOINT_INTERVAL` | `CHITTA_CHECKPOINT_INTERVAL` |
| `CC_SOUL_CTX_LANE` | `CHITTA_CTX_LANE` |
| `CC_SOUL_DEEP_SEARCH` | `CHITTA_DEEP_SEARCH` |
| `CC_SOUL_DISCIPLINE_ENFORCE` | `CHITTA_DISCIPLINE_ENFORCE` |
| `CC_SOUL_EDIT_REINDEX_RATE` | `CHITTA_EDIT_REINDEX_RATE` |
| `CC_SOUL_ENRICH_INTERVAL` | `CHITTA_ENRICH_INTERVAL` |
| `CC_SOUL_HEADLESS` | `CHITTA_HEADLESS` |
| `CC_SOUL_HOOK_BUDGET_MS` | `CHITTA_HOOK_BUDGET_MS` |
| `CC_SOUL_HOOK_ENFORCE` | `CHITTA_HOOK_ENFORCE` |
| `CC_SOUL_HOOK_STATE_DIR` | `CHITTA_HOOK_STATE_DIR` |
| `CC_SOUL_INDEX_INTERVAL` | `CHITTA_INDEX_INTERVAL` |
| `CC_SOUL_LEAN` | `CHITTA_LEAN` |
| `CC_SOUL_LEGACY_MARKERS` | `CHITTA_LEGACY_MARKERS` |
| `CC_SOUL_LOOP_LIMIT` | `CHITTA_LOOP_LIMIT` |
| `CC_SOUL_LOOP_WARN` | `CHITTA_LOOP_WARN` |
| `CC_SOUL_MAX_INDEX_FILES` | `CHITTA_MAX_INDEX_FILES` |
| `CC_SOUL_MAX_OUTPUT_CHARS` | `CHITTA_MAX_OUTPUT_CHARS` |
| `CC_SOUL_MAX_WAIT` | `CHITTA_MAX_WAIT` |
| `CC_SOUL_MCP_DIR` | `CHITTA_MCP_DIR` |
| `CC_SOUL_MODEL` | `CHITTA_MODEL` |
| `CC_SOUL_PLUGIN_DIR` | `CHITTA_PLUGIN_DIR` |
| `CC_SOUL_REINDEX_RATE_LIMIT` | `CHITTA_REINDEX_RATE_LIMIT` |
| `CC_SOUL_RETAG_INTERVAL` | `CHITTA_RETAG_INTERVAL` |
| `CC_SOUL_RLM_MODE` | `CHITTA_RLM_MODE` |
| `CC_SOUL_RLM_QUERY` | `CHITTA_RLM_QUERY` (internal pass-through, not user-facing) |
| `CC_SOUL_SADHANA_MAX` | `CHITTA_SADHANA_MAX` |
| `CC_SOUL_SADHANA_TIMEOUT` | `CHITTA_SADHANA_TIMEOUT` |
| `CC_SOUL_SNAPSHOT_TIMEOUT` | `CHITTA_SNAPSHOT_TIMEOUT` |
| `CC_SOUL_STOP_BOOTSTRAP_BYTES` | `CHITTA_STOP_BOOTSTRAP_BYTES` |
| `CC_SOUL_STOP_ENRICH_INTERVAL` | `CHITTA_STOP_ENRICH_INTERVAL` |
| `CC_SOUL_STOP_GRACE` | `CHITTA_STOP_GRACE` |
| `CC_SOUL_STOP_MAX_INCREMENT_BYTES` | `CHITTA_STOP_MAX_INCREMENT_BYTES` |
| `CC_SOUL_STORE_INTERVAL` | `CHITTA_STORE_INTERVAL` |
| `CC_SOUL_STRICT_MODE` | `CHITTA_STRICT_MODE` |
| `CC_SOUL_STRICT_MODE_DEFAULT` | `CHITTA_STRICT_MODE_DEFAULT` |
| `CC_SOUL_SUBAGENT_BASH_RECALL` | `CHITTA_SUBAGENT_BASH_RECALL` |
| `CC_SOUL_UNKNOWN_SILENCE` | `CHITTA_UNKNOWN_SILENCE` |

`CC_SOUL_RLM_QUERY`/`CC_SOUL_MCP_DIR` in `hooks/prompt-core.sh` are an
internal set-and-immediately-read pass-through to a `python3 -c` subprocess
(not a knob anyone sets externally), so they were left under the old name
rather than migrated — no compatibility concern either way.

## Migrating an existing install

```bash
# 1. Remove the old marketplace, add the renamed one
claude plugin marketplace remove genomewalker-cc-soul
claude plugin marketplace add https://github.com/genomewalker/chitta

# 2. Install the renamed plugin
claude plugin install chitta@genomewalker-chitta

# 3. If you run the dev/editable install from a source checkout, re-point it
git -C /path/to/chitta remote set-url origin git@github.com:genomewalker/chitta.git
bash scripts/dev-install.sh
```

Verify the exact `claude plugin`/`claude marketplace` subcommand syntax with
`claude plugin --help` and `claude plugin marketplace --help` first if these have
changed since this was written.

Nothing else to do: your existing `CC_SOUL_*` env vars, `project:cc-soul`
memory realm, and any scripts that shell out to the old marketplace/cache
paths keep working through the compatibility shims above. Update them to the
`CHITTA_*`/`chitta` names at your convenience, not because anything breaks
if you don't.

## Instruction and skill consolidation (2026-09-16)

- `CLAUDE.md` remains canonical. `CLAUDE.lean.md`, `.claude-plugin/CLAUDE.md`,
  and `.claude-plugin/CLAUDE-full.md` now contain short pointers and the same
  constraint summary, replacing the stale cc-soul policy and full duplicate.
- `skills/` is the maintained source. `scripts/sync-skills.sh` updates the
  committed `codex-plugin/skills/` mirror, excluding `_conventions`;
  `scripts/check-skills-sync.sh` detects drift. The existing release copy stays
  intact. No loader symlink behavior is assumed.
- Public `cc-soul-*` directory/frontmatter names are unchanged. Setup now describes
  the installer's source-first behavior; shutdown follows the service manager;
  deployment guidance defers to the canonical worktree/orchestrator constraints.
- Correction to the earlier installer list: `hooks/smart-install.sh` is absent
  from this checkout and its tracked files. `scripts/smart-install.sh` is the
  shared installer referenced by setup skills and `chitta-mcp/stack.py`.
- The [scripts index](../scripts/README.md) records every retained tracked script
  and the per-file evidence for 13 deletions. Ignored SSL build contents and all
  15 tracked `scripts/ssl_model/` files are preserved.
