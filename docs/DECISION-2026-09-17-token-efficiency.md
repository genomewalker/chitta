# Decision: chitta as the token-efficiency layer (Phase 10)

> Status as of 2026-09-17: **agreed with the owner ("chitta should be the tool
> that gives token efficiency").** Baseline measured from the agents' own
> transcripts with `scripts/token-ledger.py`; targets and steps below. Adds
> Phase 10 to `DECISION-2026-09-16-robustness-plan.md`.

## Baseline (last 7 days, list prices, ratios are the point)

| | Fable (Claude Code) | Astra (Codex) |
|---|---|---|
| Sessions | 124 | 2,300 |
| Assistant turns | 61,636 | 20,840 |
| Average context per turn | 343k tokens | 41k tokens |
| Cached input | 21.2 B tokens | 0.79 B |
| Output | 84 M tokens | 4.6 M |
| Tool outputs | 24,950 (24 M chars, 535 over 6 k) | 8,404 (32 M chars, 1,547 over 6 k) |
| Cost at list prices | about $38 k | about $560 |
| Largest session | 43,332 turns, 13.2 B cached, $24 k | 814 turns, 54 M cached, $31 |

Three facts follow. The money is on Fable's side by a factor of sixty. The
cost is turns × context: a Fable turn re-sends a 343k-token thread, so every
kilobyte of tool output and every status poll is paid on every later turn.
Output tokens are the second line (84 M at the highest price), and they are
the assistant's own words.

## What chitta changes

1. **Sessions stay short because memory carries the state.** The handoff
   capsule (Phase 3), the repo map and `code_query` (Phase 9), and recall are
   what make a fresh 20k-token session as capable as a 350k one. The Stop
   hook adds a `[budget]` line when a session's context per turn crosses a
   threshold (default 150k, `CHITTA_CONTEXT_BUDGET`): the capsule is current,
   compact or start fresh now. Measured by the ledger: average context per
   turn.
2. **Tool output is capped at the source and deduplicated.** The pre-tool
   hook wraps Bash through the sqz compressor (user-global wrapper; it has
   saved nothing since 2026-09-15 and is repaired as part of this phase), the
   read hook already routes re-reads of unchanged files to the cache, and
   outputs over 6k characters get a `§ref` with a summary instead of the
   bytes. Measured: tool chars per session, outputs over 6k.
3. **Read through the graph, not the file.** Phase 9's pre-read context and
   `code_query` replace full reads; the navigation benchmark showed 64% fewer
   bytes. Measured: bytes of file content read per task.
4. **Fewer turns.** No polling from inside a thread (background jobs return a
   summary once), batched commands, one call per step. For Astra the launcher
   `scripts/codex-stream.sh` prepends `codex-plugin/stream-preamble.md`
   (batching, output caps, gate tiers, handoff instead of thread growth,
   effort by task shape: high for implementation, medium for docs, tables and
   measurement runs, never ultra unattended). Measured: turns per task.
5. **The ledger is visible.** `scripts/token-ledger.py` runs weekly (evolve
   nightly card) and its top line joins the session-start block as
   `[tokens] this week: …` so drift is seen, not discovered on the invoice.

## Targets (measured weekly by the ledger, same work mix)

- Fable average context per turn ≤ 150k (from 343k).
- Tool output chars per Fable session halved; zero outputs over 6k chars
  reach the thread uncompressed.
- Astra turns per stream ≤ 300 (largest today 814); stream cost visible per
  stream in the launcher's report.
- Output tokens per Fable turn down 30% (shorter replies, no restating).
- Weekly cost at equal work: −50% within two weeks of the phase landing.

## Steps (each a gated Codex stream at medium effort unless it touches the daemon)

0. Ledger (done): `scripts/token-ledger.py`; weekly card; `[tokens]` line at
   session start.
1. Budget advisory in the Stop hook, driven by the transcript's usage fields,
   with the capsule freshness check; parity fixtures updated.
2. sqz repair and `§ref` for large outputs in the post-tool path; measure
   savings on a fixed replay of one day of Bash outputs.
3. Launcher and preamble in force for every stream; the plan's stream specs
   reference them instead of restating rules.
4. Two-week measurement against the targets; then decide whether the
   per-turn context needs a hard stop (a hook that refuses further tool calls
   until compaction) or the advisory suffices.

Not planned: changing the models or their effort for implementation work,
or trimming the recall block itself below what the current-truth panel needs.
