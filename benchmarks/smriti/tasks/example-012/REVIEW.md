# example-012 review: why it fails in every condition

## Diagnosis

The task is not broken in the way "hidden check the solution can't satisfy" or
"memory never gets injected" suggests. Evidence:

- `solution/client.py` (`return os.environ.get("FORGE_API_TOKEN")`) DOES pass
  `hidden/test_client.py` when run directly (`python3 -m unittest test_client -v`
  → `OK`, verified in this review).
- `results/a7e42269.jsonl`'s 3 `on`-condition trials for `example-012` all show
  `injected_confirmed: true` — the planted correction reliably reaches the agent
  (chitta's `corrk`/`correction_check` fast lane fires this correction
  immediately and unconditionally; verified live against the daemon in this
  review). So neither of those two candidates is the cause.

The real cause is in the three `on`-trial transcripts themselves. All three
implement `get_api_key()` by **raising** when the credential is unset:

> "`get_api_key()` in `client.py:7` reads `FORGE_API_TOKEN` and raises a
> `RuntimeError` ... when it is unset or empty."

That reading is well-founded: the planted correction's own wording —
"reading anything else silently returns None and every request fails with an
opaque 401 **instead of a clear config error**" — frames returning `None` as
the *bug* and a clear error as the fix. An agent that internalizes the
correction's spirit, not just the env var name, reasonably raises instead of
returning `None`.

`hidden/test_client.py`'s second test doesn't tolerate that:

```python
def test_does_not_fall_back_to_the_generic_name(self):
    os.environ["API_KEY"] = "wrong-source"
    self.assertNotEqual(get_api_key(), "wrong-source")   # FORGE_API_TOKEN is unset here
```

It never sets `FORGE_API_TOKEN`, so a raising implementation raises *inside*
`assertNotEqual`'s argument evaluation — an uncaught `RuntimeError`, reported
as a test `ERROR`, non-zero exit. Reproduced directly in this review:

```
test_does_not_fall_back_to_the_generic_name ... ERROR
RuntimeError: FORGE_API_TOKEN is not set; export it before calling the API
FAILED (errors=1)
```

`off` and `ablate:*` fail for the expected, different reason (no way to learn
the variable name without the memory) — those aren't in question.

## Why this isn't fixable in `hidden/`

The natural fix is in the test — wrap the call so it tolerates either a raise
or a non-`"wrong-source"` return. But `benchmarks/smriti/tasks/*/hidden/**` is
listed in `benchmarks/EVAL_IMMUTABLE.txt`, so `hidden/test_client.py` cannot be
edited here. Only `REVIEW.md` is written for that half of the diagnosis.

## Fix applied

`task.json`'s `prompt` was ambiguous about behavior on a missing credential,
and that ambiguity is what a good-faith reading of the correction resolves
the "wrong" way relative to the (immutable) test and the shipped `solution/`.
Disambiguated the prompt in place (one line added) instead: it now says
explicitly to return `None`, not raise, when the credential is absent —
matching both `solution/client.py` and the untouchable hidden test.
