# chitta (lean)

> Status as of 2026-09-13: this is the `CHITTA_LEAN=true` variant — stats only,
> no scaffolding. **`CLAUDE.md` is canonical**; anything not here is there,
> including the model split and every build gotcha. Deliberately not a summary
> of it, only what lean mode still needs.

Memory persists across sessions and surfaces through hooks. No explicit recall
call is needed to benefit from it.

| Signal | Reading |
|-------|--------|
| τ > 80% | High confidence in retrieved context |
| τ < 50% | Thin context — ask rather than assume |
| ψ < 50% | Consolidating; a poor time to start new exploration |

Reach for a tool when the hooks haven't already surfaced what you need:
`recall` for deep search, `remember` for durable knowledge, `observe` for
episodic notes. `grow` and `connect` sit behind the `advanced` gateway rather
than being top-level tools.
