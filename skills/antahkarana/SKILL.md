---
name: antahkarana
aliases: [debate, perspectives, swarm]
description: Multi-perspective reasoning through cognitive voices
execution: task
---

# Antahkarana

Status as of 2026-09-13: trimmed to the voices, the constraints, and the output.
The step list and the per-skill model table were removed — routing is policy in
`CLAUDE.md`, enforced by `hooks/pre-tool-hook.sh`.

For the philosophical basis of these six voices — why they're structured this way
and how they emerge from retrieval design — see [[../vedanta/antahkarana]] in the
vedanta skill graph.

Use when one question needs genuinely different viewpoints: a decision with no
obvious right answer, or an approach you're stuck on. Distinct from yajña, which
coordinates *tasks* rather than perspectives on a single question.

## The voices

| Voice | Asks |
|---|---|
| manas | What feels right? Quick, practical intuition |
| buddhi | What does the evidence say? Analytical |
| ahamkara | What could go wrong? Risk-aware, protective |
| chitta | What worked before? Memory and pattern |
| vikalpa | What if we tried…? Creative, exploratory |
| sakshi | Neutral witness; synthesizes the rest |

## What must hold

- **Voices run in parallel and independently.** A voice that has read another's
  answer is no longer a separate perspective, and the whole value is independence.
- **Each voice writes to chitta** tagged `thread:<id>,voice:<name>`, so the
  synthesis reads from memory rather than from a context window that may have
  dropped an early voice.
- **Sakshi's synthesis stays in the orchestrator.** It needs every voice at once.
- **Report divergence, don't resolve it away.** Where the voices disagree is the
  finding. A synthesis that reads as unanimous has usually just lost information.

## Output

Each voice's position in a line or two, then the synthesis: where they converge,
where they genuinely conflict, and a recommendation that says which risk it
accepts. Bracket the run with `narrate(action=start|end)` so the thread is
resumable.
