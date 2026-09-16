# Code navigation benchmark

Twenty navigation questions were fixed before measuring retrieval. Every answer
was verified by reading the named source at parent commit `18444e02` and store
commit `089a056`. The question JSON is the source of truth; paths are relative
to the active checkout. The store-lock, recall-lane caller, task-ledger writer,
daemon exit-75 and queue-ack questions are included explicitly.

Run against an already indexed **private scratch daemon**:

```bash
python3 benchmarks/codenav/run.py --socket /tmp/private/run/chitta/example.sock \
  --root /path/to/worktree --output /tmp/codenav-result.json
```

Answers come exclusively from `code_query(question, path, limit=8)`: up to eight
ranked matches plus eight graph neighbors. A hit requires both the exact file
and symbol (an enclosing namespace/class prefix is permitted). Ground truth is
never supplied to `code_query`; file contents and `read_symbol` never select or
rank the answers. Five repetitions per question give 100 RPC wall-time samples;
p95 uses the nearest-rank definition. Repeated responses must be identical.

The independent token proxy assumes a fresh task for every question. Without
the graph, it counts a full Read of the ground-truth file. With the graph, it
counts the query text, that file's pre-read symbol block (`limit=20`) including
its truncation notice, and the answer's `read_symbol` body. **Misses still pay the ground-truth body cost**; this
cost accounting does not change their failed accuracy score. The full symbol
block is charged even if the hook byte cap would remove lines, so the graph
cost is conservative. It measures bytes
presented to an agent, not daemon filesystem I/O, tokenization or an LLM trial.

Acceptance is at least 18/20 hits, p95 at most 300 ms, and at least 50% fewer
bytes. The runner exits nonzero when any gate fails. Save output outside this
package; results and logs are not committed. To verify restart determinism,
compare `query_responses` from two runs separated by a scratch-daemon restart,
without reindexing or changing source files.

This package is frozen by `benchmarks/EVAL_IMMUTABLE.txt`. Any change requires
an explicit `Eval-Change-Approved: ...` commit trailer under the same convention
as `benchmarks/current_truth`. Never change questions or thresholds to turn a
failed run into a passing one.
