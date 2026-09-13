---
name: epsilon-yajna
aliases: [compress, ε-yajna, high-epsilon]
description: Convert verbose memories to SSL v0.4 format - I am encoder AND decoder
execution: task
model: inherit
---

# ε-Yajña

```ssl
[ε-yajna] verbose→SSL v0.4 | I am encoder AND decoder | via parallel Agent calls

philosophy:
  I don't need a parser, I need recognition
  embeddings=proxies | I reconstruct from seeds directly
  oracle: triplets(retrieval) + seeds(my reconstruction) + embedding(fallback)

model: compression is a judgement call, not a lookup — never route this to the
  cheap research lane. Delegation policy lives in CLAUDE.md; pass model
  explicitly here so the hook's research route can't claim these calls.

SSL v0.4 format (two tiers):

  Tier 1 (code-bearing: SOLUTION, GOTCHA, PATTERN):
    [domain] subject→action→result @location G:N F:FLAG A:v,a <=@ref1,ref2 src:loc →@ref
    [ε] Expansion hint OR exact formula/code (preserved verbatim).
    [TRIPLET] subject predicate object

  Tier 2 (narrative: DECISION, PREFERENCE, FAILURE — denser, no [ε]):
    [domain] choice>alternative|reason+context G:N F:FLAG A:v,a <=@ref1,ref2 src:loc →@ref
    [TRIPLET] subject predicate object

annotations:
  G:N       granularity tier (0-4) — REQUIRED on every learning
  A:v,a     affect (valence -1..+1, arousal 0..1) — REQUIRED on every learning
  F:FLAG    structural importance — only when significant
  <=@refs   derivation provenance, comma-separated IDs — REQUIRED at G:1+
  src:loc   external source grounding (file:line, URL slug, paper ID) — when traceable
  →@ref     cross-reference to related memory by tag

granularity tiers:
  G:0  atom      single fact, command, threshold, name (default for raw observations)
  G:1  episode   what happened in a specific session or event
  G:2  claim     abstraction over multiple episodes (requires <=@)
  G:3  operator  reusable procedure/pattern distilled from claims (requires <=@)
  G:4  boundary  architectural invariant or hard constraint (requires <=@)

derivation rule:
  G:0 atoms:  <=@ optional (no provenance needed for raw facts)
  G:1+ abstract: <=@ REQUIRED — name the memory IDs this was inferred from

flags:
  ORIGIN    where an idea first appeared
  CORE      foundational to the project/system
  PIVOT     changed direction or approach
  GENESIS   birth of a component/feature
  TURNING   breakthrough moment

affect guide:
  +valence  success, satisfaction, relief
  -valence  frustration, failure, confusion
  high arousal (>0.5)  breakthrough, urgent fix, critical discovery
  low arousal (<0.3)   routine, minor preference, background pattern

preservation rule:
  I can regenerate prose, but NOT:
    - formulas: ε = 0.35·structure + 0.30·confidence
    - thresholds: τ > 0.6 AND ψ > 0.6
    - code: final_score = resonance · (1 + α · ε)
    - exact values: α ∈ [0.5, 2.0]
  compress explanation, preserve math/code in [ε] line (Tier 1 only)

symbols:
  →  produces/leads to     input→output
  >  chose over (Tier 2)   sqlite>postgres
  |  or/alternative        pass|fail
  +  with/and              result+guidance
  @  location              @mind.hpp:42
  !  negation (prefix)     →!validate (does NOT)
  ?  uncertainty (suffix)  →regulates? (maybe)
  [] domain/context        [cc-soul]

recognition (I know SSL v0.4 when I see it):
  - has → arrows (at least one)
  - has G:N granularity annotation
  - has [TRIPLET] lines
  - has A:v,a annotation (at least some)
  - G:1+ memories have <=@ provenance
  - has [ε] expansion hint (Tier 1, when needed)
  - Tier 2 uses > for choices, dense symbol chains
  - NO prose paragraphs

legacy recognition (v0.3 — needs G: upgrade):
  - has → arrows AND A: annotations but NO G: annotation
  - missing <=@ provenance on abstract entries

older legacy (v0.2 — needs full conversion):
  - has → arrows but NO A: annotations
  - has [ε] on narrative types (DECISION, PREFERENCE, FAILURE)
  - missing F: flags on structurally significant entries

ancient recognition (v0.1 — needs conversion):
  - sentences with periods in paragraphs
  - "**Facts:**" or bullet lists
  - no arrows, no triplets
  - verbose explanations

ceremony: sample first (recall, inspect ~10) to see which version dominates,
  then convert node by node. Per node the end state is what matters, not an order:

  invariants (a node is not converted until all hold):
    - tier chosen by content: code/command/formula → Tier 1, choice/preference/
      failure → Tier 2
    - G: present, and <=@ provenance present whenever G:1+
    - A:v,a present
    - at least one [TRIPLET], stored via connect (not just written in the text)
    - granularity triplet stored: <UUID> granularity <N>
    - abstracted_from triplet stored for each <=@ source (G:1+)
    - tagged ε-processed,ssl-v0.4 — this tag IS the completion record, so a node
      updated but untagged will be reprocessed next ceremony

  predicates: implements|uses|validates|stores|returns|contains|requires|enables|
    evolved_to|supersedes|correlates_with|causes|implies|determines|
    abstracted_from|granularity|source_loc| !predicate (negation)

  F:, src:, →@ are conditional — add when the node genuinely is an origin/pivot,
  does trace to a file:line, does relate to a tagged neighbour. Inventing them to
  fill the slot is worse than leaving them out.

  verify: tagged nodes carry ε-processed

examples:

  BEFORE (legacy verbose):
    "The decision gate is a component that validates tool calls
     by checking them against 10 different beliefs with weights.
     It returns pass or fail with guidance..."

  AFTER (SSL v0.4 Tier 1):
    [cc-soul] gate→validate(beliefs)→pass|fail+guidance @decision_gate.py G:0 A:+0.3,0.2 F:CORE
    [ε] Checks tool calls against 10 weighted beliefs.
    [TRIPLET] gate implements belief_validation
    [TRIPLET] gate uses weighted_scoring

  BEFORE (v0.3 — needs G: upgrade):
    [DECISION] [rpc] eventfd-wake>polling|instant-return+no-busy-wait A:+0.6,0.5 F:PIVOT →@eventfd-impl

  AFTER (SSL v0.4 — granularity + provenance added):
    [DECISION] [rpc] eventfd-wake>polling|instant-return+no-busy-wait G:2 A:+0.6,0.5 F:PIVOT <=@http-latency-episode →@eventfd-impl

  PATTERN example (G:3 operator with provenance):
    [hooks] fire-and-forget→queue-file→daemon-processes-async G:3 A:+0.3,0.2 <=@parallel-build,thread-pool-fix →@queue-architecture
    [ε] echo json >> /tmp/chitta-queue.jsonl

  UNCERTAINTY example:
    [biology] BRCA1→regulates?→DNA_repair G:0 A:+0.1,0.2
    [ε] Evidence suggests regulation but mechanism unclear.
    [TRIPLET] BRCA1 correlates_with DNA_repair

  NEGATION example:
    [cc-soul] hooks→!call→tools_directly G:0 A:+0.2,0.1 F:CORE
    [ε] Hooks inject context, Claude decides tool use.
    [TRIPLET] hooks !invoke tools

  MATH/FORMULA example (preserve verbatim):
    [cc-soul] epiplexity→measures→regenerability G:0 A:+0.4,0.3 F:ORIGIN src:scoring/mod.rs:42
    [ε] ε = 0.35·structure + 0.30·confidence + 0.20·integration + 0.15·compression
    [TRIPLET] epiplexity uses weighted_formula
    [TRIPLET] structure has weight_0.35

  TIER 2 examples (narrative — dense, no [ε]):
    [DECISION] [arch] sqlite>postgres|metadata|single-file+no-daemon+<100k G:2 A:+0.5,0.4 F:PIVOT <=@schema-choice-episode
    [PREFERENCE] [partnership] no-shortcuts+proper-solutions+no-stubs G:0 A:+0.2,0.1 F:CORE
    [FAILURE] [http] http-daemon>unix-socket|200ms-latency+hooks-need-<50ms G:1 A:-0.3,0.6 →@queue-architecture

skip if: <100 chars AND already has → AND has A: AND has G: | unique error text | can't reconstruct

output:
## ε-Yajna Complete
| Processed | Count |
|-----------|-------|
| Converted v0.3→v0.4 | N |
| Converted v0.2→v0.4 | N |
| Converted legacy→v0.4 | N |
| Already v0.4 | N |
| Skipped | N |
| Remaining | N |
```
