# Consolidation Redesign — stall-free memory consolidation

> **Deprecated in place. Status as of 2026-09-16.** Historical rejected proposal; current implementation and measurements: [FIELD_PERF.md](FIELD_PERF.md).
> The premise of this document is wrong.
> It assumed recall stalls because consolidation holds Rust locks that recall
> needs. Reading the actual code disproved that; the contention is at the C++ RPC
> layer, not in the Rust store. Read
> [consolidation-redesign-v2.md](consolidation-redesign-v2.md) instead, and do
> not build steps 4–7 of this document as written. Kept for the record of how
> the analysis went wrong.

Design from a multi-model room (`gpt-5.5:xhigh` + `opus-4.8:xhigh`, room `room-f3f2e863`).

## Problem

`consolidation_pass()` (Sequitur [25](#ref-25) grammar induction + FEP rebuild + Phase-12
`compress_low_surprisal` + cortical encode + snapshot) ran a ~30 s pass that held
locks the recall path also needs. parking_lot is **writer-fair**: once a writer
queues (Phase 12's `event_tape.write()`, or a queued `log_event` tape writer), every
new reader (recall) blocks behind it. One long write window → a full read outage.
Watchdog: `consolidation_pass stuck 35s`. Stopgap was `--no-hygiene` (consolidation
off) — unacceptable long-term.

## Target architecture (RCU-style immutable snapshots + atomic publish)

- **One recall root:** `static STATE: ArcSwap<Snapshot>` bundling per-field `Arc`s:
  ```
  Snapshot { epoch: u64, tape: Arc<TapeView>, derived: Arc<DerivedState>,
             semantic: Arc<AnnTier>, cortical: Arc<CorticalIndex> }
  ```
  Recall does **one** `STATE.load_full()` per query → consistent version N or N+1,
  never blocks, never tears. Unchanged fields are structurally shared (zero-copy).
- **Append-only sealed/tail tape:** writers append to a committed-mini-segment tail
  (single-writer + per-segment WAL/fsync). Consolidation consumes only the immutable
  sealed prefix `[0,N)`; writes during a pass land in `[N,N+K)`, folded next pass —
  the "reconcile against a moving target" problem disappears.
- **Phase 12 emits a delta, never mutates the tape in place** — `compress_low_surprisal`
  runs off the snapshot and produces tombstones keyed by stable ids, applied at swap.
- **Bundle mutually-consistent derived structures** (triplet_store, fep_prior,
  refutation_ledger, hypothesis_market) into one atomic publish (no torn reads).
- **Two-tier ANN for `semantic_idx`:** immutable base + small mutable overlay merged at
  query time; consolidation folds + swaps. `left-right` rejected here (doubles the
  ~600 MB matrix); reserved for small incremental maps.
- **Mandatory janitor thread:** swapper hands the old `Arc` to a reclaimer via `mpsc`;
  only it drops multi-GB structures. Otherwise the last recall thread out runs `Drop`
  on the read path — the stall, relocated and nondeterministic.
- **Crash-consistent:** `ConsolidationArtifact { base_epoch, end_lsn, tape_delta,
  derived_state, index_ops }` fsync'd to a manifest before the in-memory swap; recovery
  replays WAL from `base_lsn` + applies only complete manifests.
- **Rejected:** chunking/yield (band-aid vs writer starvation), seqlocks (wrong for
  GB pointer state), fork-CoW (multithreaded-fork footgun + IPC kills zero-copy Arc).

## Migration (each step independently shippable; watchdog held-lock-time is the gate)

1. Wrap structures in `ArcSwap`, route recall through `.load_full()`, **stand up the
   janitor reclaimer**. Writers still clone-mutate-swap under the old mutex.
2. Sealed/tail tape split with continuous sealer + per-segment WAL.
3. **Phase 12 → off-lock compute + brief apply.** ⟵ kills the dominant 13–35 s stall.
   **[DONE]** see `EventTape::compute_low_surprisal_removals` / `apply_removals` and
   `Store::consolidation_pass` Phase 12: snapshot tape+cdawg under one brief read,
   compute the removal mask off-lock, apply under a short write. (Interim form of the
   full delta-swap; no `event_tape.write()` held across the cdawg sweep.)
4. Collapse to a single `ArcSwap<Snapshot>` with bundled `DerivedState`.
5. Two-tier ANN for `semantic_idx`.
6. `left-right` for the small incremental structures.
7. Delete `--no-hygiene`.

## SLO gates

- Per-pass held-lock time → sub-ms by step 3.
- p99 recall latency *during a pass* (catches the reclamation trap — invisible to
  lock-time alone).

## Open questions (from the room)

- Mini-segment commit cadence (per event / per N ms / per K events).
- Write-epoch API surface (block until published vs return epoch + poll).
- Sequitur memory ceiling (`catch_unwind` + bounded arena — cap unspecified).
- Cortical index shape (sparse map → `left-right`, or graph → ANN treatment).
- Overlay→base fold trigger (time / size / coupled to pass).
- Manifest format + cross-version compatibility for partial-manifest replay.

<!-- BEGIN CITATIONS -->
## References

- <a id="ref-25"></a>**[25]** Craig G. Nevill-Manning and Ian H. Witten. Identifying Hierarchical Structure in Sequences: A linear-time algorithm. Journal of Artificial Intelligence Research 7, 67–82 (1997); arXiv:cs/9709102. [source](<https://arxiv.org/abs/cs/9709102>)
<!-- END CITATIONS -->
