> Status as of 2026-09-15: decision memo from Codex gpt-6-astra (read-only consultation), awaiting the owner's call. Both recommendations are proposals, not applied.

## Decision memo

**The analogy benchmark’s structural section tests the wrong thing.** Its labels explicitly identify *related memories*, not graph isomorphism, yet scoring requires one exact memory ID. A filler-invariant signature cannot distinguish semantically different memories with identical shapes. The six structural failures are valid service failures, but not evidence that VSA cannot represent analogies. ([tasks.json:555](/projects/caeg/scratch/kbd606/tmp/codex-wt-consult/benchmarks/analogy/tasks.json:555), [run.py:17](/projects/caeg/scratch/kbd606/tmp/codex-wt-consult/benchmarks/analogy/run.py:17), [analogy.rs:311](/projects/caeg/scratch/kbd606/tmp/codex-wt-consult/chitta-field/src/analogy.rs:311))

**The MDL premise also needs updating.** The live log contains a pooled acceptance: saving **97 bytes**, evidence **70,558 bytes**, three pooled chunks, for “evolve-cycle-3-started.” That contradicts “never accepts”; it does not establish useful selection. The private replay’s **0/224** remains a separate result. ([shadow log:267](/home/kbd606/.claude/mind/mdl_gate_shadow.jsonl:267), [EVOLVE.md:74](/projects/caeg/scratch/kbd606/tmp/codex-wt-consult/docs/EVOLVE.md:74))

## 1. MDL: choose **(c), drop admission compression and rely on recall ranking**

**Diagnosis:** zlib savings are the wrong operational proxy for useful conversational learning. This rejects the chosen compressor/evidence model, not MDL in general. The corpus statistic rewards reduced compressed bytes, charges raw learning length, and replaces part of a 32 KiB dictionary. It measures string reuse plus dictionary displacement; nothing in that statistic measures whether advice changes a later decision correctly. ([mdl_gate.hpp:143](/projects/caeg/scratch/kbd606/tmp/codex-wt-consult/chitta/include/chitta/mdl_gate.hpp:143))

Changing the margin until acceptance becomes nonzero would optimize acceptance, not usefulness. The new accepted project-status statement illustrates the distinction.

“Admit” should mean **store as an eligible retrieval candidate**, subject to existing deduplication—not certify truth or usefulness. This largely preserves present behavior: native storage happens before the shadow judgment. ([native_distiller.cpp:315](/projects/caeg/scratch/kbd606/tmp/codex-wt-consult/chitta/src/native_distiller.cpp:315))

Do **not** substitute the present utility posterior as a hard gate. The ledger credits every injected ID from subsequent command outcomes in a shared time window; that is association, with confounding from task difficulty and co-injected memories. Unknown outcomes are excluded appropriately, but exclusion does not establish causality. ([outcome_ledger.py:56](/projects/caeg/scratch/kbd606/tmp/codex-wt-consult/chitta-mcp/outcome_ledger.py:56))

Also, SMRITI’s 41/45 versus 33/45 demonstrates value from **planted memories**, not from this automatic distiller’s output. ([README.md:3](/projects/caeg/scratch/kbd606/tmp/codex-wt-consult/benchmarks/smriti/README.md:3))

**Cheapest falsifiable experiment:** freeze 20 independently graded, subsequent tasks from real sessions, chosen before inspecting retrieval results. Run paired trials with identical model, budget, and existing memories: one includes the preceding automatic-learning cohort through current ranking; the other excludes only that cohort. Verify actual injection. No source transcript or future information enters either prompt.

**Decision:** retain automatic admission if it produces **at least three net additional task successes out of 20**. This is a screening threshold, not statistical proof. Failure means retire automatic free-form learning admission, preserving explicit user memories and source episodes; delete the corresponding automatic storage path. ([native_distiller.cpp:322](/projects/caeg/scratch/kbd606/tmp/codex-wt-consult/chitta/src/native_distiller.cpp:322))

Retire the native MDL shadow tap, pooling, and corpus-dictionary machinery now. More compressor tuning is not the next experiment.

## 2. Analogy: choose **re-scope to explicit relation transfer**

**Diagnosis: a definite serving defect, compounded by representation noise—not a demonstrated VSA impossibility.**

The RPC forces **10,000 facts**, and extraction takes the first entries **before validity filtering**. Thus the benchmark does not exercise the advertised entire graph. ([field_memory_recall.cpp:2186](/projects/caeg/scratch/kbd606/tmp/codex-wt-consult/chitta/src/handlers/field_memory_recall.cpp:2186), [triplet.rs:523](/projects/caeg/scratch/kbd606/tmp/codex-wt-consult/chitta-field/src/organ/triplet.rs:523))

Structural source-ID recovery already exists, but only after that truncation. Missing probe signatures produce an error whose explanation the C++ wrapper discards. I cannot attribute all six historical errors specifically to truncation from this artifact alone. ([store.rs:2255](/projects/caeg/scratch/kbd606/tmp/codex-wt-consult/chitta-field/src/store.rs:2255), [ffi.rs:10164](/projects/caeg/scratch/kbd606/tmp/codex-wt-consult/chitta-field/src/ffi.rs:10164), [field_memory_recall.cpp:2222](/projects/caeg/scratch/kbd606/tmp/codex-wt-consult/chitta/src/handlers/field_memory_recall.cpp:2222))

Even surviving entities lose all but 64 lexicographically selected record terms. Candidates can come from anywhere sharing any predicate with `c`, without a supporting edge to `c`. That invites unsupported answers. ([analogy.rs:79](/projects/caeg/scratch/kbd606/tmp/codex-wt-consult/chitta-field/src/analogy.rs:79), [analogy.rs:257](/projects/caeg/scratch/kbd606/tmp/codex-wt-consult/chitta-field/src/analogy.rs:257))

**Narrow contract:** infer the directed predicate connecting `a` and `b`; return actual matching neighbors of `c`, with supporting edges. Abstain on missing or ambiguous relations. Use indexed graph lookups. Remove structural mode and VSA ranking from this endpoint; retain ordinary graph storage/querying.

**Cheapest experiment:** on one frozen snapshot, independently enumerate complete valid answers for the existing 14 proportional queries. Compare an exact relation-join baseline with current RPC output; add 14 negative queries whose target lacks the required relation.

**Decision:** require **hit@3 ≥12/14**, **14/14 negative abstentions**, and **zero unsupported returned answers**. Count missing grounding as failures, separately reported. If this fails, remove `recall_analogy` entirely; keep `query_graph`. No graph cleanup campaign or larger VSA model before that test.