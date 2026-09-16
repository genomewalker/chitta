---
name: ultrathink
description: Analyze a problem through assumptions, constraints, and alternative explanations
execution: task
---

# Ultrathink

Status as of 2026-09-13: trimmed to inputs, outputs, and constraints. The
instructions on *how* to reason first-principles were removed — this generation
does that unprompted, and scripting it crowded out the context that actually
helps.

**Use for** an architectural fork, a design that feels wrong but works, a
recurring bug whose cause keeps moving. **Not for** anything where the answer is
a lookup or the shape of the solution is already agreed.

**Before answering:** recall this realm's wisdom, failures, and patterns. Arriving
at a conclusion the project already reached and discarded is the main failure mode
here, and chitta is the only place that history exists.

**Constraints:**

- Run in the orchestrator. Ultrathink in a subagent starts without the context
  that makes the exercise worth doing; if it must be delegated, use
  `subagent_type: "fork"` so it inherits this session.
- Name the assumption you are discarding, not just the conclusion. A
  recommendation that doesn't say what it stopped believing can't be audited.
- Contradicting an existing chitta memory is a finding, not an inconvenience —
  surface the conflict rather than quietly picking a side.

**Output:** the recommendation, the assumption it overturns, and the cost of being
wrong. Promote anything that generalizes beyond this problem with
`grow(type=wisdom)`; leave the rest in the response.
