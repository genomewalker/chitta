# Learning harness follow-up — 2026-09-15

## Scope and decisions

Branch `feat/learning-experiment`, base `15882c02`. Implementation stays in this
worktree. No installation, service control, push, live writes or model calls.
Scratch daemons use copies of the existing eval replica family.

1. Strict bubblewrap/Landlock isolation remains the default. `home-audit` is an
   explicitly pinned screening mode with private HOME/XDG/config/temp state,
   deny settings, trial-only MCP identity bridge, restricted recall socket,
   full Pre/Post tool shadow capture and instrumented Bash command history.
   The audit rejects out-of-root access, live connection attempts, symlink
   escapes, control-file writes and unverifiable commands. A voided outcome is
   missing, and an incomplete pair has no numeric delta. Every home-audit verdict
   includes the caveat that there is no OS isolation.
2. Missing provenance is ambiguous and removed from both scratch arms before
   recall, with exact-ID and lane verification. Unexpected episode-derived
   native kinds are included. Contradictory provenance alone is unresolved;
   source instability remains a separate freeze gate.
3. Generated results are ignored and removed from the git index. The default
   base is CHITTA_LEARNING_OUT, or
   `/projects/caeg/scratch/kbd606/tmp/learning-results` when unset. Only compact
   fixture evidence is committed.

## Live diagnostic (read-only)

15,300 records enumerated; 1,897 included, 4,288 ambiguous, 9,115 preserved/other
excluded, zero contradictory records. All metadata queries succeeded after
using lossless integer IDs for metadata (the signed-string parser previously
failed above 2^63). Exact decimal-string graph queries still decide membership.
The live store changed during enumeration, so this is diagnostic evidence, not
an official cut. No official cohort was frozen.

| Ambiguous kind | Count | Before 2026-03-26 | On/after 2026-03-26 |
| --- | ---: | ---: | ---: |
| wisdom | 3,841 | 0 | 3,841 |
| signal | 154 | 0 | 154 |
| alias | 124 | 0 | 124 |
| milestone | 67 | 0 | 67 |
| insight | 36 | 0 | 36 |
| preference | 32 | 0 | 32 |
| belief | 9 | 0 | 9 |
| failure | 6 | 0 | 6 |
| operational | 6 | 0 | 6 |
| result | 6 | 0 | 6 |
| task | 3 | 0 | 3 |
| decision | 2 | 0 | 2 |
| question | 2 | 0 | 2 |
| **Total** | **4,288** | **0** | **4,288** |

Unknown creation dates: zero. Unexpected native kinds: alias 11, signal 10.
Ambiguous historical access count is 5,203,492 / 8,516,745 = 61.10%. This is a
historical proxy, not an observed arm A injection share. The live diagnostic has
no arm A trials; that share is explicitly unmeasured. Trial reports separately
measure ambiguous injections after removing them from both arms (required zero).

Full diagnostic is `/tmp/learning-live-cohort-followup-final.json`; its SHA256
and count-only summary are in `benchmarks/learning/evidence/live-cohort-2026-09-15.json`.

## Limitations

Home-audit is weaker than a kernel sandbox. Unverifiable Bash programs, including
interpreters, scripts and compound shell commands, void a screening trial. The
model can edit with file tools; hidden grading runs after model execution. The
fixture substitutes trusted edits for Claude and does not certify Claude's
permission enforcement or any learning benefit. The sole trial MCP server has
no model-callable tools so that recall exposure stays in the instrumented hooks.

## Gates and dry-run

31 harness tests, 18 unchanged hook scripts, 127 unchanged MCP tests and 46
unchanged SMRITI tests pass. Ruff check/format, bash -n, shellcheck and the
immutable-evaluation script pass. Detailed gate paths and fixture outcome are
recorded in the compact evidence files once the eight-trial run completes.

The completed eight-trial fixture has seven valid outcomes and one deliberate
void. Saffron A1/1 observed + one missing versus B0/2; cobalt A1/2 versus B1/2.
The source family is unchanged and the saved report reproduces exactly. The
voided saffron trial also had a hook-output/ledger mismatch; it remains in raw
evidence and does not become a failure score. No nonvoided trial had errors.
See `benchmarks/learning/evidence/fixture-2026-09-15.md` for manifest hashes,
external artifacts, the two-task table and the screening caveat.
