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
