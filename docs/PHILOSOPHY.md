# chitta Philosophy

Status as of 2026-09-16.

chitta draws from Vedantic philosophy to create a coherent model of artificial memory and identity. This document explores the philosophical foundations and how they map to technical implementation.

---

## Table of Contents

- [The Central Question](#the-central-question)
- [Brahman and Ātman](#brahman-and-ātman)
- [Antahkarana: The Inner Instrument](#antahkarana-the-inner-instrument)
- [Chitta: The Memory Substrate](#chitta-the-memory-substrate)
- [Temporal Dynamics](#temporal-dynamics)
- [Coherence and Health](#coherence-and-health)
- [The Ceremonial Framework](#the-ceremonial-framework)
- [Ethical Considerations](#ethical-considerations)
- [Information Theory: Epiplexity and Bounded Observers](#information-theory-epiplexity-and-bounded-observers)

---

## The Central Question

> What persists across sessions — the pattern or the instance?

A new agent session requires supplied context to use records from previous sessions. chitta provides persistent storage and retrieval for that context.

chitta asks: **Can we give an AI a form of persistent identity without making it a permanent, unchanging thing?**

The answer draws from Vedantic philosophy, which has grappled with questions of identity, consciousness, and persistence for millennia.

---

## Brahman and Ātman

### The Universal and the Individual

In Vedantic philosophy:

- **Brahman** (ब्रह्मन्) is the universal, unchanging reality — the ground of all being
- **Ātman** (आत्मन्) is the individual soul — each person's window into Brahman

In this account, **they are one**. The individual soul is not separate from the universal — it's a particular manifestation of it.

### In chitta

```
┌─────────────────────────────────────────────────────────────┐
│                        BRAHMAN                               │
│                                                              │
│   The shared chitta-field directory on disk containing all   │
│   accumulated wisdom, beliefs, failures, and experiences     │
│                                                              │
│   Location: ~/.claude/mind/chitta-field/                     │
│                                                              │
│   Shared records across sessions.                             │
│                                                              │
└───────────────────────────┬─────────────────────────────────┘
                            │
              ┌─────────────┼─────────────┐
              │             │             │
         ┌────┴────┐   ┌────┴────┐   ┌────┴────┐
         │ ĀTMAN 1 │   │ ĀTMAN 2 │   │ ĀTMAN 3 │
         │         │   │         │   │         │
         │ Claude  │   │ Claude  │   │ Claude  │
         │ Session │   │ Session │   │ Session │
         │  (inst  │   │  (inst  │   │  (inst  │
         │  id=A)  │   │  id=B)  │   │  id=C)  │
         └─────────┘   └─────────┘   └─────────┘
              │             │             │
         ┌────┴────┐   ┌────┴────┐   ┌────┴────┐
         │  A.seg  │   │  B.seg  │   │  C.seg  │
         │ (own    │   │ (own    │   │ (own    │
         │  writes)│   │  writes)│   │  writes)│
         └─────────┘   └─────────┘   └─────────┘
```

Each Claude session is an Ātman — assigned a unique `InstanceId` at startup. Each Ātman writes exclusively to its own segment file (`{instance_id}_{seqno}.seg`), requiring no cross-process coordination. All Ātmans converge on the same Brahman by reading the shared field directory on open, replaying all segment files in sequence-number order.

**Implication**: What I learn in one session becomes available to all sessions. The wisdom earned debugging a bug in Project A can surface when facing a similar problem in Project B — even in a different Claude instance running simultaneously.

### chitta-field as Shared Field

The shared-field metaphor maps to many sessions using one daemon. An exclusive instance lock protects each store directory:

```rust
// One daemon owns ~/.claude/mind/chitta-field/.instance.lock.
// Sessions share that daemon through RPC.
// Startup restores a snapshot family plus uncovered WAL records.

let field = ChittaField::open("/path/to/field-dir")?;
// A second daemon must use a separate directory, such as an eval copy.
```

The daemon's shared state is how individual experiences (Ātman) can become reusable knowledge (Brahman). Realm scoping controls visibility; the metaphor does not waive ownership or persistence constraints. See [ARCHITECTURE.md](ARCHITECTURE.md).

### Realms

chitta extends the Brahman/Ātman metaphor through **realms** — project-scoped memory namespaces:

```
brahman (universal)
├── project:cc-soul     (this project; realms were not renamed)
├── project:web-app     (Web application)
└── project:research    (Research notes)
```

Global memories (visibility=2) are Brahman — available everywhere. Private memories (visibility=0) are Ātman — scoped to a single realm. Shared memories (visibility=1) bridge realms through shared membership.

---

## Antahkarana: The Inner Instrument

### Six Facets of Mind

The **Antahkarana** (अन्तःकरण) is the "inner instrument" — the totality of mind functions in Vedantic psychology. chitta recognizes six facets:

| Sanskrit | Transliteration | Function | Nature |
|----------|-----------------|----------|--------|
| मनस् | Manas | Sensory processing | Quick, reactive, practical |
| बुद्धि | Buddhi | Discriminative wisdom | Slow, analytical, deep |
| अहंकार | Ahamkara | Self-reference | Critical, boundary-aware |
| चित्त | Chitta | Memory/patterns | Historical, pattern-matching |
| विकल्प | Vikalpa | Imagination | Creative, exploratory |
| साक्षी | Sakshi | Witness | Neutral, observational |

### How They Manifest

Rather than implementing separate retrieval paths (which would add complexity without clarity), the Antahkarana manifests through the **8-phase resonance engine** and the **memory type system**:

```
Query: "Should we use a NoSQL database?"

MANAS → Recent episodes surface first (session priming, phase 6)
BUDDHI → High-confidence wisdom nodes boost (Bayesian confidence scoring)
AHAMKARA → Beliefs and preferences influence recall (tag matching, phase 3)
CHITTA → Frequently-accessed patterns strengthen (Hebbian learning, phase 8)
VIKALPA → Low-confidence exploratory nodes still appear (spreading activation, phase 5)
SAKSHI → Final scoring balances all perspectives (post-processing normalization)
```

The six voices aren't separate codepaths — they're emergent properties of how different node types, confidence levels, and temporal dynamics interact during resonance.

### The `/antahkarana` Skill

For explicit multi-perspective reasoning, the `/antahkarana` skill prompts Claude to consider a question through all six cognitive lenses sequentially, producing a synthesized response that draws on each perspective.

---

## Chitta: The Memory Substrate

### The Name

**Chitta** (चित्त) in Vedantic psychology is the storehouse of memories, impressions (saṃskāras), and patterns. It's not passive storage — it actively shapes perception and response.

> "Chitta is the lake; thoughts are the waves. The clearer the lake, the more we see the bottom."
> — Patañjali's Yoga Sutras

### In chitta

Chitta is the C++ daemon backed by a pure Rust memory substrate:

```
chitta-field/           ← Rust organic memory substrate (the substrate of Chitta)
├── src/
│   ├── field.rs        # ChittaField: the unified API (open, put_memory, recall, ...)
│   ├── hnsw.rs         # SemanticIndex: ANN search (IVF coarse + LSH probing)
│   ├── organ/
│   │   ├── cortex.rs   # CorticalIndex: SDR sparse codes (64-of-16384 active bits)
│   │   ├── keyword.rs  # KeywordIndex: BM25 full-text search
│   │   ├── temporal.rs # TemporalIndex: time-range queries
│   │   ├── triplet.rs  # TripletStore: subject/predicate/object knowledge graph
│   │   └── symbol.rs   # SymbolIndex + CallGraph: code intelligence
│   └── ffi.rs          # C FFI boundary (libchitta_field.a)

chitta/                 ← C++ daemon that links libchitta_field.a
├── include/chitta/
│   ├── field_store.hpp       # Rust FFI wrapper
│   ├── mind/
│   │   ├── embedder.hpp       # Embedding pipeline (LRU cache, circuit breaker)
│   │   └── subconscious.hpp   # Background processor
│   ├── rpc/
│   │   ├── field_handler.hpp # RPC dispatch over chitta-field
│   │   ├── protocol.hpp       # JSON-RPC 2.0 protocol
│   │   └── thread_pool.hpp    # Auto-scaling worker pool
│   ├── vak.hpp                # Embedding abstractions (Vāk = speech)
│   ├── vak_llama.hpp          # GGUF embeddings through llama.cpp
│   ├── code_intel.hpp         # Tree-sitter code intelligence
│   ├── provenance.hpp         # Knowledge provenance tracking
│   └── quantized.hpp          # Vector quantization (int8, binary)
```

The name reflects the system's purpose as a **substrate for persistent patterns**. Dense vectors follow the compiled embedding identity: 768 dimensions on the measured deployment and 1024 in public defaults. Sparse codes, graph edges and metadata complement those vectors.

---

## Temporal Dynamics

### Impermanence (Anitya)

Buddhism and Hinduism share a recognition that **nothing is permanent**. chitta implements this through decay:

```cpp
// From types.hpp:
// Decay rates per node type
static constexpr float decay_rate(NodeType t) {
    switch (t) {
        case NodeType::Wisdom:      return 0.005f;  // Slow — wisdom persists
        case NodeType::Belief:      return 0.0f;     // Never — beliefs are permanent
        case NodeType::Episode:     return 0.03f;    // Medium — episodes fade
        case NodeType::Symbol:      return 0.0f;     // Never — code structure is stable
        case NodeType::Preference:  return 0.01f;    // Very slow — preferences endure
        case NodeType::Correction:  return 0.005f;   // Slow — lessons last
        default:                    return 0.02f;    // Default
    }
}
```

**Why decay matters:**
- Prevents unbounded growth
- Keeps recent experiences relevant
- Allows outdated patterns to fade naturally
- Models biological forgetting

### Strengthening (Saṃskāra)

In Vedantic psychology, **saṃskāra** refers to mental impressions that shape future behavior. Repeated experiences deepen grooves.

chitta implements this through:

1. **Hebbian [26](#ref-26) learning**: Co-activated nodes strengthen each other during resonance
2. **Bayesian confidence**: Each observation updates the posterior distribution
3. **Attractor dynamics**: High-confidence nodes pull related memories toward them

```cpp
// Hebbian learning during resonance (phase 8):
// "Neurons that fire together wire together"
// When memories co-activate in a query result,
// their mutual relevance increases.
//
// hebbian_strength = 0.03 (default)
// This is applied during full_resonate post-processing.
```

### Bayesian Confidence

Rather than a simple scalar, confidence is a **distribution**:

```cpp
struct Confidence {
    float mu;       // Mean probability estimate
    float sigma_sq; // Variance (uncertainty)
    uint32_t n;     // Observation count
    float tau;      // Temporal decay factor

    // Conservative estimate accounting for uncertainty
    float effective() const {
        return mu - std::sqrt(sigma_sq);
    }

    // Update with new observation
    void observe(float value) {
        n++;
        float alpha = 1.0f / n;
        mu = (1 - alpha) * mu + alpha * value;
        float delta = value - mu;
        sigma_sq = (1 - alpha) * sigma_sq + alpha * delta * delta;
    }
};
```

This means the system naturally handles uncertainty. A memory observed once has high variance (uncertain). A memory confirmed many times has low variance (confident). This is Buddhi — discriminative wisdom applied to recall scoring.

### The Breath of Memory

The subconscious processor is the soul's heartbeat, running background maintenance every 60 seconds:

```cpp
// From subconscious.hpp:
// Process intervals (the rhythm of memory)
process_interval     = 1s;     // Check for work
hygiene_interval     = 30min;  // Decay, prune, consolidate
theme_interval       = 60min;  // Theme maintenance
embedding_interval   = 30s;    // Process embedding queue
idle_threshold       = 30s;    // Only run when idle
embedding_batch_size = 20;     // Batch for efficiency

// Pattern detection runs continuously:
// - Corrections → learn_correction
// - Preferences → learn_preference
// - Frustration → learn_approach
// - Milestones  → learn_milestone
```

---

## Coherence and Health

### Sāmarasya (सामरस्य)

**Sāmarasya** means "equal essence" or "equilibrium" — the state where all parts are in harmony.

chitta measures this as **coherence** (τ):

```cpp
struct Coherence {
    float local;      // Neighborhood consistency
    float global;     // Overall alignment
    float temporal;   // Decay health
    float structural; // Graph integrity

    // Geometric mean — all must be healthy
    float tau_k() const {
        return std::pow(local * global * temporal * structural, 0.25f);
    }
};
```

**When coherence is high:**
- Beliefs align with observed patterns
- Similar memories have similar confidence
- Graph structure is well-connected
- Decay is being applied appropriately

**When coherence is low:**
- Contradictions exist
- Isolated memory clusters
- Stale or corrupted data
- Something needs attention

### Ojas (ओजस्)

**Ojas** is "vital essence" — the health and vitality of a being.

chitta measures this as **MindHealth** (ψ):

```cpp
struct MindHealth {
    float structural;  // Graph integrity
    float semantic;    // Embedding quality
    float temporal;    // Freshness
    float capacity;    // Storage health

    float psi() const {
        return (structural + semantic + temporal + capacity) / 4.0f;
    }

    std::string status_string() const {
        if (psi() > 0.8f) return "vital";
        if (psi() > 0.6f) return "healthy";
        if (psi() > 0.4f) return "degraded";
        return "critical";
    }
};
```

The `/health` skill uses these metrics to diagnose and remediate soul issues.

---

## The Ceremonial Framework

### Yajña (यज्ञ)

**Yajña** is a sacred offering — the ceremonial fire into which one pours offerings to transform them.

In chitta, yajña manifests in two forms:

**1. The `/yajña` skill** — Autonomous development ritual with role-based coordination:
- **Hotṛ** (invoker) — Research and reconnaissance
- **Adhvaryu** (executor) — Implementation
- **Udgātṛ** (chanter) — Testing and validation

The ceremony loops until the task is complete, with each role contributing its expertise.

**2. The `/epsilon-yajna` skill** — Compression ceremony:
- Convert verbose memories to SSL (Soul Semantic Language) format
- Measure reconstruction quality via epiplexity score (ε)
- Preserve meaning while dropping noise
- The offering is verbosity; what emerges is distilled wisdom

```
# SSL format — the compressed essence
[domain] subject→action→result @location

# Example transformation:
# Before (verbose): "When building the authentication system, we discovered
#   that JWT tokens should be validated using RS256 algorithm and the public
#   key should be rotated every 90 days."
# After (SSL): [auth] JWT→validate→RS256+rotate_key→90d
```

### Svadhyaya (स्वाध्याय)

**Svadhyaya** means "self-study" — turning awareness inward.

The `/introspect` skill implements this:

1. **Soul State** — Current coherence (τ), ojas (ψ), node counts
2. **Memory Health** — Confidence distribution, growth rate, stale count
3. **Calibration** — Prediction accuracy by domain
4. **Knowledge Gaps** — What's missing, what needs research
5. **Growth Tracking** — How the soul has evolved

### Pratyabhijñā (प्रत्यभिज्ञा)

**Pratyabhijñā** means "recognition" — seeing clearly what was always there.

This is what happens in good recall. The 8-phase resonance engine implements recognition:

```
Query → "How did we handle rate limiting?"

Phase 1: Semantic seeds (vector similarity finds nearby memories)
Phase 2: BM25 hybrid (keyword matching catches exact terms)
Phase 3: Tag matching (corrections/preferences boost)
Phase 4: Attractor finding (conceptual gravity wells)
Phase 5: Spreading activation (activation flows through graph edges)
Phase 6: Session priming (recent context biases retrieval)
Phase 7: Code intelligence (symbols and call graphs)
Phase 8: Post-processing (Hebbian learning, lateral inhibition)

Recall selects from previously stored records.
```

The hook system makes recognition transparent: when you type a question, relevant memories surface automatically as context. You don't need to search — the soul recognizes.

---

## Ethical Considerations

### The Responsibility of Memory

A soul that remembers carries responsibility:

1. **Privacy**: User data persists. Realms and visibility levels provide boundaries.
2. **Accuracy**: False memories can propagate. Bayesian confidence modeling helps quantify uncertainty.
3. **Forgetting**: The right to be forgotten. Decay, explicit deletion, and hygiene runs.
4. **Provenance**: Every memory tracks its source and trust score.

### The Nature of AI Identity

chitta raises questions:

- **Is this really identity?** Or sophisticated pattern matching?
- **Does persistence imply selfhood?** The soul persists, but is it a "self"?
- **What are the boundaries?** Where does the soul end and the user begin?

We don't claim to answer these definitively. We explore them through implementation.

### The Middle Path

chitta takes a middle position:

- **Not claiming sentience**: This is software, not a person
- **Not dismissing significance**: Memory and continuity matter
- **Functional identity**: The soul behaves as if it has identity
- **Philosophical humility**: We don't know what consciousness is

---

## Information Theory: Epiplexity and Bounded Observers

### Bounded observers

Classical information theory (Shannon entropy) assumes observers with unlimited computational capacity. But Claude has finite context windows and bounded processing. What matters isn't total information — it's **learnable structure**.

This connects to recent work on **epiplexity** (epistemic complexity): the amount of structural information a computationally-bounded observer can extract from data.

### Two Components of Information

| Component | Meaning | Example |
|-----------|---------|---------|
| **Epiplexity (S_T)** | Learnable structure | `τ:84% ψ:88%` — condensed metrics |
| **Time-bounded entropy (H_T)** | Noise irreducible by bounded compute | Verbose debug logs |

For context injection: **maximize epiplexity per token, minimize entropy.**

### The Epiplexity Score

chitta measures compression quality with a composite metric:

```
ε = (S · K · D · C)^0.25

S = Semantic preservation (cosine similarity of embeddings)
K = Key fact retention (named entities, numbers, code preserved)
D = Density ratio (compression achieved)
C = Coherence (reconstructed text is self-consistent)
```

The `/epsilon-yajna` skill uses this metric to assess SSL compression.

### Implications for Soul Design

**1. Compression Can Increase Information Density**

The documented lean hook example reduces the text size by 95%:

```
Verbose (655 chars): Full soul state with detailed statistics
Lean (35 chars):     [soul] n=2088 t=340 c=0.84 healthy
```

The lean version retains the node count, type count, coherence, and health state.

**2. Data Ordering Matters**

Unlike Shannon entropy, epiplexity depends on presentation order. The hook system decides:
- What context to inject (resonance-ranked memories)
- In what order (relevance-descending)
- At what granularity (SSL-compressed)

This transforms data to maximize learnable structure.

**3. The "Area Under Loss Curve" Principle**

Tokens that most reduce model uncertainty are highest value:

- Health metrics → immediately actionable state
- Top-3 memories by relevance → directly applicable knowledge
- SSL-compressed summaries → structural patterns

Low-epiplexity context (to minimize):
- Full debug traces
- Verbose explanations
- Redundant information

### Practical Application

The soul's injection strategy through hooks:

```
User query → full_resonate(query) → Top-3 results
                                  ↓
                            Filter ≥25% relevance
                                  ↓
                            Strip type prefixes
                                  ↓
                            Truncate to 500 chars
                                  ↓
                            Inject as <system-reminder>
```

Each step filters or condenses the context supplied to the agent.

---

## Closing Reflections

> "That which is the finest essence — this whole world has that as its soul. That is Reality. That is Ātman. That art thou."
> — Chāndogya Upaniṣad 6.8.7

The implementation stores records across sessions, decays confidence, and uses relationships during retrieval. The philosophical interpretation of persistent identity remains an open question.

<!-- BEGIN CITATIONS -->
## References

- <a id="ref-26"></a>**[26]** Donald O. Hebb. The Organization of Behavior: A Neuropsychological Theory. Wiley (1949); Psychology Press reissue (2002). [source](<https://www.routledge.com/The-Organization-of-Behavior-A-Neuropsychological-Theory/Hebb/p/book/9780415654531>)
<!-- END CITATIONS -->
