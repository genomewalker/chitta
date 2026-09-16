# Changelog

All notable changes to chitta (formerly cc-soul; renamed 2026-09-02, see
[docs/RENAME.md](docs/RENAME.md)) are documented here.

> **Gap backfilled, 2026-09-13.** 5.42.0 through 5.70.4 were originally
> released without changelog entries (~419 commits). The sections below were
> reconstructed from `git log` between tags; patch releases are grouped under
> their minor version (`## [5.x.y]`) with per-release dates on one line.

## [Unreleased]

- Move SessionStart ledger/task/handoff cards and Stop capsule assembly behind
  `ledger_op`; route admitted Stop turns through its `hook_turn` operation while
  preserving transcript event payloads and durable cursor acknowledgement.
  Fix queued `ledger_op` being silently ignored, with a real queued-capsule
  regression test. Add positive handoff/Stop and compact-card parity fixtures;
  keep local transcript parsing and bounded compatibility fallbacks.

- Route prompt lane fusion through one `prompt_context` call. Existing daemon
  recall handlers retain their query identities and scoring; C2 extraction,
  lane retagging/type filters, empty-realm retry and admission now assemble in
  process. Return per-lane status and timing plus embedding/retrieval/admission
  spans. Adjacent unchanged-hook controls preserve raw output parity when
  SessionStart correction ordering drifts between whole-suite passes.

- Add advanced `prompt_context` RPC for shared Claude Code/Codex prompt admission,
  C2 labels, hash deduplication and lane accounting. Its CLI-local timeout fallback
  uses the same native policy; older clients retain shell compatibility. Synthetic
  legacy/native and timeout outputs match byte for byte; seven replica hook
  fixtures pass three paired repetitions against unchanged hooks on one daemon.
  Regenerate RPC contracts, MCP schemas and tool documentation.

- Preserve calibrated recall confidence when repository sources are merged.
  Source BM25 ranks have no similarity calibration, so `max_relevance` and
  abstention retain their memory values; `source_hits` reports source coverage.

- Add recall `sources` (boolean, default true). `sources=false` preserves the
  memory-only ranking for ID-based evaluations; regenerate the recall input
  contract, MCP static schema and generated tool documentation.

- Index repository Markdown headings and code with source identities, SHA-256
  validation on startup/query, deletion-aware FileChanged refresh and cited
  `[doc]`/`[code]` recall rows. Frozen-family current-truth improves 20/50 to
  36/50; the current-truth and golden recall exit gates remain unmet.

### Added
- Markdown heading extraction through code intel, including hierarchical heading
  names, duplicate-heading disambiguation, fenced examples and Setext headings.
  Repository recall integration now validates sources on startup and query.
- Derived source anchors for hook file facts, with replay-safe lifecycle indexing,
  same-anchor supersession, visible freshness state and equal-score stale demotion.
  No snapshot format or queue dispatcher/ack-ledger changes.
- Stop-to-SessionStart handoff capsules in task-ledger session metadata, carrying
  an explicit next action, branch, artifact paths, blocker and source provenance.
  Capsules render before other context on the matching project and branch;
  turns without a verified action invalidate the previous session capsule.
- Clear handoff capsules even on short completion turns; add transcript-pair
  construction and strict continuation scoring with source digests. Read all
  641 authorized transcripts, pair within project directories and select the
  newest 20 cases. Both scorers produce 0/20 on that fixture: every preceding
  session lacks an explicit final plan or usable ledger action. The 18/20 gate
  remains unmet; scoring now reports branch mismatches and per-pair reasons.
### MCP surface reduction (Phase 4, 2026-09-16)

- Advertised MCP discovery: **89 → 54 tools**, **11,027 → 6,326 estimated
  tokens** (compact tools/list result, ceil(characters / 4), Anthropic
  approximation; no local Anthropic tokenizer). The actual filtered MCP list
  is measured, not the daemon's 319 schemas or the 359 pre-filter MCP rows.
  All 415 distinct schema-backed tools/native handlers are tiered: 54 core,
  302 advanced, 59 hidden. Every retained input schema and description is
  unchanged. No daemon implementation, recall behavior or handler was deleted.
- `scripts/check-mcp-surface.py` calls production `server.list_tools()` and
  enforces `--max-core 80 --max-tokens 8000` by default. New tools default to
  advanced. Hidden direct calls remain compatible; the advanced gateway now
  dispatches MCP composites locally instead of incorrectly forwarding them.
  Promoted tools also retain their existing advanced(tool=...) call path.
- Evidence: canonical skills, hook call sites/advisories, README and CLI examples;
  read-only outcome ledger scan for 2026-08-17–2026-09-16 (9,996 records).
  The ledger contains **no explicit MCP-call records**, so absence is not
  evidence of disuse. Truncated Bash command heads yield partial CLI evidence:
  remember 38, recall 21, smart_recall/get 4 each, forget 3,
  memory_outcome/recall_lanes/query_graph/recall_analogy 2 each,
  realm_detect/triplets/recall_keyword/checkpoint/memory_edit/codebase_overview/
  sadhana_start 1 each. Live read-only health_check and the RPC budget tracker
  expose only aggregate counters, not a per-tool call history.
- Core covers these observed workflows through their tools and existing
  learn/research/sadhana/triplets/memory_edit gateways. Existing hook plumbing
  stays hidden; specialized callers retain direct access. Code helpers without
  evidence in those consumers remain advanced. `advanced` itself is core so
  every unadvertised handler can be discovered and called on demand.
- Regenerate schemas and docs with `python3 scripts/gen-tools-static.py --docs`.
  This runs `scripts/gen-tools-docs.py` with current metadata readers: the old
  standalone renderer still assumes pre-extraction policy and CLI tables.
  Docs use frozen MCP schemas and list all 76 native-only handlers separately.
- Validation: 159 MCP tests, 21 hook shell suites, 4 hook Python tests,
  46 SMRITI tests, skills sync, CI Ruff and byte-identical contracts pass.
  Native release build passes (289 Rust tests, 2 ignored); CTest's initial
  17 passes plus all 8 failed-case reruns pass with `OPENBLAS_NUM_THREADS=1`,
  `OMP_NUM_THREADS=1`, `RAYON_NUM_THREADS=1` (initial NFS fixture timeouts).
  **Required restart-identity baseline remains unmet on unchanged native code:**
  worktree daemon 19/20 distinct queries, cached repeat 16/20, bounded-worker
  repeat 16/20; the fixed-query control is 20/20 throughout. These runs used
  separate private copies of replica snapshot `bbcaed33`; no live store writes.

- **Core → advanced (66):** Specialized variants, diagnostic/maintenance APIs and helpers without direct evidence in the audited consumers; callable names and schemas retained.
  `5w_search`, `ack_memory`, `approve_memory`, `ask`, `assoc_census`, `assoc_decay`,
  `compact_context`, `conflict_inspector`, `cross_harness_conflicts`, `densify_backfill`,
  `detect_contradictions`, `disable_source`, `distill_now`, `embed_coverage`, `embed_probe`,
  `flush_embeddings`, `forget_kind`, `get_embeddings`, `get_evidence_type`, `graph_pagerank`,
  `graph_traverse`, `impl_start`, `labile_memories`, `list_by_status`, `list_memories_brief`,
  `log_decision`, `log_event_ex`, `memory_provenance`, `memory_status`, `nack_memory`,
  `pending_embed_ids`, `promote_memory`, `provenance_check`, `prune_episodes`,
  `read_function`, `read_transcript`, `recall_session`, `recall_smart`, `recall_spreading`,
  `recall_ucb1`, `reconsolidate`, `reject_memory`, `remap_realms`, `remember_batch`,
  `remember_typed`, `resolve_contradiction`, `save_spectral_snapshot`, `scan_contradictions`,
  `search_symbols`, `semantic_backfill`, `set_affect`, `set_evidence_type`, `soul_repl`,
  `span_query`, `spectral_drift`, `stageb_set_surface`, `structured_recall`, `symbol_callees`,
  `symbol_callers`, `task_state`, `think_wander`, `trim_realm_names`, `triplet_query_as_of`,
  `triplet_supersede`, `verify_correction`, `what_do_i_know_about`.
- **Advanced → core (31):** Explicit skill, hook or recent CLI consumers; promoted without changing schemas.
  `connect_temporal`, `dream_list`, `dream_start`, `dream_status`, `dream_wander`,
  `explore_expand`, `explore_neighbors`, `explore_peek`, `explore_recall`, `get`,
  `habit_list`, `habit_match`, `habit_strengthen`, `health_check`, `learn_codebase`,
  `ledger_get`, `ledger_list`, `ledger_load`, `ledger_save`, `long_task_active`,
  `long_task_complete`, `long_task_event`, `long_task_snapshot`, `long_task_start`,
  `long_task_update`, `msg_send`, `query_graph`, `realm_detect`, `run_hint_enricher`,
  `set_memory_type`, `smart_recall`.
- **Unadvertised native handlers → advanced (6):** Previously absent from both policy sets; registered implementations remain callable and are now discoverable through advanced.
  `ledger_op`, `repl_execute`, `repl_session_delete`, `repl_session_get`, `repl_session_list`,
  `repl_session_set`.
- **Removed stale policy labels (9):** No registered handler or MCP schema exists in this checkout (full chitta source scan). The theme registration explicitly records that its backend was removed; these are stale listing entries, not deleted handlers. No working direct call is removed.
  `background_run_cycle`, `background_schedule`, `background_status`, `cleanup_code_wisdom`,
  `connect_batch`, `migrate_vss`, `sql_query`, `theme_assign_orphans`, `theme_maintain`.


### Added
- Restart identity gate on twenty frozen golden queries, with exact embedding
  bytes, pre-fusion candidates, and score-component diagnostics. Replica restart
  preserves the same store; evaluation pins recall time and gives embeddings a
  bounded wait, rejecting incomplete semantic lanes instead of accepting fallback.
- `no_learn` semantic recall preserves stored competitive weights and refresh
  timestamps; learning and maintenance continue to refresh them normally.

- 2026-09-16: private-replica RPC stress tool with 12 writers/12 readers for
  300 seconds, total 30-second call deadlines, named Rust lock maxima, count
  and payload/state checks, and SIGKILL/WAL replay verification. Corrected
  read-induced access-state comparison verifies 18,901 observe and 21,378
  remember acknowledgements; Rust hold limits remain unmet. Diagnostic and
  incomplete-gate runs exit nonzero; reproduction and limits are documented.
- 2026-09-16: `CHITTA_GLOBAL_LOCK=0` bypasses the dispatcher, queue and background
  global mutex without changing WAL-sync classification. Default 1 retains the
  existing policy: all three 300-second mixed-client workloads exceeded the
  50 ms Rust hold gate. Named timing records now survive native stderr writes.
  CTest 30/30, Rust 291 passed, chaos 9/9; same-copy restart identity 20/20
  versus 18/20 before, fixed controls 20/20. Inventory and limitations are in
  `docs/FIELD_PERF.md`.
- 2026-09-16 follow-up validation: 200/200 writes, zero errors, embeddings
  drained; recall p95 113.9 ms during writes / 162.8 ms full window; cached
  restart 16.10 s. Full-window latency and restart gates remain unmet. Chaos
  9/9, CTest 25/25, Rust 289 passed/2 ignored, hooks 23/23; contracts unchanged.
- 2026-09-16: UTC timestamps on daemon stderr, including Rust and native C
  diagnostics; incident reporting identifies foreign-host lock holders and
  accepts an explicit originating host. Scratch verification found all 5,528
  daemon log lines dated; this does not establish the fortnight soak gate.
- 2026-09-16: installer writes the primary-host `.daemon-node` marker and
  `primary-node.conf` guard (exit 75 elsewhere), including custom mind paths.
  Automatic secondary-host RPC fallback is documented as the next step;
  partial transport changes were not added.
- 2026-09-16: runtime placement for heartbeat, turn discipline, prompt/Stop
  recall state and PreTool caches, with real hook tests for default and local
  paths. Shared lifecycle markers stay on NFS; atomic queue mutation/ack replay
  remains blocked on a store transaction API, so local placement stays off.
- Opt-in nightly replica canary with dated reports, calibrated golden-score
  regression exits, explicit current-truth availability/calibration checks,
  and a disabled-by-default user timer (`--enable-canary`). On 2026-09-16,
  golden mean 0.492311 passed the 0.490965 lower band; week-long soak pending.
- 2026-09-16: default-off document embedding workers with bounded admission and
  separate Unix write dispatch; on private eval copies, 200 writes completed
  with loaded recall p95 139.0 → 137.7 ms and write-active p95 624.7 → 128.1 ms.
  Cached restart remained 20.0 s (5 s target unmet). Added scratch stress,
  restart/durability and dated runtime-incident instruments.
- 2026-09-16: experimental node-local queue/saddle/ledger-tail resolver and
  checkpointed ledger append/replay. Runtime placement stays off: atomic
  queue ack_id recovery and the remaining marker callers are unresolved gates.
- Replication-weighted recall factor (`replication_max`, `CHITTA_REPLICATION_MAX`),
  landed default-off: distinct-session replications per memory are cached on
  load; the live store has 0 memories with ≥2 independent replications and the
  golden delta was 0, so the card carries `prior_effort: some` (docs/EVALS.md).
- Snapshot load decodes sections in parallel from an mmap and overlaps triplet
  reconstruction with the rest of startup: snapshot phase 7.8–9.2 s → 3.6–4.3 s
  on the replica; startup sidecars are keyed to the snapshot so LSH/turbo hit
  with a WAL delta (live restart 27–33 s → 12.6 s); fused recall is
  deterministic across restarts (total-order sorts, hydration without touches).
- The per-Bash-call saddle check runs in bash/jq (81-fixture byte parity with
  `saddle_detector.py`): added PreToolUse overhead 97 → 24 ms median.
- Opt-in isolated evolve implementation streams (`--candidates N`, default 1):
  concurrent worktrees with private agent state, shared deadline and budget
  fallback, gate-passing selection by measured bet delta then patch size, all
  candidate outcomes in verdict/bet memories, and `python3 -m evolve.report`.
- Automatic-learning experiment harness: provenance-based cohort classification,
  prospective task/grader freezes, isolated paired replica trials (strict bwrap
  mode or `home-audit` with post-hoc voiding), exposure checks, deterministic
  grading and fixture-only dry runs (`benchmarks/learning/protocol.md`).
- Daemon startup caches derived search state across restarts (normalized
  embeddings, event-tape organs, quantized index): clean start 27 s → 13 s,
  optional sidecars, rollback-safe.
- Stigmergic artifact traces, after SwarmWorld [14](#ref-14) (arXiv:2608.26081) measured
  ~95% of first reuse through observing an artifact rather than being told:
  `pre-tool-hook` adds `[traces]` on the first Read of a file per session
  (memories whose text names that file; `CHITTA_FILE_TRACES=0` disables) and
  `artifact-trace.sh` (PostToolUse Write) registers newly written scripts as
  `[artifact] <path> sha:<8> purpose:…` signals so later sessions fork them.
- Two hypothesis cards for the evolve loop from the same reading: isolated
  best-of-2 implementation streams (SwarmWorld [14](#ref-14): isolated search kept the best
  single artifact) and replication-weighted recall (Buehler's [15](#ref-15) repeated runs
  reached model-dependent conclusions). The learning-experiment protocol gains
  two pre-registered secondary metrics: best-of-three per arm and artifact
  reuse rate.

### Changed
- `recall_analogy` is explicit relation transfer only (a:b :: c:?): the
  predicate(s) linking a→b come from indexed triplet lookups and the answers are
  c's actual neighbours over those predicates, with supporting edges and a
  `reason` on abstention. Structural mode and VSA [10](#ref-10) ranking are gone from the
  endpoint. Frozen-replica benchmark: hit@1 14/14, hit@3 14/14, 14/14 negative
  abstentions, 0 unsupported answers (docs/EVALS.md).
- Prompt, SessionStart and Stop hooks no longer start Python on the hot path:
  query cleaning, RPC-stats parsing, the heartbeat, session registration and the
  ledger render run in bash/jq against the daemon's CLI. Prompt hook median
  822→635 ms, SessionStart 693→599 ms on a loaded node (docs/HOOKS.md).
- Linked git worktrees resolve to the main checkout's realm in the hooks, the
  session registry and the CLI, so Codex and evolve streams recall project memory.
- Recall ranks `operational` distiller fragments below curated kinds
  (`kind_operational` 0.8) so a correction outranks verbatim transcript notes
  of equal similarity.
- The daemon's binary-update probe uses `posix_spawn` instead of `popen`; the
  `fork()` ran OpenBLAS's atfork barrier and stalled the main loop under
  embedding load.
- Chained `git add … && git commit` commands are captured as milestones.

### Fixed
- 2026-09-16 (Phase 1c): measure real Rust component RwLock waits and holds,
  including timed reads and unwinding. Named diagnostics log first use, maxima,
  and every wait or hold over 50 ms; archive poisoning remains intact.
- 2026-09-16 (Phase 1b): publish queue metadata and callbacks under narrow
  mutexes, initialize sadhana before background readers, and join foreground
  RPC and compaction workers before destroying their captured state.
- 2026-09-16 (Phase 1a): task-ledger reads, revision allocation, WAL append,
  and table publication now share a dedicated transaction mutex instead of
  depending on the RPC dispatcher lock. A 24-client regression verifies one
  lease winner, sequential revisions, and replay equality.
- Nightly literature cards were rejected by the proposal loader (source URL
  instead of a kind, prose `blast_radius`, zero effort) and silently skipped by
  the selector; the watch now writes the loader's contract.

### Removed

- Retired MDL [11](#ref-11) admission compression on 2026-09-15: native and Bash shadow taps,
  same-chunk pooling, corpus dictionaries/bootstrap, Python mirror, test targets,
  environment knobs and evolve coverage telemetry. Distilled learning storage,
  deduplication and recall ranking remain unchanged; no utility-posterior hard
  gate replaces it. Historical shadow logs remain untouched and inert. The
  paired 20-task automatic-learning experiment remains pending (see docs/EVOLVE.md).

## [5.72.0] - 2026-09-14

### Daemon / store
- Recall read path no longer writes: access touches accumulate and drain every 5 s; WAL fsync is timer-based (200 ms) with durable ops explicit; TurboQuant [5](#ref-5) index rebuilds on the maintenance thread. Twelve concurrent `recall_lanes`: 1.7–3.6 s → 0.23–0.54 s.
- Embedding context pool (`CHITTA_EMBED_CONTEXTS`, default 4) and query-embedding LRU cache.
- Task ledger (threads, inbox, artifacts, session bindings, leases) moved from `~/.claude/task-ledger.db` (NFS sqlite) into the daemon (`ledger_op` RPC) with an idempotent `task_ledger.py migrate`.
- Queue lives at `<mind>/queue.jsonl`; tag recall is a hard filter with realm applied after; correction lane realm-scoped; MDL small-evidence pooling (shadow).
- Daemon output kept in `~/.claude/mind/chittad.log` (user journal unreadable on the cluster).

### Hooks
- SessionStart lanes run concurrently (5.7–7.2 s → ~0.7–1.5 s) with a fallback line when every lane times out; SessionEnd fits its 5 s budget.
- Prompt hook: in-process realm detection; zero-distinctive-token turns skip topic lanes; UNKNOWN-band candidates need a distinctive turn token; keyword rows need two distinctive tokens or BM25 [24](#ref-24) ≥ 60; pre-tool "BEFORE RUNNING" needs 0.6 similarity; hard guard against killing the `--http` MCP process.
- Hooks honour `CHITTA_SOCKET_PATH`; failed commands recorded via `PostToolUseFailure`; Codex exit-less shape recorded as unknown.

### MCP
- Cold recall 10.8 s → 1.1 s (no Transformers import); streamable-HTTP sessions expire idle (30 min) and are capped (64); evolve runner reaps Codex.

### Evolve
- Proposal cards carry `verifiability`, `prior_effort`, `internal_evidence`; selector weights them; every cycle requires a `SELF_CHECK` test and runs a bounded survey phase first; nightly/weekly timers; `select.py` renamed `selector.py`.

### Eval
- Frozen replica copies loader markers, records its own pid, retries copies that race a save; real-agent noise calibration in `benchmarks/noise.json`; `hook_total_ms` metric.

## [5.71.0] - 2026-09-02

### Changed — project renamed `cc-soul` → `chitta`

- Plugin identity is now `chitta`, marketplace `genomewalker-chitta`, repository
  `github.com/genomewalker/chitta`. Skills are namespaced by plugin name, so
  `/cc-soul:recap` becomes `/chitta:recap`.
- Every `CC_SOUL_*` environment variable keeps working through an alias shim in
  `hooks/lib.sh` plus inline fallbacks at pre-source read sites. If both names
  are set, `CHITTA_*` wins.
- Unchanged on purpose: file and directory names, the `chitta-field` submodule,
  binaries, systemd unit names, the `.cc-soul-realm` dotfile, stored realm names
  (`project:cc-soul` is still a valid realm), `docs/CNAME`, and historical
  changelog entries. Full migration steps and the alias table:
  [docs/RENAME.md](docs/RENAME.md).

### Added — recall-biased pre-filter

- `recall` now fetches a wide candidate pool (`CHITTA_RECALL_POOL`, default 60,
  capped at 160) and over-selects it down to a rerank budget with scalar-only
  keep rules. A candidate survives if it is already inside `limit`, contains a
  literal query token, shares realm and kind with a top-5 hit at half the maximum
  score, or is a one-hop association neighbour of the head.
- A/B on the 30-query golden set, mean nDCG@20 with the reranker on: **0.435**
  pre-filter off, **0.487** on. On by default; `prefilter: false` or
  `CHITTA_RECALL_PREFILTER=0` restores the previous narrow-pool path exactly.

### Added — analogy lane, keyword realm scoping, lane ablation

- `recall_analogy`: proportional (`a:b::c:?`) and structural relation-shape
  retrieval over the triplet lane using vector-symbolic binding. No model call.
- `smart_recall`'s keyword legs are now realm-scoped, closing a cross-realm bleed.
- `CHITTA_ABLATE_LANES` disables named hook recall lanes (`sem`, `ctx`, `hyb`,
  `kw`, `corr`, `xr`) for one agent process. Consumed by `hooks/prompt-core.sh`;
  there is no daemon-side ablation flag.

### Added — SMRITI-Bench

- `benchmarks/smriti/`: an outcome-grounded memory benchmark for coding agents.
  A task passes an objective check command or it does not; no model judge, no
  reference-answer overlap score. First full matrix (9 tasks, off/on, 3 trials,
  n=27 per condition): success 23/27 off versus 27/27 on, paired token ratio
  median 0.52. MUI v2 rescore on the same data: 0.630.
- Not yet run: a live matrix over the six newer tasks or any ablation condition.

### Added — outcome ledger, MDL consolidation gate

- `chitta-mcp/outcome_ledger.py` and `hooks/outcome-ledger.sh`: fail-open JSONL
  append of `injected`, `bash_outcome` and `session_end` events, with an offline
  joiner computing Wilson-lower-bound [2](#ref-2) credit per memory.
- `chitta-mcp/mdl_gate.py` runs in **shadow mode only**. It writes to
  `mdl_gate_shadow.jsonl` and does not block distillation.

### Added — session registry, thread inference, resume selector

- `chitta-mcp/session_registry.py`, `thread_inference.py`, `resume_selector.py`,
  with tests under `chitta-mcp/tests/`.

### Fixed

- Removed `eval` and Python source interpolation from hooks.
- `.claude-plugin/plugin.json` declared `chitta-field >= 5.0.0`; the submodule
  version is 2.7.12. Corrected to `>= 2.1.0`, the documented snapshot-format
  rollback floor.

### Documentation

- `docs/tools.html` and `docs/API.md` are now generated from a live daemon by
  `scripts/gen-tools-docs.py` instead of hand-maintained. The hand-written page
  claimed "150+ tools" and listed 241; the real surface is 343.
- New pages: `docs/recall.html` (the retrieval pipeline) and
  `docs/benchmarks.html` (SMRITI-Bench, LongMemEval [16](#ref-16), LoCoMo [17](#ref-17)).

## [5.70.x] - 2026-07-06 to 2026-07-14

Patch releases: 5.70.0 (07-06), 5.70.1 (07-07), 5.70.2 (07-07), 5.70.3 (07-07), 5.70.4 (07-14).

- **soul**: C2 self-monitoring (feeling-of-knowing) confidence signal, recalibrated
  to a 2-band scale with timeout-degrade handling; workspace-admission inspector.
- **daemon**: dormant PPR injection lane, delta-HNSW [4](#ref-4) persist, `ops_log`,
  `semantic_backfill` RPC, task-state and durable-correction keyed lanes,
  `provenance_check` exposed on CLI/MCP; single-writer lock made actually
  exclusive; recall latency bounded under embed/queue load via a priority gate.
- **distill**: synchronous `distill_now` RPC; deterministic value-fact extractor;
  value-fact embed/recall moved off the RPC lock.
- **hooks/dev**: editable dev-install symlinks live hooks and MCP to the repo,
  ending the dual-copy drift; `session-end-hook.sh` deregisters sessions.
- **mcp/recall**: INT8 ONNX cross-encoder reranker; `assoc_census`/`assoc_decay`
  tools; auto-inject `source_session` on `remember`/`remember_batch`.

## [5.69.x] - 2026-07-06

Patch releases: 5.69.0–5.69.3, all 2026-07-06.

- **queue**: two-lane processing + ledger-coalesce; code-intel CLI wiring.
- **mcp**: default recall strategy changed to hybrid; grade-recall re-bootstrapped
  against it.
- **code-intel**: member-call edges populated via `find_child_by_field`, scoped
  per project.
- **distill**: fail-fast endpoint probe so a dead LLM never burns 180s/item.

## [5.68.x] - 2026-07-06

Patch releases: 5.68.0, 5.68.1, both 2026-07-06.

- **code-intel**: scoped `find_symbol` + dedupe GC surface; missing
  `field_store.hpp` FFI wrappers restored (v5.68.0 build fix).
- **queue**: checkpoint the `.processing` batch, fix graceful-shutdown tail-drop.

## [5.67.x] - 2026-07-06

Patch releases: 5.67.0, 5.67.1, both 2026-07-06.

- **recall**: C++ windowed temporal recall + Tier-1 fusion reranking; `[gap]`
  quarantine with rescore-before-truncate.
- **hooks**: enforce read-dedup by truncating re-reads to 40 lines.

## [5.66.x] - 2026-07-04 to 2026-07-06

Patch releases: 5.66.0 (07-05), 5.66.1 (07-05), 5.66.2 (07-06), 5.66.3 (07-06).

- **span-lane**: C++ surface + MCP hide-list, then live-ingest wiring.
- **queue**: `queue_status` surfaces live queue depth.
- **embed**: GGUF failover when the HTTP embed endpoint dies (2s connect
  timeout).
- **mcp**: fail closed when a `session_id` can't be resolved for messaging.
- **field**: per-realm span count fix; flat-scan exact recall with a >2M drift
  bound; lock-order fix in `span_backfill_memories`.

## [5.65.x] - 2026-06-29 to 2026-07-01

Patch releases: 5.65.0 (06-30), 5.65.1 (07-01).

- **eval**: graph-only multihop, GOLDEN_SET v6 (30 queries); G-entropy/S-entropy
  logging; grader abstain signal + multi-strategy RRF [3](#ref-3) re-ranking.
- **dream**: BridgeBrain routes dreams through bridge rooms (local/gemma) with
  fallback to LocalBrain; gap-memory seeds; prune time guard.
- **subconscious**: removed the automatic `prune_episodes` call and guarded
  against its recurrence.
- **ci**: add OpenBLAS/openblas-devel for turbovec's BLAS feature.

## [5.64.x] - 2026-06-29

Patch releases: 5.64.0, 5.64.1, 5.64.2, all 2026-06-29.

- **embed**: `CHITTA_EMBED_URL` for a dedicated embed GPU, GGUF-first priority,
  background embedding on by default; batch size 40→256.
- **embed_loop**: removed the `prune_episodes` call, respects
  `config_.embedding_interval`.
- **field**: multi-hop DAM [8](#ref-8) + scoring.json; turbovec DAM submatrix; TurboState
  lazy index.

## [5.63.0] - 2026-06-29

- **daemon**: GPU retry, queue-lock shortening, embedder decode-fallback reinit.
- **eval**: grade-recall results updated (nDCG@20=0.808).

## [5.62.x] - 2026-06-28

Patch releases: 5.62.0–5.62.6, all 2026-06-28.

- **embed**: GPU-first embedding — try Ollama before CPU GGUF.
- **daemon**: fixed an ABBA deadlock between `backfill_embedding`,
  `sync_foreign`, and pool workers; `sync_foreign` no longer re-replays the
  WAL on restart (`seen_offsets` seeding).
- **recall**: normalize RRF scores before display.
- **hooks**: fixed recall lanes losing output to subshells and never being
  injected.

## [5.61.x] - 2026-06-23 to 2026-06-28

Patch releases: 5.61.0–5.61.16, spanning 2026-06-23 to 2026-06-28.

- **mcp**: HTTP MCP transport + Bearer auth for chitta-mcp.
- **recall**: RRF hybrid reranking and a cross-encoder reranker taking G0
  nDCG@20 from 0.784 to 0.905, then 0.933 on a refreshed 174-query eval with an
  L-12 reranker; embedding model fine-tuned to Jina-v2-base-en at 768-dim
  (142k memories re-embedded), later switched to nomic-embed-text-v1.5 to fix
  domain bias.
- **eval**: GOLDEN_VERSION 3 grader with sig-primary gold keys and
  realm-scoped sweep; 150-query benchmark; real eval harness with a
  `--strategy` flag.
- **hooks**: cross-realm fallback when scoped recall returns empty; recalled
  memories escalated to `systemMessage` on cache-expired sessions.
- **docs**: synced API/ARCHITECTURE/CLI/HOOKS/chitta-field README/model table
  to code.

## [5.60.0] - 2026-06-23

- **field-rag**: Phase 1 — Modern Hopfield [7](#ref-7) [33](#ref-33) recall via `strategy=field`.

## [5.59.0] - 2026-06-23

- **recall**: RRF hybrid + embed bug fix.

## [5.58.x] - 2026-06-23

Patch releases: 5.58.0, 5.58.1, both 2026-06-23.

- **soul**: G5–G11 auto-evolving cognitive architecture.
- **eval**: G0 nDCG@10 grader + debug-recall diagnostic tool.

## [5.57.x] - 2026-06-23

Patch releases: 5.57.0, 5.57.1, both 2026-06-23.

- **hooks**: forward-bet emission in `distill.sh` + settle-predictions sweeper.
- **eval**: G0 golden-set grader + G4 persona catalog; grader queries
  recalibrated to 1.000; density tiebreak added to the persona selector.

## [5.56.x] - 2026-06-12 to 2026-06-23

Patch releases: 5.56.0–5.56.11, spanning 2026-06-12 to 2026-06-23.

- **field**: single in-RAM embedding home (−600MB RSS); sleep consolidation +
  CW sweep re-enabled; transitive forgetting; snapshot compaction; NFS ghost
  janitor.
- **hooks**: retired Edit→file_patch enforcement now that built-in Edit is the
  default; auto-recap after compact/clear; realm recall block injected at
  session start when soul-context is sparse; fall back to unfiltered recall
  when realm-filtered recall returns nothing; `dream-sweep.sh` for
  cross-session transcript distillation.
- **memory**: stop raw-turn flood, wire read-dedup enforcement, belief gate.
- **skills**: add prog-review (first-principles refactor/review workflow).
- **hooks**: fixed the ledger queue path, TODOS extraction via `jq`, and
  transcript sanitization before `jq`.

## [5.55.0] - 2026-06-11

- **field**: v2.5.1–v2.5.4 — states/payloads lock-order deadlock hotfix,
  `sync_foreign` moved under the exclusive rpc lock, CW-refresh per-query
  budget, ANN recall on by default.

## [5.54.0] - 2026-06-11

- **field**: v2.2.0–v2.5.0 — confluent merge replay + coverage-vector
  manifest, Phase 2 save-cost cuts + LWW clocks, `.hdc`/`.pld` dirty-skip,
  ApplyCtx refactor, cross-context provenance (Phase 3).

## [5.53.0] - 2026-06-10

- **hint**: PR1/PR2 real-time hint telemetry and `chitta_hintd`, a resident
  hint-extraction daemon (default-off).
- **daemon**: safe thread-pool back-pressure; `consolidation_pass` serialized
  at the daemon level; MCP daemon socket serialized with response-id
  verification; daemon finds the bge GGUF in `~/.claude/bin` without a silent
  Ollama fallback.
- **hooks**: `prompt-core.sh` token-saving pass, `MAX_OUTPUT_CHARS` lowered
  800→500 (~125 tokens per injection).
- **docs**: record chitta-field v2.1.0 as the rollback floor (V23 snapshot
  format).

## [5.52.0] - 2026-06-03

- **daemon**: lock-free recall that never blocks on an index-mutating write;
  a `rpc_mutex` lock-hold profiler (`[lockprof]`).
- **recall**: `smart_recall`/`hybrid_recall` no longer take the exclusive
  `rpc_mutex` (had blocked all recall for ~800ms); an abstain signal for
  "nothing strongly relevant" (no new model); daemon auto-start no longer
  spawns an unmanaged second writer, the root cause of store corruption.
- **docs**: consolidation-redesign v2 with the corrected mechanism and a
  stall-free RCU migration plan.
- **field**: bumped to v2.0.0 with calibrated bounded-envelope recall
  scoring, centered flat-scan, and lock-free snapshot sidecars.

## [5.51.0] - 2026-06-01

- **embed**: ship bge-large-en-v1.5 (1024-d) as the public embedding default;
  public ssl_distiller training + eval pipeline; multi-teacher synthetic SSL
  corpus generator; codex CLI teacher for synthetic corpus generation.
- **install**: fixed a public-install embed-model mismatch.

## [5.50.0] - 2026-05-30

- **cli**: `prune-memories` command (dry-run preview + `--apply
  --delete/--background`); `format-id`, `execv` self-update gate, and
  `migrate-store-format`.
- **recall**: discriminating recall via embedding mean-centering +
  robustness hardening.
- **field**: V3 WAL segment lineage fencing.

## [5.49.0] - 2026-05-28

- **embed**: migrate to the ssl_distiller_dpo 1536-dim embedding model.

## [5.48.x] - 2026-05-26 to 2026-05-28

Patch releases: 5.48.0 (05-26), 5.48.1 (05-27), 5.48.2 (05-28).

- **build**: replace ONNX with llama.cpp+nomic GGUF in CI; embed-model
  download added to smart-install; llama.cpp pin bumped b4990→b9294.
- **daemon**: `consolidation_pass` moved to a subprocess path so it no longer
  holds the exclusive `rpc_mutex` during the Sequitur [25](#ref-25)+FEP rebuild.
- **install**: smart-install preserves `--embed-model` across updates.

## [5.47.x] - 2026-05-26

Patch releases: 5.47.0, 5.47.1, both 2026-05-26.

- **field**: prune orphaned triplet `ingestion_times`, add `--embed-model`
  flag, switch to ssl_distiller_dpo.
- **docs**: add a Models section; wire `v5-model-b` as the active hint model
  (88% vs 68%).

## [5.46.x] - 2026-05-25 to 2026-05-26

Patch releases: 5.46.0, 5.46.1, 5.46.2 (all 05-25), 5.46.3 (05-26).

- **build**: remove ONNX support, always build with llama.cpp.
- **hooks**: replace deprecated `codex_hooks` with `hooks` in `[features]`
  config; reduce correction-classifier false positives in `prompt-core.sh`.
- **docs**: rewrite README, drop version-history noise, lead with what
  matters.

## [5.45.x] - 2026-05-25

Patch releases: 5.45.0, 5.45.1, 5.45.2, all 2026-05-25.

- **hooks**: move `ledger_append` before the daemon gate; fix the transcript
  parser for Claude Code JSONL.

## [5.44.x] - 2026-05-25

Patch releases: 5.44.0–5.44.4, all 2026-05-25.

- **daemon**: `EmbedQueue`, a single-owner async embed architecture; predicate
  store wired into C++ handlers + MCP.
- **hooks**: predicate re-falsification, tighter correction regex, discipline
  enforcement; auto-store in the Stop hook when the discipline threshold is
  crossed.
- **fix**: predicate operations unblocked (three independent bugs).

## [5.43.0] - 2026-05-25

- **daemon**: reliability + eval improvements (v5.43).

## [5.42.0] - 2026-05-24

- **field**: Interaction Ledger (v6.0) — reads/writes/outcomes as first-class
  events.

## [5.41.4] - 2026-05-23

- **hint**: HintYantra in-process hint extraction + auto-enrichment daemon
  thread; LlamaYantra inline GGUF embeddings via llama.cpp (no Ollama
  required); ONNX→Ollama embedding migration + hint-corpus pipeline;
  `chitta-hint-tuned` fine-tuned retrieval hint model + MCP tool.
- **recall**: pre-embed the recall query outside `rpc_mutex_`, reduce the
  Ollama timeout 30s→8s.
- **fix**: OllamaYantra retries with progressive truncation on HTTP 400;
  `PROMPT_TEMPLATE` crash fixed with per-model template selection; dedup
  `learn_codebase` pool submissions by `(path, project)`.

## [5.41.3] - 2026-05-21

### Added — LongMemEval benchmark pipeline (78% on longmemeval_s)

- `scripts/benchmark_longmemeval.py`: production-ready benchmark harness against
  the LongMemEval dataset. Ingests all haystack turns per question (no cap),
  prioritising user turns by length score. Sequential ingestion with 120 s recall
  timeout; no pipeline overlap that previously caused watchdog kills.
- Recall strategy: hybrid BM25+HDC (limit 30) + `recall_session` for non-temporal
  questions + keyword second-pass recall (strips stop words, re-queries BM25).
- `source_session` metadata on every ingested turn enables session-level
  noisy-OR aggregation via `recall_session`.
- Synthesis via `claude -p` with raw-memories context (no date-parse truncation);
  codex CLI fallback if claude unavailable.
- Achieves **78.0% (39/50)** on `longmemeval_s`, matching the best prior run.

### Added — `flush_embeddings` RPC tool

- `cf_flush_embedding_queue` FFI export, C++ handler `tool_flush_embeddings`,
  registered as `flush_embeddings` tool. Returns `{flushed: N}`.

### Fixed — `subconscious.sh` stray-daemon detection

- `kill_stray_daemons()` now only targets daemons with **no `--path` flag**
  (those that would default to the same mind path). Daemons started with an
  explicit `--path <dir>` (e.g. isolated benchmark daemons) are left untouched.

## [5.32.0] - 2026-05-19

### Added — CEC Phase 8: Cross-Session Motif Q-Values

- `CdawgState.q_value`: per-state expected-success scalar, persisted in CDAWG [6](#ref-6).
- `CdawgOrgan::update_q(terminal_sym, reward, α, γ)`: TD(0) propagation up the
  suffix-link tree, weighted by `1/|endpos|` so generic states don't absorb all
  signal. Called on every non-legacy outcome event with reward ±1.
- `CdawgOrgan::top_q_states(prefix, k)`: BFS from a prefix state, returns top-k
  reachable states by Q-value.
- `store.rs::recall_motif_value(tool, entity, k)`: maps Q-states to `RecallHit`
  with normalised score `(q+1)/2` and next-action predictions from outgoing transitions.
- New tool: **`recall_motif_value`** — "which action sequences have highest expected
  success rate from this context?"
- FFI: `cf_recall_motif_value` → JSON array `[{state_id, q_value, support, content}]`.

## [5.31.0] - 2026-05-19

### Added — CEC Phase 7: Causal Refutation Ledger

Rules that know they're wrong.

- `refutation_ledger.rs` (`RefutationLedger`): antecedent-indexed HashMap tracking
  per-rule `support` (sym_a → sym_b observed) and `contradict` (sym_a → anything
  else observed) counts with two hysteresis thresholds:
  - `refute_ratio > 0.4` → rule flips to `Refuted(ts)`; writes
    `(rule_N, "refuted_by", "contradict_at_ts=...")` triplet.
  - `refute_ratio < 0.2` → rule reinstated to `Live`.
- `SequiturRule.contradict_count`: persisted count of false-positive antecedent firings.
- `log_event()` calls `ledger.observe(prev_sym, curr_sym, ts)` on every append.
- `consolidation_pass()` calls `ledger.seed_from_rules()` after each Sequitur run.
- `store.rs::refutation_stats(k)`: human-readable top-k by refute_ratio with live/
  refuted counts.
- New tool: **`refutation_stats`** — shows which promoted procedural rules are
  being actively falsified.

## [5.30.0] - 2026-05-19

### Added — CEC Phase 6: Counterfactual Recall via CDAWG Sibling Edges

- `CounterfactualHit` struct: `{symbol, fail_ratio, taken_fail_ratio, delta, support,
  wilson_fail_lower}`.
- `CdawgOrgan::counterfactual_alternatives(context, taken_sym, min_support, k)`:
  walks to prefix state, enumerates sibling transitions, ranks by
  `delta = taken_fail_ratio - alt_fail_ratio` (positive = alternative is better).
  Wilson lower bound (90% CI, z=1.645) used as tiebreaker for uncertain estimates.
- `store.rs::recall_counterfactual(tool, entity, outcome, k)`: builds context from
  last 4 EventTape symbols, delegates to CDAWG.
- New tool: **`recall_counterfactual`** — "what alternative tool/entity would have
  had a lower failure rate in the same context?"
- Gate: `min_support = 5` (same as failure_pattern).

## [5.29.0] - 2026-05-19

### Added — Causal Episode Compiler (CEC) — Phases 1–5

Embedding-free agentic memory substrate that indexes state transitions
`(tool, entity, outcome)` rather than content. Runs in parallel with the
existing HNSW/BM25/HDC recall lanes, which are untouched.

**Phase 1 — Event Tape + CDAWG (v5.25.0)**
- `EventTape`: append-only log of `(tool_id, entity_key, outcome_class, session_id, ts_ms)`.
  Entity canonicalization is deterministic (file paths → repo-relative, URLs → hostname,
  freeform → first 40 chars lowercased).
- `CdawgOrgan`: online Compact Directed Acyclic Word Graph over packed u64 symbols
  `(tool_id << 40 | outcome_class << 32 | entity_key)`. Ephemeral — rebuilt from
  EventTape at every daemon load.
- `EventTape` serialized in `FullSnapshot` (V13 → V14 migration); existing memories
  receive a synthetic `legacy` event for warm-start.
- New CLI/MCP tools: `log_event`, `recall_last_action`, `recall_failure_pattern`,
  `recall_causal`.

**Phase 2 — TD(λ) [31](#ref-31) + PMI Causal Antecedents + Roaring (v5.26.0)**
- `CdawgState.endpos` upgraded from `Vec<u32>` to `RoaringBitmap` (O(1) cardinality
  for PMI denominator).
- `push_td_credit()`: backward eligibility traces over last 16 events, δ=+0.1 success /
  −0.2 fail, γ=0.9 decay. Skips synthetic `legacy`/`remember` events.
- `recall_causal_antecedent`: returns PMI-ranked predecessor patterns —
  `log(count(X,Y) × N / count(X) / count(Y))`.

**Phase 3 — Surprisal-Gated Writes + Involuntary Injection (v5.27.0)**
- `put_memory` computes PPM [30](#ref-30) surprisal before logging the event. If
  `surprisal > 2.0 nats`, the memory's `decay_rate` is halved (surprising
  memories burn in harder).
- `hooks/prompt-core.sh`: if `recall_failure_pattern` finds a pattern with
  `fail_ratio > 0.7 && fail_count ≥ 3`, a one-line `⚠ CEC:` warning is
  prepended to `SYSTEM_MSG` at every turn start (involuntary injection).

**Phase 4 — HDC as Heteroassociative Binder (v5.28.0)**
- `EpisodeHdcStore`: each CEC event encoded as
  `bind(R_tool, t) XOR bind(R_entity, e) XOR bind(R_outcome, o)`.
- `recall_hdcbind(known_role, known_val, query_role)`: XOR-unbind query —
  given e.g. `outcome=fail`, returns most associated tools by Hamming distance
  against the codebook.
- Ephemeral — rebuilt from EventTape at daemon load. Per-role `EpBundle`
  bit-count accumulators, not serialized.

**Phase 5 — Sequitur Grammar + Procedural KG Promotion (v5.29.0)**
- `sequitur.rs`: `run_sequitur()` counts all bigrams (consecutive symbol pairs)
  in the EventTape with frequency ≥ 5 (RePair-style grammar induction).
- `consolidation_pass()`: promotes rules to the triplet KG as four predicates:
  `compresses`, `avg_outcome`, `support`, `tape_range`. Subjects use the scheme
  `rule:tool(entity,outcome)→tool(entity,outcome)`.
- Dedup: `triplet_store.query_subject()` check prevents re-inserting on repeated runs.
- Auto-triggers every 500 events inside `log_event()`.
- `consolidation_pass --preview true --k N`: dry run showing top-N rules.
- On the live 57K-memory instance: 215 procedural rules promoted on first run,
  queryable via `chitta query --subject "rule:..."`.

## [5.21.68] - 2026-05-18

### Fixed — Pre-embed All Medium-Frequency Write Handlers

Completed the pre-embed pattern for the remaining write tools: `update`, `consolidation_merge`, `curiosity_note_gap`, `propose_change`, `profile_update`, `profile_observe`, `goal_set`, `reconsolidate`. All 15 write handlers that call `embed_text()`/`embed_ssl_aware()` now compute embeddings outside `rpc_mutex_`.

## [5.21.67] - 2026-05-18

### Fixed — More Pre-embed Before Lock

Extended the pre-embed pattern to `suggestion_track`, `anticipation_observe`, and `create_episode` — three high-frequency write tools called by hooks and the distillation pipeline that were holding the exclusive `rpc_mutex_` lock during ML inference.

## [5.21.66] - 2026-05-18

### Fixed — Daemon Lock Starvation

- **Pre-embed write tools before exclusive lock** — `remember`, `observe`, `grow`, `learn*` now compute embeddings (pure ML inference on `yantra_`, no `field_store_` access) before acquiring `rpc_mutex_` exclusively. Previously 30–60 s ML inference held the write lock, causing `health_check` and all reads to queue for 20+ minutes.
- **Thread pool min workers 2 → 8** — pool now starts with 8 workers instead of 2, providing spare capacity when individual workers are occupied by slow operations.

## [5.21.65] - 2026-05-18

### Performance — Memory & Startup Overhaul

Daemon RAM reduced from **22–26 GB → 2–4 GB**. Snapshot size reduced from **4.2 GB → 420 MB**.

Root cause was runaway triplet accumulation: the distillation pipeline re-extracted the same subject-predicate-object facts on every run with no deduplication, producing ~14 million duplicate and invalidated entries over time.

#### Fixed

- **`TripletStore` deduplication** — `add()` checks for an existing live `(subject, predicate, object)` entry before inserting; duplicates update weight in place. Eliminates the primary source of unbounded growth.
- **Invalidated triplet purge** — `purge_invalidated()` removes all `valid_to_ms != 0` entries at load time and before snapshot save. Cleared 13.7M dead entries (3.4 GB → 110 MB) on first startup after upgrade.
- **Derived index skip** — `TripletStore`'s three lookup indexes (`by_subject`, `by_object`, `by_predicate`) are cleared before serialization and rebuilt after deserialization. Previously serialized as redundant string copies (~3× snapshot bloat).
- **`AssocEdge` deduplication** — `add_assoc_edge()` upserts by `(dst, edge_type)` instead of appending.
- **`malloc_trim` after store open** — freed pages from load-time migration are returned to the OS immediately.
- **Streaming snapshot load** — deserialization now uses `BufReader` (1 MB read-ahead) instead of `std::fs::read()` which allocated the entire file as `Vec<u8>` before any deserialization.
- **Daemon lock race** — `acquire_lock()` now runs before `early_server` binds the socket; competing startups fail in milliseconds.
- **Coactivation stats cap** — `coactivation_stats` pruned to top-20 pairs per memory at load and save time.

## [5.21.45] - 2026-05-16

### Added
- **v10 snapshot format** — embeddings stored in a `.emb` sidecar (magic header + count + flat `{id, f32×256}` records); binary sparse codes in a `.bin` sidecar. Reduces main bincode snapshot size. Both sidecars are structured for future `mmap` access.
- **Two-tier HNSW** (chitta-field) — `delta_hnsw` activates above 100K memories (`HNSW_TIER2_THRESHOLD`). New inserts go to the small delta graph O(log N_delta) instead of the large base graph O(log N_total). Delta is merged into the base at checkpoint when it exceeds 10% of base size. Persisted as a `.delta.hnsw` sidecar alongside the base graph.
- **SSL NL gloss** — SSL memories are re-embedded with a natural-language prefix via a one-time migration. `gloss_ssl_content()` translates SSL triplets (subject/predicate/object) into readable English sentences before embedding, improving semantic recall of structured memories.
- **Alias memories** — when a new SSL memory is stored, a linked `kind=alias` NL memory is automatically created and connected via an `alias-of` triplet. Keeps NL and SSL representations synchronized without duplicating content.
- **Embed worker** — idle gate removed; the background embed worker now runs every 30 s during active sessions instead of waiting for an explicit activity signal.

## [5.20.6] - 2026-04-16

### Fixed
- **Queue JSONL corruption** — hooks now enqueue via native `chitta queue_write` (single-syscall atomic append, JSON compaction); removes the ~40K/day `[queue] FAILED: json parse error` burst caused by multi-line `jq` output.
- **Chain continuity warning spam** — `OpLog::replay()` now resets `chain_head = ZERO_HASH` at instance-id prefix boundaries so independent per-instance segment chains no longer trigger header-level warnings (previously ~120K/day on startup).
- **Dead-letter interleaving** — `sandbox::append_line_atomic()` uses a single POSIX `write()` with `O_APPEND | O_CLOEXEC` and an EINTR/short-write loop; replaces two-step `ofstream::<<` calls that could interleave under concurrency.
- **`queue_write` CLI arg parsing** — now uses a positional-args vector so flags interleaved after the subcommand (e.g. `chitta queue_write --json ...`) no longer shift the tool/args slots.
- **`lib.sh` fallback** — when the `chitta` binary is absent, the bash fallback now compacts the JSON via `python3` and fails loudly instead of silently injecting a malformed line.
- **Dead-letter failure visibility** — `QueueProcessor::write_failed_item()` now logs append failures and inner exceptions to stderr instead of swallowing them.
- **Safer deploy** — `CLAUDE.md` documents atomic `install` (temp+rename) before `systemctl restart`, preserving daemon uptime across the binary swap.

### Added
- Regression test `replay_across_independent_instance_chains` in `chitta-field/src/log.rs` covering the instance-boundary chain-reset invariant.

## [5.3.0] - 2026-04-01

> **Note (2026-04-16):** The FEP / Hopfield / adaptive-vigilance items below are **partially shipped**. The chitta-field sibling checkout now exports the six `cf_*` symbols with the ABI declared in `chitta/include/chitta/field_store.hpp` (`cf_reconstruction_error`, `cf_memory_surprise` are real; `cf_search_attractor`, `cf_hopfield_co_retrieval`, `cf_hopfield_stats`, `cf_adapt_vigilance` are stubs returning `CF_NOT_IMPLEMENTED` / empty `"{}"`). The pinned submodule at `cc-soul/chitta-field@762463a` does NOT yet carry these exports; `hopfield.rs`, `attractor_settle`, asymmetric prototype transitions, surprise-modulated plasticity, and the self-orthogonalising sparse encoder are still unimplemented on both sides. Bump the submodule once the full set lands.

### Added
- **FEP attractor network** — self-orthogonalizing memory representations derived from the Free Energy Principle (Spisak [19](#ref-19) & Friston, Neurocomputing 2026). Three-phase integration across chitta-field and the C++ daemon.
- **Asymmetric prototype transitions** — `strengthen_transition` gives full delta to forward direction (a→b), 0.3× to reverse (b→a). Sequential recall order encodes temporal asymmetry.
- **Asymmetric triplet weights** — new `reverse_weight` field on `TripletEntry` with `#[serde(default)]` backward compatibility. New triplets default to `reverse_weight = weight × 0.3`.
- **Surprise-modulated plasticity** — `MemoryState.surprise` stores reconstruction error from sparse encoder. `PlasticityLearner` uses it: high surprise → slow decay, low surprise → fast decay.
- **Attractor-based pattern completion** — `CorticalIndex::attractor_settle()` iteratively blends query with prototype centroids and follows asymmetric transitions (3-5 steps). New `Route::Attractor` in route learner.
- **Hopfield network module** (`hopfield.rs`) — asymmetric energy-based attractor network over memory co-activations. `settle()` propagates activation through multi-hop directed couplings with dynamic neighbor discovery.
- **Self-orthogonalizing sparse encoder** — Hebbian [26](#ref-26) update replaced with FEP-derived rule: prediction error + complexity penalty (λ=1e-4) + Gram-Schmidt partial decorrelation (1% per step) between active atoms.
- **Adaptive vigilance** — `CorticalIndex::adapt_vigilance()` lowers vigilance when prediction error is high (more prototypes needed), raises it when model is accurate.
- **Free-energy merge criterion** — `find_dup_pairs` uses `cf_reconstruction_error` to check whether merging reduces total free energy (accuracy loss vs complexity gain), with cosine threshold fallback.
- **New FFI exports** — `cf_reconstruction_error`, `cf_memory_surprise`, `cf_search_attractor`, `cf_hopfield_co_retrieval`, `cf_hopfield_stats`, `cf_adapt_vigilance`.

### Fixed
- **Negative `start_turn` in `read_transcript`** — negative offsets (Python-style `-5` = 5 from end) now resolve correctly instead of wrapping to huge unsigned values.
- **CLI parser with negative numbers** — `-5` after `--start_turn` no longer treated as a flag; numeric values with leading minus are recognized as argument values.

## [5.0.0] - 2026-03-29

### Added
- **Pre-tool hook exit 2 blocking** — hook executes compact alternative itself (e.g. `head -200 + wc -l` instead of `cat`), returns output, and exits 2 to block the original call. Real token savings.
- **chitta-research integration** — autonomous research OS built on chitta-field; 7 specialized agents, belief graph, Brahman constitution.

### Changed
- Pre-tool hook timeout raised from 3s to 10s.
- Pre-tool hook now fires before the daemon availability gate so advisories reach Claude even during daemon startup.

## [4.0.88] - 2026-03-28

### Fixed
- `daemon_available()` helper missing from `lib.sh` — all hooks were silently exiting.
- Daemon gate moved after rewrite check so large-file advisory fires even when daemon is unavailable.
- Advisory prefixed with `BEFORE RUNNING` so it surfaces in `system-reminder`.

## [4.0.82–4.0.87] - 2026-03-26

### Changed
- Pre-tool hook rewrites fall back to `additionalContext` advisory (`updatedInput` not supported in Claude Code v2.1.87+).
- `hookSpecificOutput` wrapper used for correct hook protocol.
- Pipe characters stripped from filename extraction; advisory renamed to `Large output warning`.
- Rewrite advisory accumulates with chitta recall rather than exiting early.

## [4.0.79] - 2026-03-25

### Added
- **RTK-style command rewriting** — pre-tool hook detects dangerous or verbose Bash patterns and rewrites them before execution: `cat` on large files → `head`; unbounded `find /` → `find -maxdepth 5`; unbounded `grep -r` → `grep | head -200`.
- **Output-type-aware chitta recall** — TestResults, BuildOutput, LogOutput routed to separate memory categories.

## [4.0.74] - 2026-03-22

### Added
- **PoE domain reliability** — corrections automatically lower recall scores for the target realm. Mistakes don't just get overwritten; the field learns not to trust that domain.
- **Turn discipline** — warns after 15 turns without storing a memory, prompting consolidation.

## [4.0.73] - 2026-03-21

### Added
- **chitta_thinking** — C++ thinking block extractor fires every 10 turns, mining `thinking` blocks for perception-change moments and storing them as memories.

## [4.0.68] - 2026-03-18

### Added
- **HNSW semantic index** in chitta-field — activates automatically above 2,000 memories; sub-millisecond dense recall at scale.

## [4.0.65] - 2026-03-15

### Added
- **chitta_migrate unified** — auto-detects `soul.db` (SQLite) or `chitta.duckdb` and delegates to `chitta_import` without manual configuration.

## [4.0.x] - 2026-02 to 2026-03

### Added
- **chitta-field** — pure Rust memory substrate replaces DuckDB entirely. Sparse codes (64/16,384), WAL, cortical posting index, organic decay tiers, multi-instance writes.
- **FilterLevel on BM25 code ingestion** — Signatures-only or MinimalContext modes for leaner code indexing.
- **recall_with_fallback chain** — semantic → BM25 → recency. Recall never returns empty.
- **MacOS portability** — all Linux-only APIs (`inotify`, `/proc/self/exe`) guarded with `#ifdef __linux__`. Daemon builds and runs on macOS.
- **Milestone auto-detection** — milestones detected in conversation stored directly via write queue.
- **memory-intercept.sh** — PostToolUse:Write hook captures Write operations asynchronously.
- **log-bash-history.sh** — PostToolUse async hook for Bash history pattern learning.
- **`statusMessage` in hooks** — live status text during hook execution.
- **brain plausibility metric** — Spearman correlation of chitta recall vs TRIBE v2 fMRI predictions.

### Removed
- DuckDB backend (replaced by chitta-field Rust substrate).

## [3.36.0] - 2026-02-08

### Added
- **Native MCP Tools** — Auto-detect session_id and realm for 34 tools (SESSION_TOOLS, REALM_STORE_TOOLS, REALM_FILTER_TOOLS)
- **Intelligent Memory System** — 7 query types (Aspect, Entity, Temporal, Exploratory, Relationship, Code, Meta) with intent-driven routing
- **13 Fact Aspects** — Classification system for preferences, corrections, insights, failures, decisions, approaches, milestones, goals, habits, beliefs, wisdom, code, gaps
- **Standardized tool limits** — Interactive (10-15), List (20-30), Batch (50+)

### Changed
- Query routing: classification first, search second (300-450ms vs 1200-2400ms)

## [3.35.x] - 2026-02

### Added
- Companion activation mode
- Rich ledger extraction
- Auto-distillation of episode patterns

### Changed
- Embedding model: all-MiniLM-L6-v2 → bge-base-en-v1.5 (768 dimensions)

## [3.30.x] - 2026-01

### Removed
- PostgreSQL backend (DuckDB-only now)

## [3.27.x] - 2025-12

### Added
- xMemory-inspired theme system
- Hierarchical organization
- Two-stage retrieval

## [3.17.x]

### Added
- SSL enforcement
- Code intel protection
- Cost tracking

## [3.16.x]

### Added
- Background distillation
- Code enrichment

## [3.x]

### Added
- DuckDB backend
- Tree-sitter parsing
- Call graphs

## [2.x]

- C++ rewrite (Chitta engine)

## [1.x]

- Initial Python implementation

<!-- BEGIN CITATIONS -->
## References

- <a id="ref-2"></a>**[2]** Edwin B. Wilson. Probable Inference, the Law of Succession, and Statistical Inference. Journal of the American Statistical Association 22(158), 209–212 (1927). [source](<https://doi.org/10.1080/01621459.1927.10502953>)
- <a id="ref-3"></a>**[3]** Gordon V. Cormack, Charles L. A. Clarke, and Stefan Buettcher. Reciprocal rank fusion outperforms condorcet and individual rank learning methods. SIGIR, 758–759 (2009). [source](<https://doi.org/10.1145/1571941.1572114>)
- <a id="ref-4"></a>**[4]** Yu. A. Malkov and D. A. Yashunin. Efficient and robust approximate nearest neighbor search using Hierarchical Navigable Small World graphs. IEEE TPAMI 42(4), 824–836 (2020); arXiv:1603.09320 (2016). [source](<https://arxiv.org/abs/1603.09320>) [source](<https://doi.org/10.1109/TPAMI.2018.2889473>)
- <a id="ref-5"></a>**[5]** Amir Zandieh, Majid Daliri, Majid Hadian, and Vahab Mirrokni. TurboQuant: Online Vector Quantization with Near-optimal Distortion Rate. arXiv:2504.19874 (2025). [source](<https://arxiv.org/abs/2504.19874>)
- <a id="ref-6"></a>**[6]** A. Blumer, J. Blumer, D. Haussler, R. McConnell, and A. Ehrenfeucht. Complete inverted files for efficient text retrieval and analysis. Journal of the ACM 34(3), 578–595 (1987). [source](<https://doi.org/10.1145/28869.28873>)
- <a id="ref-7"></a>**[7]** Hubert Ramsauer et al. Hopfield Networks is All You Need. arXiv:2008.02217 (2020). [source](<https://arxiv.org/abs/2008.02217>)
- <a id="ref-8"></a>**[8]** Dmitry Krotov and John J. Hopfield. Dense Associative Memory for Pattern Recognition. NeurIPS 29 (2016); arXiv:1606.01164. [source](<https://arxiv.org/abs/1606.01164>)
- <a id="ref-10"></a>**[10]** Pentti Kanerva. Hyperdimensional Computing: An Introduction to Computing in Distributed Representation with High-Dimensional Random Vectors. Cognitive Computation 1, 139–159 (2009). [source](<https://doi.org/10.1007/s12559-009-9009-8>)
- <a id="ref-11"></a>**[11]** Jorma Rissanen. Modeling by shortest data description. Automatica 14(5), 465–471 (1978). [source](<https://doi.org/10.1016/0005-1098(78)90005-5>)
- <a id="ref-14"></a>**[14]** Subhadeep Pal, Fiona Y. Wang, and Markus J. Buehler. SwarmWorld: Stigmergic technological evolution in societies of language-model agents. arXiv:2608.26081 (2026). [source](<https://arxiv.org/abs/2608.26081>) [source](<https://arxiv.org/html/2608.26081>)
- <a id="ref-15"></a>**[15]** LAMM, MIT. MetaMaterialsDiscovery: Autonomous computational studies of hierarchical metamaterial fracture. Research archive, Hugging Face (accessed 2026-09-16). [source](<https://huggingface.co/lamm-mit/MetaMaterialsDiscovery>)
- <a id="ref-16"></a>**[16]** Di Wu, Hongwei Wang, Wenhao Yu, Yuwei Zhang, Kai-Wei Chang, and Dong Yu. LongMemEval: Benchmarking Chat Assistants on Long-Term Interactive Memory. ICLR (2025); arXiv:2410.10813 (2024). [source](<https://arxiv.org/abs/2410.10813>)
- <a id="ref-17"></a>**[17]** Adyasha Maharana, Dong-Ho Lee, Sergey Tulyakov, Mohit Bansal, Francesco Barbieri, and Yuwei Fang. Evaluating Very Long-Term Conversational Memory of LLM Agents. ACL (2024); arXiv:2402.17753. [source](<https://arxiv.org/abs/2402.17753>) [source](<https://aclanthology.org/2024.acl-long.747/>)
- <a id="ref-19"></a>**[19]** Tamas Spisak and Karl Friston. Self-orthogonalizing attractor neural networks emerging from the free energy principle. Neurocomputing, article 133472 (2026); arXiv:2505.22749 (2025). [source](<https://arxiv.org/abs/2505.22749>) [source](<https://doi.org/10.1016/j.neucom.2026.133472>)
- <a id="ref-24"></a>**[24]** Stephen Robertson and Hugo Zaragoza. The Probabilistic Relevance Framework: BM25 and Beyond. Foundations and Trends in Information Retrieval 3(4), 333–389 (2009). [source](<https://doi.org/10.1561/1500000019>)
- <a id="ref-25"></a>**[25]** Craig G. Nevill-Manning and Ian H. Witten. Identifying Hierarchical Structure in Sequences: A linear-time algorithm. Journal of Artificial Intelligence Research 7, 67–82 (1997); arXiv:cs/9709102. [source](<https://arxiv.org/abs/cs/9709102>)
- <a id="ref-26"></a>**[26]** Donald O. Hebb. The Organization of Behavior: A Neuropsychological Theory. Wiley (1949); Psychology Press reissue (2002). [source](<https://www.routledge.com/The-Organization-of-Behavior-A-Neuropsychological-Theory/Hebb/p/book/9780415654531>)
- <a id="ref-30"></a>**[30]** John G. Cleary and Ian H. Witten. Data Compression Using Adaptive Coding and Partial String Matching. IEEE Transactions on Communications 32(4), 396–402 (1984). [source](<https://doi.org/10.1109/TCOM.1984.1096090>)
- <a id="ref-31"></a>**[31]** Richard S. Sutton. Learning to Predict by the Methods of Temporal Differences. Machine Learning 3, 9–44 (1988). [source](<https://doi.org/10.1023/A:1022633531479>)
- <a id="ref-33"></a>**[33]** John J. Hopfield. Neural networks and physical systems with emergent collective computational abilities. PNAS 79(8), 2554–2558 (1982). [source](<https://doi.org/10.1073/pnas.79.8.2554>)
<!-- END CITATIONS -->
