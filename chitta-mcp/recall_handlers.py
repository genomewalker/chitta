"""Recall strategy routing, lane merging, and reranking.

Implementations receive the server runtime explicitly as ``ctx`` so the public
server facade owns shared state and dependency overrides across all entry points.
"""

from __future__ import annotations

import asyncio
import json
import math


def handle_recall_smart(ctx, arguments: dict) -> str:
    """Multi-lane retrieval: query planner → semantic + typed + spreading + session lanes → RRF merge."""
    query = arguments.get("query", "")
    limit = int(arguments.get("limit", 10))
    realm = arguments.get("realm", "")
    skip_llm = arguments.get("skip_llm_plan", False)

    plan = {"entities": [], "speech_act": None, "answer_type": "fact"}

    if not skip_llm:
        try:
            import anthropic

            anthropic_client = anthropic.Anthropic()
            resp = anthropic_client.messages.create(
                model="claude-haiku-4-5-20251001",
                max_tokens=200,
                messages=[
                    {
                        "role": "user",
                        "content": (
                            f"Extract retrieval metadata from this query. Return JSON only, no prose.\n"
                            f"Fields: entities (array of key noun phrases), speech_act "
                            f"(one of: decision/correction/preference/task/result/failure/question/hypothesis/null), "
                            f"answer_type (one of: fact/decision/preference/code/temporal).\n"
                            f"Query: {query}"
                        ),
                    }
                ],
            )
            plan = json.loads(resp.content[0].text.strip())
        except Exception as exc:  # noqa: BLE001
            # The planner is a best-effort accelerator over an optional
            # dependency (`anthropic`), a network call, and unconstrained model
            # output — missing package, auth failure, timeout, and non-JSON
            # replies all mean the same thing here. Recall must still run, so
            # fall through to the empty plan: lanes 2 and 3 simply stay off.
            ctx.logger.info("recall_smart query planner unavailable: %s", exc)

    realm_arg = {"realm": realm} if realm else {}

    # Lane 1: semantic recall (always)
    sem_raw = ctx.daemon_call("recall", {"query": query, "limit": limit * 2, **realm_arg})
    try:
        sem_hits = json.loads(sem_raw).get("results", [])
    except (ValueError, AttributeError) as exc:
        # daemon_call returns plain text on error rather than raising.
        ctx.logger.warning("recall_smart semantic lane returned no JSON: %s", exc)
        sem_hits = []

    lanes = [sem_hits]

    # Lane 2: typed recall (if speech_act detected)
    if plan.get("speech_act"):
        typed_raw = ctx.daemon_call(
            "recall", {"query": query, "tag": plan["speech_act"], "limit": limit, **realm_arg}
        )
        try:
            lanes.append(json.loads(typed_raw).get("results", []))
        except (ValueError, AttributeError) as exc:
            # One dead lane must not sink the merge; RRF just fuses fewer lanes.
            ctx.logger.debug("recall_smart typed lane returned no JSON: %s", exc)

    # Lane 3: spreading activation (if entities found)
    entities = plan.get("entities", [])
    if entities:
        seed_query = " ".join(entities[:5])
        spread_raw = ctx.daemon_call(
            "recall_spreading", {"query": seed_query, "limit": limit, **realm_arg}
        )
        try:
            lanes.append(json.loads(spread_raw).get("results", []))
        except (ValueError, AttributeError) as exc:
            ctx.logger.debug("recall_smart spreading lane returned no JSON: %s", exc)

    # Lane 4: session-level recall
    sess_raw = ctx.daemon_call("recall_session", {"query": query, "limit": limit, **realm_arg})
    try:
        sess_data = json.loads(sess_raw).get("results", [])
        # Normalize session hits to same shape as memory hits
        for s in sess_data:
            s.setdefault("memory_id", s.get("session_id", ""))
            s.setdefault("text", s.get("best_evidence", ""))
        lanes.append(sess_data)
    except (ValueError, AttributeError, TypeError) as exc:
        ctx.logger.debug("recall_smart session lane returned no JSON: %s", exc)

    merged = ctx.rrf_merge(lanes, k=60, limit=limit)
    return json.dumps({"results": merged, "plan": plan})


def _kind_envelope(ctx, hit: dict) -> float:
    return 0.7 + 0.3 * ctx._KIND_MULTIPLIER.get(str(hit.get("type", "")), 1.0)


async def handle_recall_gateway(ctx, arguments: dict) -> str:
    """Unified recall with strategy routing and optional cross-encoder reranking.

    Strategies: hybrid (default), semantic, priority, temporal, smart, keyword.
    Every value maps to a tool the daemon actually serves; an unknown strategy
    falls back to plain semantic recall.

    Default is hybrid: measured strict superset of pure-semantic on the golden
    set (nDCG@20 +0.08 active, +2 pass, 0 regressions) — hybrid's BM25 lane
    catches literal tokens (filenames, IDs, paths) that pure-semantic misses at
    low cosine similarity. Callers wanting the old behavior pass strategy="semantic".
    """
    strategy = arguments.pop("strategy", "hybrid")
    # "field" used to map to a `recall_field` RPC the daemon does not implement.
    # Unknown tool names come back as an empty content array, so that strategy
    # silently returned no memories rather than erroring; dropped so it falls
    # through to semantic recall like any other unrecognized strategy.
    tool_map = {
        "semantic": "recall",
        "priority": "recall_by_priority",
        "temporal": "recall_temporal",
        "hybrid": "hybrid_recall",
        "smart": "smart_recall",
        "keyword": "recall_keyword",
        # daemon-native lane names: `recall` defaults to fused; make the
        # published strategy values resolve explicitly instead of by fallthrough.
        "fused": "recall",
    }
    tool = tool_map.get(strategy, "recall")

    loop = asyncio.get_running_loop()
    reranker = await ctx.run_reranker(ctx.get_reranker)
    if not reranker:
        return await loop.run_in_executor(ctx._executor, ctx.daemon_call, tool, arguments)

    query = arguments.get("query", "")
    limit = int(arguments.get("limit", 10))
    fetch_args = dict(arguments, limit=limit * ctx.RERANK_FETCH_MUL)
    raw_str = await loop.run_in_executor(ctx._executor, ctx.daemon_call, tool, fetch_args, True)
    try:
        raw = json.loads(raw_str)
        results = raw.get("results", [])
    except (ValueError, AttributeError) as exc:
        # No structured payload to rerank; the unreranked call below is the
        # fallback, so this is a downgrade rather than a failure.
        ctx.logger.debug("recall overfetch returned no JSON, skipping rerank: %s", exc)
        results = []
    if not results or len(results) <= limit:
        return await loop.run_in_executor(ctx._executor, ctx.daemon_call, tool, arguments)

    pairs = [(query, h.get("text", "")) for h in results]
    scores = await ctx.run_reranker(reranker.predict, pairs)
    if len(scores) != len(results):
        # A backend returning a different number of scores than candidates is
        # broken. zip would silently truncate and drop memories from recall, so
        # fall back to the daemon's own ranking instead. Checked explicitly
        # rather than with zip(strict=True), which is 3.10+.
        ctx.logger.warning(
            "reranker returned %d scores for %d candidates; skipping rerank",
            len(scores),
            len(results),
        )
        return await loop.run_in_executor(ctx._executor, ctx.daemon_call, tool, arguments)
    # The cross-encoder scores text similarity only; keep the daemon's kind
    # prior (chitta-field scoring/config.rs) so a correction still outranks a
    # verbatim transcript fragment of equal similarity. Logits pass through a
    # sigmoid so the prior is a bounded envelope, as in the daemon.
    ranked = sorted(
        zip(scores, results),
        key=lambda x: -(1.0 / (1.0 + math.exp(-float(x[0])))) * ctx._kind_envelope(x[1]),
    )
    reranked = [h for _, h in ranked[:limit]]
    return json.dumps({"results": reranked})
