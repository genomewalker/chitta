# Field performance work log

Status as of 2026-09-14: implementation and measurements complete for review; performance limits and an ancillary MCP SDK failure remain documented.

[FIELD_PERF.md](docs/FIELD_PERF.md) contains the dated before/after tables, six-item source references, allocation accounting, profiling evidence, compatibility results and unmet targets. [Plan.md](Plan.md) records decisions and discarded experiments. [Benchmark instructions](benchmarks/field-perf/README.md) describe isolation and reproduction. The primary final result is `results-primed.json`; intermediate artifact labels are mapped in the report.

Release Rust: 277 passed, 2 ignored. CTest: 18 passed. RSS falls 25.5%; hybrid p95 is 64.1 ms. First/warm ratio is 2.71x and hybrid p50 is slightly higher than BEFORE; strict adaptive-result parity is not established. No deployment was performed.
