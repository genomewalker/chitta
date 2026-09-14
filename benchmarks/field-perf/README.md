# Field performance benchmark

Status as of 2026-09-14: scratch-only benchmark for `fix/field-perf`.

From the repository root, after building locally:

```bash
benchmarks/field-perf/run.sh before --profile
# Apply/build the performance changes, then:
benchmarks/field-perf/run.sh after --profile --checkpoint
```

The runner selects the committed family with `scripts/eval-replica-select.py`,
copies its manifests, selected sidecars and WAL segments, includes the two
migration markers and lite encoder, and validates the copy before starting.
It never starts a daemon on the source directory. It uses a private `/tmp`
path/runtime/socket, `.quiesce`, `CHITTA_NO_QUEUE=1`, disabled autonomous work,
and a dedicated RPC port (default 17439; override with `--port`). Only the
`Popen`-owned child is terminated. Scratch copies remain available for auditing
and clean-restart measurements; they are not committed.

`--source` selects an eval store directory; `--binary` selects an existing local
build. The default binary is `bin/chittad`. `build-local.py` configures CMake
inside the worktree from the production compiler, Python and CHITTA cache values,
substituting this worktree's Rust library and reading dependency sources only.
Build Rust with `CHITTA_EMBED_DIM=768 CHITTA_EMBED_MODEL_ID=nomic-embed-text-v1.5`
and the repository `build.sh` wrapper before invoking CMake.

Measurements use newline-delimited socket JSON-RPC, excluding CLI process launch.
Readiness probes call `health_check`, never recall. First recall precedes a
30-second settling interval. Hybrid is measured first so other workloads cannot
consume its competitive-weight refresh backlog. Each of the five methods has
20 samples; p95 is nearest-rank (sample 19/20). The query and realm are fixed and
recorded. `recall_lanes` requests sem, ctx, hyb, kw and corr. JSON artifacts contain
latencies, ordered IDs, response shapes, source-family metadata, RSS/high-water
RSS, thread count, host load and component estimates. They omit memory text.
Profiling goes to the daemon log, never RPC response fields.

`--checkpoint` compacts **only the scratch child** after all measurements. To
measure the clean-marker startup, run again with `--source` pointing to that
result's `scratch` path plus `/chitta-field`; this creates another private copy.
The family will differ because it is a new snapshot, so that restart is a load
measurement, not a substitute for the same-family before/after recall comparison.

`compare.py` checks ordered IDs and response shapes. Smart recall uses an existing
time-seeded route learner, and the combined lanes include smart recall; strict
cross-run equality can fail even between two baseline runs. Report those
variations separately, including optional fields on the memories selected.
The Rust regression tests provide fixed-input comparisons for the optimized
search and index paths.
