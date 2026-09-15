# Automatic-learning paired results

Trials per arm: 2

| Task | A successes | B successes | Δ |
| --- | ---: | ---: | ---: |
| saffron | 2/2 | 0/2 | +1.000 |
| cobalt | 1/2 | 1/2 | +0.000 |

Δ = +1.000; net successes = +2

| Task | Trial | Arm | Success | Cohort / total injections | Empty turns | Error |
| --- | ---: | --- | ---: | ---: | ---: | --- |
| saffron | 1 | B | 0 | 0 / 3 | 0 |  |
| saffron | 1 | A | 1 | 1 / 1 | 0 |  |
| saffron | 2 | A | 1 | 2 / 2 | 0 |  |
| saffron | 2 | B | 0 | 0 / 2 | 0 |  |
| cobalt | 1 | A | 1 | 1 / 2 | 0 |  |
| cobalt | 1 | B | 0 | 0 / 2 | 0 |  |
| cobalt | 2 | B | 1 | 0 / 2 | 0 |  |
| cobalt | 2 | A | 0 | 1 / 1 | 0 |  |

Telemetry (milliseconds; recall distribution includes individual lanes):

```json
{
  "A": {
    "cohort_injections": 5,
    "total_injections": 6,
    "empty_turns": 0,
    "lane_failures": 0,
    "recall_ms": {
      "median": 13.5,
      "p95": 347.0
    },
    "hook_ms": {
      "median": 881.0,
      "p95": 1278.5
    }
  },
  "B": {
    "cohort_injections": 0,
    "total_injections": 9,
    "empty_turns": 0,
    "lane_failures": 0,
    "recall_ms": {
      "median": 34.5,
      "p95": 610.4
    },
    "hook_ms": {
      "median": 1323.0,
      "p95": 1390.3
    }
  }
}
```

NO VERDICT: dry-run fixture; no model experiment; official panel requires 20 tasks and 3 trials; OS isolation unavailable
