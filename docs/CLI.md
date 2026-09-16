# chitta CLI Reference

Status as of 2026-09-16.

`chittad` is the daemon binary (memory server, background processing, Unix socket listener).
The `chitta` client in `chitta/src/rpc_server.cpp` discovers tool commands from the daemon. Run
`chitta --help` for categories and `chitta <tool> --help` for parameters.
Global client options include `--socket-path PATH`, `--json`, `--toon`,
`--text-only` and `--help`. `CHITTA_SOCKET_PATH` selects a private daemon.
The [API reference](API.md) covers MCP tools and the additional native CLI surface.

```bash
chitta recall --query "snapshot sidecars" --limit 5
chitta recall_analogy --a "source" --b "target" --c "related entity"
chitta health_check --json
chitta ledger_op --help
```

---

## Table of Contents

- [chittad — Commands](#chittad--commands)
  - [daemon](#daemon)
  - [shutdown](#shutdown)
  - [status](#status)
  - [stats](#stats)
  - [metrics](#metrics)
  - [distill](#distill)
  - [re_embed](#re_embed)
  - [export_content](#export_content)
  - [import_embeddings](#import_embeddings)
  - [reindex](#reindex)
  - [migrate-store-format](#migrate-store-format)
  - [prune-memories](#prune-memories)
  - [health](#health)
  - [format-id](#format-id)
  - [hint_extract](#hint_extract)
- [One daemon per shared store](#one-daemon-per-shared-store-across-login-nodes)
- [Environment Variables](#environment-variables)
- [Embed Model Resolution](#embed-model-resolution)
- [Daemon Internals](#daemon-internals)
- [Troubleshooting](#troubleshooting)

---

## One daemon per shared store across login nodes

The primary host runs the daemon and owns the NFS store lock. The installer
writes `<mind>/.daemon-node` with `hostname -s`, overridden by
`CHITTA_DAEMON_NODE`, and writes `chittad.service.d/primary-node.conf` in the
user unit directory. Its guard matches the 2026-09-16 hand-installed behavior:
a missing/empty marker permits startup; a different host exits 75; the drop-in
sets `RestartPreventExitStatus=75`. Custom mind paths are passed through a
quoted environment entry. Installing on another host reassigns the marker
unless `CHITTA_DAEMON_NODE` names the existing primary. The shared unit file
takes effect on each node only after that node reloads its unit configuration.
The NFS instance lock remains the final cross-host fence.

The cross-node client contract is: when the local socket is absent, hooks and
MCP on other hosts reach the primary via HTTP JSON-RPC using `CHITTA_RPC_HOST`
and `CHITTA_RPC_PORT` (deployment convention: 7432). The primary must enable a
reachable RPC listener; the daemon's generic default remains 0 (disabled).

**Automatic fallback is the next step, not implemented by this change.** The
shell availability gate currently requires a local socket. Python hook calls
support explicit host/port, but MCP's `daemon_bridge.py` hard-codes localhost
and can attempt a local daemon start after connection failure. The CLI and
socket-only hook helpers also need consistent transport selection. Changing
only `hooks/lib.sh` and `daemon_client.py` would leave those paths inconsistent.
A complete follow-up must share endpoint resolution, preserve explicit socket
overrides, bound remote timeouts, and suppress all local startup attempts on
secondary nodes. Do not enable node-local queues on secondary nodes: only the
primary drains its own runtime directory, and no remote queue relay exists.


## chitta — Discovered tool commands

`chitta <tool> --param value` resolves tools from the selected daemon's
`tools/list`. Use `CHITTA_SOCKET_PATH` or `--socket-path` to select a daemon.
`chitta <tool> --help` shows its schema description, parameters, required flags,
and explicit defaults. Bare `chitta --help` keeps the existing usage text.

The client caches schemas in `cli-tools-<socket-hash>.json` in its runtime
socket directory (`$XDG_RUNTIME_DIR/chitta`, then `/run/user/<uid>/chitta`,
then `~/.cache/chitta`). The cache records the full socket path, daemon PID,
process start ticks, and boot ID. A different daemon identity or unknown tool
triggers a fresh `tools/list`. Cache files are private and replaced atomically;
a corrupt cache is fetched again. Previously cached help works while the daemon
is down. A first-time tool needs a running daemon to discover its schema.

Parameters follow the JSON schema: strings stay strings (including numeric IDs),
numbers and booleans use JSON values, and objects/arrays accept JSON. Arrays also
accept comma-separated values, including a single value. Both `--some-param`
and `--some_param` address `some_param`. Required parameters are checked before
calling the daemon; optional explicit schema defaults are supplied when omitted.
Descriptions may mention daemon defaults without declaring a JSON Schema
`default`; those parameters remain omitted. Unknown parameters are forwarded for
handler extensions. `remember`/`grow` retain the `--kind` alias for `--type`.
Global output and socket flags can appear before or after the tool name.

The refactor expands CLI reachability from **184 to 369 names**: **320 advertised
tools**, plus **49 legacy registered handlers**. The name contract covers
the union of advertised daemon tools and retained legacy handlers (369 names). Existing client-side commands such as
`realm_detect`, `status`, and `queue_write` keep their local behavior.

These 49 old CLI names are absent from `tools/list` but still have registered
handlers. They remain callable; the daemon validates their arguments, and their
help explicitly says no schema is advertised:

`cleanup`,
`cycle`,
`dedupe_symbols`,
`describe_symbol`,
`distill_status`,
`epiplexity_check`,
`export_soul`,
`export_training_pairs`,
`extract_symbols`,
`fep_status`,
`file_dependents`,
`file_imports`,
`harvest_scope`,
`health_check_start`,
`hygiene_run`,
`import_soul`,
`ingest_source`,
`ledger_append`,
`ledger_compile`,
`ledger_contradictions`,
`ledger_op`,
`ledger_query`,
`msg_ack`,
`predicate_attach`,
`predicate_list`,
`predicate_run`,
`queue_experiments`,
`recall_temporal_events`,
`reconcile_pass`,
`reembed_memories`,
`resolve_callsites`,
`routed_recall`,
`seed_hdc_geometry`,
`session_deregister`,
`session_heartbeat`,
`session_register`,
`tape_stats`,
`transcript_get`,
`transcript_list`,
`transcript_parse`,
`transcript_register`,
`transcript_remove`,
`transcript_update`,
`turiya_status`,
`type_hierarchy`,
`verbalize_rules`,
`version_check`,
`wiki_export`,
`witness_memory`.

These six old names have no registered handler and are dropped:

`background_schedule`, `background_status`, `cleanup_code_wisdom`, `migrate_vss`, `theme_assign_orphans`, `theme_maintain`.

CLI help contracts now use schema descriptions, schema property order,
required markers, explicit default values, and required-parameter examples.
Parameters absent from a schema disappear; newly advertised parameters appear.
The six names without handlers leave the help snapshot, 49 retained legacy
handlers receive explicit fallback help, and 191 newly reachable daemon tools
enter it. Bare usage, daemon/MCP
schemas, and all non-CLI contracts remain unchanged.

---

## chittad — Commands

### daemon

Run the background daemon. Self-daemonizes by default (double-fork). Use `-f` to stay in foreground.

```bash
chittad daemon [options]
```

**Options:**

| Option | Description | Default |
|--------|-------------|---------|
| `--path PATH` | Mind storage directory | `~/.claude/mind` |
| `--interval SECS` | Maintenance cycle interval | `60` |
| `-f, --foreground` | Run in foreground (no daemonize) | Off |
| `--verbose` | Verbose logging | Off |
| `--distill-interval MINS` | Distillation check interval (minutes) | `15` |
| `--distill-min-turns N` | Minimum turns before distillation fires | `4` |
| `--distill-script PATH` | Accepted and ignored; distillation is native C++ | — |
| `--distill-model MODEL` | Model for distillation | `gemma4:26b` |
| `--distill-token-trigger N` | Char count to trigger distillation (0=off) | `120000` |
| `--distill-cooldown SECS` | Minimum seconds between distillations | `180` |
| `--distill-max-tokens N` | Max tokens for distillation context | `8192` |
| `--distill-context-chars N` | Max chars of context fed to distiller (0=unlimited) | `0` |
| `--no-distill` | Disable automatic distillation | Off |
| `--embed-model MODEL` | GGUF embed model name | Auto (see below) |
| `--no-enrich` | Disable code-enrichment configuration; the code worker is currently inert. Does not disable the separate hint worker | Off |
| `--no-hygiene` | Disable hygiene and sleep consolidation | Off |
| `--no-autonomous` | Disable dream/think/belief-maintenance callbacks | Off |
| `--merge-policy POLICY` | Memory merge policy (`off` or `merge_aware`) | `off` |
| `--embed-interval SECS` | Background embedding interval; `--no-embed-interval` disables | `30` |
| `--rpc-port PORT` | HTTP JSON-RPC server port (also `CHITTA_RPC_PORT` env) | `0` (disabled) |
| `--http-port PORT` | HTTP visualization server port | `0` (disabled) |
| `--http-static-dir DIR` | Static files directory for HTTP viz server | — |

**What the daemon does:**

- Opens chitta-field store (Rust FFI) at the mind path
- Loads GGUF embedding model via LlamaYantra (see [Embed Model Resolution](#embed-model-resolution)); falls back to OllamaYantra HTTP
- Creates Unix domain socket via `socket_path_for_mind(mind_path)`
- Writes PID via `pid_path_for_mind(mind_path)`
- Starts thread pool (hard-coded 8 min / 16 max workers); queue depth cap via `CHITTA_MAX_QUEUE_DEPTH` (default 256)
- Sets `OMP_NUM_THREADS`, `MKL_NUM_THREADS`, `OPENBLAS_NUM_THREADS`, `ORT_NUM_THREADS` to 4
- Runs queue processor: reads `{mind}/queue.jsonl` (`CHITTA_QUEUE` overrides); failed ops written to `{mind}/queue.jsonl.failed`
- Runs hint enrichment thread (trigger: 3 new memories; cooldown: 600s; batch: 50); script via `CHITTA_HINT_ENRICHER` env or smart-install default
- Runs inotify watcher on `segments/` for same-host peer writes (Linux only); 5s fallback for foreign sync
- Detects binary self-updates every 60s via `/proc/self/exe` mtime; checks vector-space id compatibility before restart (Linux only)
- Autonomous callbacks (unless `--no-autonomous`): `dream_wander` (idle 10+ min), `think_wander` (idle 5+ min, hourly), belief maintenance

**Example:**

```bash
# Standard daemon (daemonizes)
chittad daemon

# Foreground with verbose, distillation enabled
chittad daemon --foreground --verbose --distill-interval 900 --distill-model gemma4:26b

# With HTTP JSON-RPC server
chittad daemon --rpc-port 9482

# Disable autonomous background work
chittad daemon --no-autonomous --no-hygiene
```

---

### shutdown

Gracefully stop the running daemon.

```bash
chittad shutdown
```

Sends a shutdown command via the Unix socket. The daemon saves state and exits. The command waits up to 30s for the socket to disappear. A SIGALRM watchdog force-exits after 15s if threads do not finish.

---

### status

Check if the daemon is running and responsive.

```bash
chittad status
```

---

### stats

Show memory and symbol counts.

```bash
chittad stats
```

**Output:**
```
memory_count: 1963
symbol_count: 340
yantra: ready
```

Note: no `--json` or `--fast` flags; no SUS metric output (Samarasya/Ojas scores are not produced by this command).

---

### metrics

Show memory and symbol counts (same as `stats`; future work noted in source).

```bash
chittad metrics [days]
```

`days` is an optional positional argument (default 7) but is not currently used in output.

---

### distill

Manually distill a transcript into memories.

```bash
chittad distill --transcript-path PATH [--session-id ID] [--realm REALM]
```

**Options:**

| Option | Description | Default |
|--------|-------------|---------|
| `--transcript-path PATH` | Path to transcript file (required) | — |
| `--session-id ID` | Session identifier (auto-derived from filename if omitted) | Auto |
| `--realm REALM` | Target realm | `brahman` |

Verbose output is always enabled for manual distillation.

---

### re_embed

Re-embed all memories using the current GGUF model. Requires an embed model. Calls `requeue_all_embeddings`, `force_reindex`, and `save_full_snapshot`.

```bash
chittad re_embed --embed-model MODEL
```

Use this after switching embed models to regenerate all vectors.

---

### export_content

GPU re-embed migration — phase 1. Dumps id + text as JSONL for external embedding.

```bash
chittad export_content --out FILE.jsonl
```

---

### import_embeddings

GPU re-embed migration — phase 3. Reads binary records (u64 id + f32[EMBED_DIM] vector) and writes them back into the store.

```bash
chittad import_embeddings --in FILE.bin
```

---

### reindex

Rebuild ANN indices (binary codes, coarse quantizer, LSH [73](#ref-73), HNSW [4](#ref-4)) from current in-store embeddings. No embed model required.

```bash
chittad reindex
```

Use this to rebuild indices after `import_embeddings` or if indices become corrupted.

---

### migrate-store-format

PR5 migration: stamps the `.shdr` identity sidecar on an existing store without re-embedding.

```bash
chittad migrate-store-format
```

---

### prune-memories

Remove or down-weight memories matching text substrings.

```bash
chittad prune-memories --match PATTERNS [--apply] [--delete | --background]
```

**Options:**

| Option | Description | Default |
|--------|-------------|---------|
| `--match PATTERNS` | Comma-separated substrings (case-insensitive) to match against memory text | — |
| `--apply` | Execute the action (dry-run if omitted) | Dry-run |
| `--delete` | Delete matched memories (default action when `--apply` set) | Default |
| `--background` | Down-weight matched memories instead of deleting | Off |

**Example:**

```bash
# Dry-run: show what would be deleted
chittad prune-memories --match "obsolete,stale"

# Delete matching memories
chittad prune-memories --match "obsolete,stale" --apply --delete

# Down-weight instead of delete
chittad prune-memories --match "low-confidence" --apply --background
```

---

### health

Lightweight daemon ping over the Unix socket. Never loads the FieldStore.

```bash
chittad health
```

**Output:**
```
pid: 12345
uptime_ms: 3600000
```

---

### format-id

Print the compiled vector-space id (`cf_compiled_vector_space_id()`). Used by the self-update gate to check compatibility. Never loads FieldStore.

```bash
chittad format-id
```

---

### hint_extract

Extract hints from a file or stdin. Only available when built with `CHITTA_WITH_LLAMA_CPP=ON`.

```bash
chittad hint_extract [FILE]
```

If `FILE` is omitted, reads from stdin line by line.

---

## Environment Variables

| Variable | Description | Default |
|----------|-------------|---------|
| `CHITTA_EMBED_MODEL` | Embed model name (overridden by `--embed-model`) | Build default |
| `CHITTA_MAX_QUEUE_DEPTH` | Max queue depth for thread pool job queue | `256` |
| `CHITTA_RPC_PORT` | HTTP JSON-RPC server port (overridden by `--rpc-port`) | `0` (disabled) |
| `CHITTA_MERGE_POLICY` | Memory merge policy (`off` or `merge_aware`); set by `--merge-policy` | — |
| `CHITTA_HINT_ENRICHER` | Path to hint enricher binary | Smart-install default |
| `OMP_NUM_THREADS` | OpenMP threads (set to 4 at startup) | `4` |
| `MKL_NUM_THREADS` | MKL threads (set to 4 at startup) | `4` |
| `OPENBLAS_NUM_THREADS` | OpenBLAS threads (set to 4 at startup) | `4` |
| `ORT_NUM_THREADS` | ONNX Runtime threads (set to 4 at startup) | `4` |

Variables `CHITTA_DB_PATH` and `SUBCONSCIOUS_INTERVAL` are not used by `chittad`.

---

## Embed Model Resolution

The daemon prefers in-process GGUF embedding via LlamaYantra. Model filename is determined by `cf_embed_model_id()` (build-time constant). Resolution order:

1. `--embed-model PATH` / `CHITTA_EMBED_MODEL` (an existing file)
2. `~/.claude/models/{cf_embed_model_id()}.gguf`
3. `~/.claude/bin/{cf_embed_model_id()}.gguf`
4. `<mind>/../../models/{cf_embed_model_id()}.gguf`
5. OllamaYantra HTTP fallback if no usable local backend is available; it must match the compiled embedding identity.

There is no `--model` / `--vocab` flag and no ONNX model. The embed model is not fixed at `bge-base-en-v1.5` / 768 dimensions; the actual model and dimension are determined at build time via `cf_embed_model_id()`.

---

## Daemon Internals

**Thread pool:** Hard-coded 8 min / 16 max workers. Queue depth capped by `CHITTA_MAX_QUEUE_DEPTH` (default 256).

**Store backend:** chitta-field (Rust FFI via `FieldStore`). Not DuckDB.

**Socket and PID paths:** Determined by `socket_path_for_mind(mind_path)` and `pid_path_for_mind(mind_path)`. Not necessarily under `/tmp/`.

**Embedding queue:** Flushed approximately every 5s; backfill batch size is 100.

**Shutdown:** Daemon waits up to 30s for socket to disappear. SIGALRM fires after 15s if worker threads do not finish.

**Background threads:**
- Maintenance thread
- Backfill thread (embedding backfill, batch 100)
- Distillation thread
- Hint enrichment thread (trigger: 3 new memories, cooldown: 600s, batch: 50)
- Queue processor thread (reads `{mind}/queue.jsonl`)
- ThreadPool (8-16 workers)

---

## Startup, sidecars and instance lock

On the measured live store, startup is about **9.5 s** with matching derived-state
sidecars (`.lsh`, `.turbo`, `.organs`) and parallel snapshot decode, and about
**20 s** on the first start after deployment or a format change. These are
host/store measurements, not latency guarantees. The quantized index completes
on the maintenance thread after `ready`; allow 30 s before benchmarking recall.
Inspect `load phase=<name> ms=` and `cache hit=` in `chittad.log`. Overlapping
phase durations cannot be summed to obtain startup wall time.

The store holds `<mind>/chitta-field/.instance.lock`. A lock recording a dead
PID on this host can be replaced automatically. A live or other-host holder
must not be removed. If recovery still loops, the operator must establish that
no daemon owns the store before moving a stale lock aside. The service uses
`TimeoutStopSec=300` to let shutdown finish its snapshot. See the operational
reference in [CLAUDE.md](../CLAUDE.md).

## Troubleshooting

### "Yantra not attached"

The embed model could not be loaded.

```bash
# Check GGUF model files
ls -la ~/.claude/models/
ls -la ~/.claude/bin/*.gguf

# Check what model name the build expects
chittad format-id
```

### Socket connection failed

```bash
# Check daemon is running
chittad health

# Check PID
pgrep -f "chittad daemon"

# Restart
chittad shutdown
chittad daemon
```

### Daemon not responding after binary update

The daemon self-detects binary changes every 60s and restarts automatically if the vector-space id is compatible. If the id changed (incompatible embed model), the daemon will not auto-restart — run `re_embed` after manual restart.

### Store indices corrupted

```bash
chittad reindex
```

This rebuilds binary codes, coarse quantizer, LSH, and HNSW from existing embeddings without re-embedding.

<!-- BEGIN CITATIONS -->
## References

- <a id="ref-4"></a>**[4]** Yu. A. Malkov and D. A. Yashunin. Efficient and robust approximate nearest neighbor search using Hierarchical Navigable Small World graphs. IEEE TPAMI 42(4), 824–836 (2020); arXiv:1603.09320 (2016). [source](<https://arxiv.org/abs/1603.09320>) [source](<https://doi.org/10.1109/TPAMI.2018.2889473>)
- <a id="ref-73"></a>**[73]** Aristides Gionis, Piotr Indyk, and Rajeev Motwani. Similarity Search in High Dimensions via Hashing. VLDB, 518–529 (1999). [source](<https://www.vldb.org/conf/1999/P49.pdf>)
<!-- END CITATIONS -->
