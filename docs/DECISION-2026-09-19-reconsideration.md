# Decision 2026-09-19: one stream at a time

Status as of 2026-09-19: **in force.** Supersedes the parallel-stream plan of
2026-09-17/18 until the runtime core (docs/DESIGN-2026-09-19-runtime-core.md)
is deployed with its numbers.

## Why

The day's effort went to plumbing colliding with plumbing: duplicate relaunch
supervisors, stale stream leases, a leftover embedding job holding the only
A100, four Codex threads and a seven-job GPU chain sharing one daemon. The
student loop had flat metrics across seven rounds (F1 0.53, 0.575, 0.499) and
no stopping rule.

## What changed

- **Active stream: p22-storage-core only**, until startup is under 30 s and
  health never blanks. Each phase is one merge with numbers in its decision doc.
- **Student frozen at round 4.** The GGUF exports (q8_0 4.28 GB, q4_k_m 2.50 GB)
  are the artefact. DPO, round 6, round 7 on the original dev set, and the
  second-judge panel were cancelled with the vLLM teacher job. Further rounds
  need a written stopping rule first.
- **Parked on their branches:** p16 `feat/decision-layer` (8 commits), p18
  `feat/student-productise` (6 commits). p20 router fixes merges when its gate
  passes; nothing else lands before p22 phase 1 is deployed.
- **No relaunch supervisors.** A thread runs once and hands off; the lead reads
  the handoff and decides.
