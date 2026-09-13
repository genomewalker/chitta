"""Reranking and rank-fusion for the recall gateway.

Two pieces, both independent of the daemon transport so they can be exercised
without a running chittad:

- ``get_reranker`` — the lazily-loaded cross-encoder used to reorder a recall
  overfetch. Returns None when no backend is installed, which is the signal to
  skip reranking entirely rather than to fail the recall.
- ``rrf_merge`` — reciprocal rank fusion across the multi-lane recall results.
"""

from __future__ import annotations

import asyncio
import logging
import os
from concurrent.futures import ThreadPoolExecutor
from typing import Any, Callable, Protocol

logger = logging.getLogger("chitta-mcp")

RERANKER_MODEL = "cross-encoder/ms-marco-MiniLM-L-6-v2"
# How many extra candidates to pull before reranking, as a multiple of `limit`.
RERANK_FETCH_MUL = 4
RERANK_MAX_LEN = int(os.environ.get("CHITTA_RERANK_MAX_LEN", "128"))

# Cross-encoder inference is CPU-heavy and must never occupy the asyncio event
# loop. One worker also prevents concurrent MCP sessions from oversubscribing
# the model's own inference threads and creating a latency cliff.
_rerank_executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="chitta-rerank")


async def run_reranker(func: Callable[..., Any], *args: Any) -> Any:
    """Run model loading or inference on the dedicated serial executor."""
    loop = asyncio.get_running_loop()
    return await loop.run_in_executor(_rerank_executor, func, *args)


class Reranker(Protocol):
    """The one method both backends provide: score (query, passage) pairs."""

    def predict(self, pairs: list[tuple[str, str]]) -> Any: ...


def resolve_onnx_dir() -> str:
    """Directory holding model_int8.onnx plus its tokenizer, or "" if there is none.

    CHITTA_RERANK_ONNX_DIR wins when set; otherwise autodetect the conventional
    export path.
    """
    override = os.environ.get("CHITTA_RERANK_ONNX_DIR", "")
    if override:
        return override
    default = os.path.expanduser("~/.claude/models/rerank-onnx")
    return default if os.path.isdir(default) else ""


class OnnxReranker:
    """onnxruntime-backed cross-encoder, same predict(pairs) contract as CrossEncoder.

    Runs INT8 on the onnxruntime CPU provider instead of torch. Measured e2e
    (dev): L-6 INT8 @ 8 threads, max_len=128 → ~500ms median per recall,
    multihop nDCG@20 0.688 (+0.146 vs 0.542 native), single_hop 0.852 (flat).
    """

    def __init__(self, model_dir: str) -> None:
        import onnxruntime as ort
        from transformers import AutoTokenizer

        self._tok = AutoTokenizer.from_pretrained(model_dir)
        options = ort.SessionOptions()
        options.intra_op_num_threads = int(os.environ.get("CHITTA_RERANK_THREADS", "8"))
        options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
        self._sess = ort.InferenceSession(
            os.path.join(model_dir, "model_int8.onnx"),
            options,
            providers=["CPUExecutionProvider"],
        )
        self._innames = {i.name for i in self._sess.get_inputs()}

    def predict(self, pairs: list[tuple[str, str]]) -> Any:
        if not pairs:
            return []
        enc = self._tok(
            [p[0] for p in pairs],
            [p[1] for p in pairs],
            padding=True,
            truncation=True,
            max_length=RERANK_MAX_LEN,
            return_tensors="np",
        )
        feed = {k: v for k, v in enc.items() if k in self._innames}
        return self._sess.run(None, feed)[0][:, 0]


# None = not yet attempted; False = attempted and unavailable, do not retry.
_reranker: Reranker | None | bool = None


def get_reranker() -> Reranker | None:
    """Return the process-wide reranker, loading it on first call.

    Returns None when no backend is installed. Loading pulls in onnxruntime or
    torch and can fail for many unrelated reasons (missing package, corrupt
    export, no memory); every one of them means the same thing to the caller —
    recall proceeds unreranked — so the failure is cached and logged once
    instead of retried per query.
    """
    global _reranker
    if _reranker is not None:
        return _reranker if _reranker is not False else None
    try:
        import warnings

        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            onnx_dir = resolve_onnx_dir()
            if onnx_dir and os.path.isdir(onnx_dir):
                _reranker = OnnxReranker(onnx_dir)
            else:
                from sentence_transformers import CrossEncoder

                _reranker = CrossEncoder(RERANKER_MODEL)
    except Exception as exc:  # noqa: BLE001 — see docstring: any failure means "no reranker"
        logger.info("reranker unavailable, recall will not be reranked: %s", exc)
        _reranker = False
    return _reranker if _reranker is not False else None


def rrf_merge(lists: list[list[dict]], k: int = 60, limit: int = 10) -> list[dict]:
    """Reciprocal Rank Fusion across several ranked result lists.

    Accumulates 1/(k + rank + 1) per key across all lanes, so a memory that
    places well in several lanes outranks one that tops a single lane. Keys on
    memory_id, falling back to a text prefix for lanes that do not carry ids.
    """
    key_items: dict[str, dict] = {}
    for lst in lists:
        for rank, item in enumerate(lst):
            key = item.get("memory_id") or item.get("text", "")[:80]
            rrf = 1.0 / (k + rank + 1)
            if key not in key_items:
                key_items[key] = {"rrf": rrf, "item": item}
            else:
                key_items[key]["rrf"] += rrf
    ranked = sorted(key_items.values(), key=lambda x: x["rrf"], reverse=True)
    return [r["item"] for r in ranked[:limit]]
