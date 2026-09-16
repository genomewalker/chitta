# Current repository truth (2026-09-16)

The 50 questions were manually authored from repository sources at
`1c82589b93bbe350319678db66a9d720385cdc4b`, before inspecting recall. Every answer
has a pinned file:line or heading citation. The procedural split is 30 visible /
20 holdout (24/16 answerable, 6/4 abstentions). Holdout files are public for
review; do not use their outcomes to tune retrieval or matchers.

```
python3 benchmarks/current_truth/run.py --dry-run
python3 -m unittest discover -s benchmarks/current_truth
# Export the private replica.env from scripts/eval-replica.sh first.
python3 benchmarks/current_truth/run.py --output /tmp/current-truth.json
bash scripts/eval-noise.sh --current-truth-only --current-truth-runs 3 \
  --output /tmp/current-truth-noise.json
python3 benchmarks/noise.py band current_truth.p3 --file /tmp/current-truth-noise.json
```

Recall uses project:cc-soul, fused strategy, limit 3, JSON, and --no-learn. The
runner requires CHITTA_EVAL_SOCKET inside CHITTA_EVAL_MIND, resolves symlinks,
and verifies the actual Linux listener PID and its --path match the eval mind.
It rejects live endpoints unless --live is explicit. --live produces
smoke observations only. No missing environment fallback, daemon autostart,
answer seeding, gap writes, or LLM scoring. Errors abort rather than score zero.

Here **precision@3 (p3)** follows the requested any-top-three success rule: the
fraction of the 40 answerable questions with at least one matching hit. This is
normally called hit@3, not the fraction of three returned documents relevant.
Abstention accuracy is separate: a question expecting "not recorded" succeeds
unless a predeclared wrong-answer trap matches. Empty retrieval counts as
abstention, not as an answer to an answerable question. Trap matches take
precedence over correct matches. The supplementary utility is correct answers
plus correct abstentions minus twice the wrong-confident count (misses cost 0).
Overall correct / 50 includes abstentions; per-split denominators are explicit.

Exact matchers are case-sensitive literal substrings, regex matchers search a
single result body, and ID matchers compare decimal strings without float
conversion. Numeric answers use contextual regexes to avoid incidental numbers.
These deterministic proxies cannot judge negation or arbitrary paraphrases.
Abstention is trap avoidance, not proof the retriever recognized missing truth.

F2 of the pinned robustness decision contains no original five prompts. The
five named fixtures are explicitly **reconstructed**, not the historical probe:
cache TTL variable, prompt median, SessionStart median, cached startup, and
install over a running binary. Original prompts remain requested. No result
from these fixtures can certify the historical 5/5 exit gate.

The new panel, scorer, tests, protocol, and dated baseline are frozen in
EVAL_IMMUTABLE.txt. Existing noise.json is protected and unchanged; select the
dated baseline with --file. Recalibrate when the binary, snapshot, panel,
strategy, depth, reranker or scoring configuration changes. Three-run 2-SD bands
are descriptive; zero SD does not establish equivalence or generalization.

## Provenance census

```
python3 scripts/provenance-coverage.py --socket /explicit/live/socket \
  --output /tmp/provenance-coverage.json
```

The audit invokes only list_memories_brief, memory_provenance and query_graph
through one thin CLI process. It never opens a store or uses recall. Pagination
continues to an empty page because the server caps pages at 100. Each observed
ID is counted once; JSON numbers retain full 64-bit IDs. The seven-day window
uses creation timestamps at audit start, not access or strengthening time.
Missing timestamps and timestamps after the audit clock are reported separately.

A nonempty source label identifies a writer and qualifies as source evidence;
it never invents a session. Numeric source/derived_from references qualify only
when the referenced memory is present in the enumerated set. Explicit sessions
and source paths/URLs also qualify. Multiple writer labels form one sorted
combined bucket, and unlabeled memories remain unknown. No content heuristics.

The current public metadata API omits direct source_session, although it is
stored internally. Consequently the live results are **observable-provenance
lower bounds**; zero observable session triplets cannot mean zero stored
sessions. Exact union coverage and comparison with the historical offline
1,236-session audit require an API exposing that field, outside this phase's
write scope. Live enumeration and per-memory reads are an interval census,
not an atomic snapshot; concurrent writes can affect its population.

The dated baseline is a warm calibration. To reproduce its conditioning on a
fresh copy of the same source family, run the truth-only three-pass command
once to `/tmp/current-truth-warmup.json`, then run it again to a separate report.
Keep the first report too: the initial 9/40,10/40,10/40 observations show why cold
and warm samples must not be silently mixed. The frozen final report contains
only the second three passes; docs/EVALS.md records both sets of observations.
