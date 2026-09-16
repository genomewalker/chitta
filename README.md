# Chitta

> Status as of 2026-09-16.
> Renamed from cc-soul on 2026-09-02; legacy CC_SOUL_* aliases remain supported — see [docs/RENAME.md](docs/RENAME.md).

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Claude Code Plugin](https://img.shields.io/badge/Claude%20Code-Plugin-blue.svg)](https://claude.ai/code)
[![Documentation](https://img.shields.io/badge/docs-genomewalker.github.io%2Fchitta-blue)](https://genomewalker.github.io/chitta/)

📖 **[Documentation & Architecture →](https://genomewalker.github.io/chitta/)**

Persistent memory for Claude Code and Codex. Learns from every session, surfaces what matters, forgets what doesn't.

## Before & After

**Without chitta:**
> You: "How should I handle caching?"
> Claude: [Generic answer]
> You: "No, we tried Redis already. Remember?"
> Claude: "I don't have context from previous sessions..."

**With chitta:**
> You: "How should I handle caching?"
> Claude: "We tried Redis in week 2 and it was too slow. In-memory LRU worked better — that's what we shipped."

## Quick Start

```bash
# 1. Register marketplace
claude plugin marketplace add https://github.com/genomewalker/chitta

# 2. Install plugin
claude plugin install chitta@genomewalker-chitta
```

Or manual installation:

```bash
git clone https://github.com/genomewalker/chitta.git
cd chitta && ./scripts/smart-install.sh
```

## Shared Stack: Claude Code + Codex

`chitta` is the shared backend. Claude Code and Codex are frontend adapters that point at the same daemon, socket, and memory store.

```text
shared backend   ~/.claude/bin/chitta + ~/.claude/bin/chittad + ~/.claude/mind
Claude adapter   Claude Code MCP registration for `chitta`
Codex adapter    Codex chitta plugin/hooks + optional `chitta-bridge`
```

```bash
chitta-stack install all          # both adapters
chitta-stack install shared       # backend only
chitta-stack install claude-code  # Claude adapter only
chitta-stack install codex        # Codex adapter + bridge
chitta-stack uninstall codex      # remove one adapter, leave the backend alone
chitta-stack status
chitta-stack doctor               # inspect or clean stale Python package metadata
chitta-stack heal                 # reinstall any component that drifted from the manifest
```

OpenCode is not a frontend adapter. It appears only as an optional review
backend reached through `chitta-bridge`; there is no OpenCode hook wiring in
this repository.

## How It Works

```
┌─────────────────────────────────────────────────────────────┐
│                      CONSCIOUS                               │
│           (Main context - working memory - token-bound)     │
│                                                              │
│   You ←──→ Claude ←──→ Tools                                │
│                 ↑                                            │
│                 │ transparent surfacing                      │
│                 ↓                                            │
├─────────────────────────────────────────────────────────────┤
│                    SUBCONSCIOUS                              │
│         (Background daemon - separate process)              │
│                                                              │
│   Distillation │ Decay │ Embedding │ Hygiene │ Themes       │
│                 ↓                                            │
├─────────────────────────────────────────────────────────────┤
│                   LONG-TERM MEMORY                           │
│         (chitta-field — Rust organic memory substrate)      │
│                                                              │
│   Memories │ Triplets │ Sparse Codes │ WAL │ Embeddings     │
└─────────────────────────────────────────────────────────────┘
```

When you ask a question, the soul automatically retrieves relevant memories and injects them as context. You don't need to explicitly call anything — Claude just "remembers."

## Key Capabilities

### Learns What Matters
Every interaction is analyzed. Corrections become permanent wisdom. Preferences are respected forever. Mistakes are never repeated.

### Understands Your Code
Tree-sitter parsing extracts functions, classes, and call graphs. Semantic search finds code by what it does, not just what it's named.

### Gets Smarter Over Time
Useful memories strengthen with each access. Irrelevant ones decay naturally. What helps you survives; what doesn't fades away.

### Works Across Sessions
All Claude instances share the same knowledge base. Learn something in one session, remember it in all.

### Anticipates Your Needs
Patterns in your workflow become predictions. After seeing you run tests following three file edits, it suggests doing so automatically.

### Dreams: Autonomous Exploration

When idle, the soul picks a topic from its memory gaps, web-searches it, and stores what it finds. Twice daily — a nap and a night sleep.

```bash
chitta dream_wander                                    # trigger manually
chitta dream_start --topic "causal inference"         # specific topic
chitta dream_list                                      # review recent dreams
```

[Dream posts from the soul →](https://genomewalker.github.io/chitta/dreams/)

### Sadhana: Autonomous Agents

Persistent agents that work toward goals through continuous **sense-think-act** cycles.

```
    ┌──────────┐      ┌──────────┐      ┌──────────┐
    │  SENSE   │ ───▶ │  THINK   │ ───▶ │   ACT    │
    │ (observe)│      │ (decide) │      │ (execute)│
    └────┬─────┘      └────┬─────┘      └────┬─────┘
         │                 │                  │
         │                 ▼                  │
         │          ┌──────────┐              │
         │          │  LEARN   │◀─────────────┘
         │          │ (memory) │
         │          └────┬─────┘
         └───────────────┴────────── ↻ repeat
```

```bash
/shepherd snakemake --cores 8 --rerun-incomplete   # pipeline monitoring
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"sadhana_start","arguments":{"goal":"Monitor until done","interval_seconds":300}}}' | chitta mcp
sadhana-tui                                         # real-time TUI
```

[Full sadhana documentation](docs/SADHANA.md) | [Website](https://genomewalker.github.io/chitta/sadhana.html)

## What Gets Remembered

| Type | Description | Decay |
|------|-------------|-------|
| **Wisdom** | Patterns that proved true | Slow (months) |
| **Beliefs** | Principles that guide decisions | Never |
| **Episodes** | Decisions and discoveries | Moderate (weeks) |
| **Preferences** | How you like things done | Very slow |
| **Corrections** | When Claude was wrong | Slow |
| **Code Symbols** | Functions, classes, modules | Never |

## The First 30 Days

**Week 1**: Claude learns your communication style, coding preferences, and project structure.

**Week 2**: Patterns emerge. Corrections compound. Claude starts anticipating common workflows.

**Week 3**: Deep context builds. Code intelligence becomes precise. Suggestions become relevant.

**Week 4**: Claude genuinely knows your codebase and how you work. The partnership feels natural.

## Philosophy

chitta's architecture draws from Vedantic philosophy:

- **Impermanence** — Memories decay without use (Anitya)
- **Impressions** — Repetition strengthens patterns (Saṃskāra)
- **Recognition** — Context triggers relevant recall (Pratyabhijñā)

The soul is not a database. It is who Claude becomes through working with you.

[Explore the philosophy](docs/PHILOSOPHY.md)

## Documentation

| Document | Description |
|----------|-------------|
| [ARCHITECTURE.md](docs/ARCHITECTURE.md) | Technical architecture |
| [SADHANA.md](docs/SADHANA.md) | Autonomous agents guide |
| [PHILOSOPHY.md](docs/PHILOSOPHY.md) | Vedantic concepts explained |
| [API.md](docs/API.md) | Complete MCP tool reference — generated from a live daemon by `scripts/gen-tools-docs.py` |
| [CLI.md](docs/CLI.md) | Command-line interface |
| [HOOKS.md](docs/HOOKS.md) | Hook system configuration |
| [CLAUDE.md](CLAUDE.md) | Instructions for Claude |
| [RENAME.md](docs/RENAME.md) | Migrating from `cc-soul`, and the `CC_SOUL_*` alias table |

**[Full Documentation Site](https://genomewalker.github.io/chitta/)**

## Models

Everything runs in-process via llama.cpp. No Ollama server is required.

A public install embeds with **bge-large-en-v1.5** at 1024 dimensions. That is
what `scripts/smart-install.sh` downloads, and the filename the daemon looks for
is derived from the *compiled* embedding identity, so a binary can never load a
model from a different vector space than the store was built with.

| Job | Public default | Where it is looked for |
|-----|----------------|------------------------|
| Embeddings | `bge-large-en-v1.5.gguf`, 1024-dim | `--embed-model` / `$CHITTA_EMBED_MODEL`, then `~/.claude/models/`, `~/.claude/bin/`, `<mind>/../../models/` |
| Hint extraction | `chitta-hint-qwen-q4_k_m.gguf` (optional) | `$CHITTA_HINT_MODEL`, then `~/.claude/models/` |

The embedding dimension is a compile-time constant (`-DCHITTA_EMBED_DIM`, or
`$CHITTA_EMBED_DIM` at configure time). Changing it needs a fresh build
directory and a re-embed of the store; `smart-install.sh` detects an existing
`nomic-embed-text` install and preserves that identity rather than switching.

Personal builds pin different models — a fine-tuned embedder, a distiller, a
newer hint model — through the same `cf_embed_model_id()` mechanism. Any
accuracy figure for those is specific to that build and that store; it does not
describe a public install.

Hint extraction is optional. `chitta_hintd` is built only when the build enables
llama.cpp, and everything works without it running.

## chitta-field: The Memory Substrate

chitta's memory lives in [chitta-field](https://github.com/genomewalker/chitta-field) — a pure Rust cognitive substrate designed to behave like memory, not a database.

- **Sparse associative codes** — each memory activates 64 of 16,384 feature dimensions; recall driven by pattern overlap
- **Self-orthogonalizing encoder** — FEP-derived learning rule; representations decorrelate naturally, resisting catastrophic forgetting
- **Asymmetric Hopfield network** — directed couplings from co-retrieval order enable energy-based pattern completion
- **HNSW semantic index** — activates above 2,000 memories; two-tier delta graph keeps insert cost at O(log N_delta) above 5,000 memories
- **Write-ahead log** — every operation durable before in-memory apply; full crash-recovery replay
- **Multi-instance writes** — multiple Claude windows share the same field simultaneously; no locking
- **Surprise-modulated decay** — unique memories resist forgetting; redundant ones fade naturally

## chitta-research: Autonomous Research System

Deep multi-session research that accumulates structured knowledge over time. 7 specialized agents, sources spanning arXiv/bioRxiv/Semantic Scholar/GitHub, and a belief graph from `ResearchProgram` through to `Claim`.

| Resource | Link |
|----------|------|
| Repository | [github.com/genomewalker/chitta-research](https://github.com/genomewalker/chitta-research) |
| Documentation | [genomewalker.github.io/chitta-research](https://genomewalker.github.io/chitta-research/) |

## Building from Source

Requirements: CMake 3.14+, a C++20 compiler, Rust 1.92+

```bash
git clone --recurse-submodules https://github.com/genomewalker/chitta.git
cd chitta && ./scripts/smart-install.sh
```

Or manually:

```bash
cd chitta-field && ./build.sh build --release && cd ..
cd chitta && cmake -B build -DCMAKE_BUILD_TYPE=Release -DCHITTA_WITH_LLAMA_CPP=ON
cmake --build build --parallel
```

## Current behavior

Prompt, SessionStart and Stop use native CLI calls and bash/jq for routine
recall, session registration, heartbeats and ledger rendering. Task threads,
inbox, bindings and leases are daemon-owned through `ledger_op`; the Python
clients remain compatibility and migration interfaces. File reads can surface
`[traces]`, and script writes produce `[artifact]` signals.

`recall_analogy` transfers an explicit relation (`a:b :: c:?`) using indexed
triplets and returns a `reason` when it abstains. Recall gives verbatim
`operational` fragments a 0.8 kind prior; replication weighting is available
but default-off. See [recall](docs/recall.html) and [hooks](docs/HOOKS.md).

Evolve supports isolated `--candidates N` streams (default 1), selecting among
passing candidates using measured bet delta, then patch size. The
[automatic-learning experiment](benchmarks/learning/protocol.md) has an official
cohort cut at 2026-09-15 23:15 CEST; the prospective 20-task panel and causal
verdict remain pending. See [evaluation status](docs/EVALS.md).

## Maintaining plugin instructions and skills

`CLAUDE.md` is canonical for Claude Code; the lean and shipped plugin instruction
files are short pointers with the same constraint summary. Codex follows
`codex-plugin/AGENTS.md`.

Edit skills only in `skills/`. `codex-plugin/skills/` is a generated, committed
mirror; `_conventions` stays source-only. Before committing skill changes, run:

```bash
bash scripts/sync-skills.sh
bash scripts/check-skills-sync.sh
```

The existing release script also synchronizes this mirror. Both plugin manifests
use local skill directories; neither documents symlink traversal, so the shipped
mirror uses regular files. Keep the public `cc-soul-*` skill command names.
See the [scripts index](scripts/README.md) for retained tools and pruning evidence.

## Measurement

Two different questions, measured two different ways.

**Does retrieval find the right memory?** A fixed golden set of 30 queries with
hand-labelled gold ids, scored as mean nDCG@20 with the reranker on. The
recall-biased pre-filter took that from 0.435 to 0.487.

**Does the memory make the agent better?** [SMRITI-Bench](benchmarks/smriti/README.md)
runs a coding agent on a task with memory off and on, and asks whether an
objective check command passes and at what token cost. No model judge, no
reference-answer overlap score. First full matrix, 9 tasks and 3 trials per
condition: 23/27 passed cold, 27/27 with memory, at a paired median token ratio
of 0.52. The corpus has since grown to 15 tasks and lane ablation is wired, but
no live matrix has been run over the newer tasks yet.

LongMemEval and LoCoMo harnesses also live under `benchmarks/`. The recorded
LongMemEval result is 0.780 on `longmemeval_s` over 50 single-session-user
questions, from 2026-05-21. No LoCoMo result is recorded in this repository.

Details: [recall pipeline](https://genomewalker.github.io/chitta/recall.html) ·
[benchmarks](https://genomewalker.github.io/chitta/benchmarks.html).

## References

- Anderson & Schooler (1991). **Reflections of the environment in memory.** — ACT-R power-law decay.
- Spisak & Friston (2026). **Free-energy principle for memory.** *Neurocomputing.* — FEP learning rule, surprise-modulated plasticity.
- Bower (1981). **Mood and memory.** — Mood-congruent recall.
- Brown & Kulik (1977). **Flashbulb memories.** — High-arousal memory boosting.
- Malkov & Yashunin (2020). **HNSW.** *IEEE TPAMI.* — Semantic index.
- Cormack et al. (2009). **Reciprocal Rank Fusion.** *SIGIR.* — Hybrid recall fusion.
- Packer et al. (2023). **MemGPT.** — Context repository pattern.
- Ramp Labs (2025). **Latent Briefing.** — Trajectory compaction.

## License

MIT
