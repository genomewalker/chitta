# Automatic-learning experiment protocol

Frozen rules: [decision of 2026-09-15](../../docs/DECISION-2026-09-15-learning-experiment.md).
This harness builds evidence; only the orchestrator makes the official cut and runs the panel.

## Selection and chronology

1. Make one immutable **copy of the eval replica family**, using the selector and
   copy rules in `scripts/eval-replica.sh`. Retain manifests, selected snapshot,
   sidecars, uncovered WAL segments and loader markers. Never open the original
   eval directory with another daemon. The store takes an exclusive directory lock.
2. Start a quiescent **probe clone** with the unchanged replica launcher, using
   the immutable copy as `CHITTA_LIVE_MIND` (the launcher's source variable).
   Use a private `CHITTA_EVAL_MIND`, HOME, port and runtime directory.
3. Capture the cut timestamp and fully hashed store family manifest in
   `cohort-draft.json` (schema 2). Do not enumerate or classify historical writers.
   Everything created at or before the cut is baseline in both arms. `--source`
   binds the probe's selected family to the immutable cut copy.
4. Collect the **next 20 eligible graded prompts from real sessions after the cut**,
   in timestamp order. Record every skipped prompt and exclusion reason in
   `config.selection_exclusions`. Do not choose prompts based on recall or outcomes.
5. Each prompt must be self-contained. Freeze prompt text, transcript SHA256,
   prompt timestamp (epoch milliseconds), initial commit SHA, hashes/modes of
   every initial file, dependencies, a deterministic grader, known-good commit,
   and a selection note. Reconstruct uncommitted changes into an initial commit;
   otherwise exclude. Exclude missing conversation context, unavailable dependencies,
   unreconstructable state and graders that cannot prove pre-fail/post-pass.
6. Validate every grader on an isolated worktree of the initial commit (ordinary
   nonzero exit required) and known-good commit (exit zero required). A timeout,
   missing known-good ref, changed payload or missing validation blocks freezing.
   Validation is sealed to the complete task payload; edits require revalidation.
7. At task freeze, inspect a quiescent probe of a later immutable snapshot via
   `freeze --socket ... --source ...`. Retain the original `cut_store` and cut
   timestamp, and the inspected task source as `store`. Freeze each task's
   `eligible_cohort_ids` and `future_ids` in `tasks.json`; duplicate those lists,
   timestamps and source digest in `manifest.task_cohorts[TASK_ID]`. Seal tasks
   and cohort evidence with SHA256s. Loading recomputes membership and checks
   each manifest entry. Refuse any task timestamp at or before the cut.
   The panel has three trials from the outset: 20 × 3 × 2 = **120 model runs**.
   No outcome-dependent task replacement or extra trials.

Creation-time filtering alone is insufficient: distillation can strengthen old
memories. All accessible memory content, weights and graph state come from the
same pre-task source family. A changing source or historical reconstruction
without the complete pre-task state cannot support a verdict.

## Exact provenance queries and classifications

The code uses the existing newline JSON-RPC `tools/call` endpoint on the explicit
Unix socket. For example:

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"query_graph","arguments":{"object":"distillation"}}}
```

Queries, in order:

- `health_check {}`: bind the responding PID's `/proc/PID/cmdline --path`
  to the store. The current health response does not include `db_path`.
- Official freeze: `list_memories_brief {"realm":"","limit":100,"offset":N}`
  inventories **all realms**, because hooks can use global fallback and graph
  neighbors. `inventory_scope=all` is required for official artifacts. Live
  diagnostics remain scoped to `project:cc-soul` for a comparable count,
  incrementing N by the returned count **until an empty page**. The daemon caps
  page sizes. Duplicate IDs or a changed final enumeration invalidate the cut.
- For every enumerated ID, `memory_provenance {"id":ID}` supplies a required
  creation timestamp. Use lossless JSON integers and validate the returned ID
  (the live string-ID parser fails above 2^63). No timestamp means a hard error.
- Records created at or before the cut retain only ID, kind and timestamp.
  **Do not query or classify their writer provenance.**
- For post-cut records, `query_graph {"subject":"ID"}` supplies exact writer
  triplets; `tagged=distillation` is not writer evidence. For `derived_from`,
  retrieve each exact parent with `get {"id":"PARENT"}` and validate episode
  provenance. Retain triplets, parents, metadata and content SHA256.
- Repeat family selection/manifests/file-size checks and enumeration. Full
  source hashes and stable enumeration are mandatory for task freeze. Live
  `--dry-run --cut-timestamp-ms ...` inspection is diagnostic only; it records
  the current manifest, never invents a historical cut manifest.

Post-cut classification precedence:

| Evidence | Treatment |
| --- | --- |
| Correction or episode | Explicit/preserved; stays in both arms. |
| Conflicting source triplets, or non-distillation source plus derived_from | Hard error: unresolved. |
| Exact source=distillation (also with derived_from lineage) | Automatic queue/observe admission; the labelled bash distiller may emit both. |
| Another explicit source, including mcp_tool or hook_regex | Explicit write; stays in both arms. |
| ingested_from | Separate ingester; stays in both arms. |
| derived_from → existing episode, without another source or ingested_from | Automatic native distiller, including operational value facts at native_distiller.cpp:326 and unexpected output kinds. |
| Explicit [artifact]/[done] hook signal without another writer | Stays in both arms. |
| No writer provenance | Hard error, listing every ID and kind; fix the unlabelled writer. |
| Missing or non-episode derived parent | Hard error: unresolved. |

For task timestamp T, the treatment is **automatic records created in (cut,T]**.
Every record created after T, including explicit writes, is future information
and excluded from both arms. Post-cut unlabelled records block freeze even if
later than every task; they are never silently removed as a population. There
is no global historical cohort or ambiguous-record exclusion. Old schema-1
freezes must be regenerated.

## Trial execution and isolation

Each paired task/trial randomizes A/B order from the manifest seed, independently
of outcomes. Each arm starts a new clone with `scripts/eval-replica.sh`:

- **Both arms:** remove the task's `future_ids` with `forget`, then verify
  exact-ID absence and every exclusion lane; save `future-exclusion.json`.
- **A:** retain baseline, explicit writes through T, and the task's automatic
  cohort; verify each eligible cohort ID still resolves.
- **B:** additionally remove only the task's `eligible_cohort_ids` with `forget`.
  Save `exclusion.json`. For both exclusion sets, require each exact `get`
  to return the daemon's explicit “Memory not found” response. Transport errors
  are failures, never absence. Query the task prompt through fused
  `recall(strategy=hybrid)`, graph `recall_spreading`, correction-tag recall,
  and `correction_check`; no removed ID may surface in any response.

Pin `CHITTA_REALM=project:cc-soul`, `CHITTA_UTILITY_RECALL=0` and
`CHITTA_NO_QUEUE=1`; omit **both** `CHITTA_HEADLESS` and `CC_SOUL_HEADLESS`.
Each trial has private HOME, DB/ledger, queue, runtime, socket and port.
Hook writes to queues are private even when daemon queue consumption is disabled.
Recall mutations are discarded with each trial. The source is hashed before and
after the run. Cleanup only removes a clone after its daemon has stopped.

Fresh shallow repositories fetch only each initial commit; their worktrees
retain the exact initial SHA without shared objects, alternates or future refs.
Graders and known-good objects are outside the model filesystem and graders are
materialized only after agent execution. Grader exit zero alone defines success;
Bash command credit in the outcome ledger never grades a task.

Default `--isolation=strict` requires bubblewrap [13](#ref-13) filesystem/PID isolation and Landlock [12](#ref-12) network
ABI ≥4. Only task files, fresh HOME/state, pinned runtime dependencies, system
runtime files and the restricted scratch RPC socket are visible. TCP connects
are limited to HTTPS port 443, preventing access to live daemon/MCP ports.
The original scratch socket and its raw snapshots are hidden. An RPC broker
permits recall/session-hook operations but rejects transcript/code-file readers,
imports, predicate execution, maintenance and process control. A strict trial-only MCP configuration and empty settings sources prevent live
MCP/plugin inheritance. The sole chitta stdio bridge health-checks the private
broker; it exposes no model tools, keeping recall in instrumented hooks.
Credentials, if needed, come from explicit API-key/OAuth environment variables;
no user Claude config or project transcripts are copied.

`runtime_roots` must list specific dependency directories (for example the
Python environment); their trees are hashed. Mounts exposing live HOME/mind,
the harness checkout, task repositories or the source replica are rejected.
The manifest pins model name, Claude version and executable, Python executable,
chitta/chittad binaries, embedding model, hooks/harness code, max turns,
dollar budget and timeout. A pin mismatch refuses execution.

`--dry-run` performs clone/start/ablation, real prompt and Bash hooks, private
worktrees, fixture edits, stub exits, hidden grading, capture and reporting.
The fixture explicitly supplies edits and exit codes, never grader answers read
during execution. Dry runs attempt the OS-isolation preflight and record its
failure if unavailable; their trusted local stub can exercise the rest of the
pipeline on that host. **They never certify model isolation or issue a verdict.**
Strict model runs fail before starting if that preflight fails.

### Home-audit screening on kernel 4.18

`freeze --isolation=home-audit` pins the accepted screening mode.
`run --isolation=home-audit` must agree with the frozen manifest; strict remains
the default. HOME, XDG config/data/cache/state/runtime, CLAUDE_CONFIG_DIR and
TMPDIR are private for every trial. Generated settings live in CLAUDE_CONFIG_DIR
and deny Read/Edit/Write/Glob/Grep/Bash targets outside trial-visible roots,
including live mind and project transcripts. File deny patterns use Claude
absolute `//path` syntax. PreToolUse also checks new paths and symlink escapes.
The manifest pins `allowed_tools` to Bash, Read, Edit, Write, Glob, Grep and
`permission_mode` to `dontAsk`; no bypass mode is used. CHITTA_SOCKET_PATH is
replaced by the private restricted broker socket; no inherited socket discovery.

Every tool attempt is captured before execution, including permission-denied
attempts. The shadow log retains payload, audit decision and hook output.
The hook also records Bash command history as JSONL under private HISTFILE.
Post-hoc audit joins that history and tool events. Outside-root access, live
socket/loopback connection attempts, missing evidence and audit-control writes
void the trial with `voided_reason`. Shell interpreters, scripts, substitutions,
compound commands and unknown executables are unverifiable and also void a trial.
Only simple pwd/ls/cat/head/tail/wc/true/false commands pass this conservative
audit. This limits tasks that require agent-side test execution; hidden graders
still run normally after the agent exits.

Home-audit is permission enforcement plus evidence checking, without kernel
filesystem/network isolation or protection from every client/runtime side effect.
Every verdict carries that screening caveat. A dry-run tests the trusted stub,
not Claude permission enforcement. See the upstream
[permission semantics](https://code.claude.com/docs/en/permissions) [68](#ref-68).

## Telemetry, scoring and verdict

Retain actual hook stdout/stderr and the hook-written
`CHITTA_DB_PATH/outcome_ledger.jsonl` for each trial. Join every prompt hook to an
`injected` or `recall_empty` ledger event by session and timestamp. Complete
rendered ID headers must agree with ledger IDs; a truncated number is not a new
ID. Include exposure through post-tool hook output. Missing captures, incomplete
joins or disagreement invalidate telemetry. A genuinely empty recall is a
reported outcome, not an error.

Report every paired result, grader/agent failures, cohort/total injections, empty
turns, lane failures, and median/p95 recall-lane and hook latency. At least one
A turn **for each task** must inject that task's cohort ID; B must have exactly
zero cohort exposure. Future-record injections must be zero in both arms, with
verified exclusions. Per-task report rows retain lists and A/B exposure counts;
one task's A exposure cannot validate another task.

For binary grader success s:

```text
Δ = Σtask (mean_trial s[A] − mean_trial s[B])
```

With three trials, **retain automatic admission iff Δ ≥ 3**, equivalently nine
net A successes across 60 attempts per arm. Otherwise a valid result retires
**the tested automatic writers**, including queue admission. Use the integer
net-success threshold to avoid floating-point boundary errors.

Print **NO VERDICT** for fixture/dry runs, wrong panel/trial count, missing or
duplicate outcomes, unresolved/inconsistent provenance, changed source/pins,
invalid isolation, missing grader outcomes, execution/telemetry failures,
unconfirmed A exposure or any B exposure/exclusion failure. An ordinary model
attempt that fails the grader is a scored zero; timeout/transport/missing
result envelopes are recorded separately as invalid execution.

## Commands

Use the pinned CPython; plain system Python may be too old.

```bash
PY=/maps/projects/fernandezguerra/apps/opt/conda/envs/bioinfo/bin/python3

# Diagnostic only. No official cut; read-only RPCs, scratch output.
"$PY" benchmarks/learning/freeze.py cohort --socket "$LIVE_SOCKET" \
  --realm project:cc-soul --dry-run --cut-timestamp-ms 1789430400000 \
  --out /tmp/learning-cohort-preview.json

# Orchestrator only, after starting a probe clone of an immutable COPY:
"$PY" benchmarks/learning/freeze.py cohort --socket "$PROBE_SOCKET" \
  --source "$FROZEN_COPY" --realm project:cc-soul --out cohort-draft.json

"$PY" benchmarks/learning/freeze.py task add --entry entry.json --tasks tasks-draft.json
"$PY" benchmarks/learning/freeze.py task validate --tasks tasks-draft.json
bash scripts/eval-learning.sh freeze --tasks tasks-draft.json \
  --socket "$TASK_PROBE_SOCKET" --source "$TASK_FROZEN_COPY" \
  --cohort cohort-draft.json --config config.json --out frozen
bash scripts/eval-learning.sh run --manifest frozen/manifest.json \
  --trials 3 # output defaults outside this checkout
bash scripts/eval-learning.sh report /projects/caeg/scratch/kbd606/tmp/learning-results/RUN
```

A task entry uses `grader: {"command":["/bin/sh",".learning-grader/check.sh"],
"files":{"check.sh":"..."},"timeout_s":60}`; file keys are relative to the hidden
grader directory. `repo`, `cwd_sha`, `known_good`, `prompt`,
`prompt_timestamp_ms`, `transcript_sha256` (or `transcript` file),
`selection_note`, `selection_rule:"next_eligible_graded_prompt"`,
`recall_inspected:false`, and `dependencies:[{"path":"...","sha256":"..."}]`
are required. `task add` derives initial file hashes from the committed tree.

Required config keys: `model`, `claude_version`, `claude_bin`, `chitta_bin`,
`chittad_bin`, `embed_model`, `max_turns`, `budget_usd`, `timeout_s`, `seed`.
Also declare `runtime_roots`, `selection_exclusions`, and `ld_library_path`
when needed. Use an exact model identifier and absolute executable/dependency paths.

### Reproduce fixture evidence (no model calls)

```bash
"$PY" benchmarks/learning/prepare_fixture.py --out /tmp/learning-fixture-NEW \
  --isolation=home-audit --void-trial \
  --source /projects/caeg/scratch/kbd606/tmp/chitta-eval-mind \
  --chitta-bin /home/kbd606/.claude/bin/chitta \
  --chittad-bin /home/kbd606/.claude/bin/chittad \
  --claude-bin /home/kbd606/.local/bin/claude \
  --embed-model /maps/projects/caeg/people/kbd606/models/nomic-embed-text-v1.5.gguf
bash scripts/eval-learning.sh run --manifest /tmp/learning-fixture-NEW/frozen/manifest.json \
  --isolation=home-audit --dry-run --trials 2
"$PY" -m unittest discover benchmarks/learning/tests
```

The fixture builder seeds two **synthetic** source=distillation records only on a
scratch copy. Its two-record cohort is marked fixture-only and cannot certify
an official panel. It never freezes the official cohort.


Voided trials are missing observations: their success is null, tables show
successes/observed and missing counts, and paired task/overall deltas stay null
when a pair is incomplete. They never count as grader failures or support a verdict.

Runner output defaults to `$CHITTA_LEARNING_OUT/run-TIMESTAMP`, with the base
`/projects/caeg/scratch/kbd606/tmp/learning-results` when unset. Explicit `--out`
is supported. `benchmarks/learning/results/**` is ignored and untracked; retain
only the compact `evidence/fixture-2026-09-15.md` in git. Fixture `--void-trial`
injects one denied out-of-root Read attempt (saffron trial 2 A).

<!-- BEGIN CITATIONS -->
## References

- <a id="ref-12"></a>**[12]** Linux kernel contributors. Landlock: unprivileged access control. Linux userspace API documentation (accessed 2026-09-16). [source](<https://www.kernel.org/doc/html/latest/userspace-api/landlock.html>)
- <a id="ref-13"></a>**[13]** bubblewrap contributors. bubblewrap: Low-level unprivileged sandboxing tool used by Flatpak and similar projects. Project README (accessed 2026-09-16). [source](<https://github.com/containers/bubblewrap>)
- <a id="ref-68"></a>**[68]** Anthropic. Configure permissions. Claude Code documentation (accessed 2026-09-16). [source](<https://code.claude.com/docs/en/permissions>)
<!-- END CITATIONS -->
