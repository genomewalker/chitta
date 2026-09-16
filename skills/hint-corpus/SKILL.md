---
name: hint-corpus
description: "Fine-tune the Qwen3-0.6B hint model — corpus gen, LoRA/unsloth, GGUF export, Ollama"
execution: direct
aliases: [hint-finetune, corpus-gen, build-hint-model]
---

# hint-corpus

Full pipeline to produce `chitta-hint-tuned` (Qwen3-0.6B Q4_K_M) from scratch.

## Quick Start

```bash
# 1. Generate corpus (requires Ollama + gemma4:26b or any capable model)
python3 $PLUGIN_DIR/scripts/generate_hint_corpus.py \
    --out /maps/projects/caeg/scratch/kbd606/tmp/hint_corpus_raw.jsonl \
    --model gemma4:26b \
    --target 3000

# 2. Convert to Qwen3 ChatML for unsloth
python3 $PLUGIN_DIR/scripts/convert_to_chatml.py \
    --in  /maps/projects/caeg/scratch/kbd606/tmp/hint_corpus_raw.jsonl \
    --out /maps/projects/caeg/scratch/kbd606/tmp/hint_corpus_chatml.jsonl \
    --split 0.1

# 3. Fine-tune Qwen3-0.6B + export GGUF
bash $PLUGIN_DIR/scripts/finetune_hint_qwen.sh \
    --data /maps/projects/caeg/scratch/kbd606/tmp/hint_corpus_chatml.jsonl \
    --steps 300

# 4. Register with Ollama
bash $PLUGIN_DIR/chitta-mcp/enrichers/setup_hint_model.sh
```

Set `PLUGIN_DIR` to the chitta repository or installed plugin root (for Claude Code, `${CLAUDE_PLUGIN_ROOT}`).

---

## Stage 1 — Corpus Generation

`generate_hint_corpus.py` builds diverse synthetic conversation excerpts and labels them via a teacher LLM. It covers:

| Axis | Examples |
|------|---------|
| Profession | bioinformatician, nurse, teacher, architect, chef... |
| Location | city, country, living situation |
| Language background | native/non-native/bilingual |
| Relationships | partner, children, pets |
| Health | dietary restrictions, exercise habits |
| Hobbies | sports, arts, gaming, gardening... |
| Preferences | dark mode, editors, morning/evening person |
| Education | PhD, self-taught, vocational |

**35% hard negatives** (questions, debugging requests, factual queries — output: `-`).

Key flags:
```
--target N        # examples to generate (default: 1500; recommend 3000)
--model MODEL     # teacher model (default: llama3.3:70b; gemma4:26b works well)
--neg-ratio 0.35  # fraction of negatives
--dry-run         # preview templates, no LLM calls
```

Expected runtime: ~2h for 3000 examples with gemma4:26b on a single GPU node.

---

## Stage 2 — ChatML Conversion

`convert_to_chatml.py` wraps each `{"input", "output"}` row in a ShareGPT conversation with the system prompt baked in.

**System prompt** (fixed, version-controlled):
> *You extract personal facts from conversation excerpts. Given a message or conversation, output a single concise third-person sentence about the user (e.g. "User lives in Copenhagen.", "User has two cats."). If no stable personal fact is present, output exactly: -*

`--split 0.1` writes a 10% eval holdout to `<out>_eval.jsonl`.

---

## Stage 3 — Fine-tuning

`finetune_hint_qwen.sh` runs QLoRA via unsloth:

| Hyperparameter | Default |
|----------------|---------|
| Base model | `Qwen/Qwen3-0.6B` |
| LoRA rank | 16 |
| LoRA alpha | 32 |
| Max steps | 200 |
| Batch size | 4 × grad_accum 4 = 16 effective |
| Learning rate | 2e-4 |
| Quantisation | 4-bit QLoRA (bitsandbytes) |

Requirements:
```bash
pip install "unsloth[colab-new]" xformers trl peft accelerate bitsandbytes
```

GPU note: Qwen3-0.6B fits in ~4 GB VRAM at 4-bit. CPU training is possible but slow (~30 min/100 steps).

After training, the script:
1. Merges LoRA → fp16 safetensors (`$OUT_DIR`)
2. Converts to F16 GGUF via `convert_hf_to_gguf.py` (needs llama.cpp)
3. Quantises to Q4_K_M via `llama-quantize` (~480 MB)

Override paths via environment:
```bash
CHITTA_HINT_DATA=/path/to/corpus.jsonl
CHITTA_HINT_MODEL_DIR=/path/to/merged_output
CHITTA_HINT_GGUF_DIR=/path/to/gguf_output
LLAMA_CONVERT=/path/to/llama.cpp/convert_hf_to_gguf.py
LLAMA_QUANTIZE=/path/to/llama-quantize
```

---

## Stage 4 — Ollama Registration

`setup_hint_model.sh` registers the Q4_K_M GGUF with Ollama as `chitta-hint-tuned`.

It checks `$CHITTA_HINT_GGUF_DIR` for the GGUF, falls back to F16, then safetensors.

After registration, test with:
```bash
chitta hint_enrich --dry-run
# or via MCP:
chitta run_hint_enricher --dry_run true --limit 10
```

---

## Embedding Quality Check

After registration, run the embedding benchmark:
```bash
python3 /maps/projects/caeg/scratch/kbd606/tmp/test_embeddings.py
```

Target metrics vs Qwen2.5-0.5B baseline:
| Metric | Baseline | Target |
|--------|----------|--------|
| Personal↔Personal cosine | 0.76 | >0.85 |
| Separation ratio (pp−pn) | 0.28 | >0.40 |
| NN accuracy | 5/8 | 7/8+ |

Qwen3-0.6B shares its architecture with Qwen3-Embedding-0.6B (MTEB STS 86.57) — use `--pooling last` and L2-normalize embeddings.

---

## Notes

- **Single GGUF, dual use**: same checkpoint serves generation (personal fact extraction) and embedding (last-token pooling + L2 norm). Append `<|endoftext|>` as final token for embedding mode.
- **Corpus is general-purpose** — not specific to any user. Covers 10+ diversity axes so the model generalises across professions, cultures, and relationship types.
- **Iterative improvement**: run `/hint-corpus` again after accumulating new session data. Use `--target 5000` if separation metrics plateau at 3k.
