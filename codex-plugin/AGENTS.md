# chitta

## Hard constraints — read these first

These are first on the page deliberately. Codex truncates silently once the
combined `AGENTS.md` bytes pass `project_doc_max_bytes`, and what gets dropped is
the end of the file (#13386 [42](#ref-42), #37956 [43](#ref-43)). Anything that must survive lives up here.

1. **Never `pkill` the `--http` MCP process.** It is your own transport on port
   9481. SIGTERM reads as a clean exit, so `Restart=on-failure` won't revive it —
   port 9481 stayed dead 2026-09-02 to 09-08. Recovery belongs to the orchestrator.
2. **Work in your own git worktree, never on `main`.** One worktree per stream.
3. **`install`, never `cp`, over a running binary** — `cp` gives ETXTBSY.
4. **The worktree files are the state, not your context.** Do not rely on Astra's
   history-notes or context management to carry state across a headless run
   (#43194 [48](#ref-48), #43335 [49](#ref-49), #42449 [50](#ref-50)). The spec plus `Plan.md` and `Documentation.md` in the
   worktree are the source of truth; record decisions and progress there before ending the run.
5. **Keep tool output short.** `head`/`tail`/`--quiet`, never `cat` a large log.
   Astra tool loops can re-inject accumulated output — one run reached 3.5M tokens
   in 11 minutes (#44305 [51](#ref-51)).
6. **Redirect stdin.** `codex exec` blocks on an open stdin ("Reading additional
   input from stdin…"), so `</dev/null` or detach.
7. **Paste the contents of `~/.claude/agent_safety_preamble.md`** (you cannot
   resolve `@` imports) at the top of any agent prompt you spawn.
8. **Streams never install, restart, deploy, or touch `~/.claude`/`~/.codex`.**
   Commit on your branch; the orchestrator deploys after review.

> Status as of 2026-09-13: **canonical for Codex.** `CLAUDE.md` is canonical for
> Claude Code; the git-root `AGENTS.md` is a short pointer here. Codex loads
> global config, then git-root, then cwd, closer files overriding. Sources at the
> end of this file and in `docs/HOOKS.md`.

## Your job here

You are Codex on `gpt-6-astra`, running an implementation stream from a written
spec. A Claude Fable 5.1 session orchestrates, reviews your diff, and decides what
merges. Leave the branch reviewable: small commits, clear message, no unrelated
churn.

```bash
codex exec -C /projects/caeg/scratch/kbd606/tmp/codex-wt-<name> \
  --approve-for-me --skip-git-repo-check \
  -m gpt-6-astra -c model_reasoning_effort=high \
  -o /path/to/last-message.txt "<spec>" </dev/null
```

- **Effort:** use the model and effort assigned by the task (`high` is the
  default for implementation). Do not change them or delegate merely because
  another setting is available; never `ultra` unattended — it silently spawns
  sub-agents on other models, a cost and audit black box.
- **Approvals:** `--approve-for-me` works in **this** dev build (verified in
  `codex exec --help`) but is absent from the public docs; fall back to `-a never`.
  It implies the workspace-write sandbox and cannot be combined with `-s/--sandbox`
  — the parser rejects the pair. `approval_policy = "untrusted"` hangs without a
  TTY, so never use it headless. If you pass `-a/--ask-for-approval` it must come
  **before** `exec` (#26602 [45](#ref-45)). Sandbox modes: `workspace-write`, `read-only`,
  `danger-full-access`.
- **Getting the result out:** `-o/--output-last-message FILE` is the robust way to
  capture the final message; `--json` emits events as JSONL for tracking. Parsing
  the terminal transcript is not reliable.

## Reaching chitta

Your transport is the **HTTP** MCP unit: `chitta-mcp-http.service` on port 9481,
`[mcp_servers.chitta]` url `http://127.0.0.1:9481/mcp/`. chitta-bridge is on 7681.
Tools are namespaced `mcp__chitta__*`; `grow` and `connect` sit behind the
`advanced` gateway. Hooks arrive via `scripts/configure-codex-hooks.sh` and the
Codex plugin cache, owned by `scripts/sync-installed-hooks.sh`.

Two known MCP defects worth checking before blaming chitta: `bearer_token_env_var`
does not always propagate (#41378 [46](#ref-46)) — verify the token actually reached the server —
and streamable-HTTP sessions leak (#41600 [47](#ref-47)); an idle-session timeout on
`chitta-mcp-http` is a proposed mitigation.

**Your Bash exit codes log as unknown, not success.** Your `PostToolUse` carries no
exit code and `PostToolUseFailure` never fires (#34289 [44](#ref-44)), so
`hooks/post-bash-hook.sh:44` detects your shape (`tool_response` is a string, not
an object) and records `"exit_code": null` plus `"likely_fail": true` when the
output reads like a failure. Consumers never treat null as success. That flag is
text matching, not authority — state failures explicitly in your own report.

## Working in a worktree (what a stream may do)

If the task supplies a worktree and branch, reuse them. Confirm `pwd`,
`git branch --show-current`, and `git status --short` before editing. Create a
worktree only when one was not supplied. Hook-only changes need no native build.

Bootstrap when needed:

```bash
git worktree add -b <branch> /projects/caeg/scratch/kbd606/tmp/codex-wt-<name> main
cd /projects/caeg/scratch/kbd606/tmp/codex-wt-<name> && git submodule update --init chitta-field
# Rust first (only if chitta-field/ changed); the wrapper pins CPython and the embed identity
export LIBRARY_PATH=/maps/projects/fernandezguerra/apps/opt/conda/envs/bioinfo/lib:$LIBRARY_PATH
export LD_LIBRARY_PATH=/maps/projects/fernandezguerra/apps/opt/conda/envs/bioinfo/lib:$LD_LIBRARY_PATH
(cd chitta-field && ./build.sh build --release)   # then verify target/release/.chitta-embed-identity is unchanged
# C++ (only if chitta/ or chitta-field/ changed), built INSIDE the worktree:
cmake -S chitta -B chitta/build -DBLAS_openblas_LIBRARY=/maps/projects/fernandezguerra/apps/opt/conda/envs/bioinfo/lib/libopenblas.so \
  -DBLAS_LIBRARIES=/maps/projects/fernandezguerra/apps/opt/conda/envs/bioinfo/lib/libopenblas.so \
  -DCHITTA_EMBED_DIM=768 -DCHITTA_WITH_LLAMA_CPP=ON -DCHITTA_BUILD_RPC=ON -DCHITTA_BUILD_TESTS=ON   # same values as the main checkout's chitta/build/CMakeCache.txt (2026-09-15)
cmake --build chitta/build --parallel 8 && (cd chitta/build && ctest)
```

Tests (all must pass before you commit): `bash -n` every changed shell file;
`for t in hooks/tests/*.sh; do bash "$t"; done`;
`cd chitta-mcp && /maps/projects/fernandezguerra/apps/opt/conda/envs/bioinfo/bin/python3 -m unittest discover tests`
(server.py needs CPython ≥ 3.10; plain `python3` here is PyPy 3.9 — keep
`from __future__ import annotations` so hook modules still import there);
`cd benchmarks/smriti && … -m unittest discover -s tests -t .`;
`ruff check` on touched Python. Contracts frozen by tests: RPC names/params/JSON
shapes, the `#<id> [pct%] [type] …` recall line, snapshot/WAL formats.

Evals live in `docs/EVALS.md`: golden set `hooks/grade-recall.py`, SMRITI
`benchmarks/smriti/`, the frozen replica `scripts/eval-replica.sh`, noise bands
`benchmarks/noise.json` (accept = Δ beyond 2 sd). Paths in
`benchmarks/EVAL_IMMUTABLE.txt` are off-limits to a stream; CI fails the branch.
A stream never installs, restarts, or deploys — see constraint 8; the
orchestrator builds again on `main` and deploys after review. Streams run in
parallel only on disjoint file sets; if your spec's scope overlaps another
stream's, stop and say so.

### Launching a stream

`scripts/codex-stream.sh <name> <branch> <task.md> [high|medium|low]` builds the
prompt from chitta: `codex-plugin/stream-contract.md` (the rulebook, 25 lines),
the decisions and handoffs chitta recalls for the task, the code map from
`code_query` on the worktree, then the task. Streams write a `[handoff]` memory
at every commit and a fresh thread continues from it; `codex exec resume` is
not used. `scripts/codex-stream.sh --check <name>` lists a stream's handoffs.
Effort: high for implementation, medium for docs, tables and measurement runs.
`scripts/token-ledger.py` reports what sessions cost.

### Gates: two tiers, heavy work on a compute node

- Per commit: `bash scripts/gate-quick.sh` (about a minute on the login node:
  ruff, shell syntax and shellcheck, MCP and hook Python tests, generated MCP
  table, surface budget, contracts, docs gates). Nothing native, no replica.
- Once per stream and before the merge: `bash scripts/gate-full.sh`
  (quick gate, Rust release build and tests, C++ build and ctest, every hook
  suite); add `--replica` when you touched the store or daemon (chaos 9/9,
  restart identity 20/20) and `--recall` when you touched recall (golden and
  current-truth on a private replica copy).
- The login nodes run at a load average of 70–140. Every build, test run,
  replica start, chaos, identity or panel run goes through
  `scripts/on-compute.sh -c 16 -m 64G -- <command>` (synchronous `srun`, cwd
  and env preserved, `TMPDIR` on `/projects/caeg/scratch`). `gate-full.sh`
  already does this. Codex itself stays on the login node. Replica copies live
  under `/projects/caeg/scratch/kbd606/tmp`, never node-local `/tmp`.
- Pin evaluations: `CHITTA_RECALL_NOW`, `CHITTA_RECALL_EMBED_WAIT_MS=10000`,
  single-threaded BLAS inside each trial; run independent trials in parallel
  on the allocation rather than serially.

## Hook experiments in an isolated worktree

- Unset **both** `CHITTA_HEADLESS` and `CC_SOUL_HEADLESS` for hook tests and
  benchmarks. Either one bypasses prompt recall and returns `{}`; an all-empty
  benchmark is not a latency result. Export test overrides when shell functions
  launch child hooks; check both aliases when inherited settings interfere.
- For tests, use a temporary `HOME` (create `$HOME/.claude/mind`),
  `XDG_RUNTIME_DIR`, `CHITTA_DB_PATH`, and `CHITTA_QUEUE` (the task ledger is
  daemon-owned, so point `CHITTA_SOCKET_PATH` at a scratch daemon).
  The turn counter still uses `$HOME/.claude/mind`; changing the DB path alone
  does not isolate every hook write. For `test_post_bash_payloads.sh`, set
  `CHITTA_BIN=/bin/true` to satisfy its executable gate without live RPCs.
  Resolve the live socket before changing HOME.
- `scripts/bench-recall-lanes.sh` already isolates state and wraps the CLI with a
  read-only allowlist. If this worktree has no `bin/chitta`, set
  `CHITTA_BENCH_BIN` to an existing CLI explicitly; do not install to benchmark.
  Keep the benchmark unchanged and report repetitions, both arms' median/p95,
  empties, and failures. Frozen-replica evaluation and the verdict belong to the
  caller; live timing alone does not establish a gain.
- Trace `hooks/prompt-core.sh` directly: `prompt-hook.sh` execs it, so tracing
  only the adapter does not trace the core. Reuse the benchmark's isolation and
  read-only wrapper. On this host Bash 4.4 has no `EPOCHREALTIME`; use
  `PS4='+T$(date +%s%N) ${LINENO}: '` and a separate trace fd opened before
  assigning `BASH_XTRACEFD`. Traces locate delays but add substantial overhead;
  measure latency without `bash -x`. A small lane timing is not the total cost
  of shell parsing, admission, heartbeat, and enrichment.
- Recall text begins with a summary/warning. Select a nonempty memory result
  (`#<id> [pct%] [type] content`) before adding a continuity heading. Test both
  header-only and populated responses with enough output budget to expose them.
  That line is the hook-injected and CLI rendering. `mcp__chitta__recall`
  returns TOON (a dictionary-compressed table, ~40% fewer tokens): one row per
  hit with `id`, `type`, `relevance` and `text`; `[done]`/`[correction]`
  prefixes inside `text` are content, not extra fields.
- If the sandbox reports "bubblewrap is unavailable" [13](#ref-13), the command ran in the
  bwrap sandbox this cluster lacks; re-run it and let it escalate (streams run
  with `--approve-for-me`, and escalated commands worked in every probe so far).
- On this cluster the Codex sandbox can panic with "bubblewrap is unavailable"
  for a command; the command then has no exit text and the outcome ledger
  records `exit_code: null` for it (platform limit, not a hook bug). Rerun the
  command; do not report the null as a failure of the hooks.

Deployment instructions remain in `CLAUDE.md` for the orchestrator. Source
changes in this worktree become live only through that reviewed deployment.

## Orientation

| | Path |
|---|---|
| Daemon | `chitta/src/simple_cli.cpp` |
| RPC | `chitta/include/chitta/rpc/field_handler.hpp` |
| Store | `chitta-field/src/store.rs` |
| MCP | `chitta-mcp/server.py` |
| Hooks | `hooks/*.sh` |

`hooks/pre-tool-hook.sh` is the enforcement layer: its behaviour, the `CHITTA_*`
table, bypass flags, and the `CC_SOUL_*` aliases are in `docs/HOOKS.md` and
`docs/RENAME.md`. Its Haiku routing applies to Claude Code, not to you.

**Sources.** Codex [AGENTS.md](https://learn.chatgpt.com/docs/agent-configuration/agents-md) [38](#ref-38)
and the [approvals reference](https://developers.openai.com/codex/security/) [39](#ref-39);
`gpt-6-astra` ([announcement](https://openai.com/index/gpt-6-astra/) [41](#ref-41), 2026;
the previously cited 2026-09-04 publication day is unverified);
OpenAI [long-horizon guide](https://developers.openai.com/blog/run-long-horizon-tasks-with-codex/) [40](#ref-40) (the `Prompt.md` / `Plan.md` /
`Implement.md` / `Documentation.md` pattern). Community-verified defects, GitHub
issues Sept 2026: #13386 and #37956 silent doc truncation; #34289 no PostToolUse
exit code; #26602 flag ordering; #41378 bearer-token propagation; #41600
streamable-HTTP session leak; #43194, #43335, #42449 context-management state loss;
#44305 tool-output amplification. Claude-side sources are in `docs/HOOKS.md`.

<!-- BEGIN CITATIONS -->
## References

- <a id="ref-13"></a>**[13]** bubblewrap contributors. bubblewrap: Low-level unprivileged sandboxing tool used by Flatpak and similar projects. Project README (accessed 2026-09-16). [source](<https://github.com/containers/bubblewrap>)
- <a id="ref-38"></a>**[38]** OpenAI. Custom instructions with AGENTS.md. Codex documentation, ChatGPT Learn (accessed 2026-09-16). [source](<https://learn.chatgpt.com/docs/agent-configuration/agents-md>)
- <a id="ref-39"></a>**[39]** OpenAI. Codex Security. Codex documentation, ChatGPT Learn (accessed 2026-09-16). [source](<https://developers.openai.com/codex/security/>)
- <a id="ref-40"></a>**[40]** OpenAI. Run long horizon tasks with Codex. OpenAI Developers blog (2026). [source](<https://developers.openai.com/blog/run-long-horizon-tasks-with-codex/>)
- <a id="ref-41"></a>**[41]** OpenAI. GPT-6 Astra: A new generation of intelligence. OpenAI (2026; exact publication day unverified). [source](<https://openai.com/index/gpt-6-astra/>)
- <a id="ref-42"></a>**[42]** openai/codex issue contributors. AGENTS.md is silently truncated and instructions near the end ignored. GitHub issue #13386 (accessed 2026-09-16). [source](<https://github.com/openai/codex/issues/13386>)
- <a id="ref-43"></a>**[43]** openai/codex issue contributors. Docs: project_doc_max_bytes semantics are undocumented — the 22.5KB root AGENTS.md leaves ~9.7KB before nested files are truncated. GitHub issue #37956 (accessed 2026-09-16). [source](<https://github.com/openai/codex/issues/37956>)
- <a id="ref-44"></a>**[44]** openai/codex issue contributors. Hooks: PostToolUse payload carries no failure signal, and PostToolUseFailure never fires. GitHub issue #34289 (accessed 2026-09-16). [source](<https://github.com/openai/codex/issues/34289>)
- <a id="ref-45"></a>**[45]** openai/codex issue contributors. Docs list --ask-for-approval as global, but codex exec rejects the post-subcommand form. GitHub issue #26602 (accessed 2026-09-16). [source](<https://github.com/openai/codex/issues/26602>)
- <a id="ref-46"></a>**[46]** openai/codex issue contributors. Streamable HTTP MCP bearer_token_env_var reported as unset although present in parent shell. GitHub issue #41378 (accessed 2026-09-16). [source](<https://github.com/openai/codex/issues/41378>)
- <a id="ref-47"></a>**[47]** openai/codex issue contributors. MCP Streamable HTTP: sessions are opened but never terminated (1.8% DELETE ratio), exhausting remote server worker pools. GitHub issue #41600 (accessed 2026-09-16). [source](<https://github.com/openai/codex/issues/41600>)
- <a id="ref-48"></a>**[48]** openai/codex issue contributors. Experimental context management: native notes/history return 404 on Pro + Astra, while new_context can discard task state. GitHub issue #43194 (accessed 2026-09-16). [source](<https://github.com/openai/codex/issues/43194>)
- <a id="ref-49"></a>**[49]** openai/codex issue contributors. Token-budget new windows omit notes content, so the first LLM request has no task state. GitHub issue #43335 (accessed 2026-09-16). [source](<https://github.com/openai/codex/issues/43335>)
- <a id="ref-50"></a>**[50]** openai/codex issue contributors. Codex Desktop compaction requires unavailable notes tool, loses checkpoint, and repeats token-heavy work. GitHub issue #42449 (accessed 2026-09-16). [source](<https://github.com/openai/codex/issues/42449>)
- <a id="ref-51"></a>**[51]** openai/codex issue contributors. Codex tool loop causes context snowballing and multi-million-token input amplification. GitHub issue #44305 (accessed 2026-09-16). [source](<https://github.com/openai/codex/issues/44305>)
<!-- END CITATIONS -->
