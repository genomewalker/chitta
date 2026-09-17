Turn and token discipline (applies to every command in this stream; the thread's cost is turns × context, and every command's output is re-sent on every later turn):

- Batch: one shell call runs the whole step (`cmd1; cmd2; cmd3`), not one call per command. Never poll in a loop from the thread: a long job runs through `scripts/on-compute.sh -- <cmd>` or `sbatch --wait` and returns its summary; you look at it once.
- Cap output at the source: every build, test, benchmark or gate writes its full log to a file under /projects/caeg/scratch/kbd606/tmp and shows at most 40 lines (`| tail -n 40`, or grep for the summary line). Never cat a log, a JSON report, or a diff longer than 80 lines; use `--stat`, `head`, `jq -c '.summary'`.
- Read code through chitta before opening files: `chitta code_query --query "<question>" --path <repo>` and `chitta read_symbol` return the symbol you need; `sed -n` a range only when the query cannot answer.
- Gates: `bash scripts/gate-quick.sh` per commit; `bash scripts/gate-full.sh [--replica] [--recall]` once at the end. Do not re-run the full set per commit.
- Hand off instead of growing the thread: after each committed step write one `chitta remember --type signal --realm chitta --tags handoff --content "[handoff] <stream>: step N done: <commit> ... next: <action>"` so a fresh thread can continue from memory; if the thread passes roughly 150 tool calls, finish the current step, write the handoff, and stop with the next action printed.
- Plan.md stays short (state and next action, not a log). Do not paste command output into it.
