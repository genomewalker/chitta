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
and rejects known live endpoints unless --live is explicit. --live produces
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
