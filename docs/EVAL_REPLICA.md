# Frozen eval replica

Status as of 2026-09-13.

Recall grading and SMRITI used to share the live daemon with distillation,
backfill, consolidation, and other maintenance. That made a measurement depend
on when it ran; `grade-recall.sh` has observed maintenance-driven swings of
about ±0.027 nDCG. The eval replica separates the online store from the
analytical workload by loading a copied, committed checkpoint in a second
daemon.

## Start and use the replica

```bash
scripts/eval-replica.sh start
source /projects/caeg/scratch/kbd606/tmp/chitta-eval-mind/replica.env
python3 hooks/grade-recall.py --quiet
python3 benchmarks/smriti/runner.py --help
scripts/eval-replica.sh status
scripts/eval-replica.sh snapshot-id
scripts/eval-replica.sh stop
```

`CHITTA_EVAL_MIND` changes the replica directory and `CHITTA_EVAL_PORT`
changes its HTTP RPC port (default 7433). `CHITTA_LIVE_MIND` exists for tests
and controlled snapshot sources; its default remains `~/.claude/mind`.
`replica.env` exports the Unix socket, snapshot id and seqno, port, mind path,
and pid. The grader records `replica_snapshot_id` and `replica_socket` in
`hooks/grade-recall-results.json`.

The start command is idempotent while its recorded daemon is running. It
selects the highest-seqno family whose snapshot and every manifest-recorded
sidecar exist at the committed sizes. A matching `cortex.<id>.snapshot` is
copied when present; it is an optional cache and is not part of the manifest's
commit record. The source manifests are fingerprinted before and after the
copy, and current manifest/family temp files cause a refusal. Old temp files
from unrelated, dead writers do not permanently prevent evaluation.

The daemon gets its own Unix-socket directory below the replica mind and a
separate HTTP port. Automatic distillation, enrichment, hygiene, background
embedding, autonomous work, and shared queue consumption are disabled. A
replica-local quiesce flag suppresses the remaining periodic maintenance
passes. `stop` validates the pid's command line and sends SIGTERM only to that
process.

## Limits

- The copy contains one committed snapshot family and its manifest, not live
  WAL segments. It therefore represents the snapshot seqno, not writes made
  after that checkpoint. The replica may create its own local WAL after it is
  opened; those files are never copied back.
- Direct grader and SMRITI adapter CLI calls honor `CHITTA_EVAL_SOCKET` today.
  The SMRITI runner also passes `CHITTA_SOCKET_PATH=<socket>` into the
  `claude -p` child. Hooks inside that child target the replica only after the
  separate CLI change that makes `chitta` honor `CHITTA_SOCKET_PATH` lands.
- The grader's graph-expansion helper uses the replica HTTP port. Source
  `replica.env` (or set `CHITTA_EVAL_PORT`) when using graph-based strategies
  with a non-default replica port.
- A frozen replica is disposable analytical state. Planting SMRITI memories or
  running the wrapper's provenance write changes only the replica and should
  be followed by a fresh `start` when an identical checkpoint is required.
- The daemon's quiesce flag has a 30-minute safety TTL. Restart the replica
  before a longer measurement rather than allowing periodic maintenance to
  resume.
