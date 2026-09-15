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
3. Enumerate provenance, without task recall inspection. The cut is the timestamp
   recorded when `cohort.json` is produced. `--source` binds the probe's selected
   family to the immutable source; stopping a probe must never replace that source.
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
7. Freeze `tasks.json`, `cohort.json` and their SHA256s in `manifest.json`.
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
- `list_memories_brief {"realm":"project:cc-soul","limit":100,"offset":N}`,
  incrementing N by the returned count **until an empty page**. The daemon caps
  page sizes. Duplicate IDs or a changed final enumeration invalidate the cut.
- `query_graph {"object":"distillation"}`: retain only triplets whose predicate
  is exactly `source`; `tagged=distillation` does not establish a writer.
- For **every** enumerated ID: `query_graph {"subject":"ID"}` and
  `memory_provenance {"id":"ID"}`. Keep IDs as decimal strings; never round u64
  identifiers through floating-point JSON tools.
- For every `derived_from` object: `get {"id":"PARENT"}`, requiring an existing
  parent whose type is `episode`. Keep the parent record with the child's evidence.
- Repeat family selection/manifests/file-size checks and enumeration. Full
  source-file SHA256s are required for freezing. A live `--dry-run` diagnostic
  hashes manifests, records selected file sizes, and is explicitly ineligible.

Classification precedence:

| Evidence | Treatment |
| --- | --- |
| Kind correction or episode | Exclude: preserved in both arms, regardless of writer. |
| Conflicting source triplets, or source plus derived_from | Unresolved. |
| Exact source=distillation | Include queue automatic admission, across kinds other than the preserved kinds. |
| Another explicit source (including mcp_tool) | Exclude, recording that source. |
| ingested_from | Exclude the separate ingester writer; its derived_from is not native-distiller evidence. |
| derived_from → existing episode; no other source or ingested_from; kind wisdom, belief, preference or milestone | Include native learning. These are the kinds emitted by native_distiller.cpp:45/185. |
| Same native episode provenance; kind operational | Exclude deterministic value facts (native_distiller.cpp:316). |
| Missing/non-episode parent, unexpected derived kind, or no writer provenance | Unresolved; never infer authorship from kind, text, age or a tag. |

Every record includes its reason, triplets, parent evidence, content hash and
available provenance metadata. Some live `memory_provenance` replies contain
malformed JSON; this supplemental endpoint's failure is retained verbatim in
the diagnostic. Membership still requires the independently available exact
triplets and memory/parent records. An unavailable required query fails the
enumeration. **Any unresolved classification blocks freeze and verdict.**

Per-task eligible ID lists are frozen with the task panel. In this prospective,
single-realm design every task uses the same complete eligible cohort. The
classifier does not silently omit unresolved records or expand the treatment
to ingestion, explicit memories, corrections, episodes or value facts.

## Trial execution and isolation

Each paired task/trial randomizes A/B order from the manifest seed, independently
of outcomes. Each arm starts a new clone with `scripts/eval-replica.sh`:

- **A:** complete source state, including cohort.
- **B:** soft-delete every cohort ID with `forget`. Require each exact `get`
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

Model execution requires bubblewrap filesystem/PID isolation and Landlock network
ABI ≥4. Only task files, fresh HOME/state, pinned runtime dependencies, system
runtime files and the restricted scratch RPC socket are visible. TCP connects
are limited to HTTPS port 443, preventing access to live daemon/MCP ports.
The original scratch socket and its raw snapshots are hidden. An RPC broker
permits recall/session-hook operations but rejects transcript/code-file readers,
imports, predicate execution, maintenance and process control. Strict empty MCP
configuration and empty settings sources prevent live MCP/plugin inheritance.
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
Real model runs fail before starting if that preflight fails.

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
A turn must inject a cohort ID; B must have exactly zero cohort exposure.

For binary grader success s:

```text
Δ = Σtask (mean_trial s[A] − mean_trial s[B])
```

With three trials, **retain automatic admission iff Δ ≥ 3**, equivalently nine
net A successes across 60 attempts per arm. Otherwise a valid result retires
**both tested free-form writers**, including queue admission. Use the integer
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
  --realm project:cc-soul --dry-run --out /tmp/learning-cohort-preview.json

# Orchestrator only, after starting a probe clone of an immutable COPY:
"$PY" benchmarks/learning/freeze.py cohort --socket "$PROBE_SOCKET" \
  --source "$FROZEN_COPY" --realm project:cc-soul --out cohort-draft.json

"$PY" benchmarks/learning/freeze.py task add --entry entry.json --tasks tasks-draft.json
"$PY" benchmarks/learning/freeze.py task validate --tasks tasks-draft.json
bash scripts/eval-learning.sh freeze --tasks tasks-draft.json \
  --cohort cohort-draft.json --config config.json --out frozen
bash scripts/eval-learning.sh run --manifest frozen/manifest.json \
  --out benchmarks/learning/results/RUN --trials 3
bash scripts/eval-learning.sh report benchmarks/learning/results/RUN
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
  --source /projects/caeg/scratch/kbd606/tmp/chitta-eval-mind \
  --chitta-bin /home/kbd606/.claude/bin/chitta \
  --chittad-bin /home/kbd606/.claude/bin/chittad \
  --claude-bin /home/kbd606/.local/bin/claude \
  --embed-model /maps/projects/caeg/people/kbd606/models/nomic-embed-text-v1.5.gguf
bash scripts/eval-learning.sh run --manifest /tmp/learning-fixture-NEW/frozen/manifest.json \
  --out benchmarks/learning/results/fixture-NEW --dry-run --trials 2
"$PY" -m unittest discover benchmarks/learning/tests
```

The fixture builder seeds two **synthetic** source=distillation records only on a
scratch copy. Its two-record cohort is marked fixture-only and cannot certify
an official panel. It never freezes the official cohort.

