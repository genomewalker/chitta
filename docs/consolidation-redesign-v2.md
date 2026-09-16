# Consolidation/recall stall — corrected analysis (v2, supersedes v1 premise)

> **Deprecated in place. Status as of 2026-09-16.** Historical lock analysis; current read-path and startup fixes are in [FIELD_PERF.md](FIELD_PERF.md).
> At the time this superseded
> [consolidation-redesign.md](consolidation-redesign.md), which targets the wrong
> layer. Line references below point at the tree as of 2026-06-02 and may have
> drifted; the argument has not been re-verified against the current tree.

**v1 (`consolidation-redesign.md`) is wrong about the mechanism.** It assumed recall
stalls because consolidation holds Rust locks (`event_tape.write()`, and by implication
`semantic_idx`) that recall needs, and prescribed an `ArcSwap<Snapshot>` rebuild of
`semantic_idx` (steps 4–7). Reading the actual code (2026-06-02) disproves the premise.
Do **not** build steps 4–7 as written — they target the wrong layer.

## The airtight invariant

The C++ RPC layer (`field_handler.hpp:284-336`) dispatches every tool through one
`std::shared_mutex rpc_mutex_` in three categories:

| Category | Lock taken | Examples |
|---|---|---|
| `is_read_only_tool` | **shared** (`acquire_shared_lock`) | `recall`, `search_memories`, `code_context`, `msg_inbox` |
| `is_subprocess_tool` | **none** | `consolidation_pass`, `predicate_run` |
| (default = write) | **exclusive** (`unique_lock`) | `log_event`, `observe`, `remember`, `connect`, … |

Plus two more exclusive holders outside `handle()`:
- `queue_processor.cpp:157-160` — holds `handler_.acquire_lock()` (**exclusive**) across the
  *entire* handler body of each queued write (every tool except `distill_trigger`).
- `subconscious.cpp:105-106` `write_lock()` — exclusive, for belief-maintenance `remember`s.

**Therefore:** `recall` (shared) can be blocked **only** by an *exclusive* holder, and every
exclusive holder is **write-side**. `consolidation_pass` is `is_subprocess` → holds **no**
`rpc_mutex` → **cannot directly block recall**. Whatever stalls recall is a write op holding
the exclusive `rpc_mutex` for too long.

## What actually stalled recall on 2026-06-02

Observed in `/tmp/chittad.log` after a restart, under load from several concurrent agent
sessions:
```
[watchdog] CRITICAL: log_event stuck for 32s   ← write tool, holds EXCLUSIVE rpc_mutex
[watchdog] CRITICAL: recall stuck for 31s       ← shared, queued behind it
decode: cannot decode batches with this context (calling encode() instead)   ← bge embedder
```
The dominant cost was **embedder saturation**: the backfill thread grinding a post-restart
write backlog through bge, each item hitting the slow `decode→encode` fallback, pegging CPU
and the embedder. A concurrent write tool (`log_event`) then held the exclusive `rpc_mutex`
for tens of seconds (blocked on the embedder / a Rust lock), and recall — shared — queued
behind it. When the backlog drained, recall returned to ~0.5 s. Disabling consolidation
(`CHITTA_DISABLE_CONSOLIDATION=1`) helped only **indirectly** (less CPU/OpLog competition for
the embedder); consolidation never held the lock recall needs.

Ruled out by code:
- `add_triplet` (store.rs:2894) locks only `triplet_store.write()` + `log.write()` — **not**
  `semantic_idx`. `OpLog::append` (log.rs:224) is a buffered write with batched `fdatasync`
  every 32 ops (microseconds). Neither explains a 30 s hold.
- `recall_semantic_ctx` (store.rs:1187) reads `semantic_idx/states/payloads/scoring/learners/
  ack_scores` — **none** of the locks consolidation writes.

## The one genuine lock-stall — found and FIXED

`save_full_snapshot` (store.rs:5274) held `semantic_idx.write()` across six ~600 MB sidecar
disk writes while recall needs `semantic_idx.read()`. **Fixed** (chitta-field `0d3f186`):
merge delta under a brief write, then save sidecars lock-free from the existing
`snap.semantic_idx` clone. **Verified**: a real snapshot (578 MB `.emb` + 2.6 GB snapshot,
`encoded=5281`) ran while recall latency stayed 0.5 s flat (max 1.05 s) across the window.

## Corrected fix targets (replace the semantic_idx ArcSwap plan)

The lever is the **exclusive `rpc_mutex` write-hold**, not `semantic_idx`:

1. **Profile first (do this before any rearchitecture).** Add hold-time instrumentation
   around the exclusive `rpc_mutex` acquisition in `field_handler.hpp` and
   `queue_processor.cpp`: log `tool, held_ms` for any exclusive hold > 250 ms. This pins the
   *actual* long holders empirically instead of guessing. One pass under real multi-session
   load settles the mechanism.
2. **Never hold the exclusive `rpc_mutex` across embedding.** Confirm/enforce that no write
   path embeds synchronously inside the lock (write tools already pass empty embeddings and
   let the backfill thread embed — `field_handler.hpp:301-304`; audit `log_event` and the
   `queue_processor` write bodies for any in-lock embed).
3. **Shorten the queue-processor hold.** `queue_processor.cpp:157` holds the exclusive lock
   across the whole handler per item; with a write backlog this is a sustained recall outage.
   Acquire per-write, briefly, around only the Rust mutation — or move writes to the
   `is_subprocess` discipline (own Rust RwLocks, no `rpc_mutex`), which `predicate_run` and
   `consolidation_pass` already use safely.
4. **Embedder robustness.** The `cannot decode batches with this context` fallback is the
   real latency source under load. Clamp/segment batches to `n_ctx`, cap backfill batch size,
   and rate-limit backfill so it can't peg the embedder while writes hold the lock.
5. **Consolidation re-enable** then becomes safe *once writes no longer hold the exclusive
   lock across slow work* — because consolidation never touched recall's locks to begin with.
   The `ArcSwap<semantic_idx>` rebuild (v1 steps 4–7) is unnecessary for this goal.

## SLO gate (unchanged from v1, still correct)
p99 recall latency *during* a consolidation pass + a snapshot + a write backlog, under
concurrent multi-session load. Lock-hold time alone is not enough — instrument both.

## Status
- Snapshot lock-stall: **fixed + verified** (`0d3f186`).
- Consolidation: disabled via `CHITTA_DISABLE_CONSOLIDATION=1` drop-in (stopgap) — but note
  it was never the direct recall blocker.
- Next concrete step: ship the `rpc_mutex` hold-time instrumentation (#1), run under load,
  then do the targeted write-path fix the profile points to. No `semantic_idx` rearchitecture.
