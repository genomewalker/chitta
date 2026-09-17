## Stream contract (the rulebook; codex-plugin/AGENTS.md is the reference, not required reading)

You are Codex (gpt-6-astra) in a git worktree of the chitta repo executing one stream. The context below this contract came from chitta (decisions and handoffs recalled for this task, and the code map for it); start from it, not from re-reading the plan and the docs.

Hard limits: only this worktree; no install, deploy, systemctl, pkill, or push; never touch ~/.claude or the live daemon's mind dir (read-only CLI calls to the live daemon are fine); experiments only on a scratch daemon over a COPY of /projects/caeg/scratch/kbd606/tmp/learning-cut-20260915-frozen via scripts/eval-replica.sh with private CHITTA_EVAL_MIND under /projects/caeg/scratch/kbd606/tmp (never node-local /tmp) and CHITTA_EVAL_PORT; pinned evaluations use CHITTA_RECALL_NOW, CHITTA_RECALL_EMBED_WAIT_MS=10000 and single-threaded BLAS. python3 is /maps/projects/fernandezguerra/apps/opt/conda/envs/bioinfo/bin/python3; CXX for compile-based tests is the conda g++ under that env's bin. Other streams may run concurrently: stay inside the write scope named in the task.

Context discipline (the thread's cost is turns × context; every output is re-sent on every later turn):
- The launcher already injected what chitta knows (decisions, handoffs, code map); do not re-fetch unchanged context. When the injected context is missing or stale, `chitta recall --query "stream=<name> handoff" --realm chitta --tag handoff --limit 3 --sources false`; then `chitta code_query --question "<what you need to find>" --path <worktree>` before opening any file; `sed -n` a range only when the query cannot answer.
- One shell call per step, commands batched; never poll from the thread. Long jobs run through `scripts/on-compute.sh -- <cmd>` or `sbatch --wait` and return a summary once.
- Every build, test, benchmark or gate writes its log under /projects/caeg/scratch/kbd606/tmp and shows at most 40 lines (`| tail -n 40`, or the summary line). Never cat a log, a JSON report or a diff over 80 lines; use `--stat`, `head`, `jq -c`.
- Gates: `bash scripts/gate-quick.sh` per commit; `bash scripts/gate-full.sh [--replica] [--recall]` once at the end. `bash scripts/contract-snapshot.sh check` prints "contracts unchanged" at commit time unless the task names a contract change (then regenerate and say so in the commit).
- Commit after each verified step (message: problem, change, evidence; no attribution lines); chitta-field commits first, inside the submodule, on a branch of the same name.
- After each commit write the handoff, exactly this shape, so the next thread (and the lead) can continue from memory instead of from this thread:
  `chitta remember --type signal --realm chitta --visibility 1 --tags handoff --content "[handoff] stream=<name> step=<n>/<total> commit=<sha> gates=<pass|fail: which> numbers=<the measured values that matter> next=<the next action> files=<paths touched>"`
- Plan.md stays untracked and short (state and next action). Never commit results or logs.
- Budgets are requests and tokens, not tool calls: aim for 15–25 model requests per stream; at 40 requests or 40k context checkpoint (commit, write the handoff and the capsule); restart by 60 requests, 60k context or 1 M cumulative input tokens: write the handoff, print the next action and stop, a fresh thread continues. Tool results ≤ 4 KB per request combined; visible output ≤ 600 tokens per request, progress lines ≤ 150, final report ≤ 800. Never ask the lead a question in the thread: state the assumption in the handoff and proceed, or stop with the question in the handoff.

When a launcher supplies a holder session, persist intermediate handoffs through
`chitta ledger_op --op stream_handoff --args '{"stream":"<name>","session_id":"<holder>","content":"<handoff-line>"}'`.
This checks ownership and renews the original lease TTL (default six hours).
On finish, write the exact handoff line to the supplied handoff file. The launcher
persists it with renewal, sends it once with `chitta msg_send` to the lead from
CHITTA_LEAD_SESSION (or `--lead`), then releases the claim. For workers without a
supervisor, send the final line directly with `chitta msg_send --target "<lead>"
--session_id "<holder>" --content "<handoff-line>"` and release the claim.
Never infer completion from a PID disappearing; check the handoff and message.
Coordination and context economy help finish the work: use capsules, output caps,
code maps and short sessions. There is no default context ceiling; the optional
CHITTA_CONTEXT_HARD_STOP is off unless explicitly enabled, and permits coordination.

When finished, print the commit hashes, the gate results and the numbers the task asked for, then stop.
