# Scripts index

Every retained tracked shell, Python and Slurm script is listed below. The
ignored local build/dependency tree under `ssl_model/` is not part of this
inventory; its 15 tracked files are preserved (14 scripts plus `.gitignore`).

“Manual” means an explicit operator entrypoint, not an automatically wired job.
An index entry does not authorize deployment or writes to a live store; follow
the script options and the worktree constraints in `../codex-plugin/AGENTS.md`.

## Inventory

| Script | Purpose | Caller or reference |
|---|---|---|
| [`analyze-spans.sh`](analyze-spans.sh) | Summarize captured tool spans and outcomes | Manual research/maintenance entrypoint; no tracked caller |
| [`anchor_extract_pairs.py`](anchor_extract_pairs.py) | Extract shadow-anchor injection/reply pairs for manual judgment | Manual research/maintenance entrypoint; no tracked caller |
| [`anchor_validate.py`](anchor_validate.py) | Validate the turn-entity anchor gate against labeled pairs | `scripts/anchor_extract_pairs.py` |
| [`auto-index.sh`](auto-index.sh) | Incrementally index code at session start | `hooks/session-start-hook.sh` |
| [`batch-distill.sh`](batch-distill.sh) | Batch pending transcripts through hooks/distill.sh | Manual research/maintenance entrypoint; no tracked caller |
| [`bench-recall-lanes.sh`](bench-recall-lanes.sh) | Compare recall-lane hook latency in isolated state | `codex-plugin/AGENTS.md`; `docs/HOOKS.md` |
| [`benchmark_longmemeval.py`](benchmark_longmemeval.py) | Run LongMemEval retrieval experiments | Historical reference: `CHANGELOG.md`; manual |
| [`build_constellation.py`](build_constellation.py) | Build a constellation from harvested model geometry | `scripts/harvest_all_ollama.sh` |
| [`capture-hook.sh`](capture-hook.sh) | Preserve the disabled automatic-capture entrypoint | `docs/HOOKS.md` |
| [`cec-status.sh`](cec-status.sh) | Report CEC state | `scripts/smart-install.sh` |
| [`check-docs-links.py`](check-docs-links.py) | Check relative links and fragments in generated HTML docs | Manual research/maintenance entrypoint; no tracked caller |
| [`check-eval-immutable.sh`](check-eval-immutable.sh) | Enforce protected evaluation paths across commits | `.github/workflows/ci.yml`; `benchmarks/check_eval_immutable.py` |
| [`check-no-auto-prune.sh`](check-no-auto-prune.sh) | Guard against automatic episode pruning | `.github/workflows/ci.yml` |
| [`check-skills-sync.sh`](check-skills-sync.sh) | Check canonical skills against the Codex mirror | Manual pre-commit gate; `scripts/sync-skills.sh` |
| [`classify-intent.py`](classify-intent.py) | Classify hook prompts with the trained intent model | `hooks/prompt-core.sh`; `scripts/train-hook-classifier.sh` |
| [`configure-codex-hooks.sh`](configure-codex-hooks.sh) | Configure Codex hook wiring | `codex-plugin/AGENTS.md`; `docs/RENAME.md` |
| [`configure-mcp.sh`](configure-mcp.sh) | Add or remove Claude MCP configuration | `codex-plugin/skills/cc-soul-mcp/SKILL.md`; `scripts/smart-install.sh` |
| [`contract-snapshot.sh`](contract-snapshot.sh) | Compare public schemas, help text and manifests byte for byte | Manual cleanup/release gate; assignment-protected |
| [`convert_to_chatml.py`](convert_to_chatml.py) | Convert hint pairs into ShareGPT training data | `codex-plugin/skills/hint-corpus/SKILL.md`; `skills/hint-corpus/SKILL.md` |
| [`dev-install.sh`](dev-install.sh) | Link the owner checkout into installed plugin paths | `CLAUDE.md`; `docs/HOOKS.md` |
| [`domain_bias_probe.py`](domain_bias_probe.py) | Measure embedding domain collapse and GGUF parity | `scripts/finetune_nomic_lora.py` |
| [`dump-store.py`](dump-store.py) | Export unranked memory records as JSONL | `scripts/gen-eval-queries.py` |
| [`eval-learning.sh`](eval-learning.sh) | Run the automatic-learning harness | `benchmarks/learning/evidence/gates-2026-09-15.json`; `benchmarks/learning/evidence/gates-followup3-2026-09-15.json` |
| [`eval-noise.sh`](eval-noise.sh) | Calibrate evaluation noise bands | `docs/DECISION-2026-09-15-learning-experiment.md`; `docs/EVALS.md` |
| [`eval-replica-select.py`](eval-replica-select.py) | Select the manifest-committed snapshot family | `benchmarks/field-perf/README.md`; `benchmarks/field-perf/run.py` |
| [`eval-replica.sh`](eval-replica.sh) | Manage an isolated evaluation replica | `benchmarks/analogy/README.md`; `benchmarks/field-perf/run.py` |
| [`eval_hint_models.py`](eval_hint_models.py) | Evaluate hint extraction quality on fixed cases | Manual research/maintenance entrypoint; no tracked caller |
| [`eval_recall.py`](eval_recall.py) | Run seeded end-to-end retrieval experiments (writes test memories) | Manual research/maintenance entrypoint; no tracked caller |
| [`evolve-cycle.sh`](evolve-cycle.sh) | Run the proposal-to-evaluation development cycle | `chitta-mcp/evolve/proposals.d/swarmworld-best-of-n.json`; `docs/EVOLVE.md` |
| [`expand-nl.py`](expand-nl.py) | Expand SSL memories to natural-language training pairs | Manual research/maintenance entrypoint; no tracked caller |
| [`export_constellation_web.py`](export_constellation_web.py) | Export constellation geometry for the documentation viewer | `scripts/harvest_all_ollama.sh` |
| [`extract-code-intel.sh`](extract-code-intel.sh) | Extract code intelligence seeds and relationships | `hooks/extract-code-intel.sh` |
| [`finetune_bge.py`](finetune_bge.py) | Train an embedding model on SSL/natural-language pairs | `scripts/expand-nl.py` |
| [`finetune_hint_qwen.sh`](finetune_hint_qwen.sh) | Train and export the Qwen hint model | `chitta-mcp/enrichers/setup_hint_model.sh`; `codex-plugin/skills/hint-corpus/SKILL.md` |
| [`finetune_nomic_lora.py`](finetune_nomic_lora.py) | Train a nomic LoRA adapter with mixed-realm sampling | Manual research/maintenance entrypoint; no tracked caller |
| [`gen-eval-queries.py`](gen-eval-queries.py) | Generate memory/query evaluation pairs | `hooks/chitta-eval.py` |
| [`gen-hook-corpus.py`](gen-hook-corpus.py) | Generate intent-classifier training data | `scripts/train-hook-classifier.sh` |
| [`gen-tools-docs.py`](gen-tools-docs.py) | Generate API documentation from daemon tools/list | Historical reference: `CHANGELOG.md`; manual |
| [`gen_hint_corpus_live.py`](gen_hint_corpus_live.py) | Generate hint training pairs from datasets and memory | Manual research/maintenance entrypoint; no tracked caller |
| [`gen_hint_corpus_ssl.py`](gen_hint_corpus_ssl.py) | Extract hint supervision from transcript remember calls | Manual research/maintenance entrypoint; no tracked caller |
| [`gen_model_a_corpus.py`](gen_model_a_corpus.py) | Generate retrospective-enricher training pairs | Manual research/maintenance entrypoint; no tracked caller |
| [`generate_hint_corpus.py`](generate_hint_corpus.py) | Generate synthetic hint extraction training data | `codex-plugin/skills/hint-corpus/SKILL.md`; `scripts/gen_hint_corpus_live.py` |
| [`generate_hint_training_data.py`](generate_hint_training_data.py) | Generate user-turn/retrieval-hint pairs | `scripts/generate_hint_corpus.py` |
| [`harvest_all_ollama.sh`](harvest_all_ollama.sh) | Harvest installed Ollama models and rebuild the constellation | Manual research/maintenance entrypoint; no tracked caller |
| [`harvest_ow.py`](harvest_ow.py) | Extract open-weight model geometry for CEC | `scripts/harvest_all_ollama.sh` |
| [`hint_realtime.py`](hint_realtime.py) | Extract realtime retrieval hints | `chitta/systemd/chitta-hintd.service`; `hooks/prompt-core.sh` |
| [`hint_replay.py`](hint_replay.py) | Compare Python and C++ hint extraction on identical turns | Manual research/maintenance entrypoint; no tracked caller |
| [`hintd_smoke.py`](hintd_smoke.py) | Check hint-daemon liveness, inference and concurrent requests | Manual research/maintenance entrypoint; no tracked caller |
| [`hone-gepa.py`](hone-gepa.py) | Evolve candidate distillation prompts in shadow mode | Manual research/maintenance entrypoint; no tracked caller |
| [`hook-stats.sh`](hook-stats.sh) | Summarize hook decision logs | `CLAUDE.md`; `docs/HOOKS.md` |
| [`injection_precision.py`](injection_precision.py) | Measure injected-memory/reply overlap offline | `scripts/sample_injections.py` |
| [`install-evolve-timers.sh`](install-evolve-timers.sh) | Install owner-managed evolution timers | Manual owner setup; assignment-protected |
| [`locomo-benchmark.py`](locomo-benchmark.py) | Run long-term conversational-memory evaluation | `codex-plugin/skills/locomo-benchmark/SKILL.md`; `skills/locomo-benchmark/SKILL.md` |
| [`migrate-ssl-v03.sh`](migrate-ssl-v03.sh) | Migrate legacy SSL annotations with an optional dry run | Manual research/maintenance entrypoint; no tracked caller |
| [`mine_training_pairs.py`](mine_training_pairs.py) | Mine embedding pairs from an exported memory graph | `scripts/finetune_nomic_lora.py` |
| [`msg-notify.sh`](msg-notify.sh) | Notify a session of queued cross-session messages | `hooks/session-start-hook.sh` |
| [`post-commit-hook.sh`](post-commit-hook.sh) | Record post-commit learning | `hooks/post-commit-hook.sh` |
| [`probes/provenance_surface_check.sh`](probes/provenance_surface_check.sh) | Check provenance keyed-lane callable wiring | Manual research/maintenance entrypoint; no tracked caller |
| [`probes/task_state_surface_check.sh`](probes/task_state_surface_check.sh) | Check task-state callable wiring | Manual research/maintenance entrypoint; no tracked caller |
| [`realm-retag.sh`](realm-retag.sh) | Retag project memories into the appropriate realm | `hooks/session-start-hook.sh` |
| [`recall_rerank_eval.py`](recall_rerank_eval.py) | Compare recall rerankers against store sidecars | `docs/recall-rerank-eval-results.md` |
| [`reembed_ollama.sh`](reembed_ollama.sh) | Submit GPU re-embedding through Ollama | Manual research/maintenance entrypoint; no tracked caller |
| [`release.sh`](release.sh) | Build and publish an owner-approved release; sync Codex skills | `CLAUDE.md`; `codex-plugin/skills/kriya/SKILL.md` |
| [`sample_injections.py`](sample_injections.py) | Sample injection/reply pairs for manual precision judgment | Manual research/maintenance entrypoint; no tracked caller |
| [`shared-stack.sh`](shared-stack.sh) | Forward manual shared-stack lifecycle commands to chitta-mcp/stack.py | Manual shared-stack operator; no tracked caller |
| [`shell-integration.sh`](shell-integration.sh) | Provide optional shell task tracking via provenance extraction | Manual source from shell startup; no tracked caller |
| [`smart-install.sh`](smart-install.sh) | Install binaries, models, hooks and services | `README.md`; `chitta-mcp/stack.py` |
| [`ssl_model/01_extract_data.py`](ssl_model/01_extract_data.py) | Extract transcript data for SSL model training | Manual research/Slurm workflow; preserved outside cleanup scope |
| [`ssl_model/01c_web_corpus.py`](ssl_model/01c_web_corpus.py) | Build a web-derived SSL corpus | Manual research/Slurm workflow; preserved outside cleanup scope |
| [`ssl_model/02_finetune.py`](ssl_model/02_finetune.py) | Fine-tune the SSL model | Manual research/Slurm workflow; preserved outside cleanup scope |
| [`ssl_model/02_finetune.slurm`](ssl_model/02_finetune.slurm) | Submit SSL fine-tuning to Slurm | Manual research/Slurm workflow; preserved outside cleanup scope |
| [`ssl_model/02b_dpo_finetune.py`](ssl_model/02b_dpo_finetune.py) | Train the SSL model with preference pairs | Manual research/Slurm workflow; preserved outside cleanup scope |
| [`ssl_model/02b_dpo_finetune.slurm`](ssl_model/02b_dpo_finetune.slurm) | Submit SSL preference training to Slurm | Manual research/Slurm workflow; preserved outside cleanup scope |
| [`ssl_model/03_export_gguf.sh`](ssl_model/03_export_gguf.sh) | Export the SSL adapter as GGUF | Manual research/Slurm workflow; preserved outside cleanup scope |
| [`ssl_model/04_export_only.slurm`](ssl_model/04_export_only.slurm) | Submit GGUF export to Slurm | Manual research/Slurm workflow; preserved outside cleanup scope |
| [`ssl_model/bench_embedders.py`](ssl_model/bench_embedders.py) | Compare embedding models | Manual research/Slurm workflow; preserved outside cleanup scope |
| [`ssl_model/bench_meanpool.py`](ssl_model/bench_meanpool.py) | Benchmark mean-pooling embeddings | Manual research/Slurm workflow; preserved outside cleanup scope |
| [`ssl_model/eval_public_embed.py`](ssl_model/eval_public_embed.py) | Evaluate a public embedding model | Manual research/Slurm workflow; preserved outside cleanup scope |
| [`ssl_model/generate_synthetic_corpus.py`](ssl_model/generate_synthetic_corpus.py) | Generate synthetic SSL training examples | Manual research/Slurm workflow; preserved outside cleanup scope |
| [`ssl_model/generate_web_corpus.py`](ssl_model/generate_web_corpus.py) | Generate web-derived training examples | Manual research/Slurm workflow; preserved outside cleanup scope |
| [`ssl_model/train_public.slurm`](ssl_model/train_public.slurm) | Submit public embedding training to Slurm | Manual research/Slurm workflow; preserved outside cleanup scope |
| [`sync-installed-hooks.sh`](sync-installed-hooks.sh) | Synchronize or check installed hook copies | `CLAUDE.md`; `codex-plugin/AGENTS.md` |
| [`sync-skills.sh`](sync-skills.sh) | Refresh the repository Codex skill mirror | Manual after editing canonical skills; see root README |
| [`train-hook-classifier.sh`](train-hook-classifier.sh) | Train the hook intent classifier | `scripts/smart-install.sh` |

## Operator tools moved from hooks/

Placeholder: `bridge-holes.sh`, `debug-recall.sh`, `evolve-topology.sh`, and
`settle-predictions.sh` will arrive here from the hooks worker. That worker owns
the moves; this scripts-pruning change does not move them.

## Skills maintenance

Edit only `skills/`, then run `bash scripts/sync-skills.sh` and
`bash scripts/check-skills-sync.sh`. The latter is a read-only pre-commit gate
that fails on changed, missing or extra mirrored content. `_conventions` is
source-only. Both helpers need `rsync` and work from any cwd. Sync changes only
`codex-plugin/skills/` in this checkout. The existing release script already
syncs the same source; no plugin-loader symlink behavior is assumed.

## Installer ownership

`scripts/smart-install.sh` is the installer referenced by the README and setup
skills. `scripts/dev-install.sh` repairs hook links and invokes
`scripts/sync-installed-hooks.sh`. The Claude plugin manifest loads `./skills/`;
it does not select a second installer. `hooks/smart-install.sh` is neither tracked
nor present in this checkout. The earlier mention in `docs/RENAME.md` is historical.

## Pruning evidence (2026-09-16)

Before adding this index, a literal basename search across other tracked
`*.sh`, `*.py`, `*.yml`, `*.md`, `*.json`, `*.toml`, `*.service`, and `*.timer`
files (excluding directories `ssl_model`, `target`, `build`, `.git`) found 47
unreferenced shell/Python scripts out of 91. This excludes self-references and
untracked files. Ten hits are in the untouched SSL model tree. Thirty-four
unreferenced scripts are retained as indexed manual tools or protected inputs.

All 13 deletions below had zero references under that rule. Dynamic callers
were inspected separately: the health driver selects `test-soul-tier${tier}.sh`,
so that entire obsolete family is removed together. Basename matches are only
a discovery heuristic; retained manual tools are not declared dead merely for
lacking an automated caller.

| Deleted script | Additional evidence / disposition |
|---|---|
| `batch-distill-native.sh` | Unused native-distiller batch entrypoint; stops the daemon before even honoring --dry-run. Keep batch-distill.sh for the current hooks/distill.sh path. |
| `deploy-integration.sh` | One-off deployment pinned to integration/soul-fixes and chitta-field f2805fb, with a hard-coded owner checkout; superseded by canonical deployment guidance. |
| `fix-dream-pages.py` | One-off HTML rewrite hard-codes the old cc-soul navigation and would restore stale branding. |
| `lib/chitta-lib.sh` | Unused alternate socket/MCP library; maintained hooks source hooks/lib.sh instead. |
| `lib/event-rules.sh` | Unused alternate event prefilter; no sourcing caller, and sourcing creates a legacy session-cache file. |
| `memory-cleanup.sh` | One-off cleanup calls removed sql_query, hygiene_run and restore_code_intel_confidence RPCs (absent from contracts/daemon-tool-names.txt). |
| `session-tracker.sh` | Unused manual tracker writes legacy .session_learned/.session_start files; active task/session tracking is daemon-backed. |
| `smart-inject.sh` | Unused alternate injection path invokes chittad resonate and old ONNX/vocab setup; active injection is hooks/prompt-core.sh. |
| `test-soul-health.sh` | Unreferenced driver for the obsolete SQL health tiers; removed together with all three dynamically selected children. |
| `test-soul-tier1.sh` | Only dynamic caller was test-soul-health.sh; tests removed sql_query, DuckDB extensions and SQL memory tables. |
| `test-soul-tier2.sh` | Only dynamic caller was test-soul-health.sh; tests embeddings with removed sql_query and SQL memory columns. |
| `test-soul-tier3.sh` | Only dynamic caller was test-soul-health.sh; lifecycle assertions depend on removed sql_query and SQL memory tables. |
| `token-savings.sh` | Unused standalone estimator writes .token_savings, with no tracked producer or consumer outside the script. |
