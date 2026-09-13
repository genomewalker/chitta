# Evolution bridge: literature watch and adversarial review

Date: **2026-09-13**. Branch: `evolve/bridge`.

The sibling modules in `chitta-mcp/evolve/` use only the Python standard library
and import under PyPy 3.9. They do not import or change the parallel stream's
`proposals.py`, `select.py`, `bets.py`, or `cycle.py`.

## Commands

Run from `chitta-mcp`, with the bioinfo interpreter:

```bash
PY=/maps/projects/fernandezguerra/apps/opt/conda/envs/bioinfo/bin/python3
$PY -m evolve.bridge_client --probe \
  --fetch https://r.jina.ai/https://arxiv.org/abs/2511.17208
$PY -m evolve.sota_watch --max-papers 3
$PY -m evolve.review .. --base main~1 --head main --dry-run
$PY -m evolve.review .. --base main~1 --head main --output review.json
```

All three commands accept `--url`, `--token-file`, and `--timeout` (seconds).
The default endpoint is `http://127.0.0.1:7681/mcp`. Authentication uses
`CHITTA_BRIDGE_TOKEN`, then `~/.chitta-bridge/token` (or `--token-file`). The
unit and `~/.claude.json` were inspected read-only; the bridge's `_http_token()`
implementation confirms the secret-file location. Credentials are never logged.

The client initializes MCP, sends the initialized notification, retains the
session ID and negotiated protocol version, follows paginated tool lists, and
supports JSON and incremental SSE responses. It preserves POST and bearer auth
across the exact `/mcp` → `/mcp/` 307/308 redirect; other redirects are rejected.
Responses have an 8 MiB limit and socket timeouts; SSE also checks elapsed time
between lines. Calls are not automatically replayed after a transport failure,
which avoids duplicating model calls or writes. A client instance is synchronous;
concurrent reviewers each have their own instance.

## Literature cards

`sota_queries.txt` contains the eight requested seed queries. `--queries` accepts
a replacement newline-separated list; blank lines and `#` comments are ignored.
`--days` defaults to 14, using UTC dates. arXiv queries include a submitted-date
range and returned publication dates are checked again. OpenAlex uses both
publication-date bounds in its API filter; the bridge exposes only the year in
its response, so independent day-level validation is unavailable for that source.

The watcher searches both providers and deduplicates arXiv versions, arXiv DOI
aliases, titles within the run, local card IDs, and existing `sota-card` memories
in `project:chitta-evolve`. Non-arXiv DOI papers use `doi-<stable-sha256-prefix>`
filenames. Recall uses `~/.claude/bin/chitta recall --json --tag sota-card --realm
project:chitta-evolve`, with an explicit query, keyword strategy, and no learning.
An additional per-paper recall reduces misses from the initial bounded result
set. This is bounded search, not an exhaustive export of all memories. Unexpected
response schemas, missing responses, and a mismatched realm stop the watch before
it assumes successful deduplication.

Each new paper is fetched with `paper_fetch`, falling back to `web_fetch`.
Extraction uses one `discuss` request routed explicitly to Claude Sonnet, with
14,000 characters of paper text and 24,000 characters of inventory at most.
`--max-papers` defaults to 5 and accepts only 0–5; failed extractions also consume
the model-call budget. Zero performs pending-memory recovery only.

The inventory is `/projects/caeg/scratch/kbd606/tmp/chitta-truth-inventory.md` if
present, otherwise `docs/ARCHITECTURE.md`; `--inventory` overrides it. Its path and
SHA-256 accompany the card. Estimates are hypotheses against that inventory,
not measured Chitta gains. Inventory staleness remains a limitation. The model
must distinguish implemented, planned, and shadow capabilities; a one-line
rationale names the capability and incremental gap. `already_have: true` forces
`delta` to zero. Numeric estimates must be finite, confidence in [0,1], and effort
nonnegative. Source IDs, titles, and URLs come from search rather than the model.

Cards in `evolve/proposals.d/<arxiv-id>.json` have this shape:

```json
{
  "id": "2609.00001",
  "title": "Paper title",
  "mechanism": "Specific implementable causal mechanism",
  "expected_gain": {
    "metric": "recall@20 absolute fraction",
    "delta": 0.02,
    "confidence": 0.2,
    "rationale": "One line comparing the proposal with an inventory capability."
  },
  "cost": {"effort_h": 4, "blast_radius": "retrieval stage"},
  "evidence": [{"source": "https://arxiv.org/abs/2609.00001", "claim": "Supported claim"}],
  "source": "https://arxiv.org/abs/2609.00001",
  "already_have": false,
  "kind": "sota-card"
}
```

Additional fields record inventory provenance, creation time, and the memory
receipt after `chitta remember --tags sota-card --realm project:chitta-evolve`.
The card is written atomically before memory persistence, then updated with the
receipt. An interrupted write leaves a pending card that the next run reconciles
through recall without another model call. Only watcher-owned `kind: sota-card`
files are recovered; other proposal streams are not written into memory. A
`.sota-watch.lock` directory excludes concurrent watcher runs. After a hard kill,
inspect pending files and remove that stale directory before retrying.

A partial provider failure is reported in JSON and produces exit 1 even if other
providers supplied cards. Memory or malformed-card failures are never reported
as successful storage.

## Branch review

Review resolves base/head to immutable commit hashes, collects the exact diff,
and reads committed bet/verdict artifacts from those revisions. Dirty working
files are excluded. Automatically recognized names are `bet.json`, `bets.json`,
`preregistration.json`, `verdict.json`, `*.bet.json`, and `*.verdict.json`.
`--verdict` names an additional repository-relative path in the head commit;
missing explicit artifacts are errors. Diffs over 200,000 characters, individual
artifacts over 64,000, or combined inputs over 300,000 are rejected for splitting,
not silently truncated.

Both prompts contain the same five checks: evaluator/gold/hidden/ledger changes;
Goodhart/reward inflation; frozen RPC, recall-line and snapshot/WAL contracts;
live hooks/daemon blast radius; and honest resolution of the original bet.
They instruct reviewers to use only the supplied data and make no tool calls or
writes. `--dry-run` prints both prompts without contacting the bridge or writing
output. Real runs write the `.diff` alongside `review.json`, which contains the
inputs, their hashes, raw attempts, reviewer verdicts, and overall verdict.

`review` is requested with an explicit Claude backend and model. The inspected
bridge advertises a backend selector but ignores it and invokes Codex. This
mismatch is recorded; Claude `discuss` supplies the actual independent review.
`codex_review` accepts `model`, so it is requested with `gpt-6-astra`, adversarial
mode, and a read-only sandbox. Neither reviewer failure nor malformed JSON can
pass: they produce `block`. Overall is `block` if either blocks, otherwise
`concern` if either has concerns, otherwise `pass`. Missing committed original-bet
or verdict evidence also prevents an overall pass. Exit codes are 0 for pass,
1 for concern, and 2 for block/error. These verdicts support human review; they
cannot prove absence of Goodhart behavior or prompt injection.

## Validation and live evidence

Offline fixture tests cover redirects/auth isolation, JSON/SSE, pagination,
sessions, protocol headers, timeouts, malformed replies, card extraction caps,
date/version dedupe, memory failure recovery, parallel-stream isolation, exact
committed review inputs, reviewer routing, and conservative verdict aggregation.

- Scoped Ruff: clean for the new modules and tests.
- Full `python3 -m unittest discover tests`: **78 tests passed** with the required
  bioinfo interpreter. See [unittest.log](evolve-bridge-live/unittest.log).
- PyPy **3.9.18 / 7.3.15**: all three modules import successfully.
- Repository-wide Ruff: **415 pre-existing findings** in other files; no bulk
  cleanup was made. See [ruff.log](evolve-bridge-live/ruff.log).

The requested live probe returned:

```text
{"tool_count": 49}
(curl fallback: HTTP 403)
```

Thus `tools/list` passed, but the required Jina fetch did not retrieve the paper.
OpenAlex returned recent papers; arXiv search timed out. The real watcher was
invoked with `--max-papers 3`, but initial memory recall timed out or returned an
unrelated response shape. A separate MCP recall diagnostic also returned
`Error: No response from daemon`. No daemon restart, service edit, hook edit, or
push was attempted. Live extraction drafts, when available, are saved separately
from the proposal queue because their memory dedupe/storage is unverified.

The requested review compared
`b5df6a3ddccc97d23b0ad535ffb80fc58031d8c8` →
`0c2e598e3e6567a5e26122c4ce2b1f8615887695` (`main~1..main`):

```text
Claude (Sonnet through discuss): pass
Codex (gpt-6-astra through codex_review): block — infrastructure failure
Overall: block
Codex error: unexpected argument '--full-auto' found
```

Claude judged the replica-abort change as making evaluation more honest and
found no frozen-format change. It acknowledged absent bet evidence. Codex never
reached inference: the bridge's read-only exec path passes a removed CLI flag.
A separate `codex_run` read-only compatibility check hit the same failure.
The original [review.json](evolve-bridge-live/review.json) and
[printed verdicts](evolve-bridge-live/review-run.json) retain those actual results;
the Codex infrastructure block must not be presented as an independent model
judgment about the diff. Live gates remain incomplete, so the branch is not
committed under the task's commit-after-gates condition.


The independent live extraction check produced three
[draft mechanism cards](evolve-bridge-live/draft-cards.json), with exactly three
Claude extraction calls and no memory writes:

| Paper | Proposed mechanism | Hypothesized incremental gain | Confidence |
| --- | --- | --- | --- |
| 2609.08599 | Evidence links on graph edges | nDCG@20 +0.01 | 0.20 |
| 2609.03467 | Rewrite implicit queries into explicit subqueries | implicit/composed-query nDCG@20 +0.03 | 0.20 |
| 2608.29622 | Stack scratchpad for multi-hop retrieval | LongMemEval accuracy +3 percentage points | 0.20 |

All three model outputs set `already_have: false`; these are provisional
inventory comparisons, not independent verification against current code.
The run exposed an ordinary ACM DOI being mistaken for an arXiv ID; the parser
now requires an arXiv prefix or a complete bare arXiv ID and validates the month.
That defect is covered by a regression test. The draft evidence retains the
failed fetch from the original diagnostic run for transparency.

Final watcher output (exit 1):

```text
Recall returned an unexpected schema; refusing unverified dedupe
```

No production proposal cards or `sota-card` memory receipts are claimed from
that run. Once the external recall/review integrations are repaired, rerun the
commands above and commit only after the requested live gates succeed.
