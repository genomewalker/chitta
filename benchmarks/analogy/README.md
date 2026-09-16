# Explicit relation-transfer evaluation (2026-09-15)

`recall_analogy` accepts `a`, `b`, `c`, optional `mode=proportional`, and `limit`
(default 8, bounded 1–100). Entity and predicate symbols are exact and case-sensitive.
It transfers the union of the stored directed predicates linking a→b to outgoing
edges of c. Distinct answers rank by strongest edge weight, then edge start time
(newest first), then deterministic symbol/ID ties. `score` is edge weight, not
VSA [10](#ref-10) similarity or a probability. Structural mode is rejected with an explanation.
The HDC organ remains available to other users.

Top-level `mode`, `indexed`, and `results` remain. `indexed` reports organ entry
count, including expired entries, and is never a lookup bound. New `reason` is
null for hits, `no_source_relation` if a→b has no valid edge, or
`no_target_relation` if c has no outgoing edge under those predicates.
`relations` contains the source a→b edges. Each result has `answer`, `predicate`,
`id`, `score`, `realm`, `text`, and `edges`: all supporting c→answer edges with
`subject`, `predicate`, `object`, nullable `memory_id`, `triplet_id`, `weight`,
and `valid_from_ms`. `id=0` means the strongest edge has no source memory;
the rendered response never credits a fake #0. Missing payload text does not
invalidate an explicitly stored edge.

## Frozen experiment

Use a private copy of the selector-validated eval replica family; never open the
shared store with a second process. Follow `scripts/eval-replica.sh` copy rules,
including manifests, selected sidecars, selected WALs and migration markers.
Use a private daemon path, runtime directory and port, `CHITTA_NO_QUEUE=1`, and
quiesce/disable autonomous work. No facts are planted or repaired.

```bash
export CHITTA_EVAL_SOCKET=/path/to/private/replica.sock
python3 benchmarks/analogy/baseline.py --snapshot-id bbcaed33
python3 benchmarks/analogy/run.py --snapshot-id bbcaed33 --transport rpc
```

`tasks.json` retains the original 14 proportional queries and original grounding
labels. Structural relevance tasks are removed. `baseline.py` uses the separate
indexed graph APIs to enumerate **all** valid answers before analogy evaluation;
it cross-checks ordinary graph edges against the unbounded temporal export to
detect truncation or validity discrepancies. No analogy implementation is imported.
It freezes 14 negative queries whose existing target has other outgoing edges but
none with the required relation. The full baseline and negative targets are in
`baseline.json`, linked by task, snapshot, socket, and SHA-256 in `results.json`.

Scoring uses complete exact answer sets, checks every returned target edge and
memory citation, and counts missing grounding and service errors as failures.
Negative correctness requires empty results with `no_target_relation`; errors
are never abstentions. Baseline timing includes two graph RPCs and the Python join;
endpoint timing includes one RPC (2 s deadline). Report missing grounding separately.
Keep iff hit@3 ≥12/14, negative abstentions 14/14, and unsupported answers zero.
Historical `results-rpc.json`, `coverage.json` describe the retired experiment.

## Decision: keep

Frozen snapshot bbcaed33, manifest generation 38047; no grounding changes.

| Metric | Exact join baseline | Proportional RPC |
|---|---:|---:|
| hit@1 | 14/14 | 14/14 |
| hit@3 | 14/14 | 14/14 |
| Negative abstentions | 14/14 | 14/14 |
| Unsupported answers | 0 | 0 |
| Missing grounding | 0 | 0 |
| Median latency | 1.627 ms | 0.333 ms |

Baseline latency covers 14 positive queries, each with two graph RPCs; endpoint
latency covers all 28 positive/negative calls. Both exclude daemon startup.
The baseline validates exact subject fields because legacy duplicate triplet IDs
can resolve an indexed subject entry to another subject. The endpoint has the
same explicit subject guard; the triplet organ is unchanged. All negative
argument triples are distinct, with target selection completed before scoring.
This meets the memo's keep thresholds on this small screening set; it does not
establish broad analogy performance.

Validation: `python3 -m unittest discover -s benchmarks/analogy -p 'test_*.py'`
and `ruff check benchmarks/analogy/*.py`.

<!-- BEGIN CITATIONS -->
## References

- <a id="ref-10"></a>**[10]** Pentti Kanerva. Hyperdimensional Computing: An Introduction to Computing in Distributed Representation with High-Dimensional Random Vectors. Cognitive Computation 1, 139–159 (2009). [source](<https://doi.org/10.1007/s12559-009-9009-8>)
<!-- END CITATIONS -->
