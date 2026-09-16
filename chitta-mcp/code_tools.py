"""Code intelligence and task context composites.

Implementations receive the server runtime explicitly as ``ctx`` so the public
server facade owns shared state and dependency overrides across all entry points.
"""

from __future__ import annotations

import json
import os


def read_file_lines(ctx, file_path: str, start: int, end: int) -> str:
    """Read specific line range from a file."""
    try:
        with open(file_path, encoding="utf-8", errors="replace") as f:
            lines = f.readlines()

        # Clamp to valid range
        start = max(1, start)
        end = min(len(lines), end)

        return "".join(lines[start - 1 : end])
    except OSError as e:
        # Symbol index can outlive the file it points at (moved, deleted, or
        # permissions changed). Report the path problem instead of failing the
        # whole tool call.
        return f"Error reading file: {e}"


def handle_read_symbol(ctx, arguments: dict) -> str:
    """
    Read just a symbol's code, not entire file.

    1. find_symbol(name, kind) → get file + line range
    2. Read only line_start - 3 to line_end + 1
    3. Return: [symbol @ file:start-end] + code

    Token savings: ~10x vs full file read
    """
    if "path" in arguments or "line" in arguments:
        # The navigation index validates source hashes and disambiguates overloads.
        return ctx.daemon_call("read_symbol", arguments)
    name = arguments.get("name", "")
    kind = arguments.get("kind")
    project = arguments.get("project")
    context_lines = int(arguments.get("context", 3))  # Lines before/after

    if not name:
        return "Error: name parameter required"

    # Call find_symbol on daemon with structured=True to get line numbers
    find_args = {"name": name}
    if kind:
        find_args["kind"] = kind

    response = ctx.daemon_call("find_symbol", find_args, structured=True)

    # Parse structured response: {"count": N, "symbols": [...]}
    try:
        data = json.loads(response)

        symbols = data.get("symbols", [])
        if not symbols:
            return f"No symbol found: {name}"

        # The daemon's find_symbol has no project filter, so `project` — which
        # both the read_symbol and read_function schemas advertise — is applied
        # here as a preference over the returned candidates. Without it, or with
        # no candidate under that path, this is the historical first-match pick.
        symbol = symbols[0]
        if project:
            symbol = next((s for s in symbols if project in s.get("file", "")), symbol)
        file_path = symbol.get("file", "")
        line_start = symbol.get("line_start", 1)
        line_end = symbol.get("line_end", line_start + 50)
        symbol_kind = symbol.get("kind", kind or "symbol")
        symbol_name = symbol.get("name", name)

    except (json.JSONDecodeError, KeyError, TypeError) as e:
        return f"Error parsing symbol data: {e}\nResponse: {response[:200]}"

    if not file_path or not os.path.exists(file_path):
        return f"Symbol found but file not accessible: {response}"

    # Read with context
    start = max(1, line_start - context_lines)
    end = line_end + 1

    code = ctx.read_file_lines(file_path, start, end)

    # Calculate token estimate (rough: ~4 chars per token)
    tokens = len(code) // 4

    header = (
        f"[{symbol_kind} {symbol_name} @ {file_path}:{line_start}-{line_end}] (~{tokens} tokens)"
    )
    return f"{header}\n{code}"


def handle_read_function(ctx, arguments: dict) -> str:
    """Convenience wrapper for read_symbol with kind=function."""
    arguments["kind"] = arguments.get("kind", "function")
    return ctx.handle_read_symbol(arguments)


def handle_symbol_callers(ctx, arguments: dict) -> str:
    """
    Find all callers of a symbol.
    Delegates to the daemon's symbol_callers tool, which walks the callsite
    triplet graph (predicate=calls) and resolves each callsite's file:line
    to the enclosing function/method name.
    """
    name = arguments.get("name", "")
    limit = int(arguments.get("limit", 20))

    if not name:
        return "Error: name parameter required"

    return ctx.daemon_call("symbol_callers", {"name": name, "limit": limit})


def handle_symbol_callees(ctx, arguments: dict) -> str:
    """
    Find all symbols that this symbol calls.
    Delegates to the daemon's symbol_callees tool, which scans the symbol's
    line range for callsite triplets (predicate=calls).
    """
    name = arguments.get("name", "")
    limit = int(arguments.get("limit", 20))

    if not name:
        return "Error: name parameter required"

    return ctx.daemon_call("symbol_callees", {"name": name, "limit": limit})


def handle_smart_context(ctx, arguments: dict) -> str:
    """
    Build intelligent context combining memories, code symbols, and graph relationships.

    Modes:
    - fast (default): C++ daemon single-RPC (<80ms)
    - full: daemon full_resonate (<200ms)
    - rlm: RLM-style dynamic exploration via soul_repl

    RLM mode lets Claude write exploration code for complex queries.

    When resolver_mode=True (default), prepends digest-node and decision memories
    before the code symbols section.
    """
    mode = arguments.get("mode", "fast")
    resolver_mode = arguments.get("resolver_mode", True)

    # RLM mode: use soul_repl for dynamic exploration
    if mode == "rlm":
        task = arguments.get("task", "")
        query = arguments.get("query", task)

        if not query:
            return "Error: 'task' or 'query' parameter required for RLM mode"

        # Generate exploration code based on the task
        exploration_code = f'''
# RLM-style context gathering for: {query[:100]}
results = {{}}

# 1. Semantic memory search
memories = soul.search("{query[:200]}", limit=10)
results["memories"] = [(m.id, m.score, m.content[:150]) for m in memories if m.score > 0.3]

# 2. Find related code symbols
symbols = soul.symbols(pattern="{query.split()[0] if query else ""}", limit=5)
results["symbols"] = symbols[:5] if symbols else []

# 3. Expand top memory for full context
if memories and memories[0].score > 0.5:
    expanded = soul.expand(memories[0].id, depth=2)
    results["expanded"] = expanded

# 4. Graph relationships for top result
if memories:
    triplets = soul.triplets(subject=f"memory:{{memories[0].id}}", limit=5)
    results["relations"] = [str(t) for t in triplets]

results["trajectory"] = soul.trajectory()
results
'''
        try:
            from soul_repl import execute_soul_code

            result, _ns = execute_soul_code(exploration_code)

            if result.error:
                return f"RLM exploration error: {result.error}"

            # Format output
            output = [f"[RLM Context for: {query[:50]}...]"]
            output.append(f"Trajectory: {len(result.trajectory)} soul calls")
            output.append("")

            if result.result:
                data = result.result
                if "memories" in data:
                    output.append(f"Memories ({len(data['memories'])}):")
                    for mid, score, content in data["memories"][:5]:
                        output.append(f"  [{mid}] {score:.0%} {content[:80]}...")

                if "symbols" in data and data["symbols"]:
                    output.append(f"\nSymbols ({len(data['symbols'])}):")
                    for s in data["symbols"][:3]:
                        output.append(f"  {s.get('name', '?')} @ {s.get('file', '?')}")

                if "expanded" in data:
                    output.append(f"\nExpanded context: {list(data['expanded'].keys())}")

                if "relations" in data and data["relations"]:
                    output.append(f"\nRelations: {data['relations'][:3]}")

            return "\n".join(output)

        except Exception as e:  # noqa: BLE001
            # execute_soul_code runs generated Python in a sandbox, so any
            # exception type is reachable by construction. RLM is an optional
            # accelerator; the daemon path below is the supported answer.
            ctx.logger.info("RLM smart_context failed, using daemon path: %s", e)
            return f"RLM mode failed: {e}, falling back to daemon"

    # Default: delegate to C++ daemon (fast single-RPC)
    # Resolver hierarchy: prepend digest-node + decision memories when resolver_mode=True
    if not resolver_mode:
        return ctx.daemon_call("smart_context", arguments)

    task = arguments.get("task", "")
    prefix_parts = []

    if task:
        # 1. Check for digest-node memories relevant to the task
        digest_result = ctx.daemon_call(
            "recall",
            {
                "query": task,
                "tag": "digest-node",
                "limit": 3,
            },
        )
        if digest_result and "No memories" not in digest_result and "Error" not in digest_result:
            prefix_parts.append("[digest-nodes]\n" + digest_result)

        # 2. Check for decision memories relevant to the task
        decision_result = ctx.daemon_call(
            "recall",
            {
                "query": task,
                "tag": "decision",
                "limit": 3,
            },
        )
        if (
            decision_result
            and "No memories" not in decision_result
            and "Error" not in decision_result
        ):
            prefix_parts.append("[decisions]\n" + decision_result)
        else:
            # Also try text search for [dec] prefix
            dec_result = ctx.daemon_call(
                "recall",
                {
                    "query": f"[dec] {task}",
                    "limit": 2,
                },
            )
            if dec_result and "No memories" not in dec_result and "Error" not in dec_result:
                prefix_parts.append("[decisions]\n" + dec_result)

    # 3. Fall through to existing symbol/code lookup
    code_result = ctx.daemon_call("smart_context", arguments)

    if prefix_parts:
        return "\n\n".join(prefix_parts) + "\n\n[code-context]\n" + code_result
    return code_result


def handle_lookup(ctx, arguments: dict) -> str:
    return ctx.daemon_call("lookup", arguments)
