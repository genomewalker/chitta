# Phase 5 hook parity fixtures

The fixtures are synthetic, with a fixed session, transcript and project path.
Baseline stdout/stderr is captured verbatim from the frozen evaluation replica;
no timings, memory IDs, whitespace, output fields, or errors are normalized by
this runner. Review captured baseline text before committing it. Runtime reports
and timing logs belong outside the repository.

Start a **private copy** with `scripts/eval-replica.sh`, an explicit private port,
`CHITTA_LIVE_MIND` set to the frozen source, and `CHITTA_RECALL_NOW=1789516800000`.
The runner verifies the replica PID, mind path, socket and process environment.
Use the requested CPython executable for both this script and the replica helper.

```
python3 scripts/bench-hook-parity.py check \
  --replica /tmp/chitta-p5-policy/replica \
  --cli /path/to/chitta \
  --work /tmp/chitta-p5-policy/hook-work \
  --results /tmp/chitta-p5-policy/parity-check
```

`record` creates a baseline; subsequent iterations still compare against it.
`check` never updates a baseline. Both require at least two repetitions (default
three), preceded by four complete warm-up sequences, also recorded in the local
report. Warm-up failures still fail the gate. `measure` unpins hook presentation
clocks and records real process wall time; it cannot establish byte parity.

All hook writes use a disposable HOME and private replica socket. Optional Stop
consolidation is disabled through its existing environment switch. The `pkill`
shim is a no-op because these fixtures have no notification processes; no system
process is killed. Other CLI calls, including ledger calls, reach the scratch
daemon. Hook background groups are reaped before resetting their local files.

`CHITTA_HOOK_NOW` pins hook wall-clock timestamps and displayed durations (zero),
including returned RPC lane durations. Actual timeout flags and the prompt's
budget deadline remain real. `CHITTA_RECALL_NOW` separately pins daemon scoring.
Unknown Codex Bash status remains unknown, and empty Stop/Bash stdout is compared
alongside stderr and the actual process status. Prompt fixtures require a visible
`[admit]` block, so a headless bypass cannot produce a false pass.

This is a warm-process migration gate, not the recall restart-identity gate.
The initial unconstrained natural-language probe changed lane admission counts
on repeated calls; the committed repository-path query fixes the routing input.
Existing lane/admission tests cover synthetic semantic and UNKNOWN cases. The
runner retains exact failures when replica recall or load remains nondeterministic.

When a new daemon build changes retrieval order, retain the committed historical
baseline and run an unchanged-hook control against that same daemon. Extract the
last verified hooks with `git archive <commit> hooks`, use `record --hooks
/path/to/extracted/hooks --baseline /tmp/control-baseline`, then `check
--baseline /tmp/control-baseline` for the candidate. Both runs use the same CLI,
replica and pinned inputs. Reports include CLI and hook digests. A control failure
is a retrieval stability failure; do not normalize rows or rewrite the historical
baseline to hide it. Step 2 observed two SessionStart correction rows exchanging
positions across a daemon restart, reproduced by the unchanged Step 1 hooks.

`check --reference-hooks /path/to/unchanged/hooks` runs each unchanged hook
immediately before the candidate on the same daemon, resetting local fixture
state before both. It compares all bytes and statuses, including warm-up pairs,
and records both outputs and source digests. Use this adjacent control when
mutable recall ordering drifts between whole-suite passes. It never changes the
historical baseline. `measure --reference-hooks ...` also records both arms'
wall times; candidate daemon stage spans and lane statuses are in `daemon_ms`
and `lane_status`. Lane spans overlap; retrieval includes embedding time.

For the fusion migration, add `--require-pipeline` to the gate. Every prompt
fixture must capture a daemon fusion response, including during warm-up; falling
back silently cannot count as verification of the new path. This instrumentation
copies one private response file and leaves stdout/stderr unchanged.
