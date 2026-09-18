# Distiller SSL measurement and default

Status: 2026-09-18 — ablation running; thinking remains enabled by default.

The p13 harness freezes 100 dev memories (ascending ID) from the p11b pairs,
the daemon's exact SSL and system prompts, and the earliest 339 stored teacher
outputs (creation time, then ID). Source data remain untouched. The immutable
manifest and all raw results live under
`/projects/caeg/scratch/kbd606/tmp/p13-data`; results are not committed.

The four arms use `gemma4:26b`, temperature 0.3, `/api/chat`, thinking on/off,
and `num_predict` 8192/2048. Two login-node workers share
`http://dandygpun01fl:11434` with the ongoing teacher-label job. Settings alternate
within each item, reversing order on alternate items. Failures are recorded and
can be resumed; missing or truncated responses cannot qualify a default change.

Thinking off becomes the default only if the complete 100-pair 8192-token arm
has relaxed micro triplet F1 >= 0.90 and every SSL type-line total differs by at
most 10% from thinking on. The 2048-token arm diagnoses truncation and is reported
separately. Relaxed agreement lowercases and normalizes punctuation, underscores
and whitespace in all three fields; exact agreement preserves all bytes. Both
use the production C++ parser, deduplicate per-memory triples, and report matched
and total triple counts. Empty triple collections do not constitute agreement.

Per-item records include wall latency, combined generated tokens, thinking
characters, SSL characters, type-line counts, request hash and raw response.
Ollama exposes `eval_count` for thinking plus visible output together, but does
not expose a separate thinking-token count. `thinking_tokens` is therefore null;
no character-to-token estimate is presented as a measurement.

Run `benchmarks/distill/think_ablation.py freeze|run|analyze` with `--pairs`,
`--output`, and (for analysis) `--parser`. Compile `parser_cli.cpp` with the
production `ssl_parser.cpp` and `temporal.cpp` with `-include array` (the temporal
translation unit relies on a transitive header in the daemon build). Keep the pre-change binary for
the replay comparison. Reports include the parser binary and response hashes.

Pending: completed ablation measurements, parser replay coverage, and golden
and current-truth deltas on a private copy of the frozen learning cut.

## Frozen teacher replay

2026-09-18: 339 outputs replayed with the original and changed production parsers.
Before: 149 triplets (0.4395/memory), 117/339 memories with relations (34.51%).
After: 1,358 triplets (4.0059/memory), 299/339 memories with relations (88.20%).
The original parser aborted on one oversized citation line number; its failed item
contributes zero returned relations. The new parser had zero failures. This is
parser coverage, not evidence that inferred edges improve retrieval.
`p13-data/replay.json` binds both executable hashes and the frozen manifest and
retains per-memory triples and failures. Golden/current-truth deltas remain pending.

Standalone parser tests cover all arrow spellings, chains, sets, domains, metadata,
explicit predicates/dates, choice syntax, epsilon exclusions and citation overflow.
Parity fixture: `chitta/tests/fixtures/distill-arrows.{ssl,json}`.

## Request option

`NativeDistillConfig.think` reads `CHITTA_DISTILL_THINK`; unset/unknown values keep
thinking enabled, and `0`, `false`, `off` disable it. Distillation and research
ingestion send the explicit boolean to Ollama `/api/chat`, with the original
sampling temperature and token budget in `options`. Other HTTP callers retain
the OpenAI-compatible endpoint. `distill_status.think` exposes the effective
environment setting. The local HTTP stub checks both booleans, payload options,
response content extraction and the generic client's existing request format.

The ablation remains incomplete; no qualified agreement estimate, final latency
comparison, or separate thinking-token counts are available. Ollama supplies only
a combined generated-token count; thinking characters are measured separately.
Default: **thinking enabled**, pending all 100 paired comparisons at 8192 tokens.
