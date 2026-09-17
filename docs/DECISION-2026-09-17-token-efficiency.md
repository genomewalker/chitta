# Decision: chitta as the token-efficiency layer (Phase 10)

> Status as of 2026-09-17 (round 2): **agreed with the owner and reconciled with
> Astra's consultation (Appendix).** The handoff mechanism, the thresholds and
> the deciding measurement below supersede the first draft's 150k target and
> the 150-tool-call stop. Baseline measured from the agents' own
> transcripts with `scripts/token-ledger.py`; targets and steps below. Adds
> Phase 10 to `DECISION-2026-09-16-robustness-plan.md`.

## Baseline (original numbers superseded; retained for comparison)

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

### Corrected event-window baseline

Measured with `scripts/token-ledger.py --days 7 --json`, window 2026-09-10T05:07:39.328394+00:00 to 2026-09-17T05:07:39.328394+00:00.

| Metric | Fable | Astra |
|---|---:|---:|
| Sessions | 113 | 2,316 |
| Deduplicated requests | 2,504 | 14,658 |
| Mean context per request | 406,805 | 74,052 |
| Context at latest request | 771,043 | 216,998 |
| Cached input tokens | 1,002,688,543 | 1,015,270,656 |
| Cache-write input tokens | 15,695,873 | 0 |
| Output tokens | 2,074,095 | 5,668,994 |
| Estimated cost | $2,116.70 | $693.37 |
| Requests using fallback IDs | 0 | 14,658 |

The original table above is superseded, as are numerical savings extrapolations
that depend on it. This is a corrected observational baseline, not the paired
continuation experiment. It includes nested transcript files and deduplicates
request IDs across files; missing provider IDs use the documented fallback.
Costs include cache writes using the configured provider prices, rather than
model-specific billing. Model usage and per-session lifetime mean versus last
request context are retained in the JSON report.

The original analysis (based on the superseded table): three facts follow. The money is on Fable's side by a factor of sixty. The
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
   `scripts/codex-stream.sh` builds the prompt from `codex-plugin/stream-contract.md`
   and chitta (batching, output caps, gate tiers, handoff instead of thread growth,
   effort by task shape: high for implementation, medium for docs, tables and
   measurement runs, never ultra unattended). Measured: turns per task.
5. **The ledger is visible.** `scripts/token-ledger.py` runs weekly (evolve
   nightly card) and its top line joins the session-start block as
   `[tokens] this week: …` so drift is seen, not discovered on the invoice.

## Context across streams (measured 2026-09-17)

How the streams actually got their context this week: task specs of 4 to 7 KB
restating the rules; the first commands of every stream read the plan (240
lines), AGENTS.md (240 lines) and HOOKS.md (190 lines), then grep the code;
between zero and four `chitta recall` calls per stream and no `code_query`;
nothing written back to chitta (Plan.md and reports only), so the next stream
or resume got its context from a new spec written by the lead, and resumes
carried whole threads forward. Fable did the same in one 5,000-turn thread.

The fix is structural: the stream's opening context is built from chitta by
`scripts/codex-stream.sh` (contract, recalled decisions and handoffs, code
map, task), every commit writes a `[handoff]` memory in a fixed shape, a fresh
thread continues from the handoff instead of a resume, and the lead's own
sessions start from the capsule and `/recap` rather than growing.

## Round 2: what Astra changed (2026-09-17)

- **Handoff mechanism.** Semantic `[handoff]` memories stay as a discovery
  index only. The authoritative continuation record is one bounded capsule
  (≤ 4 KB) in the task ledger's existing session metadata, extended with
  objective, constraints, state, code HEAD and dirty paths, gate evidence,
  blockers, next action, running jobs, a timestamp and a monotonically
  increasing revision with compare-and-set; states `complete`, `in progress`,
  `missing`, `invalidated` are distinct. Exact latest-by-key retrieval goes
  through the existing `ledger_op` gateway; the launcher injects that same
  bounded response, and `--check` fails on a missing, stale or HEAD-mismatched
  record. Codex native history notes: measure first (the config keys do not
  exist in this install). A git-tracked HANDOFF.md: rejected as a mandatory
  per-step channel.
- **Budgets are requests and tokens, not tool calls.** A turn is one billed
  model request. Lead: aim for ≤ 30k mean input per request; checkpoint at
  40k current input or 30 requests; start fresh by 60k or 50. Astra: 15 to 25
  requests per implementation stream; checkpoint at 40 requests or 40k
  context; restart by 60 requests, 60k context or 1 M cumulative input. The
  150-tool-call stop is withdrawn (at 41k context it still permitted 6 M
  input tokens).
- **Session start ≤ 1,500 tokens** (hard max 2,000): objective and acceptance
  200, constraints 200, active-stream manifest 450 (stream, branch/HEAD,
  status, capsule revision, one next action), evidence and decisions 250,
  blockers and next actions 200, job handles and token budget 200. Stream
  capsules load only when acting on that stream.
- **Waiting is not a model loop.** Jobs get durable ids and result files; the
  session ends or suspends and an external watcher emits one completion or
  failure event. No `status`, `sleep` or "still running" turns.
- **Delegation.** Haiku for bounded extraction and classification with exact
  checks, Sonnet for localized implementation and review with acceptance
  tests, the lead for ambiguity and integration; task-only capsules, never a
  full-history fork; children return ≤ 300 tokens plus pointers; child cost
  counts in the savings.
- **Output caps.** Lead 500 tokens per request, status 150, final synthesis
  1,000; Astra 600 / 150 / 800; artifacts go to files.
- **Modeled savings on the measured baseline** (targets, not results): context
  alone at 30k saves 91% of input; with half the requests 96%; with 500 output
  tokens per request the modeled cost is about 7% of baseline. The first
  draft's 150k target saved only 56%.
- **The ledger needs fixing before it can adjudicate**: deduplicate provider
  request ids, use event timestamps for the window instead of file mtime,
  count cache writes, keep per-model usage, report context at the last request
  as well as the lifetime mean.
- **Deciding measurement.** Paired replay of the same 20 representative tasks
  (including interrupted work, failed gates, dirty trees, stale capsules and
  background jobs): arm A resumes the long session, arm B starts fresh with a
  ≤ 1,500-token capsule plus targeted retrieval, same model, effort, tools and
  gates. B must succeed on every task A succeeds on, claim no false
  completion, and consume ≤ 25% of A's aggregate billed input; total modeled
  cost must fall. A missing capsule field is fixed before the capsule grows.

## Targets (measured weekly by the ledger, same work mix)

- Fable mean input per request ≤ 30k (from 343k); checkpoint at 40k or 30 requests, fresh session by 60k or 50.
- Tool output chars per Fable session halved; zero outputs over 6k chars
  reach the thread uncompressed.
- Astra 15 to 25 requests per implementation stream (largest today 814 turns);
  cost per stream reported by the launcher.
- Fable output ≤ 500 tokens per request (status 150, final synthesis 1,000).
- Weekly cost at equal work: −50% within two weeks of the phase landing.

## Steps (each a gated Codex stream at medium effort unless it touches the daemon)

0. Ledger (done): `scripts/token-ledger.py`; weekly card; `[tokens]` line at
   session start.
1. Budget advisory in the Stop hook, driven by the transcript's usage fields,
   with the capsule freshness check; parity fixtures updated.
2. sqz repair and `§ref` for large outputs in the post-tool path; measure
   savings on a fixed replay of one day of Bash outputs.
3. Launcher and contract in force for every stream; a task file is a one-line
   title (the launcher's query for recall and the code map) plus goal, write
   scope and task-specific gates, never the rules.
4. Two-week measurement against the targets; then decide whether the
   per-turn context needs a hard stop (a hook that refuses further tool calls
   until compaction) or the advisory suffices.

Not planned: changing the models or their effort for implementation work,
or trimming the recall block itself below what the current-truth panel needs.

## Appendix: Astra's consultation memo (verbatim)

## Consultation: context handoff and token protocol
Date: 2026-09-17. Read-only source audit; recommendations below are not benchmark results.
Verdict: adopt one bounded, deterministic capsule in chitta's existing task ledger.
Use semantic handoff memories for discovery, not as the authoritative continuation record.
Keep git commits as evidence; reject mandatory tracked HANDOFF.md updates on every branch.
Measure native Codex history notes before relying on them across independent threads.
Lead target: 30k mean input tokens/request; checkpoint at 40k or 30 requests, restart by 60k or 50.
Astra target: 20 requests × 25k input ≈ 0.50M input tokens per implementation stream.
Replace the 150-tool-call rule with request, context, and cumulative-token budgets.
Conditional lead savings: 91.3% input reduction from context alone; 95.6% with half as many requests.
With 500 output tokens/request and half the requests, modeled cached-input + output cost is 6.7% of baseline.
These are targets at equal completed work, not evidence of maintained task success or actual invoice savings.
Smallest change: extend the existing capsule schema and launcher; no new memory subsystem or core MCP tool.
Prove it on the same 20 tasks: fresh capsule versus long thread, success and total billed tokens including retries.
Only this memo was written; no commits, builds, gates, daemon starts, or memory writes were performed.

### 1. What the code actually does

Inspected `scripts/codex-stream.sh`, `codex-plugin/stream-contract.md`, `scripts/token-ledger.py`,
`chitta/include/chitta/hook_ledger_policy.hpp`, `hook_session_policy.hpp`,
`chitta/src/handlers/field_task_ledger.cpp`, `hooks/stop-core.sh`, and the token-efficiency decision.
The initial read-only handoff recall returned zero results; code_query produced no visible map.
This is missing context, not proof that the worktree contains no relevant code.

The launcher injects a 3,362-byte contract, up to four decision results, three handoff results,
a code map capped at 60 lines, and the task. It then requires the agent to recall and query again.
Remove that duplicate work when the injected payload is current and complete.
Recall selection is semantic, not an exact latest-record lookup. `cut -c1-600` truncates each line,
including the handoff's trailing next action/files; it is neither a total-byte nor token budget.
The `--check` path counts text matching stream name, displays at most 200 characters per line,
and exits successfully even with no matching handoff. It does not compare the record to HEAD.
A clean daemon response with an empty code map also gets no explicit missing-map diagnostic.
The contract's milestone handoff has no byte cap, timestamp, dirty-state marker, or exact retrieval key.
Its read-only-live-daemon wording conflicts with its required live `remember`; clarify authorized writes.

A better primitive already exists: `hook_handoff_prepare` prepares session metadata and
`hook_handoff_context` selects the latest version-1 capsule for matching project_dir and branch.
Stop extracts explicit Next/TODO and Blocker lines (400 bytes each), collects up to 20 dirty paths,
and queues a session_bind. SessionStart requests up to 100 session rows and renders the selected card.
The capsule includes source/session/thread IDs and saved_at; `verified` only means nonempty next_action.
It does NOT mean gates passed, the commit matches, or the action remains valid.
An empty newest capsule intentionally suppresses older state. That avoids resurrecting completed work,
but an unavailable/missing action can also erase useful continuation. The 100-row window needs auditing.
Paths are changed/untracked files, not committed artifacts; a clean committed step can show no artifacts.
Canonical physical paths must match across `/maps/projects` and `/projects` aliases and new worktrees.
The current schema lacks objective, acceptance criteria, constraints, evidence, HEAD, and running jobs.
Therefore the existing card cannot yet make a fresh lead as capable as the long session.

Accounting caveat: the ledger is useful triage, not yet a controlled before/after instrument.
Claude increments turns for every message containing usage without deduplicating request IDs.
Codex increments for tool calls/messages, while totals come from the last cumulative usage event.
Neither denominator is necessarily an API model request; parallel tools can inflate the Codex count.
The seven-day filter selects files by mtime and then reads their entire lifetime, not seven days of events.
Claude cache-creation tokens are counted but omitted from both cost and mean-context calculations.
Prices are fixed configurable defaults, not model-resolved billing; reported dollars remain list estimates.
Use event timestamps and deduplicated provider requests, preserve per-model usage and cache writes,
and distinguish context-at-last-request from lifetime averages before enforcing the thresholds below.
Do not count reasoning twice when the provider includes it in output tokens.

### 2. Four handoff options

Sizes below are proposed UTF-8 payload budgets unless explicitly described as measured.
English prose often costs roughly one token per four bytes; code/IDs can cost more. Tokenize to enforce.
Storage bytes, rendered prompt bytes, and per-request retransmission are different quantities.

| Option | Verdict | Bytes per handoff / fresh-thread survival | Failure modes and lead access |
|---|---|---|---|
| Semantic chitta handoff memories, as now | ADOPT as index; REJECT as sole state | Current format unbounded; target 0.5–1 KB pointer/summary. A committed memory survives thread exit, but top-k recall may not surface it. | Stale similar streams, ranking omissions, 600-character clipping, commit-to-remember crash gap, unavailable daemon. Lead can recall without thread access, but must verify exact stream, revision and HEAD. |
| Codex native history notes | MEASURE FIRST | Actual bytes and independent-thread transfer unknown here; experiment with a 2–4 KB export budget. Do not assume notes are imported by a new exec. | Client/version/auth dependence, opaque retention, unsaved crash loss, no established cross-client Fable interface. Require a documented bounded export before the lead relies on it. |
| Git-tracked HANDOFF.md per branch | REJECT as mandatory per-step channel; ADOPT only for durable human-reviewed decisions | Proposed 2–4 KB; committed content survives and is available in a fresh checkout of that branch. Uncommitted content survives only if the worktree survives. | Stale after code changes, conflicts/merge noise, missing pushes on other machines, crash before commit. Lead reads `git show branch:HANDOFF.md`; record the code commit being described, not a self-referential final commit hash. |
| Startup MCP call for exact capsule | ADOPT via existing ledger_op, preferably injected by launcher | Proposed 2–4 KB stream capsule; lead aggregate ≤6 KB. Survives if backed by acknowledged durable state; MCP itself supplies no persistence. | Wrong key, stale revision, queue lag, daemon outage, oversized response. Lead calls the same read API or launcher CLI, no transcript required. |

Choose one authoritative capsule plus optional semantic index and git evidence, not three competing summaries.
Use repository identity + stream ID as the stable key; include branch, worktree identity and code HEAD as checks.
For the lead, use a distinct lead-task key with a compact manifest of active streams and capsule revisions.
Always report missing/stale state explicitly. Read existing git status and evidence to recover; never guess “passed.”
Advance state at verified milestones, before handing off/waiting, and before restart—not only after commits.
Persist an in-progress capsule before a long mutation/job, then mark the outcome when known.
A crash can still lose work since the last acknowledged checkpoint; git diff is recovery evidence, not a summary.
Until exact retrieval exists, use existing ledger capsules plus an explicit lead manifest; do not label semantic recall reliable.

Suggested contract text (nine lines; keep the separately enforced safety/gate policy intact):
1. Start from the launcher's bounded capsule, task and policy version; do not re-fetch unchanged context.
2. Match repository, stream, branch and HEAD; absent/stale fields are unknown, never inherited as success.
3. Use the supplied code map; query or read targeted code when the map is missing or insufficient.
4. Save a ≤4 KB capsule: objective, constraints, state, HEAD/dirty paths, evidence, blockers, next action and jobs.
5. Checkpoint at each verified milestone, before background waits and before restart; require durable acknowledgement.
6. Keep code/evidence in git or referenced artifacts; optional semantic memory points to the exact capsule revision.
7. Batch independent tools, bound returned bytes, and await pushed job completion without model polling turns.
8. At 40 requests or 40k context checkpoint; restart by 60 requests, 60k context or 1M cumulative input tokens.
9. Finish with capsule revision, commits, gate status, numbers and next action; a fresh thread uses that capsule.

### 3. Lead protocol and expected savings

Define a turn for budgeting as one billed model request, not one shell command, transcript line or user turn.
Cost is sum(input_uncached × rate + cache_read × rate + cache_write × rate + output × rate).
Turns × mean context predicts input volume only; restart can reduce cache hits and add bootstrap costs.

ADOPT initial policy: aim for ≤30k mean total input/request, including system/tools and injected skills.
At the first of 40k current input or 30 requests, checkpoint and finish the bounded current operation.
Start fresh at the first of 60k input or 50 requests. Use the last request's usage, not lifetime averages.
If unavoidable static context alone exceeds the cap, reduce tool/instruction exposure first and report that floor.
Compact once as a temporary bridge when unfinished reasoning would be costly to reconstruct; save capsule first.
Compaction must restore context below 30k; otherwise start fresh. Never use compaction to justify 5,000 more turns.
An automatic reminder is advisory, not proof of persistence or enforcement. Roll out these thresholds as a measured pilot.

Session-start addition: ≤1,500 tokens (roughly 6 KB), hard max 2,000, beyond unavoidable platform policy.
Allocate 200 objective/acceptance, 200 constraints/authorization, 450 active-stream manifest,
250 evidence/decisions, 200 blockers/next actions and 200 job handles/token budget.
Each manifest row gives stream, branch/HEAD, status, capsule revision and one next action.
Load detailed stream capsules only when acting on that stream; do not inject every past decision or all tool docs.
Save owner corrections, rejected approaches with one-line reasons, and unresolved assumptions explicitly.
Fresh-session capability requires those facts, not an exhaustive narrative of how the lead discovered them.

Waiting: launch jobs once with durable job IDs and result-file paths; use pushed completion events or scheduler dependencies.
Wait in the orchestration layer, not a model loop. If no callback exists, suspend/end the session and have an external
watcher emit one result on completion or one timeout/failure event. Never repeatedly ask `status`, `sleep`, or “still running?”.
A tool that yields every few seconds followed by another model-driven poll defeats “sbatch --wait” in practice.
Batch independent completions into one manifest update. A long external wait should not require reasoning keepalives.

ADOPT selective delegation: Haiku for bounded extraction, classification and status-to-table work with exact checks;
Sonnet for localized implementation/review with clear acceptance tests; lead for ambiguity, tradeoffs and integration.
Use a task-only capsule, never a full-history fork. Return ≤300 tokens plus artifact/evidence pointers.
Delegate when ≥3 lead requests or >2k tokens of mechanical output are likely, and the task has an objective checker.
Escalate after one failed attempt, rather than spawning a cheap-agent retry storm.
These routing rules are hypotheses; no current Haiku/Sonnet price or quality advantage was independently established here.
Count child input/output, reviews and rework in savings. Do not reduce lead cost by merely moving it off its ledger.

Cap normal lead visible output at 500 tokens/request, status messages at 150, and final synthesis at 1,000.
Put long requested artifacts in files. Permit explicit task-specific exceptions; do not truncate required reasoning/evidence.
Provider output includes hidden reasoning where applicable: separately meter total generated tokens; visible caps alone
cannot guarantee the output savings below. Treat 500 as the target mean billable output for the scenario, not a proven result.

Using the supplied baseline: 61,636 × 343k ≈21.141B input; output/request ≈84M/61,636 =1,363 tokens.
Context-only at 30k: input fraction 30/343 =0.0875 (91.25% saved), before cold-cache effects.
Halving requests at equal successful work: input fraction 0.5×30/343 =0.0437 (95.63% saved).
At 500 output/request and half the requests: output fraction 0.5×500/1,363 =0.1834 (81.66% saved).
Using ledger prices on the supplied cached/output totals: 21.2B×$1.5/M +84M×$75/M =$38,100.
Combined modeled subtotal: $31,800×0.0437 +$6,300×0.1834 ≈$2,546, or 6.68% remaining (93.32% saved).
Without reducing requests: about $5,092, or 13.36% remaining (86.64% saved).
These exclude baseline uncached/cache-write corrections, added child work, retries and cache misses.
At 30,818 requests and 50/request sessions, ≥617 startups ×1,500 tokens ≈0.926M bootstrap tokens;
if additional and wholly uncached at $15/M, that is ≈$14. Larger cold prefixes cost more.
For sensitivity, an extra 1B tokens billed uncached rather than cached adds $13,500 at those defaults.
The existing 150k target saves only 1−150/343 =56.3% of input at unchanged request count; it leaves large avoidable cost.
Reject a guaranteed dollar-saving claim until equal-work quality, actual cache mix and request counts are measured.

### 4. Astra protocol

ADOPT low effort for mechanical edits/format transformations with exact checks; medium for docs, bounded scripts,
measurement analysis and localized implementation; high for uncertain concurrency, storage and cross-language changes.
Use xhigh/ultra only for a named unresolved problem with a capped experiment; not an unattended default.
The launcher currently accepts only low/medium/high, defaults to high, and takes its model from env or gpt-6-astra.
The inspected user config says gpt-5.6-sol/xhigh; that is not proof of the model/effort used by launcher invocations.
Report resolved runtime model and usage. Do not infer actual inference cost from a requested effort label.

Target 15–25 billed requests per bounded implementation stream; start checkpointing at 40, restart by 60.
Also checkpoint at 40k current context and restart by 60k or 1M cumulative input, whichever comes first.
Cap visible output at 600 tokens/request, progress at 150 and final at 800; artifact creation gets a declared exception.
Tool results ≤4 KB combined per request by default; line caps alone do not bound huge JSON lines.
Count a many-tool parallel batch as one request if that is how the provider bills it, but still cap total returned bytes.
REJECT 150 tool calls as the primary stop: at one call/request and 41k context it already permits 6.15M input.
At 60k it permits 9M; parallel calls and tool-free reasoning make the proxy inconsistent in either direction.
Keep a tool-loop watchdog only as a secondary fault detector. Split at natural milestones, not arbitrary tiny checkpoints.

Worked target: 20 requests ×25k mean input =500k input; ≤12k visible output at the 600-token cap.
Against supplied 0.8–1.8M input-token streams, input is 62.5%–27.8% of baseline (37.5%–72.2% saved).
If those baseline totals include output, compare like-for-like: illustrative total 512k is 64.0%–28.4%.
At ledger Astra rates $2/$0.5/$8 per M and 95% cache hits, 500k input +12k total billed output costs ≈$0.384.
At 80% cache hits it costs ≈$0.496; wholly uncached ≈$1.096. Hidden output beyond 12k increases each estimate.
With the same 95% cache ratio and 12k output, 0.8–1.8M input would cost $0.556–$1.131.
Those are pricing scenarios, not measured bills or a forecast of high-effort reasoning usage.
The small average 20,840/2,300 ≈9.1 recorded turns/session does not disprove large implementation tails;
repair the denominator and stratify by task shape, including failed/resumed streams, before interpreting it.

### 5. Smallest build and the deciding measurement

ADOPT an extension of existing ledger metadata, not another core MCP tool or semantic-memory subsystem.
Add bounded explicit capsule fields and monotonically increasing revision; make “complete”, “in progress”,
“missing” and “invalidated” distinct states. Include timestamp, code HEAD, dirty state, gate evidence and job handles.
Use compare-and-set revision checks to prevent late writers overwriting newer state; canonicalize repository identity.
Expose exact latest-by-key through the existing ledger gateway and have the launcher inject the same bounded response
used by the lead. Verify durable write/read acknowledgement before ending a thread; Stop remains a fallback checkpoint.
If persistence is unavailable, return an explicit error and preserve existing on-disk work; do not falsely report saved state.
No automatic transcript summarizer is necessary for this first version. The agent knows its objective and next action.
Improve `--check` to fail for missing/stale/HEAD-mismatched records and show a bounded lead-facing manifest.
Replace per-line truncation with structured field limits and a total response-token budget.
Fix ledger request/time-window accounting alongside this, or the experiment cannot establish savings.

MEASURE FIRST: paired replay of the same 20 representative tasks from identical repo/evidence snapshots.
Arm A resumes the long session; arm B starts fresh with ≤1,500-token lead capsule plus targeted retrieval.
Same runtime model/effort/tools/gates; vary ordering, exclude cross-arm memory writes, pin task truth.
Include interrupted work, failed gates, dirty trees, stale/missing capsules and background jobs in the 20-task set.
Record gate-defined task success and total input/output/cache-write tokens across lead, children, bootstrap and retries;
also record recovery errors, requests, wall time and modeled cost. No cheaper arm may skip required checks.
Decision rule: B succeeds on every task A succeeds on, introduces zero false completion claims, and consumes ≤25%
of A's aggregate billed input tokens; total modeled cost must also fall. This is a pilot criterion, not statistical proof.
If parity fails, identify the missing capsule field before enlarging the capsule or reviving a whole-thread resume.
Native-notes follow-up uses the same suite and explicitly tests new exec versus resume, post-crash persistence,
export size and Fable-readable access. Adopt only if parity and total-cost results beat the explicit capsule.

### Native-docs verification and limits

The three requested keys—`use_history_notes_extension`, `reminder_threshold_tokens`,
`reminder_message_template`—were absent from the inspected `~/.codex/config.toml` and worktree search.
The fetched official reference also did not expose those exact keys. This does not prove no private/profile override exists.
The local executable resolves to a dated custom installation; current web documentation may describe a different build.
Official [Codex configuration reference](https://developers.openai.com/codex/config-reference/) describes
`features.context_management.experimental_mode` as off by default, using notes and searchable history,
and requiring ChatGPT sign-in on Plus, Pro or Pro Lite. It does not establish portable fresh-thread capsule transfer.
The same reference documents `model_auto_compact_token_limit` and scope `total` versus `body_after_prefix`;
prefer total when testing an absolute context budget. Verify support in the deployed binary before changing config.
No config was changed, no history-note persistence experiment was run, and native handoff byte size remains unknown.
