# chitta Architecture

Status as of 2026-09-16. Rewritten as a 150-line overview from the 1,131-line
2026-09-02 version; the long form (build steps, predicate tables, decay rates,
confidence model, embedder circuit breaker) is preserved in git history:
`git show 33aacc65:docs/ARCHITECTURE.md`. Treat it as historical detail, not
current truth, where the two disagree.

chitta is a C++ daemon over an embedded Rust store, with a native CLI,
Python MCP gateways and shell hooks for Claude Code and Codex. This describes
the current source tree, including the pinned chitta-field revision
`f3176e587dced64dd3a6a663a40a58e3802b0a10`. Historical design proposals are not
the implementation contract. See [API.md](API.md), [HOOKS.md](HOOKS.md),
[CLI.md](CLI.md) and [FIELD_PERF.md](FIELD_PERF.md) for operational details.

## System Overview

```text
Claude Code / Codex
  ├─ lifecycle hooks (bash/jq) ── chitta native CLI ─┐
  └─ MCP client ── chitta-mcp Python gateways ───────┤
                                                  ▼
                           chittad: FieldRpcHandler, worker pool,
                           embedder, distiller, maintenance
                                                  │ C FFI
                                                  ▼
                           chitta-field: indexes, payloads, states,
                           triplets, event records, task ledger
                                                  │
                           snapshot families + sidecars + WAL
```

The native client and hooks honor `CHITTA_SOCKET_PATH`. Multiple sessions
share one daemon; a second daemon needs a separate store directory. The Python
MCP server composes gateway tools and limits its default advertised tool list.
Native CLI help and daemon MCP discovery are overlapping surfaces, not identical
lists; the generated [tool page](tools.html) documents both.

## Components

| Component | Source | Responsibility |
|---|---|---|
| Daemon | `chitta/src/simple_cli.cpp` | Startup, embedding, distillation, queue and maintenance |
| Native CLI | `chitta/src/rpc_server.cpp` | Tool help, argument parsing and RPC forwarding |
| RPC handler | `chitta/include/chitta/rpc/field_handler.hpp` | Dispatch, budgets and lock discipline |
| Memory recall | `chitta/src/handlers/field_memory_recall.cpp` | Candidate pooling, fusion, prefilter and formatting |
| Rust store | `chitta-field/src/field.rs`, `src/store.rs` | Persistence, search indexes and shared state |
| MCP | `chitta-mcp/server.py`, `tools_static.py` | Visibility, schemas and composed tools |
| Hooks | `hooks/hooks.json`, `hooks/*.sh` | Context injection, lifecycle and enforcement |

## Storage Layer (chitta-field)

The store lives at `<mind>/chitta-field/`. Rust keeps payloads and metadata,
dense embeddings, sparse codes and graph/index structures in memory and persists
them through snapshot families and the WAL. There is no external database server.
Storage can reside on NFS; filesystem latency and stale locks remain real concerns.

One exclusive `.instance.lock` owns the directory. Startup can replace a lock
whose recorded PID is dead on this host. It must not steal a live or other-host
lock. Open failures report their cause. The daemon service allows
`TimeoutStopSec=300` so shutdown can finish its snapshot; operator recovery is
described in [CLI.md](CLI.md#startup-sidecars-and-instance-lock).

Ordinary WAL writes use timer-based sync (200 ms by default through
`CHITTA_WAL_SYNC_MS`); explicitly durable operations force persistence. Appending
an operation and syncing it to durable storage are distinct steps. Recall access
touches accumulate and drain every five seconds instead of writing on each read.
Snapshot decode, uncovered WAL replay and loader markers together determine
recovered state. A snapshot identifier alone is insufficient for an eval family
that also contains uncovered WAL.

### Startup and derived indexes

Snapshot sections decode in a bounded parallel pool from an mmap. Triplet and
keyword reconstruction overlap other phases. Derived `.lsh`, `.turbo` and
`.organs` sidecars cache normalized search state, the quantized index and organs;
they can be rebuilt when absent or incompatible. Cache identity follows the
snapshot family, allowing matching caches with a subsequent WAL delta.

Measured live startup is about **9.5 s** with cache hits and about **20 s** for
the first start after deployment or a snapshot-format change. The quantized index
may finish rebuilding on the maintenance thread after `ready`. Phase logs use
`load phase=<name> ms=` and `cache hit=`; phases overlap and must not be summed.
Allow 30 seconds after restart before measuring recall. Historical measurements,
including unmet targets and test limitations, remain in [FIELD_PERF.md](FIELD_PERF.md).

## Semantic Index (ANN)

Dense semantic search uses the configured store indexes and exact rescoring;
the optimized quantized path and its maintenance-thread rebuild are described
in [FIELD_PERF.md](FIELD_PERF.md). Results also depend on realm filters, recall
strategy and runtime scoring. Do not infer end-to-end hook latency from an
isolated index benchmark.

## Cortical Index (SDR)

Sparse associative codes activate 64 of 16,384 features. A posting index
retrieves memories sharing active features. This is one retrieval mechanism;
the default MCP recall gateway uses hybrid retrieval, including exact terms.

## Embedding Engine (Vāk)

The normal local backend loads a GGUF through llama.cpp. Dimension is a
compile-time identity, not a per-call option: public CMake defaults are
1024-dimensional bge-large-en-v1.5, while the measured deployment uses
768-dimensional nomic-embed-text-v1.5. The daemon rejects a model dimension
that disagrees with its build. Changing identity requires a fresh build and
compatible store embeddings. `CHITTA_EMBED_CONTEXTS` defaults to four contexts;
query embeddings are cached. See [CLI.md](CLI.md#embed-model-resolution).

## Recall and Scoring

The [recall pipeline](recall.html) distinguishes daemon retrieval, the MCP
gateway reranker and hook admission. Native recall can widen a pool (default
60, cap 160), preserve candidates through a recall-biased scalar prefilter
(rerank budget 24), then truncate to the requested limit. Optional cross-encoder
reranking is a gateway concern. `full_resonate` is a separate resonance tool,
not a synonym for every recall call.

Rust scoring reads `scoring.json`. Kind priors are correction/preference 1.3,
wisdom 1.1, insight 1.05, operational 0.8, episode 0.7 and default 1.0.
The operational prior discounts verbatim distiller fragments relative to curated
conclusions at equal similarity. Factors feed a bounded scoring pipeline; these
numbers are not standalone probabilities.

Independent-session replication weighting is **default-off** (`replication_max=0`).
`CHITTA_REPLICATION_MAX` overrides the config; 1.15 is the tested opt-in cap.
Repeated reads do not count as independent replications. The measured store had
no qualifying replicated cohort and no golden-score change; see [EVALS.md](EVALS.md).

## Graph Layer (Triplets)

Triplets retain explicit directed relations alongside embedding similarity.
`recall_analogy(a,b,c)` looks up predicates connecting a to b and transfers those
predicates to actual outgoing neighbors of c. Results include supporting edges;
missing source or target relations yield a `reason` on abstention. There is no
fuzzy reverse relation inference or unsupported vector-generated answer.
Frozen-replica results were hit@1 14/14, hit@3 14/14, negative abstentions 14/14,
and zero unsupported answers.

## Session Continuity

Session checkpoints (`ledger_save` and `ledger_load`) coexist with the daemon
task ledger. `ledger_op` stores task threads, inbox, artifacts, session bindings
and exclusive leases through atomic row-change batches in the Rust WAL and
snapshots. Hooks call the native `session_register`, `session_heartbeat` and
`ledger_op` tools; `session_registry.py` and `task_ledger.py` are compatibility
clients. Explicit legacy import is the only normal use of SQLite in that client.

## Integration with Claude Code

Claude Code and Codex adapters share the backend. Routine prompt, SessionStart
and Stop work uses bash/jq and native RPC calls rather than Python starts for
cleanup, registry and ledger rendering. Explicit RLM [71](#ref-71) and opt-in background paths
can still start Python. Realm detection maps linked worktrees to their main
checkout. Topic lanes skip turns with no distinctive tokens; UNKNOWN-band
admission requires distinctive-token support. [HOOKS.md](HOOKS.md) is the
enforcement reference.

On the first Read of a file per session, artifact memories may appear as
`[traces]` (`CHITTA_FILE_TRACES=0` disables). `artifact-trace.sh` registers script
writes as `[artifact]` signals. Outcome hooks record injection and command events;
Codex exit-less responses have `exit_code:null`, with a separate text heuristic.
These events are associational evidence, not causal proof of memory utility.

## Subconscious Processor

The daemon schedules embedding, native transcript distillation, hygiene and
other maintenance. Native distillation can invoke a configured model endpoint;
native implementation does not imply no model call. Code-enrichment options
remain accepted but that worker is inert. The separate hint worker can run a
configured script after three new memories, with a 600-second cooldown and
50-memory batch; `--no-enrich` does not disable it.

## Autonomous Work and Evaluation

[Sadhana](SADHANA.md) provides persistent sense/think/act agents. The separate
[evolve loop](EVOLVE.md) implements changes in isolated worktrees, runs gates
and paired replica measurements, and leaves merge/deployment to humans.
`--candidates N` enables isolated fan-out (default 1); passing candidates are
selected by measured bet delta, then patch size.

The automatic-learning experiment recorded its official cohort cut at
2026-09-15 23:15 CEST. Its prospective panel remains pending: 20 tasks, three
trials and two arms (120 runs). Planted-memory SMRITI results do not answer this
causal question. See [EVALS.md](EVALS.md), [EVAL_REPLICA.md](EVAL_REPLICA.md) and
the [experiment protocol](../benchmarks/learning/protocol.md).

<!-- BEGIN CITATIONS -->
## References

- <a id="ref-71"></a>**[71]** Alex L. Zhang, Tim Kraska, and Omar Khattab. Recursive Language Models. arXiv:2512.24601 (2025; revised 2026). [source](<https://arxiv.org/abs/2512.24601>)
<!-- END CITATIONS -->
