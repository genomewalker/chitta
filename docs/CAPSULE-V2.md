# Capsule v2 reads and budgets

`ledger_op --op capsule_get --args '{"project_dir":"/path/to/worktree","stream_id":"name","code_head":"full SHA"}'`
returns `status`, the exact `capsule`, and a bounded `manifest`. Selection is by
canonical repository and stream (or session if no stream), across every page;
the largest revision wins even when clocks disagree. Completed, missing and
invalidated revisions suppress older records. Status is `missing`, `invalidated`,
`stale`, `head_mismatch` or `ok`. Freshness defaults to 24 hours; callers may set
`max_age_seconds` in (0, 604800]. A timestamp over 60 seconds in the future is stale.
Reads and writers share the capsule transaction lock.

`capsule_manifest` accepts `repository` or `project_dir`. Each admitted row contains
stream/session, branch, full HEAD, state, revision and next action; `omitted` counts
rows that did not fit. Retrieve individual streams with `capsule_get`.

Manifest serialization is at most 450 UTF-8 bytes. SessionStart admits complete
sections into 1400 bytes, reserving a retrieval notice within a 1500-byte ceiling.
These are conservative token upper bounds for byte-fallback tokenizers, stricter
than 450 and 1500 tokens; there is no tokenizer or network dependency in hooks.
SessionStart projects exact capsule identity, state, revision, HEAD and next action;
the exact read retains all constraints, dirty paths, gates and jobs. Oversized
sections are omitted intact and explicitly signalled. Tests exercise adversarial
UTF-8, oversized sections, 201 streams, invalidation and revision/clock disagreement.
Legacy v1 handoffs remain available only if no exact v2 record exists.

New ledger gateway operations: `capsule_get`, `capsule_manifest`. The gateway's
public tool schema remains unchanged (operation and object arguments).

The SessionStart envelope applies the same ceiling after optional code navigation.
An oversized code map is replaced by a retrieval hint; an older daemon response
that already exceeds the ceiling is replaced by an explicit retrieval notice.
