# Startup: closing the gap between `ready` and `cf_startup_indexes_ready`

Status as of 2026-09-20: **canonical for the deferred startup phases.** No
snapshot format change. Supersedes nothing; extends the step 1a deferral
recorded in `DESIGN-2026-09-19-runtime-core.md`.

## What was measured before the change

Live daemon, clean restart at 06:23Z on a 141,226-memory store (~3.4 M triplet
entries, 808 MB triplet section):

- The deferred phases ran sequentially on the single `chitta-maint` thread, so
  the broad tool gate waited on their **sum**: triplets 6,217 ms,
  triplet_replication 301, symbols 25-777, span_store 376, hdc 351,
  event_tape_organs 6,769, episode_hdc 25, keyword_reverse 1,106, turbo 611.
- `event tape organs cache hit=false` on every restart. The cache key was the
  whole event tape, and every live WAL tail carries tape events, so the key
  could never match in production.
- `.sup sidecar: 0 supersessions, 3407430 ingestion times` cost ~3 s **on the
  open path**, between the `.pld` load and the embeddings.
- `TripletStore::rebuild_indexes` built four derived maps in one sequential
  pass: 1.9 s on the frozen replica, 6.2 s live.

## Decisions

1. **Three lanes, not one thread.** `cf_open`'s maintenance closure runs the
   gate's phases as three independent lanes: triplets then the replication
   counts derived from them; the event-tape organs then the episode store that
   shares their loader; the small indexes. Order inside a lane is the only
   dependency, and nothing crosses lanes, so the gate opens on the slowest lane.
   `keyword_reverse` and `turbo` are not in the gate and stay on the maintenance
   thread after it. Cancellation still detaches rather than joins.
2. **The organs cache carries its cut.** The file is keyed on a format constant
   and stores the `TapeCut` it was built from. An open verifies that cut is
   still a prefix of the current tape and applies only the events after it.
   Both organs are pure appends over the tape and the CDAWG Q-seed is one
   whole-automaton pass, so prefix-plus-tail equals a full rebuild exactly. An
   in-place edit or a removal inside the prefix still forces a rebuild.
3. **The `.sup` sidecar is triplet state.** It loads inside the deferred triplet
   job, first, ahead of the replay drain and `clean_for_load` — the order the
   open path had. Nothing before that job reads `ingestion_times` or
   `supersession_map`. Sidecar entries no longer overwrite in-memory ones, so an
   eager open (whose loader runs after replay) keeps the replayed value.
4. **`rebuild_indexes` is parallel.** `id_to_index`, `by_subject`, `by_object`
   and `by_predicate` are built on four rayon tasks over the same slice. Each
   walks in entry order, so posting lists keep their order and `duplicate_ids`
   is unchanged; a parity test against the old single loop covers it.

## Measurement

`scripts/eval-replica.sh` on the frozen 2026-09-15 cut, on dandycomp01fl, four
cells: `{before, after} x {empty tail, live-shaped tail}`, three runs each. The
tail is 300 `connect` plus 50 `remember`, killed with SIGKILL so the WAL keeps
it. Numbers are in `/projects/caeg/scratch/kbd606/tmp/st25/measure.log`, with a
per-run daemon log next to it.

## Measured, frozen 2026-09-15 cut, dandycomp01fl, min/mean/max of 3

| cell | first correct recall (s) | broad gate open (s) |
|---|---|---|
| before, empty tail | 7.13 / 7.16 / 7.20 | 9.19 / 9.21 / 9.25 |
| after, empty tail  | 6.87 / 6.88 / 6.89 | 7.93 / 8.28 / 8.96 |
| before, live tail  | 6.06 / 6.40 / 7.08 | 8.12 / 8.14 / 8.16 |
| after, live tail   | 5.67 / 5.74 / 5.78 | 6.81 / 7.47 / 7.86 |

Every cell returned the same 909-character recall answer, so the change is
answer-neutral. The gate's phase budget went from a 2,435 ms sequential sum to
a 1,958-2,310 ms critical path, which is the triplet lane; `field_store` fell
from 5,488 ms to 5,114 ms, the `.sup` parse leaving the open path.

## Correction to the premise, and what is still open

**WAL replay never appends to the event tape.** A tail of 300 `connect` plus 50
`remember`, killed with SIGKILL, left the tape byte-identical to the snapshot's:
the pre-change binary reported `cache hit=true` on that tail, and the new one
reports `tail_events=0`. The tape is snapshot-only state, so decision 2 is
correct and answer-neutral but cannot yet pay off on the open path.

The live `hit=false` lines are **the save path**, not restarts. In
`chittad.log` on 2026-09-20 the three restarts logged `event_tape_organs`
6,769 ms `hit=false` (07:19), 326 ms `hit=true` (08:13) and 300 ms `hit=true`
(08:22); every other `hit=false` line sits next to a `[checkpoint]` line.
`store/maintenance.rs` calls `load_or_rebuild_organs` with the **new** family's
path, which never exists yet, so every checkpoint rebuilds the organs from
scratch on the maintenance thread — about once every 8-11 minutes.

Two follow-ups, both in the save path, which this pass does not own:

1. Pass the previously committed family's `.organs` to the save-path call so a
   checkpoint resumes from its cut instead of rebuilding. Decision 2 already
   provides the mechanism.
2. `rebuild_event_organs` derives the organs path from `best_full_path`, while
   the `.sup` sidecar uses the candidate that actually loaded. When a
   manifest-committed or stale family wins, those differ and the organs read
   points at another family's file. That is the likeliest cause of the 07:19
   restart miss, and it is one line in `field/opening.rs`.

## Turbo first, and a search that waits for it (2026-09-20, later)

Status as of 2026-09-20: **canonical for the deferred Turbo load.**

A stack capture at 09:02Z showed three recall threads inside
`SemanticIndex::search`, on no lock. The flat scan's raw turbovec arm found
`turbo` still `None` and fell through to the scalar loop over `all_ids()`:
141,613 vectors, 14 s under load with three concurrent callers. The Turbo load
was the last deferred job, after the three gate lanes and the keyword reverse
index, so `ready` marked the start of that window: it published at 09:02:39
against a ready at 09:02:28, and recalls issued at ready ended at 09:02:47.

1. **Turbo is its own lane, scheduled first.** It is in no gate and shares no
   state with the other lanes. The keyword reverse index stays on the
   maintenance thread, because it owns the stop receiver the lanes cannot share
   and nothing waits on it.
2. **Search waits rather than scans.** A `TurboPending` signal is armed at open
   when the load is deferred and cleared the moment the index publishes. The raw
   arm parks on it for at most `CHITTA_TURBO_WAIT_MS` (default 5000) and then
   proceeds with whatever is published, logging
   `[hnsw] search waited_ms=N for turbo published=...`. The signal clears from a
   guard's `Drop`, so a cancelled or failed startup releases waiters instead of
   making them serve out the budget.

Two lock facts this rests on. The caller's `semantic_idx` read guard is held
across the wait exactly as it was held across the scan, and for less time;
nothing on the publication path needs that lock, and `prune_turbo_changes`,
which does, runs after the signal clears. And the wait must not sit behind a
`match` scrutinee: that keeps the `turbo` read guard alive for the whole arm,
and the recursive read inside the wait then queues behind the publisher's
pending write. The first version did exactly that and deadlocked under the new
test.

### Measured, frozen 2026-09-15 cut, dandycomp01fl, mean of 3

| metric | before | after |
|---|---|---|
| Turbo published, after `ready` | 16.3 s | 4.0 s |
| Broad tool gate open, from process start | 13.87 s | 14.30 s |
| Deferred gate phases, critical path | 7,198 ms | 7,838 ms |
| `triplets` phase | 1,936 ms | 3,499 ms |
| First non-loading recall | 6.80 s | 7.21 s |
| Three concurrent recalls at `ready`, slowest | 0.16 s | 0.15 s |

Turbo publishes about twelve seconds earlier. It costs about 0.4 s on the gate:
the Turbo build saturates its own rayon pool, which slows the triplet lane from
1.9 s to 3.5 s, though the organs lane still sets the critical path.

**The replica cannot measure the recall side.** Three concurrent recalls fired
at `ready` take about 0.15 s in both cells, and the wait never fires. A trace
at the top of `SemanticIndex::search` showed the `chitta recall` tool never
reaches it on this replica, with no realm, with a realm, and with learning off;
the live 09:02Z stack capture shows three request threads inside it, so the
live request shape differs from the CLI's. What is verified here is the
mechanism, by unit test (a search parks until publication and then matches the
Turbo path; a search gives up at its bound and still answers), and the twelve
seconds earlier publication. The end-to-end recall win is unmeasured on the
replica and has to be read from the live daemon after deploy.
