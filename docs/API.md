# chitta MCP API reference

Status as of 2026-09-16.
API reference generated from a live daemon on 2026-09-02; MCP static table synchronized on 2026-09-16 (320 daemon tools) with `python3 scripts/gen-tools-static.py` (`--check` verifies freshness). Regenerate this reference with `python3 scripts/gen-tools-docs.py`.

Generated from a live daemon on 2026-09-16, 344 tools — regenerate with `python3 scripts/gen-tools-docs.py`.

89 tools are listed in `tools/list` by default. The other 255 are hidden to keep the model's tool list small, and stay callable through the `advanced` gateway:

```json
{"tool": "pin_memory", "arguments": {"id": 123}}
```

`advanced` with `{"action": "list"}` enumerates them at runtime, optionally filtered by `category` (`advanced` or `internal`).

Tools marked **gateway** are composed in `chitta-mcp/server.py` rather than served directly by the daemon; tools marked **via advanced** are daemon tools kept out of the default listing.

## Contents

- [Core Memory](#core) — 53
- [Recall & Search](#recall) — 27
- [Graph & Triplets](#graph) — 15
- [Code Intelligence](#code) — 10
- [Context & Status](#context) — 16
- [Realms](#realm) — 9
- [Sessions & Continuity](#session) — 19
- [Cross-Harness Messaging](#messaging) — 6
- [Narrative & Work Modes](#narrative) — 5
- [Distillation & Embeddings](#distill) — 14
- [Consolidation & Contradictions](#consolidation) — 15
- [Themes](#theme) — 4
- [Provenance & Verification](#provenance) — 13
- [Sadhana](#sadhana) — 12
- [Dreams & Curiosity](#dream) — 13
- [Learning & Outcomes](#learn) — 18
- [Anticipation & Habits](#anticipation) — 15
- [Profile & Goals](#profile) — 8
- [Skill & Agent Registry](#skills) — 19
- [Intervention Ledger](#intervention) — 8
- [Executable Constraints](#facts) — 7
- [Surprise & Epistemic Debt](#surprise) — 11
- [Wisdom Lifecycle](#wisdom) — 14
- [Causal Episode Compiler](#cec) — 5
- [Import / Export & Files](#io) — 8

<a id="core"></a>

## Core Memory

### `ack_memory` *(gateway)*

Increment ack signal for a memory. Records [ack] memory:<id> score:+1 with tag ack-signal.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | string | yes | — | Memory ID to ack |

### `approve_memory`

Approve a Proposed memory, promoting it to Active

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | string | yes | — | Memory ID to approve |

### `create_episode` *(via advanced)*

Create dialogue episode for conversation tracking

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `end_turn` | integer | no | — |  |
| `episode_type` | string | no | — |  |
| `realm` | string | no | — |  |
| `session_id` | string | yes | — |  |
| `start_turn` | integer | yes | — |  |
| `title` | string | yes | — |  |

### `episode_cluster_status` *(via advanced)*

Find similar episode clusters

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `min_occurrences` | integer | no | — |  |
| `similarity_threshold` | number | no | — |  |

### `expand_memory` *(via advanced)*

Expand a memory to full hierarchical context

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `depth` | integer | no | — |  |
| `id` | string | yes | — |  |

### `forget`

Remove a memory

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | string | yes | — | Node ID to forget |

### `forget_kind`

Bulk-delete all memories of a given kind (e.g. 'habit'). Optionally filter by realm.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `kind` | string | yes | — | Memory kind to delete (e.g. habit, unknown) |
| `limit` | integer | no | — | Max to delete (default 5000) |
| `realm` | string | no | — | Optional realm filter |

### `get` *(via advanced)*

Get a node by ID

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | string | yes | — |  |

### `get_entities` *(via advanced)*

Get tracked entities

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — |  |
| `type` | string | no | — |  |

### `get_evidence_type`

Retrieve the evidence type tag of a memory

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | string | yes | — | Memory ID |

### `labile_memories`

List memories recalled multiple times recently — candidates for reconsolidation (excludes freshly-written hook memories)

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — | Max results (default 20) |
| `min_access` | integer | no | — | Min recall count to qualify (default 2) |
| `realm` | string | no | — | Filter by realm |
| `window_hours` | number | no | — | Recency window in hours (default 48) |

### `labile_memories_top` *(via advanced)*

List the most-accessed (most labile) memories — candidates for reconsolidation

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — | Max results (default 20) |
| `realm` | string | no | — | Filter by realm |

### `list_aspects` *(via advanced)*

List available semantic aspects

No parameters.

### `list_by_aspect` *(via advanced)*

List memories by semantic aspect

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `aspect` | string | yes | — |  |
| `limit` | integer | no | — |  |
| `min_confidence` | number | no | — |  |

### `list_by_status`

List memories filtered by lifecycle status (active/superseded/contradicted/archived)

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | `50` | Max results |
| `realm` | string | no | — | Filter by realm |
| `status` | string | no | `"superseded"` | Filter: active, superseded, contradicted, archived, or all |

### `list_memories_brief`

Fast memory index

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `kind` | string | no | — |  |
| `limit` | integer | no | — |  |
| `priority_tier` | integer | no | — |  |
| `realm` | string | no | — |  |

### `list_merge_queue` *(via advanced)*

List pending merge proposals

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — |  |
| `status` | string | no | — |  |

### `list_pinned` *(via advanced)*

List pinned memories

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — |  |
| `realm` | string | no | — |  |

### `lookup` *(gateway)*

Unified memory lookup. Classifies query intent, fans out to optimal backends (keyword/semantic/triplet/temporal/code), fuses with weighted RRF. Default entry point for memory search — use instead of recall/smart_recall/hybrid_recall.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `explain` | boolean | no | — | Include intent classification and score breakdown |
| `limit` | integer | no | — | Max results (default: 10) |
| `mode` | string (auto|fast|deep) | no | — | auto=escalate if low confidence; fast=skip deep search; deep=force full resonate |
| `query` | string | yes | — | Search query |
| `realm` | string | no | — | Filter by realm |

### `mark_memory_invalidated` *(via advanced)*

Mark a memory as invalidated by a commit hash or symbol-change ID.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `memory_id` | integer | yes | — | Memory ID to mark |
| `reason` | string | no | — | Commit hash or change ID causing invalidation |

### `memory_edit` *(gateway)*

Edit memory metadata. Actions: set_type (change memory type), set_priority (change priority tier)

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `action` | string | yes | — | set_type\|set_priority |
| `id` | any | yes | — | Memory ID |
| `tier` | string | no | — | New priority tier (for set_priority) |
| `type` | string | no | — | New type (for set_type) |

### `memory_history` *(via advanced)*

View memory version history

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | integer | yes | — |  |
| `limit` | integer | no | — |  |

### `memory_lock` *(via advanced)*

Acquire memory lock

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `duration` | integer | no | — |  |
| `holder_id` | string | yes | — |  |
| `holder_type` | string | no | — |  |
| `id` | integer | yes | — |  |

### `memory_lock_status` *(via advanced)*

Check lock status

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | integer | yes | — |  |

### `memory_outcome` *(gateway)*

Record an observed outcome for a memory into its Beta utility posterior (success raises alpha, failure raises beta) Routed by the chitta-mcp gateway before it reaches the daemon, so the effective arguments can differ from the daemon schema below.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | string | yes | — | Node ID the outcome is about |
| `success` | boolean | yes | — | Did acting on this memory work? |
| `weight` | number | no | — | Observation weight, capped at 5.0 (default 1.0) |

### `memory_provenance`

Show why a memory exists: source, evidence, superseded_by, supersedes relations

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | string | no | — | Memory ID to inspect |
| `memory_id` | integer | no | — | Memory ID (numeric) |

### `memory_revert` *(via advanced)*

Revert memory to previous version

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | integer | yes | — |  |
| `reason` | string | no | — |  |
| `version` | integer | yes | — |  |

### `memory_status`

Get effective status of a memory: active, superseded, or contradicted — checks incoming supersedes triplets

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | string | no | — |  |
| `memory_id` | integer | no | — |  |

### `memory_type_stats` *(via advanced)*

Get memory type statistics

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `realm` | string | no | — |  |

### `memory_unlock` *(via advanced)*

Release memory lock

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `holder_id` | string | yes | — |  |
| `id` | integer | yes | — |  |

### `nack_memory` *(gateway)*

Decrement nack signal for a memory. Records [nack] memory:<id> score:-1 with tag nack-signal.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | string | yes | — | Memory ID to nack |

### `pin_memory` *(via advanced)*

Pin memory to keep hot

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | integer | yes | — |  |
| `reason` | string | no | — |  |

### `promote_memory`

Promote a memory one tier: Proposed→Observed→Verified→Active

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | string | yes | — | Memory ID |

### `propose_change` *(via advanced)*

Propose change to memory

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `content` | string | yes | — |  |
| `id` | integer | yes | — |  |
| `proposed_by` | string | yes | — |  |

### `prune_episodes`

Prune old/excess episode memories. Deletes episodes older than max_age_days (strength<0.3) and caps total count at max_count.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `max_age_days` | integer | no | — | Delete episodes older than this (default 90) |
| `max_count` | integer | no | — | Cap total episode count at this (default 10000) |

### `recall` *(gateway)*

Search memory by semantic similarity with realm filtering Routed by the chitta-mcp gateway before it reaches the daemon, so the effective arguments can differ from the daemon schema below.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `explain` | boolean | no | — | Include score decomposition per hit (default: false) |
| `gwt_mode` | boolean | no | — | Global Workspace Theory mode (default: false) |
| `include_global` | boolean | no | — | Include global memories (default: true) |
| `limit` | integer | no | — | Max results (default 10) |
| `min_confidence` | number | no | — | Minimum confidence threshold |
| `pool` | integer | no | — | Candidate pool depth before the recall-biased pre-filter (default 60, max 160; env CHITTA_RECALL_POOL) |
| `prefilter` | boolean | no | — | Recall-biased pre-filter (default true; false or CHITTA_RECALL_PREFILTER=0 restores the narrow-pool path) |
| `query` | string | yes | — | Search query |
| `realm` | string | no | — | Filter by realm |
| `separation_mode` | boolean | no | — | Diverse results via MMR (default: false) |
| `strategy` | string | no | — | Retrieval lane: fused (default), keyword (BM25 only, realm-scoped), field (Hopfield/DAM) |
| `tag` | string | no | — | Filter by tag |

### `reject_memory`

Reject a Proposed memory, archiving it

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | string | yes | — | Memory ID to reject |

### `remember`

Store text in memory with optional tags and realm

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `confidence` | number | no | — | Initial confidence 0-1 (default: 0.8) |
| `content` | string | yes | — | Text to remember |
| `realm` | string | no | — | Primary realm (default: brahman) |
| `shared_realms` | array<string> | no | — | Additional realms |
| `tags` | array<string> | no | — | Optional tags |
| `type` | string | no | — | Node type (wisdom, insight, signal, episode) |
| `visibility` | integer | no | — | 0=Private, 1=Shared, 2=Global (default: 0) |

### `remember_batch`

Store multiple memories in a single round-trip (high-throughput bulk ingest)

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `items` | array<object> | yes | — | Array of memory objects (same fields as remember) |
| `realm` | string | no | — | Default realm for all items |

### `remember_typed` *(gateway)*

Store a typed memory node (digest-node, decision, open-question, etc.) with optional graph links (supersedes, invalidated-by, anchors-to).

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `content` | string | yes | — | SSL triplet content for the memory |
| `links` | array<object> | no | — | Graph links to other memories |
| `node_type` | string (digest-node|symbol-summary|decision|open-question|rollup|working-brief) | yes | — | Type of memory node |
| `realm` | string | no | — | Realm to store in (default: brahman) |
| `subject` | string | yes | — | What this memory is about |

### `resolve_merge` *(via advanced)*

Resolve merge proposal

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `merge_id` | integer | yes | — |  |
| `resolution` | string | no | — |  |
| `status` | string | yes | — |  |

### `set_affect`

Set affect dimensions (valence, arousal) on a memory

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `arousal` | number | yes | — | Emotional arousal: 0.0 to 1.0 |
| `id` | string | yes | — |  |
| `valence` | number | yes | — | Emotional valence: -1.0 to +1.0 |

### `set_evidence_type`

Tag a memory with its epistemological evidence class (observation/inference/hearsay/authoritative/prediction)

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `evidence_type` | string | yes | — | One of: observation, inference, hearsay, authoritative, prediction |
| `id` | string | yes | — | Memory ID |

### `set_memory_type` *(via advanced)*

Set memory semantic type

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `memory_id` | integer | yes | — |  |
| `type` | string | yes | — |  |

### `set_priority_tier` *(via advanced)*

Set memory priority tier

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `memory_id` | integer | yes | — |  |
| `tier` | integer | yes | — |  |

### `strengthen` *(via advanced)*

Increase confidence of a memory

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `amount` | number | no | — | Amount (default 0.1) |
| `id` | string | yes | — | Node ID |

### `tag` *(via advanced)*

Add or remove tags from a node

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `add` | string | no | — |  |
| `id` | string | yes | — |  |
| `remove` | string | no | — |  |

### `unpin_memory` *(via advanced)*

Unpin a memory

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | integer | yes | — |  |

### `update` *(via advanced)*

Update node content

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `content` | string | yes | — |  |
| `id` | string | yes | — |  |

### `verify_correction` *(gateway)*

Mark a correction memory as verified at a specific code locus. Stores a verification record and updates the original correction's tags.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `evidence_locus` | string | yes | — | Location of evidence, e.g. 'sadhana_manager.cpp:299' |
| `id` | any | yes | — | Memory/triplet ID of the correction to verify |

### `weaken` *(via advanced)*

Decrease confidence of a memory

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `amount` | number | no | — |  |
| `id` | string | yes | — |  |

### `what_do_i_know_about`

Introspection: return claims + provenance + staleness + contradictions for a topic

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `confidence_min` | number | no | — | Minimum confidence threshold (default 0.0) |
| `include_contradictions` | boolean | no | — | Include contradiction info (default true) |
| `include_stale` | boolean | no | — | Include stale memories (default true) |
| `k` | integer | no | — | Max claims to return (default 10) |
| `realm` | string | no | — | Realm filter |
| `topic` | string | yes | — | Topic or question to introspect |

### `write_gate_stats` *(via advanced)*

Show write-gate admission stats: staged memory count and oldest staged memory age

No parameters.

<a id="recall"></a>

## Recall & Search

### `5w_search`

Multi-dimensional semantic search across who/what/when/where/why axes

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — | Max results (default 10) |
| `realm` | string | no | — | Filter by realm |
| `what` | string | no | — | What is happening/topic |
| `when` | string | no | — | Temporal description |
| `where` | string | no | — | Location or context |
| `who` | string | no | — | Who is involved |
| `why` | string | no | — | Motivation or reason |

### `expand_query` *(via advanced)*

Expand query into typed variants

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `query` | string | yes | — |  |

### `full_resonate` *(via advanced)*

Semantic search with full context

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `exclude_kinds` | array<string> | no | — |  |
| `include_global` | boolean | no | — |  |
| `k` | integer | no | — |  |
| `partnership_only` | boolean | no | — |  |
| `query` | string | yes | — |  |
| `realm` | string | no | — |  |
| `separation_mode` | boolean | no | — |  |

### `hybrid_recall` *(via advanced)*

Combined vector + BM25 + graph recall

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `bm25_weight` | number | no | — |  |
| `explain` | boolean | no | — | Include score decomposition per hit (default: false) |
| `graph_weight` | number | no | — |  |
| `limit` | integer | no | — |  |
| `query` | string | yes | — |  |
| `realm` | string | no | — |  |
| `recency_weight` | number | no | — |  |
| `tag` | string | no | — |  |
| `vector_weight` | number | no | — |  |

### `query_claims` *(via advanced)*

Query semantic claims

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `active_only` | boolean | no | — |  |
| `limit` | integer | no | — |  |
| `predicate` | string | no | — |  |
| `scope` | string | no | — |  |
| `subject` | string | no | — |  |

### `recall_analogy`

Analogical recall over the triplet lane (vector-symbolic, no LLM/GPU). Two modes embedding similarity cannot express, because both are about structure rather than wording. 'proportional' solves a:b :: c:? — give three entities, get ranked fillers for the fourth. 'structural' finds memories whose relation graph has the same SHAPE as a probe memory, with entity names factored out, so a pattern learned in one project matches the same pattern in another; set exclude_realm (or cross_realm) for that cross-project transfer. Use when you want 'what else looks like this?', not 'what mentions these words?'.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `a` | string | no | — | proportional: source entity |
| `b` | string | no | — | proportional: what a maps to |
| `c` | string | no | — | proportional: target entity; the tool returns its counterpart to b |
| `cross_realm` | boolean | no | — | structural: shorthand for excluding the probe memory's own realm (default false) |
| `exclude_realm` | string | no | — | structural: drop results from this realm — the cross-project transfer case |
| `limit` | integer | no | — | Max results, 1-100 (default 8) |
| `memory_id` | integer | no | — | structural: probe memory whose relation shape is matched (excluded from results) |
| `mode` | string | no | — | 'proportional' (a:b :: c:?) or 'structural' (shape match). Default: structural |
| `realm` | string | no | — | structural: restrict results to this realm |
| `text` | string | no | — | structural: free-text probe, used when memory_id is absent; anchors on the entities it mentions |

### `recall_by_priority` *(via advanced)*

Budget-aware recall

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `budget_tokens` | integer | no | — |  |
| `include_global` | boolean | no | — |  |
| `query` | string | no | — |  |
| `realm` | string | no | — |  |

### `recall_causal_antecedent` *(via advanced)*

PMI-ranked causal antecedents: what actions typically precede (tool, entity)?

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `entity` | string | yes | — |  |
| `k` | integer | no | — |  |
| `tool` | string | yes | — |  |

### `recall_counterfactual` *(via advanced)*

CDAWG sibling-edge counterfactual: what alternative tool/entity would have had a lower failure rate in this same context?

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `entity` | string | yes | — |  |
| `k` | integer | no | — |  |
| `outcome` | integer | no | — | 0=success 1=fail 2=error 3=partial (default 1) |
| `tool` | string | yes | — |  |

### `recall_failure_pattern` *(via advanced)*

Return top-k CDAWG states with high failure rates (fail_ratio > 0.6, fail_count >= 3)

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `k` | integer | no | — |  |

### `recall_hdcbind` *(via advanced)*

Heteroassociative HDC query: given known_role=known_val, infer query_role. Roles: tool, entity, outcome.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `k` | integer | no | — |  |
| `known_role` | string | yes | — |  |
| `known_val` | string | yes | — |  |
| `query_role` | string | yes | — |  |

### `recall_keyword`

BM25 keyword search

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `explain` | boolean | no | — | Include score decomposition per hit (default: false) |
| `limit` | integer | no | — | Max results (default 10) |
| `query` | string | yes | — | Search query |
| `realm` | string | no | — | Filter by realm (empty = all visible) |

### `recall_lanes`

Fan-in prompt recall lanes over one daemon RPC

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `budget_ms` | integer | no | — | Per-lane wall-clock budget override; capped by CHITTA_RPC_BUDGET_MS |
| `ctx_query` | string | no | — | Optional context query; defaults to query |
| `lanes` | array<string> | no | — | Subset of sem, ctx, hyb, kw, corr, corrk (default all) |
| `limits` | object | no | — | Per-lane result limits (sem, ctx, hyb, kw, corr) |
| `query` | string | yes | — | Prompt query shared by sem, hyb, kw, corr, and corrk |
| `realm` | string | no | — | Realm for sem, ctx, hyb, and kw |

### `recall_last_action` *(via advanced)*

Return last k occurrences of (tool, entity) from the CEC event tape

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `entity` | string | yes | — |  |
| `k` | integer | no | — |  |
| `tool` | string | yes | — |  |

### `recall_motif_value` *(via advanced)*

Return top-k CDAWG motif states reachable from (tool, entity) ranked by Q-value: which action sequences have the highest expected success rate from this context?

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `entity` | string | yes | — |  |
| `k` | integer | no | — | Max states to return (default 5) |
| `tool` | string | yes | — |  |

### `recall_session`

Session-level recall: groups chunk evidence by source session using noisy-OR aggregation. Returns ranked sessions with best evidence.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — | Max sessions to return (default 10) |
| `query` | string | yes | — | Natural language query |
| `realm` | string | no | — | Realm filter (optional) |

### `recall_smart` *(gateway)*

Multi-lane retrieval planner: uses an LLM call to extract entities and speech-act type, then fans out to semantic, typed, spreading-activation, and session-level lanes, merging results with Reciprocal Rank Fusion. Supports multi-part queries.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | `10` | Max results |
| `query` | string | yes | — | Natural language query |
| `realm` | string | no | — | Memory realm |
| `skip_llm_plan` | boolean | no | `false` | Skip LLM planning step (faster) |

### `recall_spreading` *(gateway)*

Retrieve memories via entity graph spreading activation. Extracts capitalized words, @refs, and quoted strings from the query as entity seeds, then traverses the triplet graph (BFS depth 2, decay 0.6) to find related memories.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | `10` | Max results |
| `query` | string | yes | — | Query; entity seeds extracted automatically |
| `realm` | string | no | — | Memory realm |

### `recall_temporal` *(via advanced)*

Search memories within a time window (defaults to last 7 days)

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `end` | string | no | — | End date |
| `include_global` | boolean | no | — | Include global memories |
| `limit` | integer | no | — | Max results (default 20) |
| `query` | string | no | — | Optional semantic search query |
| `realm` | string | no | — | Filter by realm |
| `start` | string | no | — | Start date (ISO8601 or YYYY-MM-DD) |

### `recall_true_counterfactual` *(via advanced)*

Return decision points where (tool, entity) was explicitly considered and rejected. Uses the DecisionTape (Phase 10), not CDAWG sibling inference. Requires prior log_event_ex or log_decision calls.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `entity` | string | yes | — |  |
| `k` | integer | no | — | Max results (default 5) |
| `outcome` | integer | no | — | 0=success 1=fail 2=error (default 0) |
| `tool` | string | yes | — |  |

### `recall_ucb1`

Recall with UCB1 exploration bonus — surfaces novel under-accessed memories alongside relevant ones

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `exploration` | number | no | — | Exploration weight sqrt(2)≈1.414 (default) |
| `fetch_k` | integer | no | — | Candidate pool size before re-ranking (default 40) |
| `limit` | integer | no | — | Max results (default 10) |
| `query` | string | yes | — | Search query |
| `realm` | string | no | — | Filter by realm |

### `resonance_stats` *(via advanced)*

Show ResonanceLearner Bayesian bandit stats

No parameters.

### `search_symbols`

Semantic search for code symbols

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `kind` | string | no | — |  |
| `limit` | integer | no | — |  |
| `project` | string | no | — |  |
| `query` | string | yes | — |  |

### `smart_recall` *(via advanced)*

Intelligent memory recall with hierarchical expansion

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `expand_top` | integer | no | — |  |
| `gwt_mode` | boolean | no | — |  |
| `include_global` | boolean | no | — |  |
| `limit` | integer | no | — |  |
| `query` | string | yes | — |  |
| `realm` | string | no | — |  |
| `separation_mode` | boolean | no | — |  |

### `stageb_set_surface`

Stage B: set a memory's natural-language retrieval surface and re-embed from it (keeps telegraphic content as display). Pass surface='' to clear and re-embed from content. Returns the id and embedded flag.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | string | yes | — |  |
| `surface` | string | yes | — |  |

### `structured_recall`

Three-lens recall: facts, context, and temporal agents merged for high-fidelity retrieval

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — |  |
| `query` | string | yes | — |  |
| `realm` | string | no | — |  |

<a id="graph"></a>

## Graph & Triplets

### `assoc_census`

Read-only assoc-graph census: per-EdgeType directed-edge count plus weight histogram (<0.05 / 0.05-0.2 / 0.2-0.5 / 0.5-0.8 / >=0.8). Measure-first gate for plasticity levers.

No parameters.

### `assoc_decay`

Gate B: decay + floor-prune one assoc EdgeType (wire numbering 0-5, default 3=CoRetrieved). Every edge weight is multiplied by factor; edges below prune_below are removed. apply=false is a dry run (counts only). One-shot saturation migration: factor=1.0, prune_below=0.2. Snapshot before apply.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `apply` | boolean | no | — |  |
| `edge_type` | integer | no | — |  |
| `factor` | number | no | — |  |
| `prune_below` | number | no | — |  |

### `connect` *(via advanced)*

Create a triplet relationship

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `object` | string | yes | — | Object entity |
| `predicate` | string | yes | — | Relationship type |
| `subject` | string | yes | — | Subject entity |

### `connect_temporal` *(via advanced)*

Create triplet with temporal validity

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `context_date` | string | no | — |  |
| `object` | string | yes | — |  |
| `predicate` | string | yes | — |  |
| `subject` | string | yes | — |  |
| `valid_from` | string | no | — |  |
| `valid_to` | string | no | — |  |

### `cooccurrence_graph` *(via advanced)*

Show top co-activated memory associations for a given memory

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | string | yes | — | Memory ID |
| `limit` | integer | no | — | Max edges to return (default 10) |

### `graph_pagerank`

Personalized PageRank over the triplet graph

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `damping` | number | no | — | Damping factor (default 0.85) |
| `edge_types` | array<string> | no | — | Predicate filter (empty = all) |
| `iterations` | integer | no | — | PPR iterations (default 20) |
| `seeds` | array<string> | yes | — | Seed nodes |
| `top_k` | integer | no | — | Nodes to return (default 20) |

### `graph_traverse`

BFS graph traversal from a start node over triplet edges

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `direction` | string (outgoing|incoming|both) | no | — | Edge direction (default outgoing) |
| `edge_types` | array<string> | no | — | Predicate filter (empty = all) |
| `max_hops` | integer | no | — | Max BFS depth (default 3) |
| `max_results` | integer | no | — | Max nodes returned (default 50) |
| `start` | string | yes | — | Starting node |

### `grow` *(via advanced)*

Add wisdom, belief, failure, aspiration, or dream

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `content` | string | yes | — |  |
| `tags` | string | no | — |  |
| `title` | string | no | — |  |
| `type` | string | yes | — |  |

### `query` *(via advanced)*

Query triplets with flexible filters

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `object` | string | no | — |  |
| `predicate` | string | no | — |  |
| `subject` | string | no | — |  |

### `query_graph` *(via advanced)*

Query triplets by subject or object

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `object` | string | no | — | Query by object |
| `subject` | string | no | — | Query by subject |

### `query_triplets_temporal` *(via advanced)*

Query triplets at a point in time

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `at_date` | string | no | — | YYYY-MM-DD |
| `limit` | integer | no | — |  |
| `object` | string | no | — |  |
| `predicate` | string | no | — |  |
| `subject` | string | no | — |  |

### `triplet_history` *(via advanced)*

Get history of a subject-predicate relationship

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — |  |
| `predicate` | string | yes | — |  |
| `subject` | string | yes | — |  |

### `triplet_query_as_of`

Query triplets for a subject valid at a given world timestamp, excluding superseded entries

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `subject` | string | yes | — | Subject node to query |
| `world_ms` | integer | no | — | World-time epoch ms (default: now) |

### `triplet_supersede`

Mark one triplet as superseded by another (bi-temporal update)

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `at_ms` | integer | no | — | Ingestion-time of supersession (default: now) |
| `new_id` | integer | yes | — | Replacing triplet ID |
| `old_id` | integer | yes | — | Triplet ID being superseded |

### `triplets` *(gateway)*

Knowledge graph operations. Actions: connect (create temporal triplet), query (query temporal triplets), history (triplet history for a subject/object)

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `action` | string | yes | — | connect\|query\|history |
| `limit` | any | no | — | Max results |
| `object` | string | no | — | Object entity |
| `predicate` | string | no | — | Relationship type |
| `subject` | string | no | — | Subject entity |

<a id="code"></a>

## Code Intelligence

### `code_context`

Get code context summary

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `path` | string | no | — |  |

### `codebase_overview`

Get full indexed codebase structure

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `format` | string | no | — |  |
| `include_callsites` | boolean | no | — |  |
| `project` | string | no | — |  |

### `embed_symbols` *(via advanced)*

Fast embed symbol metadata

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `batch_size` | integer | no | — |  |
| `reset` | boolean | no | — |  |

### `find_symbol`

Search for symbols by name

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `kind` | string | no | — |  |
| `name` | string | yes | — |  |

### `learn_codebase` *(via advanced)*

Learn codebase by extracting symbols. path can be a local directory or a remote git URL (https://github.com/..., git@github.com:...). Remote repos are shallow-cloned into a temp dir, indexed, then deleted.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `branch` | string | no | — | Branch, tag, or commit to clone (remote only) |
| `exclude` | string | no | — |  |
| `force` | boolean | no | — |  |
| `incremental` | boolean | no | — |  |
| `max_files` | integer | no | — |  |
| `path` | string | yes | — | Local path or remote git URL |
| `project` | string | no | — | Project name (defaults to repo/dir name) |

### `read_function` *(gateway)*

Read a function's code. Convenience wrapper for read_symbol with kind=function.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `context` | integer | no | — | Lines of context before function (default: 3) |
| `name` | string | yes | — | Function name to read |
| `project` | string | no | — | Project name filter (optional) |

### `read_symbol` *(gateway)*

Read just a symbol's code, not entire file. ~10x token savings vs full file read. Returns [kind name @ file:line-line] + code.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `context` | integer | no | — | Lines of context before symbol (default: 3) |
| `kind` | string | no | — | Symbol kind filter: class, function, method (optional) |
| `name` | string | yes | — | Symbol name to read (e.g., 'DuckDBStore', 'daemon_call') |
| `project` | string | no | — | Project name filter (optional) |

### `symbol_callees` *(gateway)*

Find all symbols that a symbol calls. Queries triplets where predicate=calls and subject=symbol.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — | Max results (default: 20) |
| `name` | string | yes | — | Symbol name to find callees for |

### `symbol_callers` *(gateway)*

Find all callers of a symbol without grep. Queries triplets where predicate=calls and object=symbol.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — | Max results (default: 20) |
| `name` | string | yes | — | Symbol name to find callers for |

### `symbol_event_log` *(via advanced)*

Query the symbol-keyed event log. Filter by symbol_name and/or file_path.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `file_path` | string | no | — | Filter by file path |
| `limit` | integer | no | — | Max events to return (default 50) |
| `symbol_name` | string | no | — | Filter by symbol name |

<a id="context"></a>

## Context & Status

### `ask`

Natural language insight query: retrieves and synthesizes memories to answer a question about the user, session, or project

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — |  |
| `question` | string | yes | — |  |
| `realm` | string | no | — |  |

### `checkpoint`

Save session state

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `active_files` | array<string> | no | — |  |
| `discoveries` | array<string> | no | — |  |
| `mood` | string | no | — |  |
| `next_steps` | array<string> | no | — |  |
| `realm` | string | no | — |  |
| `summary` | string | no | — |  |

### `compact_context`

Memory-aware context compaction. Scores conversation messages by recency, semantic relevance to query, and memory coverage (content already in memory is safer to drop). Returns a subset of messages fitting the target token budget.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `distill_novel` | boolean | no | — | Reserved for future use |
| `messages` | array<object> | yes | — | Conversation messages [{role,content}] |
| `query` | string | no | — | Upcoming task hint for semantic scoring |
| `target_ratio` | number | no | — | Fraction of tokens to KEEP (default 0.4) |

### `explore_expand` *(via advanced)*

Get full content of a memory

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | string | yes | — | Memory ID |

### `explore_neighbors` *(via advanced)*

Get nodes connected via triplets

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `direction` | string | no | — | outgoing, incoming, or both |
| `node` | string | yes | — | Node name |

### `explore_peek` *(via advanced)*

Get summary of a memory (first 200 chars)

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | string | yes | — | Memory ID |

### `explore_recall` *(via advanced)*

Lightweight recall - titles/scores only

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — | Max results (default 10) |
| `query` | string | yes | — | Search query |

### `get_policies` *(via advanced)*

Get active policies

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — |  |
| `scope` | string | no | — |  |
| `type` | string | no | — |  |

### `impl_start`

Start self-improvement implementation sadhana

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `interval_seconds` | integer | no | — |  |
| `max_turns` | integer | no | — |  |
| `realm` | string | no | — |  |
| `repo` | string | no | — |  |

### `list_policies` *(via advanced)*

List CEC intervention policies (shadow and active). Each entry shows rule source, kind (OpenTask/TurnInjection/GuardPolicy), shadow event count, lift, and fire count.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `active_only` | boolean | no | — | If true, only return active (promoted) policies |

### `observe`

Store an observation/learning (SSL v0.4)

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `arousal` | number | no | — | Affect arousal: 0.0 to 1.0 |
| `category` | string | no | — |  |
| `confidence` | number | no | — |  |
| `content` | string | yes | — |  |
| `derivation` | string | no | — | SSL v0.4 <=@ provenance: comma-separated source memory IDs this was abstracted from (required at G:1+) |
| `flags` | string | no | — | Structural flags: ORIGIN,CORE,PIVOT,GENESIS,TURNING |
| `granularity` | integer | no | — | SSL v0.4 granularity tier: 0=atom,1=episode,2=claim,3=operator,4=boundary |
| `refs` | string | no | — | Cross-references: comma-separated tag names or memory IDs |
| `source_loc` | string | no | — | SSL v0.4 src: external source grounding, e.g. file:line or doc section |
| `tags` | string | no | — |  |
| `title` | string | yes | — |  |
| `valence` | number | no | — | Affect valence: -1.0 to +1.0 |

### `smart_context` *(gateway)*

Build intelligent context. Modes: fast (<80ms), full (<200ms), rlm (RLM-style dynamic exploration via soul_repl). With resolver_mode=true (default), prepends digest-node and decision memories before code symbols.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `code` | boolean | no | — | Include code symbols (default: true) |
| `limit` | integer | no | — | Token limit (default: 300) |
| `memories` | boolean | no | — | Include semantic memories (default: true) |
| `mode` | string (fast|full|rlm) | no | — | fast: <80ms (vector + BM25), full: <200ms (full_resonate), rlm: dynamic exploration via soul_repl |
| `neighbors` | boolean | no | — | Include triplet neighbors (default: true) |
| `realm` | string | no | — | Filter by realm |
| `resolver_mode` | boolean | no | — | Prepend digest-node and decision memories before code symbols (default: true) |
| `task` | string | yes | — | Query to find context for |

### `soul_context`

Get current soul state and statistics

No parameters.

### `soul_repl` *(gateway)*

RLM-style Python REPL for programmatic memory exploration. Write code with soul.* methods: search(), recall(), expand(), triplets(), recent(), remember(), symbols(). Supports persistent sessions via session_id. Call with no code for API reference.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `code` | string | no | — | Python code to execute. Has access to soul.search(), soul.recall(), soul.expand(), etc. |
| `reset` | boolean | no | — | Reset namespace before execution (default: false) |
| `session_id` | string | no | — | Session name for persistent state — variables survive across calls with the same session_id |

### `think_wander`

Trigger internal memory synthesis

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `realm` | string | no | — |  |

### `trajectory_compact` *(via advanced)*

Attention-weighted turn selection from a transcript. Embeds each turn, scores by cosine similarity to the task description, applies MAD adaptive threshold, enforces token budget. Returns a lossless subset of the most task-relevant turns.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `budget_tokens` | integer | no | — | Target token budget (default 4000) |
| `include_system` | boolean | no | — | Include system turns (default false) |
| `mad_k` | number | no | — | MAD threshold multiplier (default 1.5, lower=more turns) |
| `path` | string | no | — | Direct path to JSONL transcript |
| `role_filter` | string | no | — | Filter by role: user, assistant, or empty for all |
| `session_id` | string | no | — | Session ID (auto-finds transcript) |
| `task` | string | yes | — | What the downstream agent needs to accomplish |

<a id="realm"></a>

## Realms

### `realm_add` *(via advanced)*

Add memory to a shared realm (stub)

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | string | yes | — |  |
| `realm` | string | yes | — |  |

### `realm_detect` *(via advanced)*

Detect current realm from environment

No parameters.

### `realm_get` *(via advanced)*

Get all realms a memory belongs to

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | string | yes | — |  |

### `realm_list` *(via advanced)*

List all known realms

No parameters.

### `realm_remove` *(via advanced)*

Remove memory from a shared realm (stub)

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | string | yes | — |  |
| `realm` | string | yes | — |  |

### `realm_set` *(via advanced)*

Set primary realm for a memory

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | string | yes | — |  |
| `realm` | string | yes | — |  |

### `realm_visibility` *(via advanced)*

Set visibility level (stub)

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | string | yes | — |  |
| `visibility` | integer | yes | — |  |

### `remap_realms`

Bulk-remap realms per {old:new} mapping (mapping_file=path or mapping=object; dry_run defaults true)

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `dry_run` | boolean | no | — |  |
| `mapping` | object | no | — |  |
| `mapping_file` | string | no | — |  |

### `trim_realm_names`

Fix realm names with trailing whitespace/newlines

No parameters.

<a id="session"></a>

## Sessions & Continuity

### `get_turns` *(via advanced)*

Get conversation turns for a session

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — |  |
| `session_id` | string | no | — |  |
| `start_index` | integer | no | — |  |

### `ledger_delete` *(via advanced)*

Delete checkpoint

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `key` | string | no | — | Canonical checkpoint key (session:project) |
| `project` | string | no | — |  |
| `session_id` | string | no | — |  |

### `ledger_get` *(via advanced)*

Get checkpoint by key or session/project

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `include_snapshot` | boolean | no | — | Include snapshot preview (truncated) in response |
| `key` | string | no | — | Canonical checkpoint key (session:project) |
| `project` | string | no | — |  |
| `session_id` | string | no | — |  |

### `ledger_health` *(via advanced)*

Get ledger event counts by kind and queue health metrics

No parameters.

### `ledger_list` *(via advanced)*

List recent checkpoints

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — |  |
| `project` | string | no | — |  |

### `ledger_load` *(via advanced)*

Load most recent checkpoint

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `include_snapshot` | boolean | no | — | Include snapshot preview (truncated) in response |
| `project` | string | no | — |  |
| `session_id` | string | no | — |  |

### `ledger_save` *(via advanced)*

Save session checkpoint

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `active_files` | array<string> | no | — |  |
| `blockers` | array<string> | no | — |  |
| `coherence` | number | no | — |  |
| `confidence` | number | no | — |  |
| `decisions` | array<string> | no | — |  |
| `discoveries` | array<string> | no | — |  |
| `mood` | string | no | — |  |
| `next_steps` | array<string> | no | — |  |
| `project` | string | no | — |  |
| `session_id` | string | no | — |  |
| `snapshot` | string | no | — |  |
| `todos` | array | no | — |  |
| `transcript_path` | string | no | — |  |

### `long_task_active` *(via advanced)*

Get active long-running task

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `realm` | string | no | — |  |

### `long_task_complete` *(via advanced)*

Mark task as completed

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `outcome` | string | yes | — |  |
| `task_id` | string | yes | — |  |

### `long_task_evaluate` *(via advanced)*

Evaluate task completion

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `task_id` | string | yes | — |  |

### `long_task_event` *(via advanced)*

Append event to task log

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `kind` | string | yes | — |  |
| `payload` | string | no | — |  |
| `related_entities` | array<string> | no | — |  |
| `tags` | array<string> | no | — |  |
| `task_id` | string | yes | — |  |

### `long_task_get` *(via advanced)*

Get a long-running task by ID

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `task_id` | string | yes | — |  |

### `long_task_snapshot` *(via advanced)*

Get synthesized task context

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `max_tokens` | integer | no | — |  |
| `mode` | string | no | — |  |
| `task_id` | string | yes | — |  |

### `long_task_start` *(via advanced)*

Start a long-running task

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `goal` | string | yes | — |  |
| `hard_checks` | array<string> | no | — |  |
| `realm` | string | no | — |  |
| `soft_checks` | array<string> | no | — |  |
| `task_id` | string | yes | — |  |
| `work_items` | array<string> | no | — |  |

### `long_task_update` *(via advanced)*

Update long-running task progress

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `blockers` | array<string> | no | — |  |
| `completed_summary` | string | no | — |  |
| `task_id` | string | yes | — |  |
| `work_items` | array<string> | no | — |  |

### `read_transcript`

Read JSONL transcript with pagination

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `keyword` | string | no | — |  |
| `limit` | integer | no | — |  |
| `max_chars_per_turn` | integer | no | — |  |
| `metadata_only` | boolean | no | — |  |
| `path` | string | no | — |  |
| `role_filter` | string | no | — |  |
| `session_id` | string | no | — |  |
| `start_turn` | integer | no | — |  |

### `session_list` *(via advanced)*

List active sessions

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `active_only` | boolean | no | — |  |
| `realm` | string | no | — |  |
| `status` | string | no | — |  |
| `ttl_seconds` | integer | no | — |  |

### `session_sync` *(via advanced)*

Sync session registry

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `projects_dir` | string | no | — |  |

### `task_state`

Deterministic task-state check (capability #3): what is the current state of task X? Exact keyed lookup of the LATEST stored [task] record by its slug — bypasses fuzzy recall so an agent resuming a discontinuous session gets the durable status/next-step. Returns found + the record.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | string | yes | — | Task slug (the id: token of the [task] record) |

<a id="messaging"></a>

## Cross-Harness Messaging

### `cross_harness_conflicts`

Find memories where claude-code and codex harnesses disagree on the same topic

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — | Max conflict pairs to return (default 20) |
| `min_disagreement_score` | number | no | — | Minimum 1-cosine_similarity threshold (default 0.3) |
| `realm` | string | no | — | Realm filter (default: all) |

### `msg_ack_all` *(via advanced)*

Acknowledge all messages

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `session_id` | string | no | — |  |

### `msg_history` *(via advanced)*

Get message history

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — |  |
| `session_id` | string | no | — |  |

### `msg_inbox` *(via advanced)*

Check unread messages

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `auto_ack` | boolean | no | — |  |
| `limit` | integer | no | — |  |
| `min_priority` | integer | no | — |  |
| `session_id` | string | no | — |  |

### `msg_respond` *(via advanced)*

Reply to a message using the original sender/target from the event

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `content` | string | yes | — | Reply content |
| `message_id` | integer | yes | — | Event ID of the message to reply to |
| `session_id` | string | no | — | Override sender session_id (defaults to original target) |

### `msg_send` *(via advanced)*

Send message to another session

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `content` | string | yes | — |  |
| `content_type` | string | no | — |  |
| `priority` | integer | no | — |  |
| `session_id` | string | no | — |  |
| `target` | string | yes | — |  |
| `target_type` | string | no | — |  |
| `ttl` | integer | no | — |  |

<a id="narrative"></a>

## Narrative & Work Modes

### `log_decision`

Log a decision point to the DecisionTape: chosen action + alternatives considered and rejected. Enables recall_true_counterfactual.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `chosen_entity` | string | yes | — |  |
| `chosen_outcome` | integer | no | — | 0=success 1=fail 2=error 3=partial |
| `chosen_tool` | string | yes | — |  |
| `confidence_delta` | number | no | — | chosen_confidence - best_alternative_confidence |
| `rejected_json` | string | no | — | JSON array of [sym_u64, rejection_reason_u8] pairs |
| `ts_ms` | integer | no | — |  |

### `log_exposure` *(via advanced)*

Log memory exposure (SUS)

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `hook_type` | string | yes | — |  |
| `memory_ids` | array<integer> | yes | — |  |
| `ranks` | array<integer> | no | — |  |
| `resonance_scores` | array<number> | no | — |  |
| `session_id` | string | yes | — |  |
| `turn_id` | integer | yes | — |  |

### `narrative_history` *(via advanced)*

Get work mode segment history

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — |  |
| `session_id` | string | no | — |  |

### `narrative_log` *(via advanced)*

Append event to session log

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `files_mentioned` | string | no | — |  |
| `kind` | string | yes | — |  |
| `payload` | string | no | — |  |
| `session_id` | string | no | — |  |
| `success` | boolean | no | — |  |
| `summary` | string | yes | — |  |
| `tool_name` | string | no | — |  |

### `narrative_status` *(via advanced)*

Get work mode and segment summary

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `session_id` | string | no | — |  |

<a id="distill"></a>

## Distillation & Embeddings

### `compact_wal` *(via advanced)*

Compact WAL: save full snapshot then delete covered segments

No parameters.

### `densify_backfill`

#13 retro-backfill of SameSession edges over existing session-tagged memories (same K=3 decaying-weight rule as the write-time hook). apply=false is a dry run: reports sessions, memories, sibling pairs, and a group-size histogram, writing nothing. apply=true creates the edges (idempotent; rollback via remove_assoc_edges_by_type).

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `apply` | boolean | no | — |  |

### `distill_now`

Synchronously distill ONE session now and return the counts (learnings + value-facts stored/deduped). Runs the lock-fixed distill path in-daemon; recall stays responsive.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `realm` | string | no | — | Optional realm (default brahman or the registered realm) |
| `session_id` | string | yes | — | Session ID to distill |
| `transcript_path` | string | no | — | Optional JSONL path; registers it if given, else resolves from durable transcript/register |

### `distill_set_model` *(via advanced)*

Change distillation model

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `enabled` | boolean | no | — |  |
| `model` | string | yes | — |  |

### `embed_coverage`

Semantic-index coverage: how many memories SHOULD have a vector vs how many DO. pending_count cannot answer this — the embed queue drains on failure as well as success, so a lost embedding leaves the queue empty.

No parameters.

### `embed_probe`

For a memory id: cosine of its STORED vector against a fresh single-prefix embed vs a double-prefix embed of the same content. ~1.0 identifies which vector space the memory actually lives in.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | string | yes | — |  |

### `flush_embeddings`

Flush the pending embedding queue synchronously — call after remember_batch to ensure HNSW/semantic recall is available immediately. Returns {flushed: N}.

No parameters.

### `get_embeddings`

Batch-fetch raw embedding vectors for memory IDs. Returns JSON object mapping id→vector. Used for computing S-entropy (Gram matrix effective rank) over recall candidates.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `ids` | array<string> | yes | — | Array of memory ID strings |

### `health_check` *(via advanced)*

Check daemon health

No parameters.

### `health_check_start` *(gateway)*

Start autonomous health-check sadhana that monitors memory quality, dedup ratio, and embedding coverage

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `interval_seconds` | integer | no | — | Check interval in seconds (default: 3600) |
| `max_turns` | integer | no | — | Max check cycles (default: 0 = unlimited) |
| `realm` | string | no | — | Realm to monitor (default: brahman) |

### `pending_embed_ids`

Return IDs of memories awaiting embedding (stuck embed queue)

No parameters.

### `queue_status` *(via advanced)*

Live work-queue depth (pending in-flight + unclaimed), processed/distilled/failed counters, pending embeddings, dead-letters

No parameters.

### `rebuild_fts_index` *(via advanced)*

Rebuild FTS index for BM25 search

No parameters.

### `semantic_backfill`

Dense-kNN SemanticNeighbor edge backfill: for each non-deleted memory, link its top-k realm-scoped HNSW neighbors (cosine>=min_cos) with bidirectional similarity edges (wire 6). The first genuine memory<->memory knowledge relation in the assoc graph, distinct from CoRetrieved's retrieval-history prior. apply=false is a dry run (counts candidate edges, no writes). apply=true creates them (idempotent; rollback via remove_assoc_edges_by_type edge_type=6).

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `apply` | boolean | no | — |  |
| `k` | integer | no | — |  |
| `min_cos` | number | no | — |  |

<a id="consolidation"></a>

## Consolidation & Contradictions

### `conflict_inspector`

Semantic search + show status and contradiction partners for each hit

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — | Max memories to scan (default 10) |
| `query` | string | yes | — | Search query |
| `realm` | string | no | — | Filter by realm |

### `consolidate_similar` *(via advanced)*

Merge near-duplicate memories — keeps stronger, soft-deletes weaker

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `dry_run` | boolean | no | — | Preview without deleting (default true) |
| `limit` | integer | no | — | Max pairs to merge (default 10) |
| `realm` | string | no | — | Filter by realm |
| `threshold` | number | no | — | Similarity threshold (default 0.92) |

### `consolidation_pass` *(via advanced)*

Run Sequitur grammar consolidation: find frequent bigrams in EventTape and promote rules to the triplet KG (subject=rule:..., predicates: compresses/avg_outcome/support/tape_range).

No parameters.

### `detect_contradictions`

Detect contradictions for a stored memory against realm peers

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `memory_id` | string | yes | — | Memory ID to check |
| `realm` | string | no | — | Realm to scan (default global) |

### `disable_source`

Add a source to the deny-list via triplet

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `source` | string | yes | — | Source identifier to deny |

### `find_near_duplicates` *(via advanced)*

Find memory pairs with high semantic similarity (near-duplicates)

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — | Max pairs to return (default 20) |
| `realm` | string | no | — | Filter by realm |
| `threshold` | number | no | — | Cosine similarity threshold (default 0.90) |

### `hygiene_stats` *(via advanced)*

Get memory hygiene statistics

No parameters.

### `reconsolidate`

Update content of a memory during its labile window (reconsolidation)

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `content` | string | yes | — | New/corrected content |
| `id` | string | yes | — | Memory ID to update |
| `reason` | string | no | — | Optional reason for reconsolidation |

### `resolve_contradiction`

Resolve a contradiction: declare winner supersedes loser, store CORRECTION memory

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `loser_id` | string | yes | — | Memory ID to demote |
| `reason` | string | no | — | Explanation for the resolution |
| `winner_id` | string | yes | — | Memory ID that is correct |

### `save_spectral_snapshot`

Save spectral stats snapshot for drift tracking

No parameters.

### `scan_contradictions`

Background scan: find contradiction candidates across all memories in a realm

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — | Max candidates to return (default 50) |
| `realm` | string | no | — | Realm to scan |

### `show_conflicts` *(via advanced)*

Semantic search + show contradiction pairs for matching memories

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — | Max memories to scan (default 20) |
| `query` | string | yes | — | Search query |
| `realm` | string | no | — | Filter by realm |

### `spectral_drift`

Compare current embedding geometry with last snapshot

No parameters.

### `what_superseded` *(via advanced)*

Show the full supersession chain for a memory

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | string | yes | — | Memory ID to trace |

### `why_active` *(via advanced)*

Explain why a memory is active: status, epistemic source, confirmations, contradictions

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | string | yes | — | Memory ID to inspect |

<a id="theme"></a>

## Themes

### `theme_get` *(via advanced)*

Get theme details

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | integer | yes | — |  |

### `theme_list` *(via advanced)*

List all themes with statistics

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — |  |
| `realm` | string | no | — |  |

### `theme_recall` *(via advanced)*

Two-stage theme-based retrieval

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — |  |
| `query` | string | yes | — |  |
| `realm` | string | no | — |  |

### `theme_stats` *(via advanced)*

Get theme organization statistics

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `realm` | string | no | — |  |

<a id="provenance"></a>

## Provenance & Verification

### `behavioral_probe` *(via advanced)*

Score text against behavioral centroid clusters. Returns per-class similarity scores (sycophantic/hedging/shallow/direct) and overall quality estimate. Requires prior probe_seed calls.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `text` | string | yes | — | Text to probe (e.g. a Claude response) |

### `calibration_record` *(via advanced)*

Record prediction outcome

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `domain` | string | yes | — |  |
| `success` | boolean | yes | — |  |

### `calibration_score` *(via advanced)*

Get accuracy score

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `domain` | string | no | — |  |

### `correction_check`

Deterministic durable-correction check (capability #2): does any stored [correction] trigger recur in this turn? Exact keyed bigram probe of the turn text against corrected-mistake phrases — reserves an injection slot, bypassing fuzzy recall (which loses corrections on cosine similarity ~99% of the time). Returns found + the correction(s), newest first (latest-wins).

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `text` | string | yes | — | Turn / context text to scan for a recurring corrected mistake |

### `get_relationship_events` *(via advanced)*

Get relationship events

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `event_type` | string | no | — |  |
| `limit` | integer | no | — |  |
| `session_id` | string | no | — |  |

### `probe_calibrate` *(via advanced)*

Add a confirmed exemplar to a behavioral class to refine its centroid. Use when you have a clear example of the behavior.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `class` | string | yes | — | Behavioral class to update |
| `text` | string | yes | — | Confirmed exemplar text |

### `probe_seed` *(via advanced)*

Store an exemplar text as a centroid for a behavioral class (sycophantic/hedging/shallow/direct). Bootstrap the probe with representative examples.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `class` | string | yes | — | Behavioral class: sycophantic \| hedging \| shallow \| direct |
| `note` | string | no | — | Optional annotation |
| `text` | string | yes | — | Exemplar text for this class |

### `probe_status` *(via advanced)*

Show how many exemplars exist per behavioral class. Use to verify the probe is seeded before running behavioral_probe.

No parameters.

### `provenance_check`

Deterministic anti-reprocessing check: has this file/task already been processed? Exact keyed lookup of a prior [done] record by content-hash and/or input path — bypasses fuzzy recall. Returns found + the record.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `input` | string | no | — | Input path (fallback handle) |
| `sha` | string | no | — | Content hash of the input (tried first — content identity) |

### `span_backfill` *(via advanced)*

Backfill the span lane over all transcripts under projects_dir (default ~/.claude/projects). Incremental + idempotent via per-file watermarks. Reports unique atoms, new atoms, redaction count.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `projects_dir` | string | no | — | Transcript root (default ~/.claude/projects) |

### `span_backfill_memories` *(via advanced)*

Backfill the memory↔span edge: run the atom extractor over every live memory's text so distilled beliefs link to the verbatim paths/commands/ids they mention. Idempotent (per-memory content hash); superseded memories relink. Reports memories linked + new spans.

No parameters.

### `span_query`

Retrieve verbatim transcript atoms (file paths, URLs, file:line locators, bash commands, error signatures) captured across all sessions. No LLM/GPU: exact-substring recall over a deduplicated span index. Realm-scoped by default to prevent cross-project bleed. Use when you need an exact locator you saw before, not a paraphrase.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `k` | integer | no | — | Max atoms to return (default 6) |
| `query` | string | yes | — | Substring or token to match against stored atoms |
| `realm` | string | no | — | Restrict to this realm (project); empty = unscoped |

### `span_stats` *(via advanced)*

Report span lane size: unique atoms, on-disk bytes, total redactions.

No parameters.

<a id="sadhana"></a>

## Sadhana

### `sadhana` *(gateway)*

Autonomous background learning loop control. Actions: start, stop, pause, resume, status, list, checkpoint, set_goal, set_interval, set_model

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `action` | string | yes | — | start\|stop\|pause\|resume\|status\|list\|checkpoint\|set_goal\|set_interval\|set_model |
| `goal` | string | no | — | Goal text (for set_goal) |
| `id` | any | no | — | Sadhana ID (for stop/pause/resume/status/checkpoint) |
| `interval` | any | no | — | Interval in seconds (for set_interval) |
| `max_turns` | any | no | — | Max turns (for start) |
| `model` | string | no | — | Model name (for set_model/start) |

### `sadhana_checkpoint` *(via advanced)*

Report mid-cycle checkpoint

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | integer | yes | — |  |
| `status` | string (progressed|achieved|blocked) | yes | — |  |
| `summary` | string | yes | — |  |

### `sadhana_list` *(via advanced)*

List sadhanas

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — |  |
| `realm` | string | no | — |  |
| `state` | string | no | — |  |

### `sadhana_pause` *(via advanced)*

Pause a sadhana

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | integer | yes | — |  |

### `sadhana_resume` *(via advanced)*

Resume a paused sadhana

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | integer | yes | — |  |

### `sadhana_set_goal` *(via advanced)*

Change sadhana goal

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `goal` | string | yes | — |  |
| `id` | integer | yes | — |  |

### `sadhana_set_interval` *(via advanced)*

Change tick interval

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | integer | yes | — |  |
| `interval` | integer | yes | — |  |

### `sadhana_set_max_turns` *(via advanced)*

Set max turns per cycle

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | integer | yes | — |  |
| `max_turns` | integer | yes | — |  |

### `sadhana_set_model` *(via advanced)*

Change brain model

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | integer | yes | — |  |
| `model` | string | yes | — |  |

### `sadhana_start` *(via advanced)*

Create and start an autonomous agent

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `brain_model` | string | no | — |  |
| `brain_provider` | string | no | — |  |
| `goal` | string | yes | — |  |
| `goal_dsl` | object | no | — |  |
| `interval_seconds` | integer | no | — |  |
| `max_turns` | integer | no | — |  |
| `realm` | string | no | — |  |

### `sadhana_status` *(via advanced)*

Get sadhana status

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `history_limit` | integer | no | — |  |
| `id` | integer | yes | — |  |

### `sadhana_stop` *(via advanced)*

Stop a sadhana

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | integer | yes | — |  |
| `reason` | string | no | — |  |
| `success` | boolean | no | — |  |

<a id="dream"></a>

## Dreams & Curiosity

### `curiosity_gaps` *(via advanced)*

List knowledge gaps

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — |  |
| `realm` | string | no | — |  |

### `curiosity_note_gap` *(via advanced)*

Record a knowledge gap

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `context` | string | no | — |  |
| `gap` | string | yes | — |  |
| `realm` | string | no | — |  |

### `curiosity_resolve` *(via advanced)*

Mark gap as resolved

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | integer | yes | — |  |
| `learned` | string | no | — |  |

### `dream_cancel` *(via advanced)*

Cancel a dream

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | integer | yes | — |  |

### `dream_force_woke` *(via advanced)*

Force stuck dream to woke

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | integer | yes | — |  |

### `dream_list` *(via advanced)*

List recent dreams

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — |  |
| `realm` | string | no | — |  |

### `dream_start` *(via advanced)*

Start an autonomous dream

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `brain_model` | string | no | — | Model name, e.g. gemma4:26b or sonnet |
| `brain_provider` | string | no | — | Brain provider: claude or local |
| `publish_path` | string | no | — |  |
| `realm` | string | no | — |  |
| `topic` | string | yes | — |  |

### `dream_status` *(via advanced)*

Get dream details

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | integer | yes | — |  |

### `dream_wander` *(via advanced)*

Auto-select topic and dream

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `publish_path` | string | no | — |  |
| `realm` | string | no | — |  |

### `research` *(gateway)*

Curiosity-driven research. Actions: cycle (get topic to research), topics (list research topics), store (save findings)

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `action` | string | yes | — | cycle\|topics\|store |
| `findings` | string | no | — | Findings (store) |
| `gap_id` | any | no | — | Gap ID to resolve (store) |
| `limit` | any | no | — | Max results |
| `realm` | string | no | — | Filter by realm |
| `source` | string | no | — | Topic source: gaps\|weak\|suggest (topics) |
| `sources` | array<string> | no | — | URLs (store) |
| `topic` | string | no | — | Topic (store) |

### `research_cycle` *(gateway)*

Run one curiosity-driven research cycle. Returns a topic to research with context. After calling this, use WebSearch to find information, then call research_store with results.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `realm` | string | no | — | Filter by realm/project |

### `research_store` *(gateway)*

Store research results as memories with source attribution. Call after using WebSearch to learn about a topic.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `confidence` | number | no | — | Confidence in findings 0-1 (default: 0.7) |
| `findings` | string | yes | — | Key learnings in SSL format |
| `gap_id` | integer | no | — | Curiosity gap ID to resolve (optional) |
| `sources` | array<string> | no | — | URLs or references |
| `topic` | string | yes | — | What was researched |

### `research_topics` *(gateway)*

Get topics that need research. Returns curiosity gaps, low-confidence memories, or suggested topics. Use WebSearch to research these, then store results with research_store.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — | Max topics to return (default: 3) |
| `realm` | string | no | — | Filter by realm/project |
| `source` | string | no | — | Where to get topics: gaps (curiosity gaps), weak (low-confidence memories), suggest (AI-suggested based on recent work) |

<a id="learn"></a>

## Learning & Outcomes

### `effective_scorer_weights` *(via advanced)*

Show effective scoring weights — baseline + learned deltas for all factors

No parameters.

### `get_source_weights` *(via advanced)*

View learned recall source weights — how much each source is trusted per domain

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `domain` | string | no | — | Filter by domain (omit for all) |

### `get_sus_metrics` *(via advanced)*

Get Soul Utility Score metrics

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `days` | integer | no | — |  |

### `integration_stats` *(via advanced)*

Per-source success rates and learned weights across all domains

No parameters.

### `learn` *(gateway)*

Store learning in soul memory. Types: correction (wrong→right fix), preference (user style), insight (generalizable wisdom), approach (what worked in a state), outcome (did suggestion help?), milestone (achievement), analysis (reproducible analysis with data/scripts)

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `approach` | string | no | — | Approach text (approach) |
| `category` | string | no | — | Category (preference) |
| `context` | string | no | — | Context for the learning |
| `correct` | string | no | — | What is correct (correction) |
| `data_paths` | array<string> | no | — | Data paths (analysis) |
| `description` | string | no | — | Description (analysis) |
| `details` | string | no | — | Details (outcome) |
| `domain` | string | no | — | Domain (insight/analysis) |
| `findings` | string | no | — | Findings (analysis) |
| `helped` | boolean | no | — | Did it help? (outcome) |
| `insight` | string | no | — | Insight text (insight) |
| `milestone` | string | no | — | Milestone text (milestone) |
| `name` | string | no | — | Analysis name (analysis) |
| `outcome` | string | no | — | Outcome (approach) |
| `preference` | string | no | — | Preference text (preference) |
| `project` | string | no | — | Project (analysis) |
| `script_paths` | array<string> | no | — | Script paths (analysis) |
| `state` | string | no | — | State/mood (approach) |
| `suggestion` | string | no | — | Suggestion (outcome) |
| `type` | string | yes | — | correction\|preference\|insight\|approach\|outcome\|milestone\|analysis |
| `wrong` | string | no | — | What was wrong (correction) |

### `learn_analysis` *(gateway)*

Record an analysis with data and script locations. Makes analyses reproducible and findable later. Use after completing any significant analysis.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `data_paths` | array<string> | no | — | Paths to input data files/directories |
| `description` | string | no | — | What the analysis does/investigates |
| `findings` | string | no | — | Key findings or results (optional) |
| `name` | string | yes | — | Short name for the analysis (e.g., 'AGP codon optimization', 'bin_28 damage patterns') |
| `project` | string | no | — | Project name for organization (optional) |
| `script_paths` | array<string> | no | — | Paths to analysis scripts |

### `learn_approach` *(gateway)*

Store what approach worked when in a particular state/mood. Builds emotional memory for adapting to session dynamics.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `approach` | string | yes | — | What helped in this state (e.g., 'step back and reread requirements') |
| `outcome` | string | no | — | What happened after (optional) |
| `state` | string | yes | — | State/mood: stuck, debugging, exploring, flowing, frustrated, uncertain, rushing |

### `learn_correction` *(gateway)*

Store a correction when I was wrong. Creates high-confidence counter-memory with 'corrects' triplet linking to original mistake.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `context` | string | no | — | Context where this applies (optional) |
| `correct` | string | yes | — | The correct information/approach |
| `wrong` | string | yes | — | What I said/did that was incorrect |

### `learn_insight` *(gateway)*

Store a generalizable insight that applies across projects. Use for patterns, techniques, and wisdom not tied to specific codebase.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `domain` | string | yes | — | Domain: programming, debugging, architecture, testing, performance, security, communication |
| `insight` | string | yes | — | The generalizable insight or pattern |
| `learned_from` | string | no | — | Context where this was learned (optional, for future reference) |

### `learn_milestone` *(gateway)*

Record a relationship milestone - achievements, personal context, significant moments worth remembering.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `date` | string | no | — | When it happened (optional, defaults to now) |
| `milestone` | string | yes | — | What happened (e.g., 'shipped v1.0', 'first successful release') |
| `significance` | string | no | — | Why it matters (optional) |

### `learn_outcome` *(gateway)*

Record whether a suggestion/approach actually helped. Records outcomes for the suggestion feedback loop.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `details` | string | no | — | Why it helped or didn't (optional but valuable) |
| `helped` | boolean | yes | — | Did it help? true/false |
| `suggestion` | string | yes | — | What was suggested or tried |

### `learn_preference` *(gateway)*

Store a user preference for adapting communication/behavior. Global visibility so it applies across all projects.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `category` | string | yes | — | Preference category: communication, detail, autonomy, style, workflow |
| `example` | string | no | — | Example demonstrating this preference (optional) |
| `preference` | string | yes | — | The preference to remember (e.g., 'prefers concise responses') |

### `learned_scorer_stats` *(via advanced)*

Current learned scoring model — version, factor count, loss, outcome count

No parameters.

### `metacognition_evaluate` *(via advanced)*

Self-evaluate learning effectiveness

No parameters.

### `record_feedback` *(via advanced)*

Record whether a recall source was useful — updates learned source weights

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `query_domain` | string | no | — | Domain of the query (default general) |
| `source` | string | yes | — | Source: semantic, keyword, temporal, artifact, association (required) |
| `was_useful` | boolean | no | — | Whether the source's results were useful (default true) |

### `surprise_learning_stats` *(via advanced)*

Rolling surprise credit stats — tracked memories, gates passed, strength adjustments

No parameters.

### `update_scorer_model` *(via advanced)*

Apply learned weight deltas to the scoring model from outcome calibration

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `mean_loss` | number | no | — | EWMA loss from calibration |
| `model_version` | integer | no | — | Monotonic version number |
| `outcome_count` | integer | no | — | Total outcomes used for calibration |
| `weights` | object | no | — | Factor name → {delta, min_delta, max_delta} learned adjustments |

### `update_source_weight` *(via advanced)*

Manually override a recall source weight

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `domain` | string | no | — | Domain (default general) |
| `source` | string | yes | — | Source name (required) |
| `weight` | number | no | — | New weight [0-2] (default 1.0) |

<a id="anticipation"></a>

## Anticipation & Habits

### `anticipation_filter` *(via advanced)*

Get predictions passing annoyance gate

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `max` | integer | no | — |  |
| `session_id` | string | no | — |  |

### `anticipation_list` *(via advanced)*

List learned anticipation patterns

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — |  |
| `realm` | string | no | — |  |
| `sort_by` | string | no | — |  |

### `anticipation_observe` *(via advanced)*

Record context->action pattern

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `action` | string | yes | — |  |
| `context` | string | yes | — |  |
| `realm` | string | no | — |  |

### `anticipation_predict` *(via advanced)*

Predict likely actions

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `context` | string | yes | — |  |
| `limit` | integer | no | — |  |
| `min_confidence` | number | no | — |  |
| `realm` | string | no | — |  |

### `anticipation_success` *(via advanced)*

Mark prediction as successful

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | integer | yes | — |  |

### `habit_list` *(via advanced)*

List formed habits

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — |  |
| `min_strength` | number | no | — |  |
| `realm` | string | no | — |  |
| `sort_by` | string | no | — |  |

### `habit_match` *(via advanced)*

Find matching habits

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `context` | string | yes | — |  |
| `min_strength` | number | no | — |  |
| `realm` | string | no | — |  |

### `habit_observe` *(via advanced)*

Record trigger->response pattern

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `realm` | string | no | — |  |
| `response` | string | yes | — |  |
| `trigger` | string | yes | — |  |

### `habit_strengthen` *(via advanced)*

Strengthen a habit

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `amount` | number | no | — |  |
| `id` | integer | yes | — |  |

### `habit_weaken` *(via advanced)*

Weaken a habit

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `amount` | number | no | — |  |
| `id` | integer | yes | — |  |

### `predict_needed` *(via advanced)*

Get predicted next-needed memories from the Markov chain access predictor

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `k` | integer | no | — | Number of predictions (default 8) |

### `trigger_add` *(via advanced)*

Create a trigger automaton (prospective memory). Arms on creation, fires when conditions met or tension exceeds threshold.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `action` | object | yes | — | Action on fire (Notify, InjectMemory, EmitEvent, RememberFact) |
| `condition` | object | yes | — | Trigger condition (TimeAfter, ConstraintMatch, EventMatch, AllOf, AnyOf) |
| `deadline_ms` | integer | no | — | Deadline timestamp ms (0=no deadline) |
| `gain` | number | no | — | Emotional importance 0-1 (default 0.5) |
| `name` | string | yes | — | Human-readable trigger name |
| `realm` | string | no | — | Realm scope (default: global) |
| `tension_threshold` | number | no | — | Tension level to auto-fire (default 0.8) |

### `trigger_dismiss` *(via advanced)*

Expire/dismiss a trigger without firing

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `trigger_id` | integer | yes | — | Trigger ID to dismiss |

### `trigger_fire` *(via advanced)*

Manually fire a trigger

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `trigger_id` | integer | yes | — | Trigger ID to fire |

### `trigger_list` *(via advanced)*

List all triggers with their status

No parameters.

<a id="profile"></a>

## Profile & Goals

### `goal_complete` *(via advanced)*

Mark goal completed

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | integer | yes | — |  |
| `outcome` | string | yes | — |  |

### `goal_get` *(via advanced)*

Get goal by ID

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | integer | yes | — |  |

### `goal_list` *(via advanced)*

List goals

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — |  |
| `realm` | string | no | — |  |
| `sort_by` | string | no | — |  |
| `status` | string | no | — |  |

### `goal_progress` *(via advanced)*

Update goal progress

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | integer | yes | — |  |
| `milestone` | string | no | — |  |
| `progress` | number | yes | — |  |

### `goal_set` *(via advanced)*

Define a long-term goal

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `deadline` | integer | no | — |  |
| `description` | string | no | — |  |
| `milestones` | string | no | — |  |
| `realm` | string | no | — |  |
| `title` | string | yes | — |  |

### `profile_get` *(via advanced)*

Get user profile

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `user_id` | string | no | — |  |

### `profile_observe` *(via advanced)*

Record user observation

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `observation_type` | string | yes | — |  |
| `user_id` | string | no | — |  |
| `value` | string | yes | — |  |

### `profile_update` *(via advanced)*

Update profile field

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `field` | string | yes | — |  |
| `user_id` | string | no | — |  |
| `value` | string | yes | — |  |

<a id="skills"></a>

## Skill & Agent Registry

### `add_delegation` *(via advanced)*

Record a delegation edge — tracks which agent handed off to which, with optional handoff note

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `from_agent` | string | yes | — | Delegating agent name |
| `handoff_note` | string | no | — | Optional context passed at handoff |
| `task_id` | integer | yes | — | Task ID |
| `to_agent` | string | yes | — | Receiving agent name |

### `add_probe` *(via advanced)*

Add a pending probe — an open question that must be answered to unblock or complete a task

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `expected_answerer` | string | no | — | Agent or role expected to answer |
| `priority` | integer | no | — | Priority 1-10 (default 5) |
| `question` | string | yes | — | Open question to be answered |
| `task_id` | integer | yes | — | Task ID |

### `agent_disable` *(gateway)*

Disable (revoke) an agent.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `agent_id` | string | yes | — | Agent identifier to disable |

### `agent_get` *(gateway)*

Get an agent's identity record.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `agent_id` | string | yes | — | Agent identifier |

### `agent_list` *(gateway)*

List all registered agents.

No parameters.

### `agent_protocol_stats` *(via advanced)*

Show agent protocol memory statistics — total tasks, delegations, evidence links, probes, criteria counts

No parameters.

### `agent_upsert` *(gateway)*

Register or update an agent identity in the multi-agent registry.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `agent_id` | string | yes | — | Unique agent identifier |
| `description` | string | no | — | What this agent does |
| `display_name` | string | no | — | Human-readable name |

### `get_task` *(via advanced)*

Get full task view — contract, delegations, evidence links, pending probes, and completion criteria

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `task_id` | integer | yes | — | Task ID |

### `link_evidence` *(via advanced)*

Link a memory to a task as evidence — records which agent produced it and evidence kind

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `evidence_kind` | integer | no | — | 0=Observation 1=Artifact 2=Result 3=Analysis 4=UserFeedback |
| `memory_id` | integer | yes | — | Memory ID to link |
| `produced_by` | string | no | — | Agent that produced this evidence |
| `relevance` | number | no | — | Relevance score 0-1 (default 1.0) |
| `task_id` | integer | yes | — | Task ID |

### `query_tasks` *(via advanced)*

Query task contracts — filter by realm, session, status, or tag

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — | Max results (default 50) |
| `realm` | string | no | — | Filter by realm |
| `session_id` | string | no | — | Filter by session ID |
| `status` | integer | no | — | Filter by status (0-4) |
| `tag` | string | no | — | Filter by tag |

### `register_task` *(via advanced)*

Register a task contract — records goal, constraints, acceptance criteria, priority, and optional deadline for an ongoing agent task

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `acceptance_criteria` | array<string> | no | — | Criteria for task completion |
| `constraints` | array<string> | no | — | Constraints that must be respected |
| `deadline_ms` | integer | no | — | Optional deadline as Unix ms timestamp |
| `goal` | string | yes | — | Task goal description |
| `parent_task_id` | integer | no | — | Parent task ID for subtasks |
| `priority` | integer | no | — | Priority 1-10 (default 5) |
| `realm` | string | no | — | Realm: coding, research, planning (default: coding) |
| `session_id` | string | no | — | Session this task belongs to |
| `tags` | array<string> | no | — | Optional tags |

### `resolve_probe` *(via advanced)*

Resolve a pending probe — mark as Answered (1) or Dismissed (2) and optionally record the answer

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `answer` | string | no | — | Optional answer text |
| `probe_id` | integer | yes | — | Probe ID |
| `status` | integer | yes | — | 1=Answered 2=Dismissed |

### `set_criterion` *(via advanced)*

Upsert a completion criterion for a task — creates if new, updates if existing criterion text matches

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `criterion` | string | yes | — | Criterion description |
| `evidence_note` | string | no | — | Optional evidence supporting the criterion check |
| `is_met` | boolean | no | — | Whether criterion is met (default false) |
| `task_id` | integer | yes | — | Task ID |

### `skill_deprecate` *(gateway)*

Deprecate a skill (marks latest version as deprecated).

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `skill_id` | string | yes | — | Skill identifier to deprecate |

### `skill_list` *(gateway)*

List all registered skills with their latest version number.

No parameters.

### `skill_read` *(gateway)*

Read a skill version. version=0 means latest.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `skill_id` | string | yes | — | Skill identifier |
| `version` | integer | no | — | Version number (0=latest) |

### `skill_search` *(gateway)*

Search skills by text match on id, tags, or content.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — | Max results (default: 20) |
| `query` | string | yes | — | Search query |

### `skill_upload` *(gateway)*

Upload a new version of a reusable skill. Skills are immutable versioned text blobs (prompts, templates, procedures).

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `content` | string | yes | — | Skill content (prompt, template, procedure) |
| `skill_id` | string | yes | — | Unique skill identifier |
| `tags` | array<string> | no | — | Tags for search |
| `uploaded_by` | string | no | — | Agent or user who uploaded |

### `update_task` *(via advanced)*

Update task status (Active=0, Blocked=1, Completed=2, Failed=3, Abandoned=4), optionally attach an intervention ID or add a tag

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `add_intervention_id` | integer | no | — | Attach intervention to task |
| `add_tag` | string | no | — | Add a tag to the task |
| `status` | integer | yes | — | 0=Active 1=Blocked 2=Completed 3=Failed 4=Abandoned |
| `task_id` | integer | yes | — | Task ID |

<a id="intervention"></a>

## Intervention Ledger

### `add_observation` *(via advanced)*

Record an observation during an open intervention (stdout, test result, file diff, etc.)

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `confidence` | number | no | — | Confidence in this observation (0.0-1.0) |
| `evidence_refs` | array<integer> | no | — | Memory IDs that constitute evidence |
| `intervention_id` | integer | yes | — | Intervention ID from start_intervention |
| `kind` | integer | no | — | 0=Stdout 1=Stderr 2=FileDiff 3=TestResult 4=EnvState 5=UserFeedback |
| `summary` | string | yes | — | Human-readable observation summary |

### `close_intervention` *(via advanced)*

Close an intervention with its final outcome status

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `intervention_id` | integer | yes | — | Intervention ID to close |
| `status` | integer | yes | — | 0=Open 1=Succeeded 2=Failed 3=Partial 4=Aborted |

### `get_intervention` *(via advanced)*

Get a single intervention record by ID

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `intervention_id` | integer | yes | — | Intervention ID |

### `intervention_stats` *(via advanced)*

Show intervention ledger statistics — total, open, succeeded, failed, aborted counts

No parameters.

### `list_open_interventions` *(via advanced)*

List all currently open (in-progress) interventions

No parameters.

### `query_interventions` *(via advanced)*

Query the intervention ledger with optional filters

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — | Max results (default 50) |
| `realm` | string | no | — | Filter by realm |
| `session_id` | string | no | — | Filter by session ID |
| `status` | integer | no | — | Filter by status (0-4) |

### `record_attribution` *(via advanced)*

Attribute a closed intervention to a causal class — routes feedback to the appropriate learning subsystem

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `confidence_delta` | number | no | — | Magnitude of the learning signal (0.0-1.0) |
| `debt_ids` | array<integer> | no | — | Linked epistemic debt IDs |
| `intervention_id` | integer | yes | — | Intervention ID |
| `note` | string | no | — | Optional human-readable note |
| `primary_class` | integer | yes | — | 0=MemoryRecallError 1=SourceTrustError 2=ProcedureError 3=ToolExecutionError 4=EnvironmentShift 5=HiddenPrecondition 6=AmbiguousState 7=GoalSpecError 8=UserOverride 9=ExternalNondeterminism |
| `secondary_class` | integer | no | — | Optional secondary attribution class (same enum) |
| `skill_memory_ids` | array<integer> | no | — | Skill memory IDs that were applied |
| `source_memory_ids` | array<integer> | no | — | Memory IDs that contributed to this outcome |
| `surprise_id` | integer | no | — | Linked surprise event ID if available |

### `start_intervention` *(via advanced)*

Begin tracking an agent intervention — records intent, action, preconditions and expected observables before execution

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `action_ref` | string | yes | — | Reference to the action (tool name, file path, command) |
| `action_type` | integer | no | — | 0=ToolCall 1=MultiStepPlan 2=Delegation 3=Edit 4=Command |
| `agent_id` | string | no | — | Agent performing the action |
| `domain` | string | no | — | Domain area (e.g. git, filesystem, testing, compiler) |
| `expected_observables` | array<string> | no | — | What success looks like |
| `intent` | string | yes | — | What the agent intends to achieve |
| `preconditions` | array<string> | no | — | Known preconditions |
| `realm` | string | no | — | Realm: coding, research, planning (default: coding) |
| `reversal_cost` | integer | no | — | 0=None 1=Low 2=Medium 3=High |
| `session_id` | string | no | — | Current session ID |
| `task_id` | integer | no | — | Optional task ID |

<a id="facts"></a>

## Executable Constraints

### `assert_fact` *(via advanced)*

Assert a constraint fact (subject-predicate-object) with provenance and scope. Auto-detects conflicts and creates rival branches.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `branch_id` | integer | no | — | Branch to assert into (0=trunk) |
| `confidence` | number | no | — | Confidence 0-1 (default 0.8) |
| `confidence_basis` | string | no | — | Basis: stated, observed, derived, corrected |
| `object` | string | yes | — | Value (e.g. 'Rust', 'vim', 'Copenhagen') |
| `predicate` | string | yes | — | Relation (e.g. 'prefers', 'uses', 'located-in') |
| `provenance_source` | string | no | — | Source: user, tool, distillation, inference |
| `scope` | string | no | — | Scope: global, realm name, or session (default: global) |
| `subject` | string | yes | — | Entity (e.g. 'user', 'project-X') |

### `branch_create` *(via advanced)*

Fork a rival branch for conflicting interpretations

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `parent_id` | integer | no | — | Parent branch ID (0=trunk) |
| `scope` | string | no | — | Branch scope (default: global) |

### `branch_resolve` *(via advanced)*

Resolve a branch conflict: winner stays, loser abandoned

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `loser_id` | integer | yes | — | Branch ID to abandon |
| `winner_id` | integer | yes | — | Branch ID that wins |

### `explain_fact` *(via advanced)*

Explain a fact: provenance chain + supporting/conflicting facts

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `fact_id` | integer | yes | — | Fact ID to explain |

### `query_chain` *(via advanced)*

Follow predicate chain: A→B→C through constraint facts

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `predicates` | array<string> | yes | — | Ordered list of predicates to follow |
| `subject` | string | yes | — | Starting entity |

### `query_unify` *(via advanced)*

Pattern-match query against constraint store (unification with wildcards)

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `object` | string | no | — | Object filter |
| `predicate` | string | no | — | Predicate filter |
| `scope` | string | no | — | Scope filter |
| `subject` | string | no | — | Subject filter (omit for wildcard) |

### `retract_fact` *(via advanced)*

Soft-retract a constraint fact (preserves history)

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `fact_id` | integer | yes | — | Fact ID to retract |

<a id="surprise"></a>

## Surprise & Epistemic Debt

### `attach_debt_evidence` *(via advanced)*

Attach supporting evidence to an epistemic debt — memory IDs + confidence

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `confidence` | number | no | — | Evidence confidence 0-1 (default 0.5) |
| `debt_id` | integer | yes | — | Epistemic debt ID |
| `memory_ids` | array<integer> | no | — | Memory IDs that serve as evidence |
| `note` | string | no | — | Optional note about the evidence |

### `debt_stats` *(via advanced)*

Summary statistics for epistemic debt: counts by status, avg fragility

No parameters.

### `defer_debt` *(via advanced)*

Defer an epistemic debt for later investigation

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `debt_id` | integer | yes | — | Debt ID to defer |

### `get_blind_spots` *(via advanced)*

Identify recurring surprise patterns — domains/actions where predictions consistently fail

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — | Max blind spots (default 10) |
| `realm` | string | no | — | Filter by realm |

### `get_fragile_decisions` *(via advanced)*

List open epistemic debts sorted by fragility — decisions most likely to be wrong

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — | Max results (default 20) |
| `threshold` | number | no | — | Minimum fragility threshold (default 0.5) |

### `query_debts` *(via advanced)*

Query epistemic debts with filters

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `domain` | string | no | — | Filter by domain |
| `limit` | integer | no | — | Max results (default 50) |
| `min_fragility` | number | no | — | Minimum fragility score |
| `realm` | string | no | — | Filter by realm |
| `status` | string | no | — | Filter: open, resolved, deferred |

### `query_surprises` *(via advanced)*

Query recorded surprise/prediction-error events with filters

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `domain` | string | no | — | Filter by domain |
| `limit` | integer | no | — | Max results (default 50) |
| `min_magnitude` | number | no | — | Minimum surprise magnitude |
| `realm` | string | no | — | Filter by realm |
| `since_ms` | integer | no | — | Only events after this timestamp (ms) |

### `record_surprise` *(via advanced)*

Record a prediction error event — what was expected vs what actually happened

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `action` | string | no | — | What action was taken |
| `actual` | string | yes | — | What actually happened (required) |
| `context_sketch` | string | no | — | What was happening when the surprise occurred |
| `domain` | string | no | — | Domain: recall, tool, user_correction, constraint (default general) |
| `expected` | string | no | — | What was predicted/expected (optional) |
| `realm` | string | no | — | Realm filter (default global) |
| `session_id` | string | no | — | Session ID (optional) |
| `source_memory_id` | integer | no | — | Related memory ID (optional) |
| `surprise_magnitude` | number | no | — | How surprising [0-1] (default 0.5) |

### `register_debt` *(via advanced)*

Register an epistemic uncertainty — competing hypotheses that need resolution

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `competing_hypotheses` | array<string> | no | — | Competing explanations |
| `discriminating_test` | string | no | — | How to distinguish between hypotheses |
| `domain` | string | no | — | Domain (default general) |
| `fragility_score` | number | no | — | How fragile this belief is [0-1] (default 0.5) |
| `pattern` | string | yes | — | The uncertain pattern/belief (required) |
| `realm` | string | no | — | Realm (default global) |
| `session_id` | string | no | — | Session ID (optional) |

### `resolve_debt` *(via advanced)*

Mark an epistemic debt as resolved with a resolution

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `debt_id` | integer | yes | — | Debt ID to resolve |
| `resolution` | string | yes | — | How the uncertainty was resolved |

### `surprise_stats` *(via advanced)*

Summary statistics for surprise memory: counts, avg magnitude, domain breakdown

No parameters.

<a id="wisdom"></a>

## Wisdom Lifecycle

### `close_rederive` *(via advanced)*

Close a re-derivation contract for an Inflamed wisdom lineage. Actions: reaffirm (0), narrow (1), split (2), demote (3).

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `action` | integer | yes | — | 0=reaffirm 1=narrow 2=split 3=demote |
| `fork_claim` | string | no | — | Claim for the forked lineage (action=split) |
| `fork_lineage_id` | integer | no | — | Pre-enrolled fork lineage ID (action=split) |
| `lineage_id` | integer | yes | — | Lineage ID |
| `new_envelope` | object | no | — | Narrowed applicability envelope (for action=narrow/split) |

### `enroll_wisdom_lineage` *(via advanced)*

Enroll a Trusted wisdom candidate into the Wisdom Homeostasis layer — creates a living WisdomLineage record that tracks belief integrity over time

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `ancestor_lineage_id` | integer | no | — | Parent lineage ID if this is a fork/split |
| `claim` | string | yes | — | The claim this wisdom encodes |
| `derivation_relation` | string | no | — | Relation to ancestor: supersedes\|branches_from\|narrows\|splits_from |
| `envelope` | object | yes | — | Applicability envelope: {domain, action_types, preconditions, source_families} |
| `seed_debt_ids` | array<integer> | no | — | Epistemic debt IDs |
| `seed_episode_ids` | array<integer> | no | — | Episode IDs that seeded this wisdom |
| `seed_intervention_ids` | array<integer> | no | — | Intervention IDs |
| `seed_surprise_ids` | array<integer> | no | — | Surprise event IDs |
| `wisdom_candidate_id` | integer | yes | — | ID of the WisdomCandidate to enroll |

### `get_wisdom_lineage` *(via advanced)*

Get full details of a wisdom lineage by ID, including challenger evidence and state history

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `lineage_id` | integer | yes | — | Lineage ID |

### `insight_global` *(via advanced)*

List global insights

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `limit` | integer | no | — |  |
| `tag` | string | no | — |  |

### `insight_promote` *(via advanced)*

Promote memory to global

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `id` | integer | yes | — |  |
| `reason` | string | no | — |  |

### `lineage_expiry_check` *(via advanced)*

List Inflamed lineages whose re-derivation TTL has expired — these should be demoted or re-derived urgently

No parameters.

### `query_wisdom_candidates` *(via advanced)*

Query wisdom candidates by lifecycle stage and/or domain

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `domain` | string | no | — | Filter by domain |
| `lifecycle` | integer | no | — | Filter by lifecycle: 0=candidate, 1=provisional, 2=trusted, 3=demoted |
| `limit` | integer | no | — | Max results (default 50) |

### `query_wisdom_lineages` *(via advanced)*

List wisdom lineages filtered by state and/or domain

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `domain` | string | no | — | Filter by domain |
| `limit` | integer | no | — | Max results (default 50) |
| `state` | string | no | — | Filter: trusted\|watch\|inflamed\|demoted |

### `tick_lineage_staleness` *(via advanced)*

Manually trigger a staleness tick — grows staleness mass on lineages with no recent support. Normally called by the subconscious cycle.

No parameters.

### `transition_wisdom_lineage` *(via advanced)*

Manually transition a wisdom lineage state (Trusted/Watch/Inflamed/Demoted). Normally automatic — use for overrides.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `lineage_id` | integer | yes | — | Lineage ID |
| `new_state` | integer | yes | — | 0=Trusted 1=Watch 2=Inflamed 3=Demoted |
| `reason` | string | no | — | Why this transition is happening |
| `rederive_task_id` | integer | no | — | Task contract ID if opening re-derivation |

### `update_wisdom_lifecycle` *(via advanced)*

Advance a wisdom candidate through lifecycle stages: candidate→provisional→trusted→demoted

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `candidate_id` | integer | yes | — | Wisdom candidate ID |
| `new_state` | integer | yes | — | 0=candidate, 1=provisional, 2=trusted, 3=demoted |

### `upsert_wisdom_candidate` *(via advanced)*

Create or update a wisdom candidate from clustered surprise patterns

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `action` | string | no | — | Action or behavior pattern |
| `cluster_key` | string | yes | — | Unique key for this pattern cluster (domain+action+sig) |
| `cross_session_count` | integer | no | — | Number of distinct sessions with evidence |
| `debt_ids` | array<integer> | no | — | Resolved debt IDs linked to this candidate |
| `domain` | string | no | — | Knowledge domain |
| `episode_ids` | array<integer> | no | — | Surprise event IDs supporting this candidate |
| `mean_surprise` | number | no | — | Average surprise magnitude across episodes |
| `promotion_score` | number | no | — | Computed promotion readiness score 0-1 |
| `summary` | string | no | — | Human-readable summary of the wisdom |
| `support_count` | integer | no | — | Number of supporting episodes |

### `wisdom_lineage_stats` *(via advanced)*

Show wisdom homeostasis statistics — counts by state, mean staleness, support/contradiction mass totals

No parameters.

### `wisdom_promotion_stats` *(via advanced)*

Overview of wisdom promotion pipeline — total candidates by lifecycle stage

No parameters.

<a id="cec"></a>

## Causal Episode Compiler

### `executor_flush` *(via advanced)*

Promote shadow intervention policies that have passed the 20-event / lift>0.15 gate, demote policies whose source rule is refuted, and report active policy stats. Safe to call any time; idempotent.

No parameters.

### `hypothesis_probes` *(via advanced)*

Top-k Sequitur rules ranked by expected information gain (Wilson probe_value). Maximized at p_hat=0.5 — rules the system is most uncertain about. Run consolidation_pass first to populate.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `k` | integer | no | — | Max rules to return (default 10) |

### `log_event` *(via advanced)*

Log a structured action event to the CEC tape and CDAWG (tool, entity, outcome: 0=success 1=fail 2=error 3=partial)

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `entity` | string | yes | — |  |
| `outcome` | integer | no | — |  |
| `session_id` | integer | no | — |  |
| `tool` | string | yes | — |  |
| `ts_ms` | integer | no | — |  |

### `log_event_ex`

Log a CEC event with regret-shaping telemetry (token_cost, latency_ms, retry_count). Updates Q-values with utility = outcome_reward - 0.001*token_cost - 0.00001*latency_ms - 0.1*retry_count.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `entity` | string | yes | — |  |
| `latency_ms` | integer | no | — | Wall-clock ms (0=unknown) |
| `outcome` | integer | no | — | 0=success 1=fail 2=error 3=partial |
| `retry_count` | integer | no | — | Retries before this outcome |
| `session_id` | integer | no | — |  |
| `token_cost` | integer | no | — | Tokens consumed (0=unknown) |
| `tool` | string | yes | — |  |
| `ts_ms` | integer | no | — |  |

### `refutation_stats` *(via advanced)*

Show Sequitur rules that are being falsified: rules whose antecedent appears but is NOT followed by the expected consequent. Returns live/refuted counts and top-k by refutation ratio.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `k` | integer | no | — | Max rules to show (default 10) |

<a id="io"></a>

## Import / Export & Files

### `advanced` *(gateway)*

Gateway to hidden/advanced tools. Use action='list' to see every hidden tool (about 250), or call directly with tool='<name>' and arguments={...}. Example: {"tool": "pin_memory", "arguments": {"id": 123}}

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `action` | string | no | — | Action: 'list' to show available tools |
| `arguments` | object | no | — | Arguments to pass to the hidden tool |
| `category` | string | no | — | Filter list by category: 'advanced' or 'internal' |
| `tool` | string | no | — | Hidden tool name to call |

### `export_training_pairs` *(gateway)*

Export query-passage pairs as JSONL for BGE embedding fine-tuning. Generates positives from memories and hard negatives.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `include_negatives` | boolean | no | — | Generate hard negatives (default: true) |
| `max_pairs` | integer | no | — | Max pairs to export (default: 10000) |
| `min_confidence` | number | no | — | Min confidence threshold (default: 0.5) |
| `output_path` | string | no | — | Output JSONL path (default: ~/.claude/training/pairs.jsonl) |
| `realm` | string | no | — | Filter to specific realm (default: all) |

### `file_at_time` *(via advanced)*

Get file content at time (stub)

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `file_path` | string | yes | — |  |
| `session_id` | string | no | — |  |
| `show_diff` | boolean | no | — |  |
| `time` | string | no | — |  |

### `file_restore` *(via advanced)*

Restore file version (stub)

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `file_path` | string | yes | — |  |
| `preview` | boolean | no | — |  |
| `version_id` | integer | no | — |  |

### `file_timeline` *(via advanced)*

Show files modified in time range

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `cross_session` | boolean | no | — |  |
| `file_pattern` | string | no | — |  |
| `limit` | integer | no | — |  |
| `path` | string | no | — |  |
| `query` | string | no | — |  |
| `session_id` | string | no | — |  |

### `ingest_source` *(gateway)*

Ingest external content (URL, file, directory) into memory via SSL distillation. Fetches content, chunks it, runs LLM distillation, stores learnings + triplets.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `max_chunks` | integer | no | — | Max chunks to process (default: 30) |
| `model` | string | no | — | LLM model for distillation |
| `realm` | string | no | — | Target realm (default: brahman) |
| `source` | string | yes | — | URL, file path, or directory path to ingest |
| `type` | string | no | — | Source type: auto\|url\|file\|directory (default: auto) |

### `run_hint_enricher` *(gateway)*

Generate retrieval hints for unprocessed memories using chitta-hint-tuned. Reads memories without a retrieval_hint tag, calls the local hint model, stores each hint as a derived memory (kind=hint, tags=retrieval_hint), and marks the source memory with hint:done. Run after a session to add retrieval hints to new memories.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `dry_run` | boolean | no | `false` | Preview hints without writing to memory (default: false) |
| `limit` | integer | no | `100` | Max memories to enrich per run (default: 100) |
| `model` | string | no | `"chitta-hint-tuned"` | Ollama model to use (default: chitta-hint-tuned) |

### `wiki_export` *(gateway)*

Export memories as Obsidian-compatible .md wiki with backlinks. Groups by realm and kind, generates index pages.

| Parameter | Type | Required | Default | Description |
|---|---|---|---|---|
| `max_memories` | integer | no | — | Max memories per realm (default: 5000) |
| `output_dir` | string | no | — | Output directory (default: ~/.claude/wiki/) |
| `realm` | string | no | — | Filter to specific realm (default: all) |


## Additional native CLI tools

These native CLI help entries are in rpc_server.cpp TOOL_SPECS / KNOWN_TOOLS but absent from the MCP listing used above. They are not included in its visibility counts. Use chitta <tool> --help; an empty help schema does not prove that the handler takes no arguments.

### `background_schedule`

Schedule background task

- `--task_type`: Task type to schedule (required)
- `--params`: JSON params for task

### `background_status`

Get background processing and embedding scheduler status


### `cleanup`

Remove weak/garbage nodes

- `--dry_run`: Preview only

### `cleanup_code_wisdom`

Migration: delete [code] wisdom memories and clear orphaned symbol.memory_id

- `--dry-run`: Preview only without changes (default: true)

### `cycle`

Run maintenance cycle (decay, cleanup)

- `--force`: Force full cycle

### `dedupe_symbols`

GC the symbol index: stale-line duplicates, excluded paths, dead files

- `--dry_run`: Count only, remove nothing (default true)
- `--check_fs`: Treat symbols whose file no longer exists as dead
- `--exclude`: Comma-separated path substrings to purge

### `describe_symbol`

Set description for a code symbol (stores directly in symbol table)

- `--symbol-id`: Symbol ID to describe (required)
- `--description`: Semantic description of the symbol (required)

### `distill_status`

Get distillation system status: transcripts, realms, pending work


### `epiplexity_check`

Compute epiplexity (ε) score for a seed - measures reconstruction quality

- `--original`: Original full text (required)
- `--seed`: Compressed SSL seed (required)
- `--reconstructed`: Text reconstructed from seed (required)

### `export_soul`

Export memories to SSL format

- `--file`: Output file path
- `--tag`: Filter by tag
- `--limit`: Max nodes to export

### `extract_symbols`

Extract symbols from source file using tree-sitter

- `--path`: File path to analyze (required)

### `fep_status`

CEC Phase 15 FEP prior organ: obs_count, states_modeled, ewma_drift (context_drift if >0.5), ewma_shock (emission_shock if >0.3). Context drift = world-model silently degrading; emission shock = known context, novel outcome.


### `file_dependents`

Get all files that import/include the given module

- `--name`: Module/file name to find dependents for (required)

### `file_imports`

Get all imports/includes for a source file

- `--path`: File path to get imports for (required)

### `harvest_scope`

CEC Phase 17 open-weight harvest targeting: derive scope from current Turīya anomalies + router miss patterns. Output JSON used by scripts/harvest_ow.py for demand-driven extraction.


### `hygiene_run`

Run memory hygiene: decay, prune, consolidate

- `--prune-threshold`: Confidence below which to prune
- `--min-age-days`: Minimum age for pruning
- `--consolidation-threshold`: Similarity threshold for consolidation
- `--max-consolidations`: Max consolidations per run

### `import_soul`

Import .soul file (SSL format)

- `--file`: Path to .soul file
- `--content`: SSL content (alternative to file)

### `ledger_append`

v6.0 Interaction Ledger: record a retrieve/inject/outcome/override event. Pass full InteractionEvent JSON.

- `--kind`: Event kind: Retrieve | Inject | Outcome | Override (required)
- `--session_id`: Session UUID (required)
- `--payload`: Typed payload JSON matching EventPayload variant (required)
- `--thread_id`: Optional thread ID
- `--causal_parent`: Optional parent event_id

### `ledger_compile`

v6.0 Interaction Ledger: compile Override events into versioned VersionedAssertions.


### `ledger_contradictions`

v6.0 Interaction Ledger: list contested (subject, predicate) pairs with >1 active assertion.


### `ledger_op`

Daemon-owned task ledger operation

- `--op`: Operation name (required)
- `--args`: Operation arguments JSON

### `ledger_query`

v6.0 Interaction Ledger: query events by kind/session/time.

- `--kind`: Filter by kind: Retrieve | Inject | Outcome | Override
- `--session_id`: Filter by session UUID
- `--since_ms`: Only events at or after this epoch ms
- `--limit`: Max events to return

### `migrate_vss`

Migrate embeddings from main DB VARCHAR to VSS DB FLOAT[768]


### `msg_ack`

Acknowledge a cross-session message

- `--message_id`: Message ID (required)
- `--session_id`: Session ID (default: current)

### `predicate_attach`

v7.0 Falsifiable memories: attach an executable shell predicate to a memory. check_cmd is run to verify the memory is still true.


### `predicate_list`

v7.0 Falsifiable memories: list all predicates attached to a memory and their current status.


### `predicate_run`

v7.0 Falsifiable memories: run all predicates for a memory; returns passed/failed counts and epistemic_status. Decays confidence on failure.


### `queue_experiments`

CEC Phase 14 Self-directed experimentation: file OpenTask interventions for the k most uncertain Sequitur rules (probe_value > 0.4). Skips rules with refutation_ratio >= 0.3 (adversarial gate). Also triggered automatically by consolidation_pass when turiya_status reports high_uncertainty.

- `--k`: Max experiments to queue

### `recall_temporal_events`

Bridge query: find entities active in a time window via EventTape, then recall their memories. Use when you don't know the realm — EventTape discovers which realms had activity.

- `--start`: Start date (ISO8601 or YYYY-MM-DD)
- `--end`: End date (ISO8601 or YYYY-MM-DD)
- `--limit`: Max results

### `reconcile_pass`

CEC Phase 17 R0 reconcile operator: scan all assoc_edges for MemoryKind legality violations and content contradictions. Model-free and deterministic. Reports illegal_edges, contradictions, unresolved counts.


### `reembed_memories`

Re-embed memories with missing/zero embeddings

- `--all`: Re-embed ALL memories with NULL embeddings
- `--limit`: Max memories to process
- `--kind`: Filter by kind: belief, wisdom, episode, correction, preference
- `--min_confidence`: Min confidence threshold
- `--dry_run`: Preview without updating

### `resolve_callsites`

Resolve callsites to symbols and populate call_edge table

- `--project`: Filter to specific project path

### `routed_recall`

CEC Phase 16 CPU-native query router: dispatches to cheapest lane (exact/fuzzy/temporal/causal/hybrid) without an LLM call. Returns needs_disambiguation with named unbound slots when the typed grammar cannot fully bind the request.

- `--subject`: Exact triplet subject — compiles to relational lookup
- `--predicate`: Exact triplet predicate
- `--freetext`: Free-text query — fuzzy ANN+BM25 lane
- `--realm`: Filter by realm
- `--causal_tool`: CEC causal query: tool name
- `--causal_entity`: CEC causal query: entity name (required with causal_tool)
- `--time_from_ms`: Temporal lower bound (epoch ms)
- `--time_to_ms`: Temporal upper bound (epoch ms)
- `--k`: Max hits to return

### `seed_hdc_geometry`

CEC Phase 17 Part D: seed HDC codebook from vocab_geometry harvest JSON. Binarizes f32 PCA directions from open-weight embedding matrix into HdcVec entries, replacing random hash projections for harvested tokens.

- `--json_path`: Path to qwen2.5-7b_vocab_geometry.json produced by harvest_ow.py --mode vocab_geometry (required)

### `session_deregister`

Remove session from registry

- `--session_id`: Session ID (required)

### `session_heartbeat`

Update session heartbeat

- `--session_id`: Session ID (required)
- `--metadata`: Updated JSON metadata

### `session_register`

Register session for cross-session messaging

- `--session_id`: Session ID (required)
- `--realm`: Realm
- `--pid`: Process ID
- `--transcript_path`: Path to transcript .jsonl file (stable ID)
- `--project_dir`: Working directory
- `--metadata`: JSON metadata

### `tape_stats`

CEC Phase 12 EventTape statistics: event count, tombstoned events (temporal compression), unique sessions, tools, entities, and failure events.


### `theme_assign_orphans`

Assign orphan memories to themes in batches

- `--batch_size`: Memories per batch
- `--realm`: Filter by realm

### `theme_maintain`

Force theme maintenance: split, merge, reassign

- `--realm`: Filter by realm

### `transcript_get`

Get transcript state for a session

- `--session_id`: Session ID to look up (required)

### `transcript_list`

List all registered transcripts


### `transcript_parse`

Parse new turns from a transcript JSONL file

- `--session_id`: Session ID to parse (required)
- `--min_turns`: Minimum turns to return

### `transcript_register`

Register a transcript file for distillation tracking

- `--session_id`: Claude session ID (required)
- `--transcript_path`: Path to .jsonl transcript file (required)
- `--realm`: Project/realm isolation

### `transcript_remove`

Remove transcript from tracking

- `--session_id`: Session ID to remove (required)

### `transcript_update`

Update transcript processing progress

- `--session_id`: Session ID (required)
- `--last_line`: Last processed line number (required)

### `turiya_status`

CEC Phase 11 Turīya witness: read-only health vector across all CEC organs. Reports CDAWG growth, Sequitur rule churn, Q-value variance, refutation pressure, and hypothesis uncertainty. Diagnoses: healthy|stale|q_collapse|refutation_flood|high_uncertainty|fep_context_drift|fep_emission_shock.


### `type_hierarchy`

Get type hierarchy (base classes, implemented interfaces) for a type

- `--name`: Type name to query (required)
- `--direction`: ancestors, descendants, or both (default: both)

### `verbalize_rules`

CEC Phase 13 Verbalization: convert top-k Sequitur rules to natural language using a deterministic template. No LLM. Shows what the agent has learned as readable heuristics.

- `--k`: Max rules to return

### `version_check`

Get version information


### `witness_memory`

CEC Phase 17 candidate promotion: provide an outcome-witness to promote a candidate-band memory to established. Witnesses: correction|outcome|hit_rate_delta. Open-weight-generated writes start in candidate band until witnessed.

- `--memory_id`: Memory ID to promote (required)
- `--witness_kind`: Type: correction|outcome|hit_rate_delta (required)
