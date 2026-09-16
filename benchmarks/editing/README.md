# Edit-primitive benchmark

Measures token cost and accuracy of cc-soul's editing primitives against
two baselines, across fastedit's P1–P22 edit-pattern taxonomy.

## Strategies

| name | simulates | payload counted |
|---|---|---|
| `baseline_read_write` | full Read + full Write | file in + file out |
| `baseline_edit`       | Claude's default Edit (padded context) | old_str+new_str with ~2 lines of surrounding context |
| `file_patch`          | cc-soul's `file_patch(old_str, new_str)` — minimal uniqueness-anchored | old_str + new_str only |
| `symbol_patch`        | cc-soul's `symbol_patch(file, symbol, body)` | symbol + new body |

All four produce byte-equivalent output (100% accuracy on the current
case set). The difference is how many tokens the model had to emit.

## Running

```bash
cd benchmarks/editing
python3 runner.py            # no deps
pip install tiktoken         # for accurate BPE token counts
python3 runner.py
```

## Caveats (read before trusting the numbers)

1. **Case files are small** (3–10 lines). Savings scale with file size —
   `baseline_read_write` would blow up on a 500-line file while
   `file_patch` stays flat. To see realistic savings, add cases that
   embed the edit inside a larger surrounding file.
2. **strategies.py is a faithful re-implementation**, not a live call
   into the chitta daemon. Semantics match (uniqueness check for
   file_patch; regex symbol lookup mirrors tree-sitter symbol_patch for
   the Python cases here), but a live integration test is a TODO.
3. **No LLM in loop.** This measures the primitive efficiency, not
   end-to-end agent behavior. Fastedit's "~98% combined" number
   includes its 1.7B model fallback path; we don't have that and don't
   need it — our primitives are deterministic.

## Comparison to fastedit

Pattern labels (P1 `add_guard` … P22 `remove_parameter`) follow
[parcadei/fastedit](https://github.com/parcadei/fastedit) [64](#ref-64) for
comparability. Test cases are independently authored.

<!-- BEGIN CITATIONS -->
## References

- <a id="ref-64"></a>**[64]** parcadei. fastedit: AST-aware fast code editing via local 1.7B model. GitHub repository (accessed 2026-09-16). [source](<https://github.com/parcadei/fastedit>)
<!-- END CITATIONS -->
