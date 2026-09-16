# STRUCTURED_EXTRACTOR_DESIGN.md

> **Deprecated in place. Status as of 2026-09-16.** Unshipped migration proposal; [HOOKS.md](HOOKS.md#automatic-learning-admission) describes the current contract.
> Native daemon distillation is shipped; Stop still extracts typed markers.
> The per-turn Python extractor and proposed migration below are historical design, not installation instructions.

## Context

`hooks/stop-hook.sh` currently performs regex-based learning extraction from the last assistant message: it scans for `[SOLUTION] [GOTCHA] [PREFERENCE] [DECISION] [FAILURE] [PATTERN] [LEARN] [CORRECTION] [EVENT]` (see `hooks/stop-hook.sh:173`) plus `[USED:id]` feedback (`hooks/stop-hook.sh:187`, regex at `:202`) and `[TRIPLET]` (`:265`). The comment header at `:8` enumerates the contract. This design replaces the typed-marker extraction path (Tier-1 learning write) with a structured LLM-driven extractor. The `[USED:id]` strengthening path and `[TRIPLET]` connect path are NOT affected (they carry IDs, not prose).

Critically, `hooks/distill.sh` already implements a mature LLM extractor (SSL v0.4 prompt, GPU endpoint discovery, deterministic regex fallback — `distill.sh:57-274`) but runs out-of-band (invoked by chittad, not the hook). This design makes that pathway the primary on-stop mechanism and retires the in-hook regex scan.

## 1. Location — Decision: (c) Hybrid, daemon-side async

The hook MUST remain fast (`CHITTA_MAX_WAIT=2s`, `stop-hook.sh:12`). We therefore split:

- **Hook side (sync, <100ms):** extract `[USED:id]` and `[TRIPLET]` markers (ID-bearing, cheap), store the assistant turn (`stop-hook.sh:122`), then enqueue a single `distill_request` queue write naming the transcript path, session, turn-range, and realm.
- **Daemon side (async worker):** a `distill_request` queue handler invokes `hooks/distill.sh` (already in tree), which performs the LLM call against the discovered GPU endpoint (`distill.sh:210-218`) and writes `observe` events with `source=distillation` — which means they land as `Active (0)` per `CONTRACTS.md:133-137` rather than `Proposed (4)` as hook_regex did.

Justification: option (a) shells out to an LLM inside the hook — fails SLA and conflicts with the GPU-endpoint-cached model at `/tmp/ollama-server-*.url`. Option (b) losing hook context (turn index, session_id, realm) is wasteful since the hook already has them. (c) is the only split that preserves stop-hook latency AND gets the `distillation` source-trust upgrade.

## 2. Schema — 6 kinds

Output schema emitted by the LLM (JSON lines, one per learning). Reuses SSL v0.4 semantics already parsed by `parse_ssl_annotations` (`lib.sh:196`) and the queue processor's category confidence map (`POLICY.md:109-119`).

```json
{
  "kind":        "lesson|gotcha|decision|preference|correction|pattern",
  "title":       "≤80 char summary",
  "content":     "SSL-formatted line (preserves [ε] verbatim block if Tier 1)",
  "scope":       "project|global|partnership",
  "realm":       "brahman|<project-name>",
  "confidence":  0.0-1.0,
  "evidence": {
    "turn_indices": [int, ...],
    "quote":        "≤240 char verbatim snippet grounding the claim"
  },
  "affect":      { "valence": -1..1, "arousal": 0..1 },
  "flags":       ["CORE","PIVOT",...],
  "granularity": 0-4
}
```

Kinds map 1:1 to existing queue `observe` categories (`tools_static.py:464`) — no daemon changes needed. Six kinds (not seven): `failure` folds into `lesson` with negative valence (`A:-*.*`), reducing taxonomy drift; `correction` stays separate because it carries 0.95 default confidence (`POLICY.md:111`) and triggers supersession (`POLICY.md:71`). `scope` is new: `partnership` routes preferences into `[partnership:pref]` (`stop-hook.sh:65`); `project`/`global` routes to the detected or `brahman` realm (`stop-hook.sh:129`).

Evidence `turn_indices` unlocks later RLM [71](#ref-71)-style grounding (current hook has no provenance beyond "regex match on [X]" — `stop-hook.sh:182`). Quotes are bounded to 240 chars to prevent prompt-echo inflation.

## 3. LLM call

- **Model:** local `ollama` via the discovered endpoint (`distill.sh:210-218`) — maintainer already runs a GPU-cached instance; zero marginal cost, no rate limits. Recommended: `qwen2.5:14b-instruct` or `llama-3.1:8b-instruct` at temperature 0.2. The existing `$MODEL` pass-through from the header file (`distill.sh:27`) stays.
- **Context budget:** 24k tokens in / 2k out. Reuse `distill.sh`'s smart head+tail truncation (`:41-54`) with tighter caps (12k head / 12k tail) — stop-hook extractor sees only *last-turn* context plus the user prompt that triggered it, NOT full session. Full-session distillation remains chittad's periodic job.
- **Prompt sketch:**

```
SYSTEM: You extract structured learnings from one assistant turn. Output
ONLY newline-delimited JSON objects matching the schema below. No prose,
no markdown. Empty output is valid when nothing was learned.

SCHEMA:
{kind, title, content, scope, realm, confidence, evidence{turn_indices,quote},
 affect{valence,arousal}, flags[], granularity}

RULES:
- kind ∈ {lesson,gotcha,decision,preference,correction,pattern}
- content uses SSL v0.4: "[domain:abbr] subj→act→result G:N F:FLAG A:v,a"
- Tier-1 kinds (gotcha, pattern, lesson-w/-code) append "[ε] <verbatim>"
- quote MUST be a substring of the input conversation
- Skip anything the user corrected — the corrected version is the learning
- Confidence: 0.9 for explicit statements, 0.7 for inferred, <0.6 → drop

INPUT:
  session_id: {id}
  realm:      {realm}
  last_turn_index: {n}
  user_prompt: {prompt_text}
  assistant_response: {response_text}

OUTPUT: JSONL, max 8 objects.
```

## 4. Migration — 2-release deprecation

- **v5.x (current + next):** keep regex loop at `stop-hook.sh:172-185` gated behind `CHITTA_LEGACY_MARKERS=1` (default **on** for v5.x). New extractor runs unconditionally; dedup via existing `DEDUP_FILE` content hash (`lib.sh:161-166`) prevents double-writes when the assistant emits both explicit `[SOLUTION]` and the LLM re-extracts the same content. Source label for the new path: `distillation` (auto-promoted). Source label for legacy regex: `hook_regex` (stays `Proposed`).
- **v6.0:** delete the regex loop and the `case` mapper (`stop-hook.sh:85-95`, `:172-185`). Keep `to_ssl()` — distill.sh reuses it. Update `CHANGELOG.md` and `hooks/stop-hook.sh:6-8` header.

`[USED:id]`, `[TRIPLET]`, the compliance tracker (`:287`), and curiosity-gap detection (`:258`) are out of scope — they survive untouched.

## 5. Failure modes

| Mode | Current behavior | New behavior |
|---|---|---|
| No GPU endpoint | distill.sh falls through to regex fallback (`distill.sh:220-273`) | Same fallback runs daemon-side; hook already returned. User sees no delay. |
| Malformed JSON from LLM | N/A | Parser is line-oriented (JSONL); bad lines logged to `$MIND_PATH/.distill_parse_errors.jsonl`, good lines stored. |
| Empty response | N/A | Log `[distill] no learnings` and exit 0 — turn still stored losslessly at `stop-hook.sh:122`. |
| LLM timeout (180s cap, `distill.sh:293`) | N/A | Queue entry marked failed in `.failed_observations.jsonl` (same pattern as `stop-hook.sh:199`); a nightly retry pass re-processes. |
| Daemon down | regex path still ran | Hook-side enqueue persists to `/tmp/chitta-queue.jsonl` (`lib.sh:93-130`); daemon drains on next start. Zero loss. |
| Rate-limit | N/A (local) | If endpoint returns 429, worker backs off exponentially and re-queues; never blocks the hook. |

## 6. Minimal MVP (one commit)

**In scope:**
- New file `chitta-mcp/extractors/stop_extractor.py` implementing the prompt + JSONL parser + `observe` emission. ~150 lines.
- New daemon queue tool `distill_turn` (thin wrapper calling the extractor). Registered in `chitta-mcp/tools_static.py`.
- `hooks/stop-hook.sh`: one new `queue_write "distill_turn" "{session_id, turn_index, transcript_path, realm}"` after existing `store_turn` at `:122`. Legacy regex block gated behind env flag but still default-on.
- `docs/CHANGELOG.md` + `CONTRACTS.md` addition noting new `source=distillation` usage per-turn.

**Out of scope:** removing legacy regex (v6 job); changing `[USED:id]` or `[TRIPLET]`; touching chittad C++; prompt tuning beyond the sketch; multi-turn windows; cloud API fallback.

## 7. Test plan

1. **Positive:** Transcript where assistant writes "Fixed it by passing `--no-verify` to `git commit` when the hook hangs." Expected: one `gotcha`-kind JSON with `content` containing `[project:gotcha]`, `quote` matching the fix sentence, `confidence ≥ 0.85`, `evidence.turn_indices=[current]`. Assert via `chitta sql_query "SELECT category,source FROM memory WHERE session_id=?"` that `source='distillation'` and status=Active (0).
2. **Negative:** Transcript is a single "OK, done." — extractor MUST emit empty JSONL, hook exits 0, no memory row created, no parse errors in log.
3. **Noisy:** 40k-char transcript full of tool output, error tracebacks, and code dumps with NO reusable learning (e.g., repeated `ls` listings). Assert: at most 1 memory written (ideally 0), no crash, completes within the 180s timeout, head+tail truncation visible in daemon log.

## Picks summary

- **Location (1):** Hybrid — hook enqueues, daemon worker runs `distill.sh`-style extractor.
- **Schema kinds (2):** `lesson, gotcha, decision, preference, correction, pattern` (six; failure folded into lesson-with-negative-valence).
- **MVP scope:** one extractor script + one `distill_turn` queue tool + one `queue_write` line in `stop-hook.sh`; legacy regex stays behind env flag for v5.x.

<!-- BEGIN CITATIONS -->
## References

- <a id="ref-71"></a>**[71]** Alex L. Zhang, Tim Kraska, and Omar Khattab. Recursive Language Models. arXiv:2512.24601 (2025; revised 2026). [source](<https://arxiv.org/abs/2512.24601>)
<!-- END CITATIONS -->
