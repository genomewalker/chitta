# Distiller SSL measurement and default

Status: 2026-09-18 — final decision: thinking stays enabled. Manual retention is
92/112 facts (82.14%), below 90%. Parser replay and replica evaluation are complete.
The lead stopped the remaining ablation/control workers at 11:00; do not restart
or treat the partial sample as a completed 100-item ablation. Full-gate validation
is accepted with the two environmental hook failures resolved by the lead's
11:05 login-node reruns, as detailed below.

## Measurement protocol and decision

The harness freezes 100 dev memories (ascending ID) from the p11b pairs, the
daemon's exact SSL and system prompts, and the earliest 339 stored teacher
outputs (creation time, then ID). Source data remain untouched. The immutable
manifest, raw responses and reports live under
`/projects/caeg/scratch/kbd606/tmp/p13-data`; results are not committed.

The planned four arms use `gemma4:26b`, temperature 0.3, Ollama `/api/chat`,
thinking on/off and `num_predict` 8192/2048. At most two login-node workers used
`http://dandygpun01fl:11434`. Settings alternate within each item, reversing order
on alternate items. An independent thinking-on control was also planned for the
same 100 items at 8192 tokens. The lead stopped these workers to free the teacher
server for JSON labeling; there is no measured on/on self-agreement result.

The original default-change rule required relaxed micro triplet F1 >= 0.90 and
every SSL type-line total within 10% on the complete 100-pair 8192-token arm.
Relaxed agreement normalizes case, punctuation, underscores and whitespace;
exact agreement preserves bytes. Both use production parser output and deduplicate
per-memory triples. Empty collections do not constitute agreement.

The lead added a semantic criterion: **thinking off keeps >= 90% of relations
and facts**, with type-line totals within 10% and a 30-item manual fact review.
String agreement alone cannot establish semantic retention for a sampled
generator; the uncompleted control does not establish its self-agreement either.
The completed manual review fails the fact threshold, so **thinking stays on**
regardless of the remaining control. Higher raw relation counts do not override
that failure. The decision is final for this stream; the ablation is partial.

## Measured partial arms

Immutable `p13-data/report-1789715922544703165.json` contains 30 complete
8192-token pairs (60/400 planned primary responses at that checkpoint). These
numbers describe that frozen report, not all responses later collected. It has
no 2048-token or control responses; neither arm has a reported comparison.

| Measure at 8192 tokens | Thinking on | Thinking off |
| --- | ---: | ---: |
| Median latency, seconds | 248.568 | 140.047 |
| Median generated tokens, including thinking | 3557 | 286 |
| Median thinking characters | 8356.5 | 0 |
| Median SSL characters | 494 | 687 |
| Parsed relations per memory (mean) | 3.267 | 5.433 |
| Memories with relations | 23/30 (76.67%) | 27/30 (90.00%) |

There are 98 versus 163 deduplicated relations. Exact agreement is 6 matches,
F1 0.045977; relaxed agreement is 8 matches, F1 **0.061303**. The latency reduction
is about 44%; generated tokens fall about 12.4-fold. Ollama exposes only combined
`eval_count`, so separate thinking-token counts are unavailable and recorded as
null, not estimated from characters. Per-item records retain latency, tokens,
thinking/SSL characters, type counts, raw responses and request hashes.

| SSL type-line total | Thinking on | Thinking off |
| --- | ---: | ---: |
| BELIEF | 11 | 41 |
| CITE | 1 | 1 |
| CORRECTION | 5 | 15 |
| DECISION | 4 | 16 |
| EVENT | 16 | 16 |
| FAILURE | 0 | 2 |
| GOTCHA | 13 | 12 |
| OPERATIONAL | 31 | 45 |
| PATTERN | 8 | 4 |
| PREFERENCE | 3 | 1 |
| SOLUTION | 11 | 14 |
| TRIPLET | 5 | 14 |

Type counts fail the 10% drift criterion, including PATTERN (50%) and DECISION
(300%). More output relations are not evidence that the same facts survive.

Run `benchmarks/distill/think_ablation.py freeze|run|analyze` with `--pairs`,
`--output` and (for analysis) `--parser`. Compile `parser_cli.cpp` with production
`ssl_parser.cpp` and `temporal.cpp`, using `-include array` for the temporal
translation unit. Preserve the pre-change binary for replay. Reports bind parser
and response hashes. `--control` uses a separate immutable control manifest;
`queue_control.py` serializes phases to respect the two-worker limit. These are
reproducibility instructions, not authorization to resume the stopped experiment.

## Manual fact review

The preselected first 30 frozen dev IDs were reviewed against their source and
primary thinking-on/off 8192-token outputs. Immutable
`p13-data/manual-review/report.json` binds 30 item-level reviews, source hashes,
response hashes and the frozen manifest. This was an unblinded, single-reviewer
Codex assessment, not an independently adjudicated accuracy benchmark.

Deduplicated, source-supported propositions in thinking-on output formed the
denominator; semantic preservation in thinking-off output was assessed against
the source. Concrete values and paths count. Unsupported on claims were excluded
and flagged separately, along with unsupported off claims. Five on bodies were
empty: they contribute no denominator and are not scored as perfect retention.
This measures retention of supported on facts, not exhaustive source recall.

Thinking off retained **92/112 facts (82.14%)** across the 25 nonempty references,
below the lead's **90% fact-retention threshold**. Higher relation counts cannot
make this review pass. Examples include an observed 59% pipeline stall converted
into a target 59% reduction, Slurm concurrency `%10` corrupted to `%1`, and
omitted benchmark numbers and paths. Both arms also contain unsupported claims;
8 on outputs and 17 off outputs have at least one explicitly flagged claim or
corruption. These descriptive counts are not precision estimates.

## Frozen teacher replay

339 stored outputs were replayed with original and changed production parsers.
Before: **149 triplets (0.4395/memory)**, **117/339 covered memories (34.51%)**.
After: **1,358 triplets (4.0059/memory)**, **299/339 covered memories (88.20%)**.
The original parser aborted on one oversized citation line number; its failed
item contributes zero returned relations. The changed parser had zero failures.
`p13-data/replay.json` binds executable hashes, manifest, per-memory triples and
failures. This measures parser coverage, not relation correctness or retrieval.

Typed arrow chains yield adjacent relations, including right-hand sets; explicit
TRIPLET predicates remain intact. Standalone tests cover all arrow spellings,
chains, sets, domains, metadata, explicit predicates/dates, choice syntax,
epsilon exclusions and citation overflow. Updated parity fixture:
`chitta/tests/fixtures/distill-arrows.{ssl,json}` (parser commit `a72a7ea2`).

## Request option

`NativeDistillConfig.think` reads `CHITTA_DISTILL_THINK`; unset/unknown values keep
thinking enabled, and `0`, `false`, `off` disable it. Distillation and research
ingestion send the explicit boolean to Ollama `/api/chat`, preserving sampling
temperature and token budget in `options`. Other HTTP callers retain the
OpenAI-compatible endpoint. `distill_status.think` exposes the effective setting.
The local HTTP stub checks both booleans, payload options, content extraction and
the generic client's request format. Default: **thinking enabled**.

## Replica evaluation

`benchmarks/distill/run_replay.sh` starts a fresh private copy using
`scripts/eval-replica.sh` and stops its own daemon on exit. Run it through
`on-compute.sh` or in a Slurm allocation after building this worktree. It uses
`CHITTA_RECALL_NOW=1789423200`, embedding wait 10000 ms and single-threaded BLAS.
The Python runner validates the socket listener's replica path before any graph
write. It records three golden nDCG@20 runs and the current-truth panel for each
of three stages: untouched frozen graph, graph with the legacy parser's unique
relations, and graph with the new parser's additional unique relations. Legacy
relations remain present, including any absent from the changed parser output.
The reported parser delta is densified minus legacy; the frozen stage provides
an additional control. Each stage is saved independently with the final report
binding the replay hash, daemon hash, snapshot ID and pinned clock.

`p13-data/retrieval-json/report.json` binds the replay and daemon hashes,
frozen snapshot `da86decb`, and pinned clock `1789423200`. The private replica
received 149 legacy unique relations followed by 1,208 additional unique relations
from the changed parser (1,357 unique relations in total). It stopped cleanly.

| Measure | Frozen | Legacy | Densified | Densified minus legacy |
| --- | ---: | ---: | ---: | ---: |
| Golden mean nDCG@20 (3 runs) | 0.416126 | 0.416126 | 0.416126 | 0 |
| Current-truth P@3 | 0.20 | 0.20 | 0.20 | 0 |
| Answerable hits (40 questions) | 8 | 8 | 8 | 0 |
| Correct abstentions (10 questions) | 10 | 10 | 10 | 0 |
| Utility | 18 | 18 | 18 | 0 |

All three golden samples at every stage were identical. The observed coverage
gain therefore produces no measured retrieval improvement on these panels.
No live graph or daemon configuration was changed.

The added relations are available to graph tools and `what_do_i_know_about`.
They increase the graph information those tools can expose; ranked recall on
these golden and current-truth panels is unchanged. No improvement in the
correctness of graph-tool answers was measured.

Replay uses JSON over the validated private Unix socket so flag-like entity
names such as `--realm` survive transport. A regression test covers those names
and RPC errors. The completed evaluation used a fresh `p13-replay-json` replica.

## Validation and environmental failures

The completed compute full gate (`p13-data/full-control.log`) passed 302 Rust
tests (2 ignored), all 36 C++ tests and 39 hook suites. Its raw result was FAIL
because `test_handoff_capsule.sh` and `test_noise_hook_metric.sh` failed on the
compute node. Diagnostics included compiler/Python environment differences and
NFS temporary-directory cleanup; a pinned compute rerun still failed.

At 11:05 on 2026-09-18 the lead confirmed that **both failing hook tests pass in
this worktree on the login node, with TMPDIR=/tmp and with the default TMPDIR**.
The lead classified the compute failures as environmental and instructed this
stream not to chase them. Full-gate validation is therefore accepted using the
compute results plus those successful targeted reruns; the saved raw FAIL log
is not represented as a clean single-invocation pass. No hook fixes were needed.

The earlier optional replica-gate attempt also had reporting problems: restart
identity had 60/60 identical ordered results over three restarts, but the wrapper
searched only the final three output lines and lost its summary. The chaos
wrapper likewise lacked the expected summary; its complete panel was not
verified and no chaos pass is claimed. Those wrapper changes are outside this
stream. Retrieval evidence above comes from the completed private replica run.

The six distill harness tests, parser/request tests and prior per-commit quick
gates passed; contracts were unchanged. The final documentation commit runs the
quick gate and contract check again. No binaries were installed, services
restarted, or live mind/configuration changed.
