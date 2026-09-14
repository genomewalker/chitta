# Analogy smoke evaluation — 2026-09-14

Twenty handwritten tasks: 14 proportional and six structural. `tasks.json`
contains the live graph facts, expected answer strings or memory IDs, and the
structural probe/answer contents. Labels were fixed before retrieval was run.
Exact-subject/object filtering removes unrelated rows returned by query_graph.
Where a target has several verified objects for the same relation, any is valid.
The structural pairs are related memories sharing outgoing cites/tagged relations;
they are human relevance labels, not exhaustive graph-isomorphism labels.
Several tasks reverse the same analogy: these 20 scores are correlated smoke
checks, not an estimate of broad analogy competence.

```bash
bash scripts/eval-replica.sh status
export CHITTA_EVAL_SOCKET=/projects/caeg/scratch/kbd606/tmp/chitta-eval-mind/run/chitta/chitta-2511933830.sock
python3 benchmarks/analogy/run.py --snapshot-id bbcaed33
```

The default invokes `chitta recall_analogy --json` with explicit socket routing,
closed stdin, limit=3, and a two-second cap per query. It never selects a live
socket implicitly. Exact answer matching ignores case and surrounding whitespace;
ID matching preserves all uint64 digits. Errors, malformed responses, and timeouts
count as misses. Median latency includes every attempt, including CLI startup.
Task SHA-256, timestamp, socket, snapshot, per-style scores, and per-task results
are written to `results.json`. `CHITTA_BIN` or `--cli` selects an existing CLI.

The installed CLI rejects recall_analogy in its static command allowlist. Therefore
all 20 default CLI attempts failed before retrieval. A separately labelled RPC
diagnostic bypasses that client-side issue without changing the CLI or daemon:

```bash
python3 benchmarks/analogy/run.py --snapshot-id bbcaed33 --transport rpc \
  --output benchmarks/analogy/results-rpc.json
```

RPC sends only tools/call(recall_analogy) to the explicit Unix socket, with a
2-second deadline and no daemon startup behavior. Its latency excludes CLI startup.
The frozen replica returned zero hits at both ranks: 10 proportional calls returned
unrelated candidates, four returned no candidates, and all six structural probes
reported the lane unavailable. `coverage.json` independently verifies that all
proportional grounding facts and all structural probe/expected memories exist in
snapshot bbcaed33 (manifest generation 38047).

**The analogy lane is not ready for a hook (hit@3 = 0.00 < 0.30).** No lane tuning
was performed. See results files for exact timings and errors. Regression checks:
`bash hooks/tests/test_analogy_eval.sh`.
