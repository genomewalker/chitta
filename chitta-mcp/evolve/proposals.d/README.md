Drop literature mechanism cards here as `*.json`, one object per file. The
schema is `../proposal.schema.json`; the stdlib loader validates its required
fields. Use `source: "hypothesis"` for the exploration quota. No example JSON
is installed as a candidate, so demonstrations cannot enter the real backlog.

Example (replace the illustrative prediction and citation with real evidence):

```json
{
  "id": "paper-identifier-mechanism-name",
  "title": "Deduplicate concurrent embedding requests",
  "mechanism": "Share one pending embedding future per normalized query so repeated requests do not occupy independent queue entries.",
  "expected_gain": {"metric": "mean_nDCG", "delta": 0.02, "confidence": 0.4},
  "cost": {"effort_h": 3, "blast_radius": 2},
  "evidence": [{"citation": "paper URL", "observation": "measured finding"}],
  "source": "hypothesis"
}
```

`delta` is signed in the evaluator metric's units: for rates, 0.02 means two
percentage points, not two percent relative. Use comparable bounded metrics
for scoring, not raw milliseconds or unbounded counters. `blast_radius` is a
unitless risk multiplier (1 = local change, 2 = several paths, higher = wider).
The canonical proposal ID is SHA-256(title + newline + mechanism), truncated
to 16 hex characters after whitespace/case normalization. An author's `id`
is descriptive; changing the mechanism creates a new canonical experiment.
