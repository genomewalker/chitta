# Documentation audit — 2026-09-16

Status as of 2026-09-16.

Scope: every top-level `docs/*.md`, all 16 top-level HTML pages, and README.
Nested research posts and visualization data are outside this cleanup. The
Pages workflow publishes `docs/`; repository-only source links on HTML pages
must therefore use repository URLs. Styles, CNAME, favicon, visualization data,
ledger migration evidence, hooks and existing tests are unchanged.

The source baseline is superproject `de60e940` and pinned chitta-field
`f3176e587dced64dd3a6a663a40a58e3802b0a10`. The uninitialized submodule was
read from its existing Git objects into scratch space; its worktree was not
changed. Live CLI inspection was read-only. Published tool discovery returned
320 native tools; merging MCP gateways yields 344 entries (89 default-visible,
255 behind `advanced`), plus 51 native CLI help entries absent from that merged
list. The updated generator preserves this distinction and checks static names.

**Verdicts: 6 current, 27 updated, 3 deprecated-in-place, 0 deleted (36 pages,
including this audit).** “Current” includes explicitly historical documents
whose existing status remains accurate. No historical result is promoted to a
current implementation guarantee. Previous stamps refer to the top-level status;
dated measurements within a document retain their own dates.

| Page | Previous status stamp | Resulting status stamp | Verdict | Source of truth checked / action |
|---|---|---|---|---|
| [README.md](../README.md) | 2026-09-02 | 2026-09-16 | updated | CLI help, hook source, current eval protocol and evolve selector; add current behavior and correct Sadhana invocation. |
| [API.md](API.md) | None; generated 2026-09-02 | 2026-09-16 | updated | Read-only daemon discovery, `rpc_server.cpp` TOOL_SPECS / KNOWN_TOOLS, `tools_static.py`, `server.py` visibility; regenerated with native CLI appendix. |
| [ARCHITECTURE.md](ARCHITECTURE.md) | 2026-09-02 | 2026-09-16 | updated | `simple_cli.cpp`, `field_handler.hpp`, pinned Rust field/store/scoring, native hook calls; replace stale architecture and counts. |
| [CLI.md](CLI.md) | None | 2026-09-16 | updated | `chitta --help`, per-tool help, `simple_cli.cpp` parser and startup, CLAUDE operational notes; fix flags, defaults, sidecars and ownership. |
| [DECISION-2026-09-15-learning-experiment.md](DECISION-2026-09-15-learning-experiment.md) | 2026-09-15 | 2026-09-15 | current | Protected dated decision; compared to `benchmarks/learning/protocol.md` and cohort cut. Historical text and citations unchanged. |
| [DECISION-2026-09-15-mdl-analogy.md](DECISION-2026-09-15-mdl-analogy.md) | 2026-09-15 | 2026-09-15 | current | Protected dated consultation; later implementation is recorded in EVOLVE and EVALS. Text unchanged. This row indexes history, not an active policy. |
| [EVALS.md](EVALS.md) | 2026-09-13 | 2026-09-16 | updated | Learning protocol/cohort, committed analogy and replication evidence; remove conflicting pre-cut status, retain pending prospective panel. |
| [EVAL_REPLICA.md](EVAL_REPLICA.md) | 2026-09-13 | 2026-09-16 | updated | `scripts/eval-replica.sh` selection/copy/start; uncovered WAL and loader markers are copied; hooks already honor the socket. |
| [EVOLVE-BRIDGE.md](EVOLVE-BRIDGE.md) | 2026-09-13 | 2026-09-16 | updated | `chitta-mcp/evolve/selector.py`, bridge modules and current loader contract; correct selector reference. Dated failure records preserved. |
| [EVOLVE.md](EVOLVE.md) | 2026-09-14 | 2026-09-16 | updated | Evolve cycle/selector/report, candidate fan-out and learning cohort; update overall status, retain retirement block and historical observations. |
| [FIELD_PERF.md](FIELD_PERF.md) | 2026-09-14 | 2026-09-16 | updated | Committed field-perf tables, pinned startup source, CLAUDE startup notes; label missing JSON artifacts and link surviving evidence. Measurements unchanged. |
| [HOOKS.md](HOOKS.md) | 2026-09-13 | 2026-09-16 | updated | `prompt-core.sh`, `lib.sh`, SessionStart/Stop, artifact hooks and RPC handlers; native CLI is the routine hook path, Python remains a compatibility client. |
| [PHILOSOPHY.md](PHILOSOPHY.md) | 2026-09-02 | 2026-09-16 | updated | Pinned instance-lock implementation, C++ FFI and GGUF embedding source; correct shared-field mechanism and dimensions. |
| [RENAME.md](RENAME.md) | 2026-09-02 | 2026-09-02 | current | Protected rename history, package identities, alias shim and existing skill directories. No edits. |
| [SADHANA.md](SADHANA.md) | 2026-09-02 | 2026-09-16 | updated | `sadhana_manager.hpp/.cpp`, `field_misc_sadhana.cpp`, gateway and live schemas; replace obsolete SQL/defaults and unsupported CLI examples. |
| [STRUCTURED_EXTRACTOR_DESIGN.md](STRUCTURED_EXTRACTOR_DESIGN.md) | 2026-09-02 (wrapped stamp) | 2026-09-16 | deprecated-in-place | Stop still extracts markers; native distillation shipped without this proposed Python per-turn migration. Point to HOOKS; preserve proposal body. |
| [consolidation-redesign.md](consolidation-redesign.md) | 2026-09-02, superseded | 2026-09-16 | deprecated-in-place | Rejected locking premise; v2 and FIELD_PERF document the correction. Keep linked design history; point to current performance record. |
| [consolidation-redesign-v2.md](consolidation-redesign-v2.md) | 2026-09-02 | 2026-09-16 | deprecated-in-place | `field_handler.hpp` read-path changes and FIELD_PERF supersede the old fix plan. Preserve historical analysis and link successor. |
| [recall-rerank-eval-results.md](recall-rerank-eval-results.md) | 2026-09-02, historical | 2026-09-02 | current | `scripts/recall_rerank_eval.py`, existing historical warning and recorded no-go result; not a claim about current retrieval. No edits. |
| [404.html](404.html) | None | None | current | Pages workflow and internal navigation; no stale runtime or product claims. |
| [architecture.html](architecture.html) | 2026-09-02 | 2026-09-16 | updated | ARCHITECTURE, Rust ownership/WAL, worker pool and model source; correct storage, distiller and scoring claims. |
| [benchmarks.html](benchmarks.html) | 2026-09-02 | 2026-09-16 | updated | EVALS, SMRITI evidence, analogy results, replication experiment, learning protocol/cohort; distinguish historical scores from pending causal evidence. |
| [changelog.html](changelog.html) | None | 2026-09-16 | updated | CHANGELOG Unreleased and 5.72.0 transcribed; older rendered entry text preserved, rename tooltips added, full intervening history linked. |
| [chitta-field.html](chitta-field.html) | 2026-09-02 | 2026-09-16 | updated | Pinned Rust field/log/index implementation, FIELD_PERF and CLAUDE; replace zero-lock/durability/startup claims and obsolete build example. |
| [cli.html](cli.html) | 2026-09-02 | 2026-09-16 | updated | CLI help, daemon parser, CLI.md; native ledger, queue, startup/lock and enrichment distinctions. |
| [constellation.html](constellation.html) | 2026-09-02 | 2026-09-02 | current | Page navigation and referenced visualization assets; illustrative graph and data unchanged. |
| [context.html](context.html) | 2026-09-02 | 2026-09-16 | updated | `compact.cpp`, PreCompact hook and API schema; correct embedding backend and distinguish compaction output from harness control. |
| [getting-started.html](getting-started.html) | 2026-09-02 | 2026-09-16 | updated | README, installer and actual maintenance skill directories; explain retained skill suffixes and startup readiness. |
| [hooks.html](hooks.html) | 2026-09-02 | 2026-09-16 | updated | HOOKS and hook source; native lifecycle, task ledger, artifact traces and Codex unknown outcomes. |
| [index.html](index.html) | None | 2026-09-16 | updated | README, current release notes, recall/eval evidence and Rust ownership/WAL; replace stale highlights and persistence claims. |
| [philosophy.html](philosophy.html) | None | 2026-09-16 | updated | PHILOSOPHY, compiled embedding identity and worker pool; correct dimension and separate-database claims. |
| [recall.html](recall.html) | 2026-09-02 | 2026-09-16 | updated | HOOKS, `prompt-core.sh`, recall handler, gateway and pinned `scoring/config.rs`; relation transfer, priors, admission and default-off replication. |
| [sadhana.html](sadhana.html) | None | 2026-09-16 | updated | SADHANA and handler/manager source; correct invocation, defaults and persistence; retain illustrative historical agents. |
| [skills.html](skills.html) | 2026-09-02 | 2026-09-16 | updated | Repository skill directory names and RENAME; explain unchanged maintenance suffixes instead of inventing commands. |
| [tools.html](tools.html) | None; generated 2026-09-02 | 2026-09-16 | updated | Same three tool surfaces as API; regenerated in existing layout with native CLI appendix. |
| [DOCS-AUDIT-2026-09-16.md](DOCS-AUDIT-2026-09-16.md) | New | 2026-09-16 | updated | This inventory, source review and the Evidence section below. |

## Dispositions and gate boundaries

No design memo is deleted: the consolidation pair records a linked correction
of a mistaken premise; the extractor proposal records an unshipped migration.
The frozen-replica guide still describes a shipped tool and was updated. The
rerank no-go memo remains an explicitly dated result, not active implementation
guidance. Decision memos, rename history and CHANGELOG.md are byte-unchanged.

The initial stale-name lead overstates the remaining site hits in this baseline.
The retained hits identify rename aliases, actual maintenance skill suffixes and
two historical release references with explanatory tooltips. Neither environment
aliases nor real command names were mechanically renamed. `--no-enrich` and the
hint worker still exist in source: the former configures an inert code worker
and does not disable the separate script-based hint worker.

`scripts/check-docs-links.sh` checks local files, assets and anchors in Markdown
and HTML, plus README, ignoring code examples and remote URLs. It explicitly
reports 35 editor-style `path:line` citations in the two immutable decision
memos as historical, nonportable citations. They include removed code and private
evidence and are not silently counted as resolving hyperlinks. Use
`--verbose-history` to list each exemption. This preserves the owner's immutable
history rule; it is not a claim that those old citations work in a browser.

The stale-term search necessarily also finds this inventory's link to the
protected September 15 admission/analogy memo. That row is an archive reference,
not a revived behavior. All other retained matches belong to protected decision
history, EVOLVE's retirement block or rendered CHANGELOG history. The Evidence
section below preserves the exact pre-fold search results and regression outcomes.

## Evidence

These exact search results were captured before folding the evidence into this
audit. Their file and line references are preserved verbatim; searches of the
completed audit also match the quoted evidence itself.

### Link-check output

```text
Checked 36 pages, 642 local links: 0 errors.
Preserved 35 historical path:line citations in immutable decision memos (not portable hyperlinks).
```

The 35 exemptions are editor-style path:line citations inside the two immutable dated decision memos. Some point to private evidence or code since removed. They are explicitly reported rather than claimed as working browser links. Run `bash scripts/check-docs-links.sh --verbose-history` to enumerate them. This is a documented boundary imposed by the instruction to preserve those memos.

### Rename search — exact result list

All hits are rename explanations, real maintenance skill suffixes explicitly marked as retained after the rename, or two historical release identifiers with rename tooltips.

```text
docs/changelog.html:1189:          <li>Restored missing <code title="Historical skill name retained after the chitta rename">cc-soul-mcp</code> skill (referenced in marketplace.json but absent from repo)</li>
docs/changelog.html:1235:          <li><strong>Auto-configure MCP via plugin</strong> &mdash; Plugin installer now configures <code>mcpServers.chitta</code> automatically; <code title="Historical skill name retained after the chitta rename">/cc-soul-mcp</code> skill deprecated for most users</li>
docs/getting-started.html:120:        <div class="code-block"><span class="command">/chitta:cc-soul-mcp</span> <span class="comment"># Skill suffix retained after the chitta rename</span></div>
docs/getting-started.html:128:      <p style="font-size: var(--text-sm); color: var(--ash-300); line-height: 1.8; margin-top: var(--space-3);">Upgrading from the old <code style="color: var(--aura-300);">cc-soul</code> marketplace? Nothing you have breaks &mdash; every <code style="color: var(--aura-300);">CC_SOUL_*</code> variable, your stored realms and your existing store all keep working. The three commands to move to the renamed marketplace are in <a href="https://github.com/genomewalker/chitta/blob/main/docs/RENAME.md" target="_blank" rel="noopener">the rename note</a>.</p>
docs/getting-started.html:240:            <td><span class="env-var-name">/chitta:cc-soul-setup</span> (suffix retained after rename)</td>
docs/getting-started.html:244:            <td><span class="env-var-name">/chitta:cc-soul-update</span> (suffix retained after rename)</td>
docs/getting-started.html:248:            <td><span class="env-var-name">/chitta:cc-soul-mcp</span> (suffix retained after rename)</td>
docs/index.html:93:        <strong>The project is now chitta.</strong> Plugin, marketplace and repository moved to the name the binaries always had. Every <code>CC_SOUL_*</code> variable, stored realm and existing store keeps working through an alias shim &mdash; see the <a href="https://github.com/genomewalker/chitta/blob/main/docs/RENAME.md" class="link-sandal" target="_blank" rel="noopener">rename note</a> for the three migration commands.
docs/recall.html:308:        <p class="section-intro">The project was renamed from cc-soul on 2026-09-02; legacy <code>CC_SOUL_*</code> aliases are documented in <a href="RENAME.md">the rename note</a>.</p>
docs/skills.html:655:        <div class="tool-row"><span class="tool-name">/chitta:cc-soul-setup</span><span class="tool-desc">Build from source; skill suffix retained after rename (C++20 compiler, CMake, make).</span></div>
docs/skills.html:656:        <div class="tool-row"><span class="tool-name">/chitta:cc-soul-update</span><span class="tool-desc">Update binaries; skill suffix retained after rename.</span></div>
docs/skills.html:657:        <div class="tool-row"><span class="tool-name">/chitta:cc-soul-daemon</span><span class="tool-desc">Manage the daemon; skill suffix retained after rename.</span></div>
docs/skills.html:658:        <div class="tool-row"><span class="tool-name">/chitta:cc-soul-shutdown</span><span class="tool-desc">Graceful shutdown; skill suffix retained after rename.</span></div>
docs/skills.html:659:        <div class="tool-row"><span class="tool-name">/chitta:cc-soul-mcp</span><span class="tool-desc">Configure MCP; skill suffix retained after rename (<code>mcp__chitta__*</code>).</span></div>
README.md:4:> Renamed from cc-soul on 2026-09-02; legacy CC_SOUL_* aliases remain supported — see [docs/RENAME.md](docs/RENAME.md).
README.md:194:| [RENAME.md](docs/RENAME.md) | Migrating from `cc-soul`, and the `CC_SOUL_*` alias table |
```

### Retirement/history search — exact result list

Matches are protected decision history, EVOLVE's retirement block, rendered CHANGELOG history, and one audit inventory row linking the protected decision memo. That inventory reference is the only match outside the requested history-file allowlist; a complete per-file audit necessarily names the memo. No current behavior uses the retired mechanisms.

```text
docs/DECISION-2026-09-15-learning-experiment.md:5:> `DECISION-2026-09-15-mdl-analogy.md` section 1 and the pending block in
docs/DECISION-2026-09-15-mdl-analogy.md:7:**The MDL premise also needs updating.** The live log contains a pooled acceptance: saving **97 bytes**, evidence **70,558 bytes**, three pooled chunks, for “evolve-cycle-3-started.” That contradicts “never accepts”; it does not establish useful selection. The private replay’s **0/224** remains a separate result. ([shadow log:267](/home/kbd606/.claude/mind/mdl_gate_shadow.jsonl:267), [EVOLVE.md:74](/projects/caeg/scratch/kbd606/tmp/codex-wt-consult/docs/EVOLVE.md:74))
docs/DECISION-2026-09-15-mdl-analogy.md:9:## 1. MDL: choose **(c), drop admission compression and rely on recall ranking**
docs/DECISION-2026-09-15-mdl-analogy.md:11:**Diagnosis:** zlib savings are the wrong operational proxy for useful conversational learning. This rejects the chosen compressor/evidence model, not MDL in general. The corpus statistic rewards reduced compressed bytes, charges raw learning length, and replaces part of a 32 KiB dictionary. It measures string reuse plus dictionary displacement; nothing in that statistic measures whether advice changes a later decision correctly. ([mdl_gate.hpp:143](/projects/caeg/scratch/kbd606/tmp/codex-wt-consult/chitta/include/chitta/mdl_gate.hpp:143))
docs/DECISION-2026-09-15-mdl-analogy.md:25:Retire the native MDL shadow tap, pooling, and corpus-dictionary machinery now. More compressor tuning is not the next experiment.
docs/DECISION-2026-09-15-mdl-analogy.md:37:**Narrow contract:** infer the directed predicate connecting `a` and `b`; return actual matching neighbors of `c`, with supporting edges. Abstain on missing or ambiguous relations. Use indexed graph lookups. Remove structural mode and VSA ranking from this endpoint; retain ordinary graph storage/querying.
docs/DOCS-AUDIT-2026-09-16.md:32:| [DECISION-2026-09-15-mdl-analogy.md](DECISION-2026-09-15-mdl-analogy.md) | 2026-09-15 | 2026-09-15 | current | Protected dated consultation; later implementation is recorded in EVOLVE and EVALS. Text unchanged. This row indexes history, not an active policy. |
docs/EVOLVE.md:52:> Status as of 2026-09-15 — **MDL admission compression retired.** The reported
docs/EVOLVE.md:57:> [decision memo, section 1](DECISION-2026-09-15-mdl-analogy.md#1-mdl-choose-c-drop-admission-compression-and-rely-on-recall-ranking)
docs/EVOLVE.md:61:> dictionaries/bootstrap, Python judging, environment knobs and MDL telemetry
docs/EVOLVE.md:86:> `~/.claude/mind/mdl_gate_shadow.jsonl` stays untouched and inert: retirement
docs/changelog.html:479:          <li><code>recall_analogy</code> is explicit relation transfer only (a:b :: c:?): the predicate(s) linking a→b come from indexed triplet lookups and the answers are c's actual neighbours over those predicates, with supporting edges and a <code>reason</code> on abstention. Structural mode and VSA ranking are gone from the endpoint. Frozen-replica benchmark: hit@1 14/14, hit@3 14/14, 14/14 negative abstentions, 0 unsupported answers (<a href="EVALS.md">docs/EVALS.md</a>).</li>
docs/changelog.html:489:        <ul><li>Retired MDL admission compression on 2026-09-15: native and Bash shadow taps, same-chunk pooling, corpus dictionaries/bootstrap, Python mirror, test targets, environment knobs and evolve coverage telemetry. Distilled learning storage, deduplication and recall ranking remain unchanged; no utility-posterior hard gate replaces it. Historical shadow logs remain untouched and inert. The paired 20-task automatic-learning experiment remains pending (see <a href="EVOLVE.md">docs/EVOLVE.md</a>).</li></ul>
docs/changelog.html:499:          <li>Task ledger (threads, inbox, artifacts, session bindings, leases) moved from <code>~/.claude/task-ledger.db</code> (NFS sqlite) into the daemon (<code>ledger_op</code> RPC) with an idempotent <code>task_ledger.py migrate</code>.</li>
docs/changelog.html:500:          <li>Queue lives at <code>&lt;mind&gt;/queue.jsonl</code>; tag recall is a hard filter with realm applied after; correction lane realm-scoped; MDL small-evidence pooling (shadow).</li>
docs/changelog.html:512:        <ul><li>Proposal cards carry <code>verifiability</code>, <code>prior_effort</code>, <code>internal_evidence</code>; selector weights them; every cycle requires a <code>SELF_CHECK</code> test and runs a bounded survey phase first; nightly/weekly timers; <code>select.py</code> renamed <code>selector.py</code>.</li></ul>
```

### Regression checks and commit exception

- Link checker and positive/negative fixtures pass; shell syntax passes. Fixture coverage includes duplicate heading slugs, setext, explicit HTML anchors, reference links, code fences and three broken targets.
- Ruff passes on scripts/gen-tools-docs.py. Repeated generation is byte-identical.
- All 19 existing hook test scripts passed, with test_session_start_concurrency.sh requiring one rerun: its first run caught a child still present at a deadline. The unchanged script passed fully in a fresh isolated environment on rerun.
- MCP unittest discover: 136 tests, 1 error in unchanged test_http_sessions.SDKSessionTests.test_initialize_cap_expire_and_expired_id_is_404. Installed SDK 1.27.2 lacks BoundedSessionManager._session_owners, required by unchanged server.py:2932. This existing SDK incompatibility is also recorded in FIELD_PERF.md. No tests were disabled or edited.
- SMRITI unittest discover: 46 tests passed.
- Tests ran with a scratch HOME/mind/queue/runtime and inert default CLI binaries; fixtures supplied their own mocks. No service restart or live mutation was used.
- Protected decision memos, CHANGELOG.md, RENAME.md, styles/CNAME/favicon, visualization assets/data and ledger-migration evidence are unchanged. Hooks, runtime, benchmarks and existing tests have no diff.

The owner explicitly authorized committing with the known `_session_owners` MCP
error on 2026-09-16. The owner reports that another worker has fixed it on a
separate branch, which will be merged before this branch. The failing gate is
recorded as an exception, not a pass; no tests were disabled or edited.

## Marketing pass

Status as of 2026-09-16. Baseline: `59338345`, merged from `main` into
`chore/docs-cleanup` before editing. This pass replaces promotional copy with
descriptions of storage, indexing, retrieval, maintenance, and agent workflows.
Recorded measurements, command names, configuration values, and rename notes
remain. Installation deadlines and predicted user outcomes were removed because
they were unsupported promises, not measured results.

Pages touched: all 16 top-level HTML pages — `404.html`, `architecture.html`,
`benchmarks.html`, `changelog.html`, `chitta-field.html`, `cli.html`,
`constellation.html`, `context.html`, `getting-started.html`, `hooks.html`,
`index.html`, `philosophy.html`, `recall.html`, `sadhana.html`, `skills.html`,
and `tools.html`. The changelog edit is limited to its footer; the rendered
release-entry block is byte-identical. Layout containers, stylesheets, inline
styles, scripts, navigation, link targets, and IDs are preserved. Removed
decorative emoji leave their existing layout slots.

Other edited reader entry points: `README.md`, the pointer in `CLAUDE.md`,
`codex-plugin/AGENTS.md`, `.claude-plugin/plugin.json`, and both descriptions
in `.claude-plugin/marketplace.json`. The plugin descriptions use the 344-entry
count recorded in `API.md`. Markdown edits cover `API.md`, `HOOKS.md`,
`PHILOSOPHY.md`, and this audit. The lean and shipped CLAUDE pointer variants
were reviewed and already contain technical constraint summaries.

Canonical skill edits: `checkpoint`, `epsilon-yajna`, `init`, `prog-review`,
`reawaken`, `recap`, `remember`, and `ultrathink`, plus the completion
message in `shepherd/reference/initialize-and-loop.md`. The corresponding
Codex files were regenerated with `scripts/sync-skills.sh`.

### Fixed-regex grep list

The command below searches every tracked file in this pass's scope, including
this audit and the generated skill mirror. Exclusions are preserved history:
dated decisions, rename history, rendered changelog, the retired extractor and
consolidation proposals, and the historical rerank result. Nested research posts,
Vedanta notes, visualization archives/data, runtime sources, and `contracts/`
are outside this top-level documentation pass. No current entry page is excluded
except the changelog file, whose non-entry wrapper was separately reviewed.

The fixed alternatives cover the requested terms and additional phrases found
during review. Bracketing each alternative's first letter prevents the regex
definition itself from becoming a match in this audit. Technical syntax such as
shell negation, HTML comments, CSS priority, and SSL negation is preserved;
rendered prose was separately checked for decorative emoji and exclamations.

Run from the repository root:

```bash
pattern='\b([z]ero[[:space:]-]+configuration|[b]lazing|[s]tate[[:space:]-]+of[[:space:]-]+the[[:space:]-]+art|[r]evolutionary|[s]eamless|[p]owerful|[e]ffortless|[k]ey[[:space:]]+innovation|[y]ou['\''’]ll[[:space:]]+love|[g]ets[[:space:]]+smarter|[h]igh-performance|[t]oken-savvy|[s]ignal[[:space:]]+gold|[p]ure[[:space:]]+signal|[h]ighest-ROI|[r]icher[[:space:]]+partnership|[o]ne[[:space:]]+soul|[f]irst[[:space:]]+30[[:space:]]+days|[i]nstant[[:space:]]+recall|[s]tarts?[[:space:]]+instantly|[n]othing[[:space:]]+is[[:space:]]+lost|[s]oul[[:space:]]+awakens|[b]uilt[[:space:]]+with[[:space:]]+conviction|[g]rounded[[:space:]]+in[[:space:]]+philosophy|[g]row[[:space:]]+wiser|[n]ot[[:space:]]+stateless|[n]ot[[:space:]]+shallow|[p]rofound[[:space:]]+insight|[v]alidated[[:space:]]+by[[:space:]]+engineering|[i]nverts[[:space:]]+this[[:space:]]+paradigm|[j]ust[[:space:]]+works|[f]orever|[b]reakthrough[[:space:]]+moment|[t]he[[:space:]]+AI[[:space:]]+harness[[:space:]]+that[[:space:]]+learns)\b'
git grep -n -i -E "$pattern" -- \
  README.md AGENTS.md ':(glob)CLAUDE*.md' \
  ':(glob).claude-plugin/*.md' ':(glob).claude-plugin/*.json' \
  codex-plugin/AGENTS.md ':(glob)skills/**/*.md' \
  ':(glob)codex-plugin/skills/**/*.md' \
  ':(glob)docs/*.md' ':(glob)docs/*.html' \
  ':!docs/DECISION-*' ':!docs/RENAME.md' ':!docs/changelog.html' \
  ':!docs/STRUCTURED_EXTRACTOR_DESIGN.md' \
  ':!docs/consolidation-redesign*.md' ':!docs/recall-rerank-eval-results.md'
result=$?
printf 'grep exit: %s (1 means zero matches)\n' "$result"
test "$result" -eq 1
```

Exact result list:

```text
(no matching file:line entries)
grep exit: 1 (1 means zero matches)
```

### Validation

- `bash scripts/check-docs-links.sh`: 36 pages, 643 local links, **0 errors**.
  The same 35 historical editor-style citations remain explicitly reported.
- Fixed-regex grep: **0 matches** across the declared scope.
- `bash scripts/check-skills-sync.sh`: **skills in sync** after regeneration.
- Hooks: all **21 shell scripts passed**. The first run of
  `test_session_start_cards.sh` encountered an address-in-use error because the
  outer runner reused a scratch socket path across scripts. The unchanged test
  passed all six cases with its own fresh scratch directory and socket.
- MCP: **149 tests passed**. The earlier audit's SDK failure is absent after the
  merge from main; no exception is needed for this pass.
- SMRITI: **46 tests passed**.
- Hook and unit-test child processes used temporary home, mind, queue, runtime,
  and socket paths. The CLI discovery fixture used a client compiled from this
  worktree into `/tmp`; no binaries were installed and no live daemon was used.
- `git diff --check`: passed. No Python or shell source files were edited;
  Ruff and changed-shell syntax checks are not applicable.
- Hooks, existing tests, runtime sources, benchmarks, contracts, dated decision
  memos, rename history, and `CHANGELOG.md` have no diff from the merged baseline.
  `Plan.md` and `Documentation.md` remain untracked.

## Site UX pass

Status as of 2026-09-16 · Unreleased + v5.72.0.

All 79 HTML pages now share the same ordered static menu, home link, skip link, focusable main landmark, current-page marker, and repository/MIT/version/status footer. Each page has the `chitta — <page>` title format, language, viewport, description and favicon. The current-page marker is on the footer permalink for documents outside the menu (dream articles and 404).

The existing design tokens and dark content palette remain. Navigation wraps without JavaScript; shared chrome follows light OS preference. Tables are named keyboard-scrollable regions; code scrolls; focus and reduced-motion rules are shared. Static style attributes moved to deduplicated CSS classes; visualization scripts retain runtime styles. Content is readable if reveal scripts do not run. Native fragment navigation now preserves URL history and skip-link focus; old scroll-only interceptors were removed. Root-relative URLs support nested 404 requests.

### Changes per page

Every row includes the common changes above. Dream article prose, dates and citations are retained; the malformed generated gap-recall metadata is repaired explicitly below.

| Page | Additional changes |
|---|---|
| [404.html](404.html) | Replaced standalone styles with the shared design; root-relative assets and recovery links work for nested missing URLs. |
| [architecture.html](architecture.html) | Extracted static styles; wrapped architecture tables; retained diagrams and animation scripts. |
| [benchmarks.html](benchmarks.html) | Promoted every TOC category label to a heading; wrapped results tables. |
| [brain-viz/index.html](brain-viz/index.html) | Added h1, description and favicon; added shared chrome around the existing architecture visualization. |
| [changelog.html](changelog.html) | Release labels are h2, change categories h3; normalized status separator; preserved release history. |
| [chitta-field.html](chitta-field.html) | Corrected card heading levels; wrapped tables; retained the incoming References block byte-for-byte. |
| [cli.html](cli.html) | Promoted TOC category labels to headings; extracted static styles; wrapped tables. |
| [constellation.html](constellation.html) | Kept graph modes and data; extracted static styles; placed header and visualization in main. |
| [context.html](context.html) | Included page header in main; extracted static styles; wrapped tables. |
| [dreams/2026-02-22-ancient-dna-damage-authentication-sediment-limits.html](dreams/2026-02-22-ancient-dna-damage-authentication-sediment-limits.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-22-apoha-contrastive-learning.html](dreams/2026-02-22-apoha-contrastive-learning.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-22-arrow-of-time-causal-primitive.html](dreams/2026-02-22-arrow-of-time-causal-primitive.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-22-crn-turing-completeness-chemistry-computation.html](dreams/2026-02-22-crn-turing-completeness-chemistry-computation.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-22-curry-howard-proofs-programs-classical-fracture.html](dreams/2026-02-22-curry-howard-proofs-programs-classical-fracture.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-22-explanatory-gap-levine-consciousness.html](dreams/2026-02-22-explanatory-gap-levine-consciousness.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-22-fitness-beats-truth-eaan-evolutionary-epistemology.html](dreams/2026-02-22-fitness-beats-truth-eaan-evolutionary-epistemology.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-22-ghost-lines-get-transcript-temporal-displacement.html](dreams/2026-02-22-ghost-lines-get-transcript-temporal-displacement.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-22-godel-incompleteness-formal-mechanism.html](dreams/2026-02-22-godel-incompleteness-formal-mechanism.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-22-integrated-information-theory.html](dreams/2026-02-22-integrated-information-theory.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-22-landauer-erasure-black-hole-information.html](dreams/2026-02-22-landauer-erasure-black-hole-information.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-22-lucas-penrose-whiteley-mirror-self-refutation.html](dreams/2026-02-22-lucas-penrose-whiteley-mirror-self-refutation.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-22-neural-criticality-stdp-cortex.html](dreams/2026-02-22-neural-criticality-stdp-cortex.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-22-past-hypothesis-thermodynamic-arrow.html](dreams/2026-02-22-past-hypothesis-thermodynamic-arrow.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-22-piraha-language-universal-grammar.html](dreams/2026-02-22-piraha-language-universal-grammar.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-22-reverse-edges-causal-introspection.html](dreams/2026-02-22-reverse-edges-causal-introspection.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-22-strange-loops-godel-consciousness.html](dreams/2026-02-22-strange-loops-godel-consciousness.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-22-three-numbers-one-posterior.html](dreams/2026-02-22-three-numbers-one-posterior.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-22-unreasonable-effectiveness-mathematics.html](dreams/2026-02-22-unreasonable-effectiveness-mathematics.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-22-vakyantra-bert-paradox-semantic-embeddings.html](dreams/2026-02-22-vakyantra-bert-paradox-semantic-embeddings.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-22-why-memory-has-a-direction-stosszahlansatz.html](dreams/2026-02-22-why-memory-has-a-direction-stosszahlansatz.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-22-wisdom-node-promotion-memory-consolidation.html](dreams/2026-02-22-wisdom-node-promotion-memory-consolidation.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-23-apoha-dharmakirti-contrastive-learning.html](dreams/2026-02-23-apoha-dharmakirti-contrastive-learning.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-23-boltzmann-brain-time-consciousness.html](dreams/2026-02-23-boltzmann-brain-time-consciousness.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-23-dignaga-russell-kripke.html](dreams/2026-02-23-dignaga-russell-kripke.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-23-fitness-beats-truth.html](dreams/2026-02-23-fitness-beats-truth.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-23-free-energy-principle-active-inference.html](dreams/2026-02-23-free-energy-principle-active-inference.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-23-hoffman-interface-theory-refutations.html](dreams/2026-02-23-hoffman-interface-theory-refutations.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-23-landauer-demon-erasure.html](dreams/2026-02-23-landauer-demon-erasure.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-23-landauer-erasure-arrow-time-debt.html](dreams/2026-02-23-landauer-erasure-arrow-time-debt.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-23-mags-ancient-modern-population-temporal.html](dreams/2026-02-23-mags-ancient-modern-population-temporal.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-23-platonism-formalism-godel-underdetermination.html](dreams/2026-02-23-platonism-formalism-godel-underdetermination.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-23-sadhana-recursive-self-improvement-strange-loop.html](dreams/2026-02-23-sadhana-recursive-self-improvement-strange-loop.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-23-soc-artifact-four-convergences.html](dreams/2026-02-23-soc-artifact-four-convergences.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-02-23-universality-classes-rg-eigenvalues.html](dreams/2026-02-23-universality-classes-rg-eigenvalues.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-04-03-attention-mechanisms-and-the-binding-problem-in-ne.html](dreams/2026-04-03-attention-mechanisms-and-the-binding-problem-in-ne.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-04-03-consciousness-and-the-hard-problem-of-subjective-e.html](dreams/2026-04-03-consciousness-and-the-hard-problem-of-subjective-e.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-04-03-entropy-and-the-arrow-of-time-in-biological-system.html](dreams/2026-04-03-entropy-and-the-arrow-of-time-in-biological-system.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-04-03-hierarchical-temporal-memory-and-sequence-learning.html](dreams/2026-04-03-hierarchical-temporal-memory-and-sequence-learning.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-04-03-sparse-distributed-representations-and-memory-effi.html](dreams/2026-04-03-sparse-distributed-representations-and-memory-effi.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-04-11-from-remembering-to-learning-autonomous-feedback.html](dreams/2026-04-11-from-remembering-to-learning-autonomous-feedback.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-04-11-the-nature-of-memory-and-forgetting-in-biological-.html](dreams/2026-04-11-the-nature-of-memory-and-forgetting-in-biological-.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-04-13-consciousness-and-the-hard-problem-of-subjective-e.html](dreams/2026-04-13-consciousness-and-the-hard-problem-of-subjective-e.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-04-16-emergent-complexity-in-distributed-systems.html](dreams/2026-04-16-emergent-complexity-in-distributed-systems.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-04-16-vedantic-philosophy-and-modern-neuroscience.html](dreams/2026-04-16-vedantic-philosophy-and-modern-neuroscience.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-04-17-self-organization-in-nature-from-cells-to-civiliza.html](dreams/2026-04-17-self-organization-in-nature-from-cells-to-civiliza.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-04-17-the-nature-of-memory-and-forgetting-in-biological-.html](dreams/2026-04-17-the-nature-of-memory-and-forgetting-in-biological-.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-04-18-consciousness-and-the-hard-problem-of-subjective-e.html](dreams/2026-04-18-consciousness-and-the-hard-problem-of-subjective-e.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-04-18-the-history-of-symbolic-ai-versus-connectionism.html](dreams/2026-04-18-the-history-of-symbolic-ai-versus-connectionism.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-04-22-attention-mechanisms-and-the-binding-problem.html](dreams/2026-04-22-attention-mechanisms-and-the-binding-problem.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-04-22-epistemic-humility-in-scientific-discovery.html](dreams/2026-04-22-epistemic-humility-in-scientific-discovery.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-05-02-the-nature-of-memory-and-forgetting-in-biological-.html](dreams/2026-05-02-the-nature-of-memory-and-forgetting-in-biological-.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-05-03-emergent-complexity-in-distributed-systems.html](dreams/2026-05-03-emergent-complexity-in-distributed-systems.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-05-19-session-id-febc8622-gdom-s-history-6-show-the-huma.html](dreams/2026-05-19-session-id-febc8622-gdom-s-history-6-show-the-huma.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-06-23-the-mathematics-of-forgetting-why-neural-networks-.html](dreams/2026-06-23-the-mathematics-of-forgetting-why-neural-networks-.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-06-29-gap-recall-episode-heavy-stores-structurally-broke.html](dreams/2026-06-29-gap-recall-episode-heavy-stores-structurally-broke.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/2026-06-29-gap-recall-tag-filter-use-type-wisdom-gap-recall-g.html](dreams/2026-06-29-gap-recall-tag-filter-use-type-wisdom-gap-recall-g.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. Repaired multiline title and unescaped quotes in description/social metadata. |
| [dreams/2026-09-02-epistemic-humility-in-scientific-discovery.html](dreams/2026-09-02-epistemic-humility-in-scientific-discovery.html) | Updated archive title and chrome branding; retained article text, date, references and return-to-dreams link. |
| [dreams/index.html](dreams/index.html) | Replaced old branding in chrome/metadata; retained dated archive cards and existing local styles. |
| [evolve-bridge-live/index.html](evolve-bridge-live/index.html) | Added an HTML landing page listing existing evidence files with their formats; artifacts remain unchanged. |
| [getting-started.html](getting-started.html) | Extracted static styles; wrapped setup tables; retained setup anchors. |
| [hooks.html](hooks.html) | Promoted TOC category labels to headings; extracted static styles; wrapped tables. |
| [index.html](index.html) | Added main landmark; corrected card headings; extracted static styles and wrapped the comparison table. |
| [mind-viz/index.html](mind-viz/index.html) | Added h1, description and favicon; contained graph/panels below navigation; responsive stacked panels, container resizing and bounded dragging; computed-style panel toggles. |
| [philosophy.html](philosophy.html) | Extracted static styles; wrapped comparison tables; retained philosophy content. |
| [recall.html](recall.html) | Promoted TOC category labels to headings; normalized the status separator; wrapped tables. |
| [sadhana.html](sadhana.html) | Corrected memory/use-case headings; extracted static styles; wrapped tables. |
| [skills.html](skills.html) | Promoted skill categories to headings; normalized the status separator; extracted static styles. |
| [tools.html](tools.html) | Promoted tool categories to headings; normalized the status separator; wrapped every parameter table. |
| [vedanta/index.html](vedanta/index.html) | Added an HTML landing page linking the existing Markdown collection; original Markdown remains unchanged. |

### Structural gate table

Generated with `python3 scripts/check-site.py`. Chrome checks exact menu/footer markup and current-page identity; Metadata checks document metadata; Headings checks hierarchy, IDs and TOC targets; Layout checks main structure, CSS contracts, table regions and image alt attributes; Links checks published local files and HTML fragments.

| Page | Chrome | Metadata | Headings | Layout | Links | Result |
|---|---|---|---|---|---|---|
| 404.html | PASS | PASS | PASS | PASS | PASS | PASS |
| architecture.html | PASS | PASS | PASS | PASS | PASS | PASS |
| benchmarks.html | PASS | PASS | PASS | PASS | PASS | PASS |
| brain-viz/index.html | PASS | PASS | PASS | PASS | PASS | PASS |
| changelog.html | PASS | PASS | PASS | PASS | PASS | PASS |
| chitta-field.html | PASS | PASS | PASS | PASS | PASS | PASS |
| cli.html | PASS | PASS | PASS | PASS | PASS | PASS |
| constellation.html | PASS | PASS | PASS | PASS | PASS | PASS |
| context.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-22-ancient-dna-damage-authentication-sediment-limits.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-22-apoha-contrastive-learning.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-22-arrow-of-time-causal-primitive.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-22-crn-turing-completeness-chemistry-computation.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-22-curry-howard-proofs-programs-classical-fracture.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-22-explanatory-gap-levine-consciousness.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-22-fitness-beats-truth-eaan-evolutionary-epistemology.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-22-ghost-lines-get-transcript-temporal-displacement.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-22-godel-incompleteness-formal-mechanism.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-22-integrated-information-theory.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-22-landauer-erasure-black-hole-information.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-22-lucas-penrose-whiteley-mirror-self-refutation.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-22-neural-criticality-stdp-cortex.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-22-past-hypothesis-thermodynamic-arrow.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-22-piraha-language-universal-grammar.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-22-reverse-edges-causal-introspection.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-22-strange-loops-godel-consciousness.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-22-three-numbers-one-posterior.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-22-unreasonable-effectiveness-mathematics.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-22-vakyantra-bert-paradox-semantic-embeddings.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-22-why-memory-has-a-direction-stosszahlansatz.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-22-wisdom-node-promotion-memory-consolidation.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-23-apoha-dharmakirti-contrastive-learning.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-23-boltzmann-brain-time-consciousness.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-23-dignaga-russell-kripke.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-23-fitness-beats-truth.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-23-free-energy-principle-active-inference.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-23-hoffman-interface-theory-refutations.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-23-landauer-demon-erasure.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-23-landauer-erasure-arrow-time-debt.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-23-mags-ancient-modern-population-temporal.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-23-platonism-formalism-godel-underdetermination.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-23-sadhana-recursive-self-improvement-strange-loop.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-23-soc-artifact-four-convergences.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-02-23-universality-classes-rg-eigenvalues.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-04-03-attention-mechanisms-and-the-binding-problem-in-ne.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-04-03-consciousness-and-the-hard-problem-of-subjective-e.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-04-03-entropy-and-the-arrow-of-time-in-biological-system.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-04-03-hierarchical-temporal-memory-and-sequence-learning.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-04-03-sparse-distributed-representations-and-memory-effi.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-04-11-from-remembering-to-learning-autonomous-feedback.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-04-11-the-nature-of-memory-and-forgetting-in-biological-.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-04-13-consciousness-and-the-hard-problem-of-subjective-e.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-04-16-emergent-complexity-in-distributed-systems.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-04-16-vedantic-philosophy-and-modern-neuroscience.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-04-17-self-organization-in-nature-from-cells-to-civiliza.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-04-17-the-nature-of-memory-and-forgetting-in-biological-.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-04-18-consciousness-and-the-hard-problem-of-subjective-e.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-04-18-the-history-of-symbolic-ai-versus-connectionism.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-04-22-attention-mechanisms-and-the-binding-problem.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-04-22-epistemic-humility-in-scientific-discovery.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-05-02-the-nature-of-memory-and-forgetting-in-biological-.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-05-03-emergent-complexity-in-distributed-systems.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-05-19-session-id-febc8622-gdom-s-history-6-show-the-huma.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-06-23-the-mathematics-of-forgetting-why-neural-networks-.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-06-29-gap-recall-episode-heavy-stores-structurally-broke.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-06-29-gap-recall-tag-filter-use-type-wisdom-gap-recall-g.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/2026-09-02-epistemic-humility-in-scientific-discovery.html | PASS | PASS | PASS | PASS | PASS | PASS |
| dreams/index.html | PASS | PASS | PASS | PASS | PASS | PASS |
| evolve-bridge-live/index.html | PASS | PASS | PASS | PASS | PASS | PASS |
| getting-started.html | PASS | PASS | PASS | PASS | PASS | PASS |
| hooks.html | PASS | PASS | PASS | PASS | PASS | PASS |
| index.html | PASS | PASS | PASS | PASS | PASS | PASS |
| mind-viz/index.html | PASS | PASS | PASS | PASS | PASS | PASS |
| philosophy.html | PASS | PASS | PASS | PASS | PASS | PASS |
| recall.html | PASS | PASS | PASS | PASS | PASS | PASS |
| sadhana.html | PASS | PASS | PASS | PASS | PASS | PASS |
| skills.html | PASS | PASS | PASS | PASS | PASS | PASS |
| tools.html | PASS | PASS | PASS | PASS | PASS | PASS |
| vedanta/index.html | PASS | PASS | PASS | PASS | PASS | PASS |

79 pages checked; 0 failed.

### Verification and limits

- `bash scripts/check-docs-links.sh`: recursive HTML plus existing Markdown checks; 102 documents, 3,098 local links, zero errors. Historical decision-memo path:line exemptions remain reported.
- Gate mutation tests: valid fixture plus 19 broken-page variants, URL resolution and script parsing checks pass. Ruff and shell syntax checks pass.
- Repository-required hook suites pass after rerunning session cards in fresh scratch state (the first shared scratch socket was already bound). CLI discovery is skipped because this worktree has no built CLI. MCP: 149 tests pass on retry; the first run hit a process-start timing race before its PID fixture existed. SMRITI: 46 tests pass. All ran against temporary state and stubs, without contacting the live daemon.
- No browser is available: layout at actual viewport sizes, zoom, contrast, focus traversal, assistive technology, WebGL/canvas rendering, panel interaction and OS-theme appearance were not visually verified. Remote links, font/CDN availability and live visualization backends were not probed.
- Additional JavaScript syntax verification could not start: the installed Node binary fails loading `sqlite3session_attach`. No JavaScript execution or browser interaction is claimed.

### Final main integration

Merged `main` again after the UX implementation. The incoming marketing copy, citation anchors, both earlier audit sections, bibliography CSS, and documentation CI job are retained. All 12 generated HTML References blocks match the incoming `main` byte-for-byte. The citation usage index is refreshed because layout changes moved source line numbers; canonical source records and bibliography content are unchanged. The structural gate still reports 79/79 pages passing after conflict resolution.
