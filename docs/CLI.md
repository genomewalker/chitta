# chitta CLI Reference

`chittad` is the daemon binary (memory server, background processing, Unix socket listener).
The `chitta` client binary is a separate tool not covered by `simple_cli.cpp`; this document covers `chittad` commands only.

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
- [Environment Variables](#environment-variables)
- [Embed Model Resolution](#embed-model-resolution)
- [Daemon Internals](#daemon-internals)
- [Troubleshooting](#troubleshooting)

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
| `--distill-interval SECS` | Distillation check interval (seconds) | `900` (15 min) |
| `--distill-min-turns N` | Minimum turns before distillation fires | — |
| `--distill-script PATH` | Custom distillation script | — |
| `--distill-model MODEL` | Model for distillation | `gemma4:26b` |
| `--distill-token-trigger N` | Char count to trigger distillation (0=off) | `0` |
| `--distill-cooldown SECS` | Minimum seconds between distillations | `180` |
| `--distill-max-tokens N` | Max tokens for distillation context | `8192` |
| `--distill-context-chars N` | Max chars of context fed to distiller (0=unlimited) | `0` |
| `--no-distill` | Disable automatic distillation | Off |
| `--embed-model MODEL` | GGUF embed model name | Auto (see below) |
| `--no-enrich` | Disable enrichment | Off |
| `--no-hygiene` | Disable hygiene and sleep consolidation | Off |
| `--no-autonomous` | Disable dream/think/belief-maintenance callbacks | Off |
| `--merge-policy POLICY` | Memory merge policy (`off` or `merge_aware`) | — |
| `--embed-interval SECS` | Enable background embedding with this interval | Off |
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
- Runs hint enrichment thread (trigger: 3 new memories; cooldown: 600s; batch: 50); binary via `CHITTA_HINT_ENRICHER` env or smart-install default
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

Rebuild ANN indices (binary codes, coarse quantizer, LSH, HNSW) from current in-store embeddings. No embed model required.

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

1. `~/.claude/models/{cf_embed_model_id()}.gguf`
2. `~/.claude/bin/{model}.gguf`
3. OllamaYantra HTTP (fallback if no GGUF found)

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
