#!/usr/bin/env python3
"""G0 recall grader — nDCG@10 scoring against memory-ID ground truth.

Two-phase design:
  --bootstrap   Discover gold memory IDs by running bootstrap queries, save to
                grade-recall-goldids.json. Must be run once (or after memories
                change significantly) before grading.
  (default)     Load gold IDs, run natural-language queries, compute nDCG@10.

nDCG@10: for each query, rank position of gold memories in recall@LIMIT results.
DCG = sum(1/log2(pos+2) for gold hits in top LIMIT). nDCG = DCG/IDCG.
Binary relevance (1 if gold ID in results, 0 otherwise).

Usage:
  grade-recall.py --bootstrap [--limit 20]
  grade-recall.py [--limit 20] [--min 0.7] [--quiet]

Exit 0 if mean_nDCG@LIMIT >= --min, else 1.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import shlex
import subprocess
import sys
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

try:
    import numpy as _np

    _HAS_NUMPY = True
except ImportError:
    _HAS_NUMPY = False


def g_entropy(scores):
    """Shannon entropy of the softmax over recall scores."""
    if len(scores) < 2:
        return 0.0
    mx = max(scores)
    exps = [math.exp(s - mx) for s in scores]
    total = sum(exps)
    probs = [e / total for e in exps]
    return -sum(p * math.log(p + 1e-12) for p in probs)


def s_entropy_from_embeddings(result_ids):
    """Effective-rank entropy: eigenvalues of the cosine Gram matrix of real embedding vectors.

    Calls `chitta get_embeddings` to fetch 1024-dim vectors per candidate.
    Returns None when numpy unavailable or embeddings can't be fetched.
    """
    if not _HAS_NUMPY or len(result_ids) < 2:
        return 0.0 if _HAS_NUMPY else None
    try:
        out = subprocess.run(
            _chitta_cli(
                "get_embeddings",
                "--ids",
                json.dumps([str(i) for i in result_ids]),
                "--json",
            ),
            capture_output=True,
            text=True,
            timeout=30,
        )
        if out.returncode != 0:
            return None
        data = json.loads(out.stdout)
        emb_map = data.get("embeddings", data.get("structured", {}))
        vecs = [emb_map[str(rid)] for rid in result_ids if str(rid) in emb_map]
        if len(vecs) < 2:
            return None
        E = _np.asarray(vecs, dtype=float)
        norms = _np.linalg.norm(E, axis=1, keepdims=True)
        norms[norms == 0] = 1.0
        E = E / norms
        gram = E @ E.T
        eig = _np.linalg.eigvalsh(gram)
        eig = eig[eig > 1e-10]
        if eig.size == 0:
            return 0.0
        probs = eig / eig.sum()
        return float(-_np.sum(probs * _np.log(probs + 1e-12)))
    except (ImportError, ValueError, FloatingPointError):
        # numpy absent, or a matrix eigendecomposition that did not converge.
        # None is the "metric unavailable" value the report already handles.
        return None


# Cross-encoder reranker — loaded once, used for all queries
_reranker = None
_RERANKER_MODEL = "cross-encoder/ms-marco-MiniLM-L-12-v2"
_RERANK_FETCH_MUL = 4  # fetch this many × limit, then rerank to top-limit


def _sigmoid(x: float) -> float:
    return 1.0 / (1.0 + math.exp(-x))


ABSTAIN_SCORE_THRESHOLD = 0.6  # epistemic: sigmoid(cross-encoder) below threshold → undetermined
ABSTAIN_BAND_EPS = 0.05  # posterior-band: gold/non-gold score gap < ε → rank is noise

# C2 self-monitoring ("feeling of knowing") — monotone binning of the daemon's
# Platt-calibrated max_relevance (field_memory_recall.cpp: sigma(3.27*cos-0.85)),
# a production hybrid-lane scalar: no cross-encoder, no torch, endpoint-free.
#
# 2-BAND (v5.70.1 correction): the 3-band KNOWN(>=.86)/THIN(>=.81) split shipped
# in v5.70.0 calibrated on an earlier store state, but store drift inverted the
# KNOWN-vs-THIN boundary (re-measured KNOWN gold-hit@3 .62 < THIN .64) and no
# threshold restored monotonic k=3 separation. Re-scoped to 2 honest bands:
# KNOWN (confident-but-verify, maxrel >= .81) vs UNKNOWN (< .81). KNOWN-vs-UNKNOWN
# IS monotonic — all 6 probes UNKNOWN, hit@10 clean. C2_THIN_MAXREL retained as
# an alias of the single boundary so ignition_admit and older refs keep working.
C2_KNOWN_MAXREL = 0.81  # single confident-band cut (was 0.86 in the 3-band)
C2_THIN_MAXREL = 0.81  # boundary: all 6 out-of-domain probes < .81; all 30 answerable >= .814

# Out-of-domain probes: no gold exists by construction — must land in UNKNOWN.
C2_PROBES = [
    "recipe for sourdough bread starter",
    "quarterly marketing budget forecast Q3",
    "javascript react useEffect dependency array",
    "wine pairing for grilled salmon",
    "olympic figure skating scoring rules",
    "kubernetes ingress nginx annotation timeout",
]


def c2_tag(max_relevance: float, n_results: int) -> str:
    # 2-band: confident-but-verify (KNOWN) vs boundary (UNKNOWN).
    if n_results == 0 or max_relevance < C2_KNOWN_MAXREL:
        return "UNKNOWN"
    return "KNOWN"


def _platt(cos: float) -> float:
    """Daemon's Platt calibration (field_memory_recall.cpp:521) applied per-item.
    max_relevance == max(_platt(sim) for items); ignition uses the same scale."""
    return 1.0 / (1.0 + math.exp(-(3.27 * cos - 0.85)))


def ignition_admit(platt_scores: list[float], budget: int = 3) -> tuple[list[int], list[int]]:
    """Per-item bimodal admit on the C2-calibrated scale: >= KNOWN cut ignites
    (admitted, up to prod budget), [THIN, KNOWN) is the dead-band ([gap]),
    below THIN drops. A flat low recall ignites nothing — the no-ignition case.
    Returns (admitted_idx, gap_idx)."""
    admitted = [i for i, p in enumerate(platt_scores) if p >= C2_KNOWN_MAXREL][:budget]
    gap = [
        i
        for i, p in enumerate(platt_scores)
        if C2_THIN_MAXREL <= p < C2_KNOWN_MAXREL and i not in admitted
    ]
    return admitted, gap


def _load_reranker():
    global _reranker
    if _reranker is not None:
        return _reranker
    try:
        import warnings

        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            from sentence_transformers import CrossEncoder

            _reranker = CrossEncoder(_RERANKER_MODEL)
    except Exception:  # noqa: BLE001
        # Loading a torch cross-encoder can fail on a missing package, a stale
        # HF cache, a download error, or OOM. They all mean the same thing to
        # the grader — no reranking — so the sentinel is cached and not retried.
        _reranker = False  # sentinel: tried and failed
    return _reranker


GOLDEN_VERSION = 6

# Natural-language queries a user would actually type, paired with:
#   bootstrap_query: a targeted query to find the gold memory during bootstrap
#   gold_signature:  unique string that ONLY appears in the target memory (not in the query)
#   desc:            human label
GOLDEN_SET = [
    {
        "query": "chaos nodes",
        "bootstrap_query": "dandycomp ssh direct no slurm chaos nodes",
        "gold_signature": "dandycomp*",  # matches dandycomp*fl and dandycomp*-fl variants
        "desc": "cluster alias",
    },
    {
        "query": "how does provenance deduplication work",
        "bootstrap_query": "content_prov_idx done signal write-gate dedup hash provenance",
        "gold_signature": "[done] provenance",
        "desc": "prov dedup gate",
    },
    {
        "query": "how does domain reliability work",
        "bootstrap_query": "domain reliability beta prior decay record_success can only decay",
        "gold_signature": "can only decay",
        "desc": "reliability wiring",
    },
    {
        "query": "why does recall get capped per realm",
        "bootstrap_query": "stratify recall realm cap stratify_recall_hits per-realm slots Thompson",
        "gold_signature": "stratify_recall_hits",
        "desc": "sampler fix",
    },
    {
        "query": "NFS resurrection problem",
        "bootstrap_query": "Isilon NFS resurrection snapshot deleted files resurrect",
        "gold_signature": "Isilon",
        "desc": "NFS gotcha",
    },
    {
        "query": "dream sweep gap filling",
        "bootstrap_query": "dream wander gap curiosity cross-session distillation sweep",
        "gold_signature": "dream-sweep",
        "desc": "dream sweep",
    },
    {
        "query": "forward bet prediction horizon",
        "bootstrap_query": "prediction horizon status:open forward bet wisdom testable",
        "gold_signature": "status:open",
        "desc": "forward bet",
    },
    {
        "query": "how to build chitta-field",
        "bootstrap_query": "chitta-field build cmake cargo rust toolchain 1.92 rustup build.sh",
        "gold_signature": "1.92",
        "desc": "build toolchain",
    },
    {
        "query": "how does semantic search work",
        "bootstrap_query": "HNSW semantic search embedding bge cosine similarity RRF chitta-field",
        "gold_signature": "hnsw",  # matches both SSL memories and NL-expansion variants
        "desc": "search arch",
    },
    {
        "query": "distillation pipeline wisdom episode",
        "bootstrap_query": "distill wisdom episode ssl synthesis pipeline",
        "gold_signature": "distill",
        "desc": "distillation pipeline",
    },
    # --- restored from goldids v4 ---
    {
        "query": "how does the dream cycle run",
        "bootstrap_query": "dream-pipeline dream_start local-brain gemma4 publish_path sadhana_manager",
        "gold_signature": "publish_path",
        "desc": "dream cycle sadhana brain",
    },
    {
        "query": "how does the grader abstain on uncertain queries",
        "bootstrap_query": "G0 grader nDCG abstain band epistemic cross-encoder ABSTAIN PASS FAIL",
        "gold_signature": "0 FAIL",
        "desc": "abstain signal cross encoder recall",
    },
    {
        "query": "how does hybrid recall fuse multiple strategies",
        "bootstrap_query": "RRF Reciprocal Rank Fusion hybrid BM25 field HNSW rrf_k chitta-field",
        "gold_signature": "rrf_k",
        "desc": "RRF fusion hybrid bm25 field strategy",
    },
    {
        "query": "how does dream wander fill knowledge gaps",
        "bootstrap_query": "dream_wander curiosity gap-finding exploration cc-soul dream topic",
        "gold_signature": "curiosity-first",
        "desc": "gap memory curiosity dream wander topic",
    },
    {
        "query": "how to recover chitta-field after NFS failure",
        "bootstrap_query": "chitta-field snapshot recovery NFS restore rebuild binary restore-from-NFS",
        "gold_signature": "restore-from-NFS-snapshot",
        "desc": "chitta field store snapshot NFS resurrection",
    },
    {
        "query": "why does dream publication only work with local brain",
        "bootstrap_query": "dream publication local brain provider BridgeBrain restriction only-works",
        "gold_signature": "only-works-with-local-brain",
        "desc": "BridgeBrain local gemma room create dream",
    },
    {
        "query": "how are chitta forward bets resolved",
        "bootstrap_query": "chitta forward bets testable prediction horizon resolution wisdom",
        "gold_signature": "prediction horizon",
        "desc": "forward bet resolution prediction check",
    },
    # --- expanded eval set (v6) ---
    {
        "query": "why does cmake rebuild not pick up rust source changes",
        "bootstrap_query": "cmake build relink rust source archive chitta compilation",
        "gold_signature": "cmake-build-only-relinks-existing-archives",
        "desc": "cmake relink rust rebuild",
    },
    {
        "query": "how to recover HNSW index after corruption",
        "bootstrap_query": "chittad reindex HNSW emb files direct-mode recovery rebuild",
        "gold_signature": "chittad reindex",
        "desc": "HNSW recovery reindex",
    },
    {
        "query": "how are BM25 and vector search combined",
        "bootstrap_query": "dam_blend_alpha BM25 HNSW weight blending strategy",
        "gold_signature": "dam_blend_alpha",
        "desc": "BM25 HNSW blend alpha",
    },
    {
        "query": "how does chitta-bridge install work",
        "bootstrap_query": "chitta-bridge codex plugin install path skills configuration install.sh",
        "gold_signature": "install.sh",
        "desc": "chitta-bridge install skills",
    },
    {
        "query": "what strategies does chitta routing combine",
        "bootstrap_query": "multi-strategy routing BM25 HNSW Hopfield triplet-graph field",
        "gold_signature": "multi-strategy-routing",
        "desc": "multiway routing strategy blend",
    },
    {
        "query": "what is Modern Hopfield network in chitta",
        "bootstrap_query": "Field-RAG Modern Hopfield associative memory network field pattern",
        "gold_signature": "Modern Hopfield",
        "desc": "Hopfield field memory network",
    },
    {
        "query": "what tools does the chitta-bridge plugin expose",
        "bootstrap_query": "chitta-bridge MCP tools review rescue room soul skills",
        "gold_signature": "chitta-bridge Codex plugin created",
        "desc": "chitta-bridge plugin tools",
    },
    {
        "query": "what binary format do chitta snapshots use",
        "bootstrap_query": "chitta-field store V23 sectioned snapshot format binary magic",
        "gold_signature": "V23 sectioned",
        "desc": "V23 snapshot format",
    },
    {
        "query": "how does graph traversal find related memories",
        "bootstrap_query": "chitta graph_traverse start hops bfs neighbor nodes triplet",
        "gold_signature": "graph_traverse",
        "desc": "graph_traverse BFS",
    },
    {
        "query": "what does the dream pipeline do",
        "bootstrap_query": "dream-pipeline sadhana gemma meditation publish local brain refactor",
        "gold_signature": "dream-pipeline-refactor",
        "desc": "dream-pipeline refactor",
    },
    # --- multi-hop: indirect vocabulary bridge from symptom → root cause ---
    {
        "query": "what caused recall to block for nearly a second",
        "bootstrap_query": "smart_recall hybrid_recall EXCLUSIVE lock blocking 800ms is_read_only_tool field_handler RwLock",
        "gold_signature": "EXCLUSIVE lock",
        "desc": "multihop: recall lock contention",
    },
    {
        "query": "why do chitta recall scores look the same for every result",
        "bootstrap_query": "bge-large anisotropic cosine uniform scores flat embedding space chitta-field Platt calibration",
        "gold_signature": "anisotropic",
        "desc": "multihop: score collapse anisotropy",
    },
    {
        "query": "how does chitta dream wander curiosity gap",
        "bootstrap_query": "dream_wander curiosity gap-finding exploration cc-soul dream topic",
        "gold_signature": "curiosity-first",
        "desc": "multihop: dream wander curiosity",
    },
]

RESULTS_PATH = Path(__file__).resolve().parent / "grade-recall-results.json"
GOLD_IDS_PATH = Path(__file__).resolve().parent / "grade-recall-goldids.json"

CHITTA_EVAL_SOCKET = os.environ.get("CHITTA_EVAL_SOCKET")


def _chitta_cli(*args: str) -> list[str]:
    """Build a chitta command, pinning every eval call to the replica socket."""
    cmd = ["chitta", *args]
    if CHITTA_EVAL_SOCKET:
        cmd += ["--socket-path", CHITTA_EVAL_SOCKET]
    return cmd


def _replica_snapshot_id() -> str | None:
    explicit = os.environ.get("CHITTA_EVAL_SNAPSHOT_ID")
    if explicit:
        return explicit
    if not CHITTA_EVAL_SOCKET:
        return None
    eval_mind = Path(
        os.environ.get("CHITTA_EVAL_MIND", "/projects/caeg/scratch/kbd606/tmp/chitta-eval-mind")
    )
    try:
        lines = (eval_mind / "replica.env").read_text().splitlines()
        value = next(
            line.split("=", 1)[1] for line in lines if line.startswith("CHITTA_EVAL_SNAPSHOT_ID=")
        )
        values = shlex.split(value)
        return values[0] if values else None
    except (OSError, StopIteration, ValueError):
        return None


GREEN = "\033[32m"
RED = "\033[31m"
YELLOW = "\033[33m"
BOLD = "\033[1m"
DIM = "\033[2m"
RESET = "\033[0m"


GRADE_REALM = "project:cc-soul"


def _recall_full(query: str, limit: int, strategy: str = "") -> dict:
    """Full recall response — keeps top-level fields (max_relevance, abstain)."""
    env = dict(os.environ, SQZ_NO_DEDUP="1")
    # --no-learn: the eval must never mutate the store it measures. Without it,
    # every graded recall enqueues co-retrieval strengthening (+0.05/pair) that
    # drains on the next snapshot, reinforcing near-but-wrong hits on a sparse
    # store — the store degrades exactly the queries we measure.
    cmd = [
        "chitta",
        "recall",
        "--query",
        query,
        "--limit",
        str(limit),
        "--realm",
        GRADE_REALM,
        "--json",
    ]
    # CC_GRADE_LEARN=1 removes the hygiene flag for the pollution-magnitude
    # experiment (WITHOUT-flag arm of the S0 drain protocol). Default keeps
    # --no-learn so routine grading never mutates the store it measures.
    if os.environ.get("CC_GRADE_LEARN") != "1":
        cmd.insert(-1, "--no-learn")
    # CC_GRADE_EXTRA_ARGS="--prefilter false" etc.: A/B a per-request daemon
    # knob against the same running daemon instead of restarting it per arm.
    extra = os.environ.get("CC_GRADE_EXTRA_ARGS", "").split()
    if extra:
        cmd[-1:-1] = extra
    if strategy:
        cmd += ["--strategy", strategy]
    if CHITTA_EVAL_SOCKET:
        cmd += ["--socket-path", CHITTA_EVAL_SOCKET]
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=60, env=env)
        parsed = json.loads(out.stdout)
        return parsed if isinstance(parsed, dict) else {}
    except (OSError, subprocess.SubprocessError, json.JSONDecodeError):
        # chitta missing, over its 60s budget, or a non-JSON reply. An empty
        # dict scores as a miss for this query rather than aborting the grade.
        return {}


def _recall_raw(query: str, limit: int, strategy: str = "") -> list[dict]:
    return _recall_full(query, limit, strategy).get("results", [])


RRF_STRATEGIES = ("hybrid", "bm25", "field")  # multi-strategy fusion sources
_RRF_K = 60  # RRF constant; 60 is standard literature default


def _rerank_pool(query: str, pool: list[dict], limit: int) -> tuple[list[dict], list[float] | None]:
    """Cross-encoder rerank a candidate pool. Returns (top_limit, normed_scores_or_None)."""
    reranker = _load_reranker()
    if not reranker or len(pool) <= limit:
        return pool[:limit], None
    pairs = [(query, h.get("text", "")) for h in pool]
    raw = reranker.predict(pairs)
    ranked = sorted(zip(raw, pool), key=lambda x: -float(x[0]))
    top = ranked[:limit]
    return [h for _, h in top], [_sigmoid(float(s)) for s, _ in top]


def _rank_rrf(query: str, limit: int) -> tuple[list[dict], list[float] | None]:
    """Reciprocal Rank Fusion across RRF_STRATEGIES, then cross-encoder rerank.

    Each strategy contributes 1/(k+rank+1) to every hit's RRF score.
    The fused pool is reranked by cross-encoder for final ordering.
    """
    fetch = limit * _RERANK_FETCH_MUL
    all_hits: dict[str, dict] = {}
    rrf_scores: dict[str, float] = {}
    for strategy in RRF_STRATEGIES:
        for rank, hit in enumerate(_recall_raw(query, fetch, strategy)):
            hid = str(hit["id"])
            all_hits.setdefault(hid, hit)
            rrf_scores[hid] = rrf_scores.get(hid, 0.0) + 1.0 / (_RRF_K + rank + 1)
    pool = [all_hits[hid] for hid in sorted(rrf_scores, key=lambda x: -rrf_scores[x])]
    return _rerank_pool(query, pool, limit)


_CHITTA_RPC_PORT = int(
    os.environ.get(
        "CHITTA_EVAL_PORT" if CHITTA_EVAL_SOCKET else "CHITTA_RPC_PORT",
        "7433" if CHITTA_EVAL_SOCKET else "7432",
    )
)


def _chitta_rpc(tool: str, args: dict) -> dict:
    body = json.dumps(
        {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "tools/call",
            "params": {"name": tool, "arguments": args},
        }
    ).encode()
    req = urllib.request.Request(
        f"http://localhost:{_CHITTA_RPC_PORT}",
        data=body,
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=15) as r:
        return json.loads(r.read()).get("result", {})


def _graph_hop(seed_ids: list[str], max_hops: int = 2, max_nodes: int = 30) -> set[str]:
    """BFS via triplet graph from seed IDs. Returns neighbor memory IDs (numeric strings only)."""
    found: set[str] = set()
    for sid in seed_ids[:5]:
        try:
            result = _chitta_rpc(
                "graph_traverse", {"start": str(sid), "max_hops": max_hops, "max_nodes": max_nodes}
            )
            for hit in result.get("structured", {}).get("hits", []):
                node = str(hit.get("node", ""))
                if node.isdigit() or (node.lstrip("-").isdigit()):
                    found.add(node)
        except (
            OSError,
            subprocess.SubprocessError,
            json.JSONDecodeError,
            AttributeError,
            TypeError,
        ):
            # One unreachable seed must not drop the whole traversal; the other
            # seeds still contribute to `found`.
            pass
    return found - set(seed_ids)


def _fetch_memory(mid: str) -> dict | None:
    try:
        out = subprocess.run(
            _chitta_cli("get", "--id", mid, "--json"),
            capture_output=True,
            text=True,
            timeout=10,
        )
        d = json.loads(out.stdout)
        return {
            "id": mid,
            "text": d.get("content", ""),
            "realm": d.get("realm", ""),
            "relevance": 0.0,
            "confidence": d.get("confidence", 0.5),
        }
    except (OSError, subprocess.SubprocessError, json.JSONDecodeError, AttributeError):
        # Memory id no longer resolvable, or the daemon is down. None means
        # "not fetchable" and the caller skips it.
        return None


_G_ENTROPY_HOP_THRESHOLD = 2.995  # ceiling: above → recall is maximally diffuse → trigger hop


def _rank_multihop(query: str, limit: int) -> tuple[list[dict], list[float] | None]:
    """Entropy-gated graph hop: hybrid recall → g_entropy check → triplet-graph expansion → rerank.

    Only triggers when g_entropy > _G_ENTROPY_HOP_THRESHOLD (diffuse, low-confidence recall).
    """
    fetch = limit * _RERANK_FETCH_MUL
    primary = _recall_raw(query, fetch, "hybrid")

    reranker = _load_reranker()
    if reranker and len(primary) > limit:
        pairs = [(query, h.get("text", "")) for h in primary]
        raw_scores = reranker.predict(pairs)
        ranked_primary = sorted(zip(raw_scores, primary), key=lambda x: -float(x[0]))
        primary_top = [h for _, h in ranked_primary[:limit]]
        primary_scores = [_sigmoid(float(s)) for s, _ in ranked_primary[:limit]]
    else:
        primary_top = primary[:limit]
        primary_scores = None

    # Gate: expand only when BOTH uncertain top-score AND diffuse distribution
    # Top-score alone catches underscored golds; g_entropy alone is too compressed (2.87–2.99)
    top_score = max(primary_scores) if primary_scores else 1.0
    ge = g_entropy(primary_scores) if primary_scores else 0.0
    if top_score >= ABSTAIN_SCORE_THRESHOLD or ge <= _G_ENTROPY_HOP_THRESHOLD or not primary_top:
        return primary_top, primary_scores

    all_hits: dict[str, dict] = {str(h["id"]): h for h in primary}
    rrf_scores: dict[str, float] = {}
    for rank, h in enumerate(primary):
        hid = str(h["id"])
        rrf_scores[hid] = rrf_scores.get(hid, 0.0) + 1.0 / (_RRF_K + rank + 1)

    # Pass 1: graph hop — traverse triplet graph, keep neighbors that score > 0.3 vs query
    seed_ids = [str(h["id"]) for h in primary_top[:5]]
    neighbor_ids = _graph_hop(seed_ids, max_hops=2, max_nodes=40)
    if neighbor_ids and reranker:
        neighbor_mems = [
            m
            for mid in list(neighbor_ids)[:20]
            if str(mid) not in all_hits
            and (m := _fetch_memory(mid))
            and (m.get("realm", "") in ("", GRADE_REALM))
        ]
        if neighbor_mems:
            hop_pairs = [(query, m.get("text", "")) for m in neighbor_mems]
            hop_scores = reranker.predict(hop_pairs)
            for score, mem in zip(hop_scores, neighbor_mems):
                if _sigmoid(float(score)) > 0.30:  # only keep plausible neighbors
                    hid = str(mem["id"])
                    all_hits[hid] = mem
                    rrf_scores[hid] = rrf_scores.get(hid, 0.0) + 1.0 / (_RRF_K + 0 + 1)

    merged_pool = [all_hits[hid] for hid in sorted(rrf_scores, key=lambda x: -rrf_scores[x])]
    return _rerank_pool(query, merged_pool, limit)


_FWD_BONUS = 1.0 / (_RRF_K + 1)  # forward-bet neighbor gets one top-rank RRF vote
_FWD_SEEDS = 3  # trajectory anchor = top-N already-admitted items


def _rank_fwd(query: str, limit: int) -> tuple[list[dict], list[float] | None]:
    """Forward-looking admission (J-lens principle): admit an item because the
    current trajectory will NEED it, not only because it matches the query now.

    Unlike multihop's entropy gate (reactive: expand only when recall is diffuse),
    this ALWAYS pulls the triplet-graph neighborhood of the top-admitted seeds and
    gives any pool member in that neighborhood a forward-bet RRF bonus — the
    "look up everything about the entity before you know which fact you need"
    dynamic (Nanda's Michael Jordan example). Endpoint-free: re-weights the same
    fetched pool, no reranker, no new query path. Measurable with no cross-encoder.
    """
    fetch = limit * _RERANK_FETCH_MUL
    primary = _recall_raw(query, fetch, "hybrid")
    if len(primary) <= 1:
        return primary[:limit], None

    # Base RRF score from the primary ranking (rank-based, reranker-independent).
    rrf: dict[str, float] = {}
    pool: dict[str, dict] = {}
    for rank, h in enumerate(primary):
        hid = str(h["id"])
        pool[hid] = h
        rrf[hid] = rrf.get(hid, 0.0) + 1.0 / (_RRF_K + rank + 1)

    # Trajectory prior: neighborhood of the top-admitted seeds. The point of
    # forward-looking admission is to pull in items the query-match MISSED but the
    # trajectory will need (Jordan -> {basketball, Chicago}), so neighbors NOT
    # already in the pool are fetched and injected as forward-bet candidates.
    seeds = [str(h["id"]) for h in primary[:_FWD_SEEDS]]
    neighbors = _graph_hop(seeds, max_hops=2, max_nodes=40)
    for hid in neighbors:
        if hid in pool:
            rrf[hid] = rrf.get(hid, 0.0) + _FWD_BONUS  # already retrieved: boost
        else:
            mem = _fetch_memory(hid)  # missed by query: inject
            if mem and mem.get("text"):
                pool[hid] = mem
                rrf[hid] = _FWD_BONUS

    ordered = [pool[hid] for hid in sorted(rrf, key=lambda x: -rrf[x])]
    return _rerank_pool(query, ordered, limit)


def _rank(query: str, limit: int, strategy: str = "") -> tuple[list[dict], list[float] | None]:
    """Fetch + rerank. strategy='rrf' runs multi-strategy RRF; 'multihop' adds entropy-gated graph hop.
    Returns (hits, sigmoid_normed_scores) or (hits, None) if no reranker."""
    if strategy == "rrf":
        return _rank_rrf(query, limit)
    if strategy == "multihop":
        return _rank_multihop(query, limit)
    if strategy == "fwd":
        return _rank_fwd(query, limit)
    reranker = _load_reranker()
    fetch = limit * _RERANK_FETCH_MUL if reranker else limit
    results = _recall_raw(query, fetch, strategy)
    if not reranker or len(results) <= limit:
        return results[:limit], None
    pairs = [(query, h.get("text", "")) for h in results]
    raw = reranker.predict(pairs)
    ranked = sorted(zip(raw, results), key=lambda x: -float(x[0]))
    top = ranked[:limit]
    return [h for _, h in top], [_sigmoid(float(s)) for s, _ in top]


def recall(query: str, limit: int, strategy: str = "") -> list[dict]:
    hits, _ = _rank(query, limit, strategy)
    return hits


def ndcg(positions: list[int], n_gold: int, k: int) -> float:
    dcg = sum(1.0 / math.log2(p + 2) for p in positions if p < k)
    idcg = sum(1.0 / math.log2(i + 2) for i in range(min(n_gold, k)))
    return dcg / idcg if idcg > 0 else 0.0


def bootstrap(limit: int, strategy: str = "") -> dict[str, list[str]]:
    """Discover gold memory IDs for each golden query. Returns {desc: [id, ...]}."""
    gold_ids: dict[str, list[str]] = {}
    for item in GOLDEN_SET:
        sig = item["gold_signature"].lower()
        results = recall(item["bootstrap_query"], limit, strategy)
        hits = [r for r in results if sig in r.get("text", "").lower()]
        if not hits:
            results2 = recall(item["query"], limit, strategy)
            hits = [r for r in results2 if sig in r.get("text", "").lower()]
        ids = [
            h["id"] for h in hits[:3]
        ]  # top-3 ranked; avoids IDCG inflation from broad signatures
        gold_ids[item["desc"]] = ids
        status = f"{GREEN}found {len(ids)} gold(s){RESET}" if ids else f"{RED}NOT FOUND{RESET}"
        print(f"  {item['desc']:35} {status}")
        if ids:
            for h in hits[:2]:
                print(f"    {DIM}{h['text'][:80]}{RESET}")
    return gold_ids


def grade_one(item: dict, gold_ids: dict, limit: int, strategy: str = "") -> dict:
    ids = gold_ids.get(item["desc"], [])
    sig = item["gold_signature"].lower()
    results, scores = _rank(item["query"], limit, strategy)

    # C2 is defined on the production hybrid lane regardless of graded strategy
    c2_full = _recall_full(item["query"], 10, "hybrid")
    c2_maxrel = c2_full.get("max_relevance", 0.0)
    c2 = c2_tag(c2_maxrel, len(c2_full.get("results", [])))

    # Ignition eval data: per-item Platt + gold positions on the SAME hybrid pool
    # C2 read — baseline and treatment share one recall, same-session by construction.
    hyb = c2_full.get("results", [])
    hyb_platt = [_platt(h.get("similarity", 0.0)) for h in hyb]
    hyb_ids = [h["id"] for h in hyb]
    hyb_gold = sorted(
        {i for i, h in enumerate(hyb) if sig in h.get("text", "").lower()}
        | {hyb_ids.index(g) for g in ids if g in hyb_ids}
    )
    result_ids = [r["id"] for r in results]
    result_texts = [r.get("text", "").lower() for r in results]

    # Sig-based positions: primary stable key (survives re-ingest/NFS resurrection)
    sig_positions = [i for i, t in enumerate(result_texts) if sig in t]

    # ID-based positions: secondary (may be stale after re-ingest)
    id_positions = [result_ids.index(gid) for gid in ids if gid in result_ids]

    # Union: take best coverage; sig is durable, ID is exact
    positions = sorted(set(sig_positions) | set(id_positions))
    sig_fallback = bool(sig_positions) and not id_positions

    n_gold = max(len(ids), len(positions), 1)
    score = ndcg(positions, n_gold, limit)

    # Abstain signals — two distinct triggers, do not collapse
    epistemic_abstain = False  # cross-encoder unconfident (w ≤ threshold)
    band_abstain = False  # gold/non-gold scores too close to trust ranking
    gold_max_score = None
    if scores and positions:
        gold_scores = [scores[p] for p in positions if p < len(scores)]
        if gold_scores:
            gold_max_score = max(gold_scores)
            best_gold_pos = min(positions)
            if gold_max_score < ABSTAIN_SCORE_THRESHOLD:
                epistemic_abstain = True
            # Band abstain: only when a non-gold item is ranked above the best gold
            # and their scores are within ε — forced ranking is noise, not signal.
            if best_gold_pos > 0:
                blocker_score = scores[best_gold_pos - 1]  # item ranked just above best gold
                if blocker_score - gold_max_score < ABSTAIN_BAND_EPS:
                    band_abstain = True

    return {
        "query": item["query"],
        "desc": item["desc"],
        "gold_ids": ids,
        "n_gold": len(ids),
        "n_results": len(results),
        "gold_positions": positions,
        "sig_fallback": sig_fallback,
        "ndcg": score,
        "pass": score >= 0.5,
        "epistemic_abstain": epistemic_abstain,
        "band_abstain": band_abstain,
        "abstain": epistemic_abstain or band_abstain,
        "gold_max_score": gold_max_score,
        "c2": c2,
        "c2_maxrel": c2_maxrel,
        "hyb_platt": hyb_platt,
        "hyb_gold_positions": hyb_gold,
        "g_entropy": g_entropy(scores) if scores else 0.0,
        "s_entropy": s_entropy_from_embeddings(result_ids)
        if result_ids
        else (0.0 if _HAS_NUMPY else None),
    }


def main():
    ap = argparse.ArgumentParser(description="G0 recall grader (nDCG@10)")
    ap.add_argument("--bootstrap", action="store_true", help="discover gold IDs and save")
    ap.add_argument("--limit", type=int, default=20)
    ap.add_argument(
        "--bootstrap-limit", type=int, default=640, help="depth for bootstrap (default 640)"
    )
    ap.add_argument("--min", type=float, default=0.7)
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--no-reranker", action="store_true", help="skip cross-encoder re-ranking")
    ap.add_argument(
        "--strategy",
        default="hybrid",
        help="recall strategy: 'hybrid'|'bm25'|'field' or 'rrf' (multi-strategy RRF+rerank). Default hybrid = what prompt-core.sh production recall uses; measured strict superset of pure-semantic on the golden set (pass -1 abstain +2, 0 regressions).",
    )
    ap.add_argument(
        "--c2",
        action="store_true",
        help="C2 calibration report: bin queries by KNOWN/THIN/UNKNOWN, "
        "verify gold-hit-rate is monotonic; exit 1 if miscalibrated",
    )
    ap.add_argument(
        "--ignition",
        action="store_true",
        help="ignition A/B on one shared hybrid pool per query: baseline "
        "top-3 admit vs per-item bimodal admit (Platt >= C2 KNOWN cut); "
        "reports admitted-are-gold precision + gold coverage; exit 1 on kill",
    )
    a = ap.parse_args()
    if CHITTA_EVAL_SOCKET:
        # A dead replica must abort, not score 0.0 (happened 2026-09-13 when the
        # replica refused to start mid-save and the grader ran anyway).
        probe = subprocess.run(
            _chitta_cli("recall", "--query", "ping", "--limit", "1"),
            capture_output=True,
            text=True,
            timeout=60,
        )
        if probe.returncode != 0 or "Cannot connect" in (probe.stdout + probe.stderr):
            print(
                f"eval replica at {CHITTA_EVAL_SOCKET} is not reachable; aborting", file=sys.stderr
            )
            return 2
    if a.no_reranker:
        global _reranker
        _reranker = False

    if a.bootstrap:
        blimit = a.bootstrap_limit
        print(
            f"{BOLD}Bootstrapping gold memory IDs (limit={blimit}, strategy={a.strategy or 'default'})...{RESET}"
        )
        gold_ids = bootstrap(blimit, a.strategy)
        GOLD_IDS_PATH.write_text(
            json.dumps({"version": GOLDEN_VERSION, "ids": gold_ids}, indent=2) + "\n"
        )
        print(f"\n{GREEN}Saved: {GOLD_IDS_PATH}{RESET}")
        found = sum(1 for v in gold_ids.values() if v)
        print(f"{found}/{len(GOLDEN_SET)} queries have gold IDs.")
        return

    # Load gold IDs
    if not GOLD_IDS_PATH.exists():
        print(f"{RED}No gold IDs file. Run: grade-recall.py --bootstrap{RESET}", file=sys.stderr)
        sys.exit(2)

    stored = json.loads(GOLD_IDS_PATH.read_text())
    gold_ids = stored.get("ids", {})

    records = [grade_one(it, gold_ids, a.limit, a.strategy) for it in GOLDEN_SET]

    if a.c2:
        # Calibration = honesty check: the tag must PREDICT gold presence.
        # 2-band (v5.70.1): KNOWN (confident-but-verify, maxrel >= .81) vs UNKNOWN.
        bins = {"KNOWN": [], "UNKNOWN": []}
        for r in records:
            bins[r["c2"]].append(r)
        print(f"{BOLD}C2 calibration 2-band (KNOWN>={C2_KNOWN_MAXREL} on max_relevance){RESET}")
        rates = []
        for tag in ("KNOWN", "UNKNOWN"):
            b = bins[tag]
            h3 = sum(1 for r in b if any(p < 3 for p in r["gold_positions"]))
            h10 = sum(1 for r in b if any(p < 10 for p in r["gold_positions"]))
            rate = h3 / len(b) if b else None
            rates.append(rate)
            n = len(b)
            print(
                f"  {tag:8} n={n:2d}  hit@3={h3}/{n} ({(h3 / n if n else 0):.2f})  hit@10={h10}/{n}"
            )
            for r in b:
                mark = (
                    GREEN + "+" + RESET
                    if any(p < 3 for p in r["gold_positions"])
                    else RED + "-" + RESET
                )
                print(f"    {mark} maxrel={r['c2_maxrel']:.3f} {r['desc'][:40]}")
        probe_tags = []
        for q in C2_PROBES:
            full = _recall_full(q, 10, "hybrid")
            tag = c2_tag(full.get("max_relevance", 0.0), len(full.get("results", [])))
            probe_tags.append(tag)
            color = GREEN if tag == "UNKNOWN" else RED
            print(
                f"  probe {color}{tag:8}{RESET} maxrel={full.get('max_relevance', 0.0):.3f} {q[:44]!r}"
            )
        present = [r for r in rates if r is not None]
        monotonic = all(a_ >= b_ for a_, b_ in zip(present, present[1:]))
        probes_ok = all(t != "KNOWN" for t in probe_tags)
        ok = monotonic and probes_ok
        verdict = GREEN + "CALIBRATED" + RESET if ok else RED + "MISCALIBRATED" + RESET
        print(f"\n{BOLD}{verdict}{RESET} monotonic={monotonic} probes_not_known={probes_ok}")
        sys.exit(0 if ok else 1)

    if a.ignition:
        # A/B on the shared per-query hybrid pool: baseline = prod fixed top-3 admit,
        # treatment = ignition_admit. Same recall, same ranking — nDCG is identical
        # by construction (ignition is admit-only); the treatment metric is precision.
        b_adm = b_gold = t_adm = t_gold = t_gap = 0
        b_hits = t_hits = 0
        no_ignite = []
        for r in records:
            gold = set(r["hyb_gold_positions"])
            base = list(range(min(3, len(r["hyb_platt"]))))
            adm, gap = ignition_admit(r["hyb_platt"])
            b_adm += len(base)
            b_gold += len(gold & set(base))
            t_adm += len(adm)
            t_gold += len(gold & set(adm))
            t_gap += len(gap)
            b_hits += bool(gold & set(base))
            t_hits += bool(gold & set(adm))
            if not adm:
                no_ignite.append(r["desc"])
            if not a.quiet:
                print(
                    f"  base {len(gold & set(base))}/{len(base)}  ign {len(gold & set(adm))}/{len(adm)}"
                    f" gap:{len(gap)}  maxrel={r['c2_maxrel']:.3f}  {r['desc'][:44]}"
                )
        n = len(records)
        pool_ndcg = (
            sum(
                ndcg(
                    r["hyb_gold_positions"],
                    max(len(r["gold_ids"]), len(r["hyb_gold_positions"]), 1),
                    10,
                )
                for r in records
            )
            / n
            if n
            else 0.0
        )
        bp = b_gold / b_adm if b_adm else 0.0
        tp = t_gold / t_adm if t_adm else 0.0
        print(f"\n{BOLD}Ignition A/B (n={n}, shared hybrid pool, no-rerank){RESET}")
        print(f"  nDCG@10 (identical both arms): {pool_ndcg:.4f}")
        print(
            f"  baseline top-3 : precision {b_gold}/{b_adm} ({bp:.3f})  coverage {b_hits}/{n} ({b_hits / n:.3f})"
        )
        print(
            f"  ignition       : precision {t_gold}/{t_adm} ({tp:.3f})  coverage {t_hits}/{n} ({t_hits / n:.3f})"
            f"  dead-band items {t_gap}"
        )
        if no_ignite:
            print(
                f"  no-ignition queries ({len(no_ignite)}): " + "; ".join(d[:32] for d in no_ignite)
            )
        ok = tp > bp and t_hits >= b_hits
        verdict = GREEN + "PASS" + RESET if ok else RED + "KILL" + RESET
        print(f"\n{BOLD}{verdict}{RESET} precision_up={tp > bp} coverage_kept={t_hits >= b_hits}")
        sys.exit(0 if ok else 1)

    mean_ndcg = sum(r["ndcg"] for r in records) / len(records) if records else 0.0
    active = [r for r in records if not r["abstain"]]
    mean_ndcg_active = sum(r["ndcg"] for r in active) / len(active) if active else 0.0
    n_pass = sum(1 for r in records if r["pass"] and not r["abstain"])
    n_fail = sum(1 for r in records if not r["pass"] and not r["abstain"])
    n_abstain = sum(1 for r in records if r["abstain"])
    ts = datetime.now(timezone.utc).isoformat()

    g_vals = [r["g_entropy"] for r in records if r.get("g_entropy") is not None]
    mean_g_entropy = sum(g_vals) / len(g_vals) if g_vals else 0.0
    if len(g_vals) > 1:
        g_var = sum((g - mean_g_entropy) ** 2 for g in g_vals) / (len(g_vals) - 1)
        abstain_g_entropy_threshold = mean_g_entropy + 1.5 * math.sqrt(g_var)
    else:
        abstain_g_entropy_threshold = None
    s_vals = [r["s_entropy"] for r in records if r.get("s_entropy") is not None]
    mean_s_entropy = sum(s_vals) / len(s_vals) if s_vals else None

    # Canonical baseline config (frozen 2026-07-07): v6 golden / reranker on /
    # limit=10. The cross-encoder stalls at depth-20 (>9min), so limit=20 is NOT
    # a routine baseline. Never mix limits when comparing runs.
    canonical = not a.no_reranker and a.limit == 10
    if not canonical and not a.quiet:
        print(
            f"WARNING: non-canonical config (baseline = v{GOLDEN_VERSION}/reranker/limit=10); "
            f"this run: {'no-reranker' if a.no_reranker else 'reranker'}/limit={a.limit}"
        )

    report = {
        "ts": ts,
        "replica_snapshot_id": _replica_snapshot_id(),
        "replica_socket": CHITTA_EVAL_SOCKET,
        "canonical_baseline": "v6/reranker/limit=10 (frozen 2026-07-07)",
        "is_canonical_run": canonical,
        "version": GOLDEN_VERSION,
        "limit": a.limit,
        "reranker": not a.no_reranker,
        "metric": "mean_nDCG",
        "score": mean_ndcg,
        "score_active": mean_ndcg_active,
        "n": len(records),
        "n_pass": n_pass,
        "n_fail": n_fail,
        "n_abstain": n_abstain,
        "mean_g_entropy": mean_g_entropy,
        "abstain_g_entropy_threshold": abstain_g_entropy_threshold,
        "mean_s_entropy": mean_s_entropy,
        "records": records,
    }
    RESULTS_PATH.write_text(json.dumps(report, indent=2) + "\n")

    # Store [gap] memories for abstain cases — seeds future dream curiosity
    for r in records:
        if r.get("abstain"):
            reason = "epistemic" if r.get("epistemic_abstain") else "band"
            gap_content = (
                f"[gap] query={r['query']!r} reason={reason} "
                f"nDCG={r['ndcg']:.3f} gold_score={r.get('gold_max_score') or 'n/a'}"
            )
            subprocess.run(
                _chitta_cli(
                    "remember",
                    "--content",
                    gap_content,
                    "--realm",
                    "project:cc-soul",
                    "--type",
                    "wisdom",
                    "--tags",
                    "gap,abstain,grader",
                    "--visibility",
                    "1",
                ),
                capture_output=True,
                timeout=10,
            )

    if not a.quiet:
        for r in records:
            if r["abstain"]:
                reason = "epistemic" if r["epistemic_abstain"] else "band"
                score_str = (
                    f"w={r['gold_max_score']:.2f}" if r["gold_max_score"] is not None else "w=?"
                )
                pos_str = str(r["gold_positions"]) if r["gold_positions"] else "not found"
                print(
                    f"{YELLOW}[ABSTAIN/{reason}]{RESET} {r['query']!r:45} nDCG={r['ndcg']:.3f}  pos={pos_str}  {score_str}"
                )
            else:
                color = GREEN if r["pass"] else RED
                mark = "PASS" if r["pass"] else "FAIL"
                pos_str = str(r["gold_positions"]) if r["gold_positions"] else "not found"
                no_gold = f"{YELLOW}(no gold IDs){RESET}" if not r["n_gold"] else ""
                sig_tag = f"{DIM}[sig]{RESET}" if r.get("sig_fallback") else ""
                score_tag = (
                    f"  w={r['gold_max_score']:.2f}" if r["gold_max_score"] is not None else ""
                )
                print(
                    f"{color}[{mark}]{RESET} {r['query']!r:45} nDCG={r['ndcg']:.3f}  pos={pos_str}{score_tag} {no_gold}{sig_tag}"
                )
        verdict = GREEN if mean_ndcg >= a.min else RED
        print(f"\n{BOLD}{verdict}mean nDCG@{a.limit} = {mean_ndcg:.3f}{RESET}  (min={a.min})")
        if n_abstain:
            verdict2 = GREEN if mean_ndcg_active >= a.min else RED
            print(
                f"  active (excl. {n_abstain} abstain): {BOLD}{verdict2}{mean_ndcg_active:.3f}{RESET}  "
                f"pass={n_pass} fail={n_fail} abstain={n_abstain}"
            )
        print(f"report: {RESULTS_PATH}")

    print(f"SCORE={mean_ndcg:.4f}")
    sys.exit(0 if mean_ndcg >= a.min else 1)


if __name__ == "__main__":
    sys.exit(main())
