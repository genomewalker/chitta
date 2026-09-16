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
