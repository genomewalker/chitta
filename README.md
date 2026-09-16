# Chitta

> Status as of 2026-09-16.
> Renamed from cc-soul on 2026-09-02; legacy CC_SOUL_* aliases remain supported — see [docs/RENAME.md](docs/RENAME.md).

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Claude Code Plugin](https://img.shields.io/badge/Claude%20Code-Plugin-blue.svg)](https://claude.ai/code)
[![Documentation](https://img.shields.io/badge/docs-genomewalker.github.io%2Fchitta-blue)](https://genomewalker.github.io/chitta/)

**[Documentation & Architecture →](https://genomewalker.github.io/chitta/)**

Persistent memory for Claude Code and Codex. A C++ daemon manages a Rust store; hooks record session information and inject retrieved context.

## Recall example

A caching query can retrieve a stored decision about Redis or an in-memory LRU cache, including the recorded rationale. The result depends on what was stored and which records pass recall admission.

## Installation

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

## Architecture

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
│         (chitta-field — Rust memory store)      │
│                                                              │
│   Memories │ Triplets │ Sparse Codes │ WAL │ Embeddings     │
└─────────────────────────────────────────────────────────────┘
```

On each prompt, hooks retrieve memories and inject admitted results as context. This path does not require an explicit tool call by the agent.

## Capabilities

### Session records
Hooks and transcript distillation record corrections, preferences, and session outcomes. Recall can return those records in later sessions.

### Code indexing
Tree-sitter parsing extracts functions, classes, and call graphs. Semantic search finds code by what it does, not just what it's named.

### Reinforcement and decay
Access reinforces memories; confidence decays over time according to memory type.

### Shared store
Claude Code and Codex sessions connected to the same daemon share its memory store.

### Action prediction
Recorded context/action pairs supply predictions, such as suggesting tests after a sequence of file edits.

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

## Memory types

| Type | Description | Decay |
|------|-------------|-------|
| **Wisdom** | Patterns that proved true | Slow (months) |
| **Beliefs** | Principles that guide decisions | Never |
| **Episodes** | Decisions and discoveries | Moderate (weeks) |
| **Preferences** | How you like things done | Very slow |
| **Corrections** | When Claude was wrong | Slow |
| **Code Symbols** | Functions, classes, modules | Never |

## Memory lifecycle

Session observations record preferences and corrections. Code indexing records project structure. Repeated context/action pairs contribute to workflow predictions; recall and confidence decay determine which stored records are surfaced.

## Philosophy

chitta's architecture draws from Vedantic philosophy:

- **Impermanence** — Memories decay without use (Anitya)
- **Impressions** — Repetition strengthens patterns (Saṃskāra)
- **Recognition** — Context triggers relevant recall (Pratyabhijñā)

These terms describe memory decay, reinforcement, and retrieval in the implementation.

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

chitta's memory lives in [chitta-field](https://github.com/genomewalker/chitta-field) — a Rust memory store with sparse codes, graph relationships, and persistence.

- **Sparse associative codes** — each memory activates 64 of 16,384 feature dimensions; recall driven by pattern overlap
- **Self-orthogonalizing encoder** — FEP-derived learning rule [19](#ref-19); representations decorrelate naturally, resisting catastrophic forgetting
- **Asymmetric Hopfield network** [19](#ref-19) [33](#ref-33) — directed couplings from co-retrieval order enable energy-based pattern completion
- **HNSW [4](#ref-4) semantic index** — activates above 2,000 memories; two-tier delta graph keeps insert cost at O(log N_delta) above 5,000 memories
- **Write-ahead log** — every operation durable before in-memory apply; full crash-recovery replay
- **Multi-instance writes** — multiple Claude windows share the same field simultaneously; no locking
- **Surprise-modulated decay** — unique memories resist forgetting; redundant ones fade naturally

## chitta-research: Autonomous Research System

Multi-session research that accumulates structured knowledge over time. 7 specialized agents, sources spanning arXiv/bioRxiv/Semantic Scholar/GitHub, and a belief graph from `ResearchProgram` through to `Claim`.

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

Retrieval quality and task outcomes are measured separately.

**Retrieval quality.** A fixed golden set of 30 queries with
hand-labelled gold ids, scored as mean nDCG@20 with the reranker on. The
recall-biased pre-filter took that from 0.435 to 0.487.

**Task outcomes.** [SMRITI-Bench](benchmarks/smriti/README.md)
runs a coding agent on a task with memory off and on, and asks whether an
objective check command passes and at what token cost. No model judge, no
reference-answer overlap score. First full matrix, 9 tasks and 3 trials per
condition: 23/27 passed cold, 27/27 with memory, at a paired median token ratio
of 0.52. The corpus has since grown to 15 tasks and lane ablation is wired, but
no live matrix has been run over the newer tasks yet.

LongMemEval [16](#ref-16) and LoCoMo [17](#ref-17) harnesses also live under `benchmarks/`. The recorded
LongMemEval result is 0.780 on `longmemeval_s` over 50 single-session-user
questions, from 2026-05-21. No LoCoMo result is recorded in this repository.

Details: [recall pipeline](https://genomewalker.github.io/chitta/recall.html) ·
[benchmarks](https://genomewalker.github.io/chitta/benchmarks.html).

## License

MIT

Citation context: the memory-design references include ACT-R activation
[1](#ref-1), Spisak and Friston's attractor model [19](#ref-19), mood and
flashbulb-memory research [20](#ref-20) [21](#ref-21), HNSW [4](#ref-4), RRF
[3](#ref-3), and MemGPT [22](#ref-22). Ben Geist's Latent Briefing post
(2026-04-10) concerns KV-cache memory sharing, not a validation of chitta's text
compaction [23](#ref-23).

<!-- BEGIN CITATIONS -->
## References

- <a id="ref-1"></a>**[1]** John R. Anderson and Lael J. Schooler. Reflections of the Environment in Memory. Psychological Science 2(6), 396–408 (1991). [source](<https://doi.org/10.1111/j.1467-9280.1991.tb00174.x>)
- <a id="ref-3"></a>**[3]** Gordon V. Cormack, Charles L. A. Clarke, and Stefan Buettcher. Reciprocal rank fusion outperforms condorcet and individual rank learning methods. SIGIR, 758–759 (2009). [source](<https://doi.org/10.1145/1571941.1572114>)
- <a id="ref-4"></a>**[4]** Yu. A. Malkov and D. A. Yashunin. Efficient and robust approximate nearest neighbor search using Hierarchical Navigable Small World graphs. IEEE TPAMI 42(4), 824–836 (2020); arXiv:1603.09320 (2016). [source](<https://arxiv.org/abs/1603.09320>) [source](<https://doi.org/10.1109/TPAMI.2018.2889473>)
- <a id="ref-16"></a>**[16]** Di Wu, Hongwei Wang, Wenhao Yu, Yuwei Zhang, Kai-Wei Chang, and Dong Yu. LongMemEval: Benchmarking Chat Assistants on Long-Term Interactive Memory. ICLR (2025); arXiv:2410.10813 (2024). [source](<https://arxiv.org/abs/2410.10813>)
- <a id="ref-17"></a>**[17]** Adyasha Maharana, Dong-Ho Lee, Sergey Tulyakov, Mohit Bansal, Francesco Barbieri, and Yuwei Fang. Evaluating Very Long-Term Conversational Memory of LLM Agents. ACL (2024); arXiv:2402.17753. [source](<https://arxiv.org/abs/2402.17753>) [source](<https://aclanthology.org/2024.acl-long.747/>)
- <a id="ref-19"></a>**[19]** Tamas Spisak and Karl Friston. Self-orthogonalizing attractor neural networks emerging from the free energy principle. Neurocomputing, article 133472 (2026); arXiv:2505.22749 (2025). [source](<https://arxiv.org/abs/2505.22749>) [source](<https://doi.org/10.1016/j.neucom.2026.133472>)
- <a id="ref-20"></a>**[20]** Gordon H. Bower. Mood and memory. American Psychologist 36(2), 129–148 (1981). [source](<https://doi.org/10.1037/0003-066X.36.2.129>)
- <a id="ref-21"></a>**[21]** Roger Brown and James Kulik. Flashbulb memories. Cognition 5(1), 73–99 (1977). [source](<https://doi.org/10.1016/0010-0277(77)90018-X>)
- <a id="ref-22"></a>**[22]** Charles Packer, Sarah Wooders, Kevin Lin, Vivian Fang, Shishir G. Patil, Ion Stoica, and Joseph E. Gonzalez. MemGPT: Towards LLMs as Operating Systems. arXiv:2310.08560 (2023). [source](<https://arxiv.org/abs/2310.08560>)
- <a id="ref-23"></a>**[23]** Ben Geist. Latent Briefing: Efficient Memory Sharing for Multi-Agent Systems via KV Cache Compaction. Ramp Labs Research (2026-04-10). [source](<https://labs.ramp.com/research/latent-briefing-kv-cache/>)
- <a id="ref-33"></a>**[33]** John J. Hopfield. Neural networks and physical systems with emergent collective computational abilities. PNAS 79(8), 2554–2558 (1982). [source](<https://doi.org/10.1073/pnas.79.8.2554>)
<!-- END CITATIONS -->
