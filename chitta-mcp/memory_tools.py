"""Typed memory, learning, and memory editing composites.

Implementations receive the server runtime explicitly as ``ctx`` so the public
server facade owns shared state and dependency overrides across all entry points.
"""

from __future__ import annotations

import datetime
import json
import re


def handle_verify_correction(ctx, arguments: dict) -> str:
    """
    Mark a correction memory as verified at a given evidence locus.
    Stores a new [correction:<id>] verified_at:<locus> memory and
    updates the original correction to append 'verified' in its tags.
    """
    mem_id = str(arguments.get("id", ""))
    evidence_locus = arguments.get("evidence_locus", "")

    if not mem_id:
        return "Error: 'id' parameter required"
    if not evidence_locus:
        return "Error: 'evidence_locus' parameter required"

    # Store verification record
    verify_result = ctx.daemon_call(
        "remember",
        {
            "content": f"[correction:{mem_id}] verified_at:{evidence_locus} by:agent",
            "tags": ["correction", "verified"],
            "type": "correction",
            "visibility": 2,
        },
    )

    # Update original memory tags to include 'verified'
    ctx.daemon_call(
        "tag", {"id": int(mem_id) if mem_id.isdigit() else mem_id, "tags": ["verified"]}
    )

    return f"Correction {mem_id} verified at {evidence_locus}\n{verify_result}"


def handle_ack_memory(ctx, arguments: dict) -> str:
    """
    Increment ack signal for a memory.
    Stores a [ack] memory:<id> score:+1 entry with tag ack-signal.
    """
    mem_id = str(arguments.get("id", ""))
    if not mem_id:
        return "Error: 'id' parameter required"

    # Route to the daemon's ack_memory RPC so the ack actually updates ack_scores
    # (the recall scoring input). Previously this stored an inert [ack] memory and
    # ack_score stayed permanently 0.
    ctx.daemon_call("ack_memory", {"id": mem_id})

    return json.dumps({"acked": mem_id})


def handle_nack_memory(ctx, arguments: dict) -> str:
    """
    Decrement nack signal for a memory.
    Stores a [nack] memory:<id> score:-1 entry with tag nack-signal.
    """
    mem_id = str(arguments.get("id", ""))
    if not mem_id:
        return "Error: 'id' parameter required"

    # Route to the daemon's nack_memory RPC so the nack actually updates ack_scores.
    ctx.daemon_call("nack_memory", {"id": mem_id})

    return json.dumps({"nacked": mem_id})


def handle_memory_outcome(ctx, arguments: dict) -> str:
    """
    Record an outcome observation for a memory's utility posterior
    (alpha += weight on success, beta += weight on failure).
    """
    mem_id = str(arguments.get("id", ""))
    if not mem_id:
        return "Error: 'id' parameter required"
    success = arguments.get("success")
    if not isinstance(success, bool):
        return "Error: 'success' (boolean) parameter required"
    params = {"id": mem_id, "success": success}
    weight = arguments.get("weight")
    if weight is not None:
        params["weight"] = float(weight)
    return ctx.daemon_call("memory_outcome", params)


def handle_remember_typed(ctx, arguments: dict) -> str:
    """
    Store a typed memory node with optional graph links.

    node_type: digest-node | symbol-summary | decision | open-question | rollup | working-brief
    links: list of {predicate, target_id} using supersedes | invalidated-by | anchors-to
    """
    node_type = arguments.get("node_type", "")
    content = arguments.get("content", "")
    subject = arguments.get("subject", "")
    links = arguments.get("links", [])
    realm = arguments.get("realm", "brahman")

    if not node_type:
        return "Error: 'node_type' parameter required"
    if node_type not in ctx._TYPED_NODE_TYPES:
        return f"Error: node_type must be one of {sorted(ctx._TYPED_NODE_TYPES)}"
    if not content:
        return "Error: 'content' parameter required"
    if not subject:
        return "Error: 'subject' parameter required"

    # Store primary memory with typed tags
    store_args: dict = {
        "content": content,
        "tags": [f"typed:{node_type}", node_type],
        "type": "wisdom",
        "visibility": 2,
        "realm": realm,
    }

    remember_result = ctx.daemon_call("remember", store_args)

    # Extract new ID from result (format varies; try to parse integer)
    new_id = None
    id_match = re.search(r"\b(\d+)\b", remember_result)
    if id_match:
        new_id = id_match.group(1)
    if not new_id:
        new_id = f"{subject[:20].replace(' ', '_').lower()}:{node_type}"

    # Write link triplet memories
    links_written = 0
    if isinstance(links, list):
        for link in links:
            predicate = link.get("predicate", "")
            target_id = str(link.get("target_id", ""))
            if not predicate or not target_id:
                continue
            if predicate not in ctx._LINK_PREDICATES:
                continue
            ctx.daemon_call(
                "remember",
                {
                    "content": f"[link] {new_id} {predicate} {target_id}",
                    "tags": ["link", predicate],
                    "type": "episode",
                    "visibility": 2,
                    "realm": realm,
                },
            )
            links_written += 1

    return json.dumps({"id": new_id, "node_type": node_type, "links_written": links_written})


def _slug(ctx, text: str, width: int = 40) -> str:
    """Triplet-node slug: lowercase, underscore-joined, width-capped."""
    return text[:width].replace(" ", "_").lower()


def _store_learning(
    ctx,
    content: str,
    tags: list[str],
    mem_type: str,
    visibility: int,
    subject: str,
    predicate: str,
    obj: str,
) -> str:
    """Store one learning memory and link it into the triplet graph.

    Every handle_learn_* variant differs only in how it renders `content` and
    which triplet edge it draws; this is the write half they share. Returns the
    daemon's remember() response so callers that surface it can, and the rest
    can ignore it.
    """
    remembered = ctx.daemon_call(
        "remember",
        {
            "content": content,
            "tags": tags,
            "type": mem_type,
            "visibility": visibility,
        },
    )
    ctx.daemon_call("connect", {"subject": subject, "predicate": predicate, "object": obj})
    return remembered


def handle_learn_correction(ctx, arguments: dict) -> str:
    """Store a correction, flagging it when similar corrections already exist."""
    wrong = arguments.get("wrong", "")
    correct = arguments.get("correct", "")
    context = arguments.get("context", "")

    if not wrong or not correct:
        return "Error: both 'wrong' and 'correct' parameters required"

    # Repeat-mistake check. Advisory only: daemon_call returns an error string
    # rather than raising, so a failed recall simply yields no matches and no
    # warning — it must never block the correction write.
    existing = ctx.daemon_call("recall", {"query": wrong[:100], "tag": "correction", "limit": 3})
    high_matches = [int(m) for m in re.findall(r"\[(\d+)%\]", str(existing)) if int(m) > 70]
    repeat_warning = ""
    if high_matches:
        repeat_warning = (
            f"\n⚠️ REPEAT MISTAKE DETECTED ({len(high_matches)} similar corrections "
            f"exist, highest {max(high_matches)}% match)"
        )

    # SSL form — action first, so a truncated render still shows the solution.
    content = f"[correction] USE: {correct}\nNOT: {wrong}"
    if context:
        content += f"\n@{context.replace(' ', '-').lower()}"

    tags = ["correction", "high-priority"]
    if repeat_warning:
        content += "\n[REPEAT-MISTAKE]"
        tags.append("repeat-mistake")

    wrong_slug = ctx._slug(wrong, 50)
    correct_slug = ctx._slug(correct, 50)
    # visibility 2 (global): corrections apply everywhere, not just this realm.
    ctx._store_learning(
        content,
        tags,
        "correction",
        2,
        subject=correct_slug,
        predicate="corrects",
        obj=wrong_slug,
    )
    return (
        f"Correction stored:\n  USE: {correct}\n  NOT: {wrong}\n"
        f"  Triplet: {correct_slug} → corrects → {wrong_slug}{repeat_warning}"
    )


def handle_learn_preference(ctx, arguments: dict) -> str:
    """Store a user preference. Global visibility — preferences apply everywhere."""
    category = arguments.get("category", "general")
    preference = arguments.get("preference", "")
    example = arguments.get("example", "")

    if not preference:
        return "Error: 'preference' parameter required"

    content = f"[preference:{category}] {preference}"
    if example:
        content += f"\nExample: {example}"

    ctx._store_learning(
        content,
        ["preference", category],
        "preference",
        2,
        subject="user",
        predicate=f"prefers_{category}",
        obj=ctx._slug(preference, 50),
    )
    return f"Preference stored:\n  Category: {category}\n  Preference: {preference}"


def handle_learn_insight(ctx, arguments: dict) -> str:
    """Store a generalizable insight. Always global — these are cross-project."""
    domain = arguments.get("domain", "general")
    insight = arguments.get("insight", "")
    learned_from = arguments.get("learned_from", "")

    if not insight:
        return "Error: 'insight' parameter required"

    content = f"[insight:{domain}] {insight}"
    if learned_from:
        content += f"\nLearned from: {learned_from}"

    ctx._store_learning(
        content,
        ["insight", domain, "cross-project"],
        "wisdom",
        2,
        subject=domain,
        predicate="has_insight",
        obj=ctx._slug(insight),
    )
    return f"Insight stored (global):\n  Domain: {domain}\n  Insight: {insight}"


def handle_learn_approach(ctx, arguments: dict) -> str:
    """Store what approach worked in a given state, building emotional memory."""
    state = arguments.get("state", "general")
    approach = arguments.get("approach", "")
    outcome = arguments.get("outcome", "")

    if not approach:
        return "Error: 'approach' parameter required"

    content = f"[approach:{state}] When {state}: {approach}"
    if outcome:
        content += f"\nOutcome: {outcome}"

    ctx._store_learning(
        content,
        ["approach", state, "emotional-memory"],
        "wisdom",
        2,
        subject=state,
        predicate="helped_by",
        obj=ctx._slug(approach),
    )
    return f"Approach stored:\n  State: {state}\n  Approach: {approach}"


def handle_learn_outcome(ctx, arguments: dict) -> str:
    """Record whether a suggestion actually helped, closing the feedback loop."""
    suggestion = arguments.get("suggestion", "")
    helped = arguments.get("helped", False)
    details = arguments.get("details", "")

    if not suggestion:
        return "Error: 'suggestion' parameter required"

    outcome_type = "worked" if helped else "failed"
    content = f"[outcome:{outcome_type}] {suggestion}"
    if details:
        content += f"\nWhy: {details}"

    tags = ["outcome", outcome_type, "success" if helped else "failure"]
    ctx._store_learning(
        content,
        tags,
        "episode",
        2,
        subject=ctx._slug(suggestion),
        predicate="resulted_in",
        obj=outcome_type,
    )
    return f"Outcome recorded:\n  Suggestion: {suggestion}\n  Helped: {helped}"


def handle_learn_milestone(ctx, arguments: dict) -> str:
    """Record a relationship milestone. Global — milestones matter everywhere."""
    milestone = arguments.get("milestone", "")
    significance = arguments.get("significance", "")
    date = arguments.get("date", "") or datetime.date.today().isoformat()

    if not milestone:
        return "Error: 'milestone' parameter required"

    content = f"[milestone] {date}: {milestone}"
    if significance:
        content += f"\nSignificance: {significance}"

    ctx._store_learning(
        content,
        ["milestone", "relationship", "achievement"],
        "episode",
        2,
        subject="partnership",
        predicate="achieved",
        obj=ctx._slug(milestone),
    )
    return f"Milestone recorded:\n  Date: {date}\n  Milestone: {milestone}"


def handle_learn_analysis(ctx, arguments: dict) -> str:
    """Record an analysis with its data and script locations, so it can be rerun."""
    name = arguments.get("name", "")
    description = arguments.get("description", "")
    data_paths = arguments.get("data_paths", [])
    script_paths = arguments.get("script_paths", [])
    findings = arguments.get("findings", "")
    project = arguments.get("project", "")

    if not name:
        return "Error: 'name' parameter required"

    if not isinstance(data_paths, list):
        data_paths = [data_paths]
    if not isinstance(script_paths, list):
        script_paths = [script_paths]

    content = f"[analysis:{project or 'general'}] {name}"
    if description:
        content += f"\nDescription: {description}"
    if data_paths:
        content += f"\nData: {', '.join(data_paths)}"
    if script_paths:
        content += f"\nScripts: {', '.join(script_paths)}"
    if findings:
        content += f"\nFindings: {findings}"

    analysis_slug = ctx._slug(name)
    # visibility 0: an analysis is project-bound, so it stays private to its realm.
    ctx._store_learning(
        content,
        ["analysis", project or "general", "reproducibility"],
        "episode",
        0,
        subject=project or "general",
        predicate="has_analysis",
        obj=analysis_slug,
    )
    for path in data_paths:
        if path:
            ctx.daemon_call(
                "connect",
                {
                    "subject": analysis_slug,
                    "predicate": "uses_data",
                    "object": path[:60],
                },
            )

    # Render the joined form, not the list repr: a caller may pass either a
    # list or a single comma-separated string, and the confirmation should look
    # the same either way.
    return (
        f"Analysis recorded:\n  Name: {name}\n"
        f"  Data: {', '.join(data_paths)}\n  Scripts: {', '.join(script_paths)}"
    )


def handle_learn_gateway(ctx, arguments: dict) -> str:
    """Unified learning gateway.

    Types: correction, preference, insight, approach, outcome, milestone, analysis
    """
    learn_type = arguments.pop("type", "")
    if not learn_type:
        return "Error: 'type' parameter required (correction/preference/insight/approach/outcome/milestone/analysis)"
    handler_map = {
        "correction": ctx.handle_learn_correction,
        "preference": ctx.handle_learn_preference,
        "insight": ctx.handle_learn_insight,
        "approach": ctx.handle_learn_approach,
        "outcome": ctx.handle_learn_outcome,
        "milestone": ctx.handle_learn_milestone,
        "analysis": ctx.handle_learn_analysis,
    }
    handler = handler_map.get(learn_type)
    if not handler:
        return f"Unknown learn type: {learn_type}. Use: {', '.join(handler_map.keys())}"
    return handler(arguments)


def handle_triplets_gateway(ctx, arguments: dict) -> str:
    """Unified triplet operations.

    Actions: connect (default), query, history, query_as_of, supersede, traverse, pagerank
    """
    action = arguments.pop("action", "connect")
    tool_map = {
        "connect": "connect_temporal",
        "query": "query_triplets_temporal",
        "history": "triplet_history",
        "query_as_of": "triplet_query_as_of",
        "supersede": "triplet_supersede",
        "traverse": "graph_traverse",
        "pagerank": "graph_pagerank",
    }
    tool = tool_map.get(action)
    if not tool:
        return f"Unknown action: {action}. Use: {', '.join(tool_map.keys())}"
    return ctx.daemon_call(tool, arguments)


def handle_memory_edit_gateway(ctx, arguments: dict) -> str:
    """Unified memory editing.

    Actions: set_type, set_priority
    """
    action = arguments.pop("action", "")
    if not action:
        return "Error: 'action' required (set_type/set_priority)"
    tool_map = {
        "set_type": "set_memory_type",
        "set_priority": "set_priority_tier",
    }
    tool = tool_map.get(action)
    if not tool:
        return f"Unknown action: {action}. Use: {', '.join(tool_map.keys())}"
    # The composite tool exposes the concise public key `id`, while both
    # daemon editing handlers use `memory_id`. Normalize at the gateway.
    if "id" in arguments and "memory_id" not in arguments:
        arguments["memory_id"] = arguments.pop("id")
    return ctx.daemon_call(tool, arguments)
