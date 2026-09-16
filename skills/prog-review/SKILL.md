---
name: prog-review
description: "First-principles review — question requirements, delete unnecessary parts, simplify, optimize with evidence, automate last. Use for code review, refactor, performance, or architecture."
execution: direct
---

# Programming Refactor Review

Status as of 2026-09-13: trimmed to the ordering discipline and the evidence
rules. The checklists of what counts as dead code, a simplification, or an
optimization were removed — that is standard practice, and listing it crowded out
the part that is actually a constraint.

## Review order

Question the requirement, delete, simplify, then optimize, then automate — **in
that order**. This is a strict ordering, not motivational advice: optimizing code
that should have been deleted is the failure this skill exists to prevent. Skip
ahead only when the user asked for a narrow task.

For performance work, check in this order: remove unnecessary work, reduce
passes, change the algorithm, change the representation, reduce copying,
parallelize, then optimize individual operations. Measure the effect of each
candidate change.

## Evidence rules

- **A deletion recommendation carries its own safety check.** Name the specific
  thing that would catch you being wrong — the call-site search, the test, the
  fixture comparison, the log. "Probably unused" without a check is not a finding.
- **Performance claims are measured or hedged, never asserted.** Say "profile to
  confirm" and mean it. A benchmark plan needs realistic input sizes, a
  correctness comparison against baseline, and a stated success threshold.
- **Automate only what is stable.** A repeated workflow with detectable failure
  and well-defined output qualifies. A workaround, or a design still under
  question, does not.

## Output

Lead with the deletion or simplification with the largest supported effect. One-line verdict: delete, simplify, optimize, or automate. Keep
correctness issues, maintainability issues, performance risk, and automation
opportunities visibly separate — they carry different urgency and the reader
triages on that. Offer a concrete patch where there's enough context to write one.

Bioinformatics pipelines in this project have recurring specific smells worth
checking: dense arrays where sparse would do, repeated BAM/FASTA parsing,
redundant coordinate conversions, avoidable decompression, accidental O(n²).
