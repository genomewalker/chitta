"""Research, sadhana, REPL, and hint enrichment helpers.

Implementations receive the server runtime explicitly as ``ctx`` so the public
server facade owns shared state and dependency overrides across all entry points.
"""

from __future__ import annotations

import json
import os
import subprocess


def handle_research_topics(ctx, arguments: dict) -> str:
    """Get topics that need research from various sources."""
    source = arguments.get("source", "gaps")
    limit = arguments.get("limit", 3)
    realm = arguments.get("realm", "")

    topics = []

    if source == "gaps" or source == "all":
        # Get unresolved curiosity gaps
        gaps_result = ctx.daemon_call("curiosity_gaps", {"limit": limit, "realm": realm})
        if gaps_result and "No gaps" not in gaps_result:
            # Parse gaps from response
            for line in gaps_result.split("\n"):
                if line.startswith("#") and "[" in line:
                    # Extract gap ID and content
                    parts = line.split("]", 1)
                    if len(parts) > 1:
                        gap_id = parts[0].split("#")[-1].strip()
                        content = parts[1].strip()
                        topics.append(
                            {
                                "type": "gap",
                                "id": gap_id,
                                "topic": content[:200],
                                "source": "curiosity_gaps",
                            }
                        )

    if source == "weak" or source == "all":
        # Get low-confidence memories that might need verification
        weak_result = ctx.daemon_call(
            "recall", {"query": "uncertain unclear unverified", "limit": limit}
        )
        if weak_result and "No memories" not in weak_result:
            for line in weak_result.split("\n"):
                if line.startswith("[") and "%" in line:
                    # Extract confidence and content
                    conf_match = line.split("%")[0].strip("[")
                    if conf_match.isdigit() and int(conf_match) < 30:
                        content = line.split("]", 2)[-1].strip()
                        topics.append(
                            {
                                "type": "weak_memory",
                                "confidence": int(conf_match),
                                "topic": content[:200],
                                "source": "low_confidence",
                            }
                        )

    if not topics:
        return "No research topics found. Consider:\n- Adding curiosity gaps with curiosity_note_gap\n- Or specify source='suggest' for AI-suggested topics"

    output = f"Research Topics ({len(topics)} found):\n"
    output += "=" * 40 + "\n\n"
    for i, t in enumerate(topics[:limit], 1):
        output += f"{i}. [{t['type']}] {t['topic']}\n"
        if t.get("id"):
            output += f"   Gap ID: {t['id']} (use with research_store to resolve)\n"
        output += "\n"

    output += (
        "\nNext: Use WebSearch to research these topics, then call research_store with findings."
    )
    return output


def handle_research_store(ctx, arguments: dict) -> str:
    """Store research results as memories with source attribution."""
    topic = arguments.get("topic", "")
    findings = arguments.get("findings", "")
    sources = arguments.get("sources", [])
    gap_id = arguments.get("gap_id")
    confidence = arguments.get("confidence", 0.7)

    if not topic or not findings:
        return "Error: topic and findings are required"

    # Format sources
    source_str = ""
    if sources:
        source_str = "\nSources: " + " | ".join(sources[:3])

    # Create memory content in SSL format
    content = f"[research] {topic}\n{findings}{source_str}"

    # Store as wisdom with research tag
    ctx.daemon_call(
        "remember",
        {
            "content": content,
            "type": "wisdom",
            "confidence": confidence,
            "tags": ["research", "web-learned"],
        },
    )

    output = f"Research stored: {topic[:50]}...\n"

    # Resolve curiosity gap if provided
    if gap_id:
        ctx.daemon_call("curiosity_resolve", {"id": gap_id, "learned": findings[:500]})
        output += f"Resolved gap #{gap_id}\n"

    # Create triplet linking research to topic
    ctx.daemon_call(
        "connect",
        {
            "subject": "research",
            "predicate": "learned_about",
            "object": topic.replace(" ", "_")[:50],
        },
    )

    return output + "Memory created with 'research' tag."


def handle_research_cycle(ctx, arguments: dict) -> str:
    """Run one research cycle - returns topic with context for web search."""
    realm = arguments.get("realm", "")

    # Try curiosity_gaps first (tag-based query)
    gaps_result = ctx.daemon_call("curiosity_gaps", {"limit": 5, "realm": realm})
    if gaps_result and "No" not in gaps_result and "#" in gaps_result:
        for line in gaps_result.split("\n"):
            if "#" in line and ":" in line:
                parts = line.split(":", 1)
                if len(parts) > 1:
                    gap_id = parts[0].strip().replace("#", "").strip()
                    topic = parts[1].strip()

                    # Get context from related memories
                    context_result = ctx.daemon_call("recall", {"query": topic[:100], "limit": 3})

                    output = "Research Cycle: Topic Found\n"
                    output += "=" * 40 + "\n\n"
                    output += f"Topic: {topic}\n"
                    output += f"Gap ID: {gap_id}\n\n"

                    output += "Related memories:\n"
                    if context_result and "No memories" not in context_result:
                        ctx_lines = [
                            line
                            for line in context_result.split("\n")
                            if line.strip() and "[gap]" not in line
                        ][:5]
                        output += "\n".join(ctx_lines) + "\n\n"
                    else:
                        output += "(none found)\n\n"

                    output += "Instructions:\n"
                    output += f"1. Use WebSearch to research: {topic}\n"
                    output += f"2. Call research_store with findings and gap_id={gap_id}\n"
                    return output

    # Fallback: search for gap content via recall
    # Try to find memories with "[gap]" in content
    gaps_result = ctx.daemon_call(
        "recall",
        {
            "query": "How does DuckDB HNSW vector indexing",  # Use actual gap content
            "limit": 10,
        },
    )

    if gaps_result and "No memories" not in gaps_result:
        for line in gaps_result.split("\n"):
            if "[gap]" in line:
                # Extract content after the tags
                parts = line.split("]")
                if len(parts) >= 3:
                    content = parts[-1].strip()
                    if content and len(content) > 10:
                        output = "Research Cycle: Topic Found\n"
                        output += "=" * 40 + "\n\n"
                        output += f"Topic: {content[:200]}\n\n"
                        output += "Instructions:\n"
                        output += f"1. Use WebSearch to research: {content[:100]}\n"
                        output += "2. Call research_store with topic and findings\n"
                        return output

    return "No research topics available. Add curiosity gaps with curiosity_note_gap first."


def handle_research_gateway(ctx, arguments: dict) -> str:
    """Unified research gateway.

    Actions: topics, store, cycle
    """
    action = arguments.pop("action", "cycle")
    handler_map = {
        "topics": ctx.handle_research_topics,
        "store": ctx.handle_research_store,
        "cycle": ctx.handle_research_cycle,
    }
    handler = handler_map.get(action)
    if not handler:
        return f"Unknown research action: {action}. Use: {', '.join(handler_map.keys())}"
    return handler(arguments)


def handle_soul_repl(ctx, arguments: dict) -> str:
    """
    RLM-style REPL for programmatic soul exploration.

    Executes Python code in a sandbox with soul.* methods exposed.
    Supports persistent sessions via session_id — variables survive across calls.
    Session namespaces are stored natively in chitta-field (repl_sessions.json),
    not in the wisdom-memory graph.
    """
    code = arguments.get("code", "")
    reset = arguments.get("reset", False)
    session_id = arguments.get("session_id", "")

    if not code.strip():
        return """Soul REPL - RLM-style Memory Exploration

Write Python code to explore memories programmatically.
Available methods:

  soul.search(query, limit=20)     - Semantic search
  soul.recall(query, limit=10)     - Hybrid recall (semantic + keyword + graph)
  soul.expand(memory_id, depth=3)  - Drill down: SSL -> Episode -> Full turns
  soul.triplets(subject=, predicate=, object=)  - Knowledge graph query
  soul.recent(hours=24)            - Recent memories by time
  soul.remember(content, tags=[])  - Store new memory
  soul.symbols(pattern=, kind=)    - Search code symbols
  soul.read_symbol(name)           - Read symbol source
  soul.stats()                     - Soul statistics
  soul.trajectory()                - Get exploration path

Persistent sessions (variables survive across calls):
  Pass session_id="my_session" to keep variables alive.
  Pass reset=true to clear session state.
"""

    try:
        raw = ctx.daemon_call(
            "repl_execute",
            {
                "session_id": session_id or "default",
                "code": code,
                "reset": reset,
                "max_output": 10000,
            },
        )
        try:
            result = json.loads(raw)
        except (ValueError, TypeError):
            return raw or "(no output)"

        output = []
        if result.get("output"):
            output.append(result["output"])
        if result.get("error"):
            output.append(f"\nError:\n{result['error']}")
        traj = result.get("trajectory", [])
        if traj:
            output.append(f"\n[Trajectory: {len(traj)} soul calls]")
            for t in traj[-5:]:
                output.append(
                    f"  - {t['method']}({', '.join(f'{k}={repr(v)[:30]}' for k, v in t['args'].items())})"
                )
        if session_id:
            output.append(f"\n[Session: {session_id}]")

        return "\n".join(output) if output else "(no output)"

    except (ValueError, TypeError, KeyError, AttributeError) as e:
        # The daemon reports REPL errors inside its own JSON payload; reaching
        # here means the envelope itself was not the shape we expect.
        ctx.logger.warning("malformed repl_execute response: %s", e)
        return f"REPL Error: {e}"


def handle_sadhana_gateway(ctx, arguments: dict) -> str:
    """Unified sadhana control.

    Actions: start, stop, pause, resume, status, list, checkpoint,
             set_goal, set_interval, set_model
    """
    action = arguments.pop("action", "status")
    tool = f"sadhana_{action}"
    return ctx.daemon_call(tool, arguments)


def handle_run_hint_enricher(ctx, arguments: dict) -> str:
    script = os.path.join(os.path.dirname(__file__), "enrichers", "hint_enricher.py")
    mind = os.environ.get("MIND", os.path.expanduser("~/.claude/mind"))
    cmd = [
        "python3",
        script,
        "--mind",
        mind,
        "--limit",
        str(arguments.get("limit", 100)),
        "--model",
        str(arguments.get("model", "chitta-hint-tuned")),
    ]
    if arguments.get("dry_run"):
        cmd.append("--dry-run")
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        out = result.stdout.strip()
        err = result.stderr.strip()
        if result.returncode != 0:
            return f"[hint_enricher] error (rc={result.returncode})\n{err}\n{out}"
        return out if out else (err if err else "[hint_enricher] done (no output)")
    except (OSError, subprocess.SubprocessError) as e:
        # Interpreter or script missing, or the 300s timeout elapsed.
        return f"[hint_enricher] failed: {e}"
