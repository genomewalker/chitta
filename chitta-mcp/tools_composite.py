"""Hand-written MCP composite schemas; every name must have a server handler.

The generator validates this list against server.COMPOSITE_HANDLERS without
importing the server or requiring the MCP SDK.
"""

COMPOSITE = [
    {
        "name": "advanced",
        "description": "Gateway to hidden/advanced tools. Use action='list' to see every hidden tool "
        "(about 250), or call directly with tool='<name>' and arguments={...}. Example: "
        '{"tool": "pin_memory", "arguments": {"id": 123}}',
        "inputSchema": {
            "type": "object",
            "properties": {
                "action": {
                    "type": "string",
                    "description": "Action: 'list' to show available tools",
                },
                "category": {
                    "type": "string",
                    "description": "Filter list by category: 'advanced' or 'internal'",
                },
                "tool": {"type": "string", "description": "Hidden tool name to call"},
                "arguments": {
                    "type": "object",
                    "description": "Arguments to pass to the hidden tool",
                },
            },
        },
    },
    {
        "name": "soul_repl",
        "description": "RLM-style Python REPL for programmatic memory exploration. Write code with "
        "soul.* methods: search(), recall(), expand(), triplets(), recent(), remember(), "
        "symbols(). Supports persistent sessions via session_id. Call with no code for "
        "API reference.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "code": {
                    "type": "string",
                    "description": "Python code to execute. Has access to "
                    "soul.search(), soul.recall(), "
                    "soul.expand(), etc.",
                },
                "reset": {
                    "type": "boolean",
                    "description": "Reset namespace before execution (default: false)",
                },
                "session_id": {
                    "type": "string",
                    "description": "Session name for persistent state "
                    "— variables survive across calls "
                    "with the same session_id",
                },
            },
        },
    },
    {
        "name": "read_symbol",
        "description": "Read just a symbol's code, not entire file. ~10x token savings vs full file "
        "read. Returns [kind name @ file:line-line] + code.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "name": {
                    "type": "string",
                    "description": "Symbol name to read (e.g., 'DuckDBStore', 'daemon_call')",
                },
                "kind": {
                    "type": "string",
                    "description": "Symbol kind filter: class, function, method (optional)",
                },
                "path": {"type": "string", "description": "Exact path from code_query"},
                "line": {"type": "integer", "description": "Definition line from code_query"},
                "context": {
                    "type": "integer",
                    "description": "Lines of context before symbol (default: 3)",
                },
                "project": {"type": "string", "description": "Project name filter (optional)"},
            },
            "required": ["name"],
        },
    },
    {
        "name": "read_function",
        "description": "Read a function's code. Convenience wrapper for read_symbol with kind=function.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "name": {"type": "string", "description": "Function name to read"},
                "context": {
                    "type": "integer",
                    "description": "Lines of context before function (default: 3)",
                },
                "project": {"type": "string", "description": "Project name filter (optional)"},
            },
            "required": ["name"],
        },
    },
    {
        "name": "symbol_callers",
        "description": "Find all callers of a symbol without grep. Queries triplets where "
        "predicate=calls and object=symbol.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "name": {"type": "string", "description": "Symbol name to find callers for"},
                "limit": {"type": "integer", "description": "Max results (default: 20)"},
            },
            "required": ["name"],
        },
    },
    {
        "name": "symbol_callees",
        "description": "Find all symbols that a symbol calls. Queries triplets where predicate=calls and "
        "subject=symbol.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "name": {"type": "string", "description": "Symbol name to find callees for"},
                "limit": {"type": "integer", "description": "Max results (default: 20)"},
            },
            "required": ["name"],
        },
    },
    {
        "name": "lookup",
        "description": "Unified memory lookup. Classifies query intent, fans out to optimal backends "
        "(keyword/semantic/triplet/temporal/code), fuses with weighted RRF. Default entry "
        "point for memory search — use instead of recall/smart_recall/hybrid_recall.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "query": {"type": "string", "description": "Search query"},
                "limit": {"type": "integer", "description": "Max results (default: 10)"},
                "realm": {"type": "string", "description": "Filter by realm"},
                "mode": {
                    "type": "string",
                    "enum": ["auto", "fast", "deep"],
                    "description": "auto=escalate if low confidence; "
                    "fast=skip deep search; deep=force full "
                    "resonate",
                },
                "explain": {
                    "type": "boolean",
                    "description": "Include intent classification and score breakdown",
                },
            },
            "required": ["query"],
        },
    },
    {
        "name": "smart_context",
        "description": "Build intelligent context. Modes: fast (<80ms), full (<200ms), rlm (RLM-style "
        "dynamic exploration via soul_repl). With resolver_mode=true (default), prepends "
        "digest-node and decision memories before code symbols.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "task": {"type": "string", "description": "Query to find context for"},
                "mode": {
                    "type": "string",
                    "enum": ["fast", "full", "rlm"],
                    "description": "fast: <80ms (vector + BM25), full: "
                    "<200ms (full_resonate), rlm: dynamic "
                    "exploration via soul_repl",
                },
                "limit": {"type": "integer", "description": "Token limit (default: 300)"},
                "memories": {
                    "type": "boolean",
                    "description": "Include semantic memories (default: true)",
                },
                "code": {"type": "boolean", "description": "Include code symbols (default: true)"},
                "neighbors": {
                    "type": "boolean",
                    "description": "Include triplet neighbors (default: true)",
                },
                "realm": {"type": "string", "description": "Filter by realm"},
                "resolver_mode": {
                    "type": "boolean",
                    "description": "Prepend digest-node and "
                    "decision memories before code "
                    "symbols (default: true)",
                },
            },
            "required": ["task"],
        },
    },
    {
        "name": "learn_correction",
        "description": "Store a correction when I was wrong. Creates high-confidence counter-memory with "
        "'corrects' triplet linking to original mistake.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "wrong": {"type": "string", "description": "What I said/did that was incorrect"},
                "correct": {"type": "string", "description": "The correct information/approach"},
                "context": {
                    "type": "string",
                    "description": "Context where this applies (optional)",
                },
            },
            "required": ["wrong", "correct"],
        },
    },
    {
        "name": "learn_preference",
        "description": "Store a user preference for adapting communication/behavior. Global visibility "
        "so it applies across all projects.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "category": {
                    "type": "string",
                    "description": "Preference category: communication, "
                    "detail, autonomy, style, workflow",
                },
                "preference": {
                    "type": "string",
                    "description": "The preference to remember (e.g., 'prefers concise responses')",
                },
                "example": {
                    "type": "string",
                    "description": "Example demonstrating this preference (optional)",
                },
            },
            "required": ["category", "preference"],
        },
    },
    {
        "name": "learn_insight",
        "description": "Store a generalizable insight that applies across projects. Use for patterns, "
        "techniques, and wisdom not tied to specific codebase.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "domain": {
                    "type": "string",
                    "description": "Domain: programming, debugging, "
                    "architecture, testing, performance, "
                    "security, communication",
                },
                "insight": {
                    "type": "string",
                    "description": "The generalizable insight or pattern",
                },
                "learned_from": {
                    "type": "string",
                    "description": "Context where this was learned "
                    "(optional, for future "
                    "reference)",
                },
            },
            "required": ["domain", "insight"],
        },
    },
    {
        "name": "learn_approach",
        "description": "Store what approach worked when in a particular state/mood. Builds emotional "
        "memory for adapting to session dynamics.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "state": {
                    "type": "string",
                    "description": "State/mood: stuck, debugging, "
                    "exploring, flowing, frustrated, "
                    "uncertain, rushing",
                },
                "approach": {
                    "type": "string",
                    "description": "What helped in this state (e.g., "
                    "'step back and reread "
                    "requirements')",
                },
                "outcome": {"type": "string", "description": "What happened after (optional)"},
            },
            "required": ["state", "approach"],
        },
    },
    {
        "name": "learn_outcome",
        "description": "Record whether a suggestion/approach actually helped. Builds feedback loop for "
        "improving future suggestions.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "suggestion": {"type": "string", "description": "What was suggested or tried"},
                "helped": {"type": "boolean", "description": "Did it help? true/false"},
                "details": {
                    "type": "string",
                    "description": "Why it helped or didn't (optional but valuable)",
                },
            },
            "required": ["suggestion", "helped"],
        },
    },
    {
        "name": "learn_milestone",
        "description": "Record a relationship milestone - achievements, personal context, significant "
        "moments worth remembering.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "milestone": {
                    "type": "string",
                    "description": "What happened (e.g., 'shipped "
                    "v1.0', 'first successful release')",
                },
                "significance": {"type": "string", "description": "Why it matters (optional)"},
                "date": {
                    "type": "string",
                    "description": "When it happened (optional, defaults to now)",
                },
            },
            "required": ["milestone"],
        },
    },
    {
        "name": "learn_analysis",
        "description": "Record an analysis with data and script locations. Makes analyses reproducible "
        "and findable later. Use after completing any significant analysis.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "name": {
                    "type": "string",
                    "description": "Short name for the analysis (e.g., 'AGP "
                    "codon optimization', 'bin_28 damage "
                    "patterns')",
                },
                "description": {
                    "type": "string",
                    "description": "What the analysis does/investigates",
                },
                "data_paths": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Paths to input data files/directories",
                },
                "script_paths": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Paths to analysis scripts",
                },
                "findings": {"type": "string", "description": "Key findings or results (optional)"},
                "project": {
                    "type": "string",
                    "description": "Project name for organization (optional)",
                },
            },
            "required": ["name"],
        },
    },
    {
        "name": "verify_correction",
        "description": "Mark a correction memory as verified at a specific code locus. Stores a "
        "verification record and updates the original correction's tags.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "id": {
                    "anyOf": [{"type": "integer"}, {"type": "string"}],
                    "description": "Memory/triplet ID of the correction to verify",
                },
                "evidence_locus": {
                    "type": "string",
                    "description": "Location of evidence, e.g. 'sadhana_manager.cpp:299'",
                },
            },
            "required": ["id", "evidence_locus"],
        },
    },
    {
        "name": "ack_memory",
        "description": "Increment ack signal for a memory. Records [ack] memory:<id> score:+1 with tag "
        "ack-signal.",
        "inputSchema": {
            "type": "object",
            "properties": {"id": {"type": "string", "description": "Memory ID to ack"}},
            "required": ["id"],
        },
    },
    {
        "name": "nack_memory",
        "description": "Decrement nack signal for a memory. Records [nack] memory:<id> score:-1 with tag "
        "nack-signal.",
        "inputSchema": {
            "type": "object",
            "properties": {"id": {"type": "string", "description": "Memory ID to nack"}},
            "required": ["id"],
        },
    },
    {
        "name": "remember_typed",
        "description": "Store a typed memory node (digest-node, decision, open-question, etc.) with "
        "optional graph links (supersedes, invalidated-by, anchors-to).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "node_type": {
                    "type": "string",
                    "enum": [
                        "digest-node",
                        "symbol-summary",
                        "decision",
                        "open-question",
                        "rollup",
                        "working-brief",
                    ],
                    "description": "Type of memory node",
                },
                "content": {"type": "string", "description": "SSL triplet content for the memory"},
                "subject": {"type": "string", "description": "What this memory is about"},
                "links": {
                    "type": "array",
                    "items": {
                        "type": "object",
                        "properties": {
                            "predicate": {
                                "type": "string",
                                "enum": ["supersedes", "invalidated-by", "anchors-to"],
                            },
                            "target_id": {"type": "string"},
                        },
                        "required": ["predicate", "target_id"],
                    },
                    "description": "Graph links to other memories",
                },
                "realm": {"type": "string", "description": "Realm to store in (default: brahman)"},
            },
            "required": ["node_type", "content", "subject"],
        },
    },
    {
        "name": "research_topics",
        "description": "Get topics that need research. Returns curiosity gaps, low-confidence memories, "
        "or suggested topics. Use WebSearch to research these, then store results with "
        "research_store.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "source": {
                    "type": "string",
                    "description": "Where to get topics: gaps (curiosity "
                    "gaps), weak (low-confidence memories), "
                    "suggest (AI-suggested based on recent "
                    "work)",
                },
                "limit": {"type": "integer", "description": "Max topics to return (default: 3)"},
                "realm": {"type": "string", "description": "Filter by realm/project"},
            },
        },
    },
    {
        "name": "research_store",
        "description": "Store research results as memories with source attribution. Call after using "
        "WebSearch to learn about a topic.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "topic": {"type": "string", "description": "What was researched"},
                "findings": {"type": "string", "description": "Key learnings in SSL format"},
                "sources": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "URLs or references",
                },
                "gap_id": {
                    "type": "integer",
                    "description": "Curiosity gap ID to resolve (optional)",
                },
                "confidence": {
                    "type": "number",
                    "description": "Confidence in findings 0-1 (default: 0.7)",
                },
            },
            "required": ["topic", "findings"],
        },
    },
    {
        "name": "research_cycle",
        "description": "Run one curiosity-driven research cycle. Returns a topic to research with "
        "context. After calling this, use WebSearch to find information, then call "
        "research_store with results.",
        "inputSchema": {
            "type": "object",
            "properties": {"realm": {"type": "string", "description": "Filter by realm/project"}},
        },
    },
    {
        "name": "sadhana",
        "description": "Autonomous background learning loop control. Actions: start, stop, pause, "
        "resume, status, list, checkpoint, set_goal, set_interval, set_model",
        "inputSchema": {
            "type": "object",
            "properties": {
                "action": {
                    "type": "string",
                    "description": "start|stop|pause|resume|status|list|checkpoint|set_goal|set_interval|set_model",
                },
                "id": {
                    "anyOf": [{"type": "integer"}, {"type": "string"}],
                    "description": "Sadhana ID (for stop/pause/resume/status/checkpoint)",
                },
                "goal": {"type": "string", "description": "Goal text (for set_goal)"},
                "interval": {
                    "anyOf": [{"type": "integer"}, {"type": "string"}],
                    "description": "Interval in seconds (for set_interval)",
                },
                "model": {"type": "string", "description": "Model name (for set_model/start)"},
                "max_turns": {
                    "anyOf": [{"type": "integer"}, {"type": "string"}],
                    "description": "Max turns (for start)",
                },
            },
            "required": ["action"],
        },
    },
    {
        "name": "learn",
        "description": "Store learning in soul memory. Types: correction (wrong→right fix), preference "
        "(user style), insight (generalizable wisdom), approach (what worked in a state), "
        "outcome (did suggestion help?), milestone (achievement), analysis (reproducible "
        "analysis with data/scripts)",
        "inputSchema": {
            "type": "object",
            "properties": {
                "type": {
                    "type": "string",
                    "description": "correction|preference|insight|approach|outcome|milestone|analysis",
                },
                "wrong": {"type": "string", "description": "What was wrong (correction)"},
                "correct": {"type": "string", "description": "What is correct (correction)"},
                "context": {"type": "string", "description": "Context for the learning"},
                "preference": {"type": "string", "description": "Preference text (preference)"},
                "category": {"type": "string", "description": "Category (preference)"},
                "insight": {"type": "string", "description": "Insight text (insight)"},
                "domain": {"type": "string", "description": "Domain (insight/analysis)"},
                "approach": {"type": "string", "description": "Approach text (approach)"},
                "state": {"type": "string", "description": "State/mood (approach)"},
                "outcome": {"type": "string", "description": "Outcome (approach)"},
                "suggestion": {"type": "string", "description": "Suggestion (outcome)"},
                "helped": {"type": "boolean", "description": "Did it help? (outcome)"},
                "details": {"type": "string", "description": "Details (outcome)"},
                "milestone": {"type": "string", "description": "Milestone text (milestone)"},
                "name": {"type": "string", "description": "Analysis name (analysis)"},
                "description": {"type": "string", "description": "Description (analysis)"},
                "data_paths": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Data paths (analysis)",
                },
                "script_paths": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Script paths (analysis)",
                },
                "findings": {"type": "string", "description": "Findings (analysis)"},
                "project": {"type": "string", "description": "Project (analysis)"},
            },
            "required": ["type"],
        },
    },
    {
        "name": "research",
        "description": "Curiosity-driven research. Actions: cycle (get topic to research), topics (list "
        "research topics), store (save findings)",
        "inputSchema": {
            "type": "object",
            "properties": {
                "action": {"type": "string", "description": "cycle|topics|store"},
                "topic": {"type": "string", "description": "Topic (store)"},
                "findings": {"type": "string", "description": "Findings (store)"},
                "sources": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "URLs (store)",
                },
                "gap_id": {
                    "anyOf": [{"type": "integer"}, {"type": "string"}],
                    "description": "Gap ID to resolve (store)",
                },
                "source": {
                    "type": "string",
                    "description": "Topic source: gaps|weak|suggest (topics)",
                },
                "limit": {
                    "anyOf": [{"type": "integer"}, {"type": "string"}],
                    "description": "Max results",
                },
                "realm": {"type": "string", "description": "Filter by realm"},
            },
            "required": ["action"],
        },
    },
    {
        "name": "triplets",
        "description": "Knowledge graph operations. Actions: connect (create temporal triplet), query "
        "(query temporal triplets), history (triplet history for a subject/object)",
        "inputSchema": {
            "type": "object",
            "properties": {
                "action": {"type": "string", "description": "connect|query|history"},
                "subject": {"type": "string", "description": "Subject entity"},
                "predicate": {"type": "string", "description": "Relationship type"},
                "object": {"type": "string", "description": "Object entity"},
                "limit": {
                    "anyOf": [{"type": "integer"}, {"type": "string"}],
                    "description": "Max results",
                },
            },
            "required": ["action"],
        },
    },
    {
        "name": "memory_edit",
        "description": "Edit memory metadata. Actions: set_type (change memory type), set_priority "
        "(change priority tier)",
        "inputSchema": {
            "type": "object",
            "properties": {
                "action": {"type": "string", "description": "set_type|set_priority"},
                "id": {
                    "anyOf": [{"type": "integer"}, {"type": "string"}],
                    "description": "Memory ID",
                },
                "type": {"type": "string", "description": "New type (for set_type)"},
                "tier": {"type": "string", "description": "New priority tier (for set_priority)"},
            },
            "required": ["action", "id"],
        },
    },
    {
        "name": "recall_spreading",
        "description": "Retrieve memories via entity graph spreading activation. Extracts capitalized "
        "words, @refs, and quoted strings from the query as entity seeds, then traverses "
        "the triplet graph (BFS depth 2, decay 0.6) to find related memories.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "query": {
                    "type": "string",
                    "description": "Query; entity seeds extracted automatically",
                },
                "limit": {"type": "integer", "description": "Max results", "default": 10},
                "realm": {"type": "string", "description": "Memory realm"},
            },
            "required": ["query"],
        },
    },
    {
        "name": "recall_smart",
        "description": "Multi-lane retrieval planner: uses a fast LLM call to extract entities and "
        "speech-act type, then fans out to semantic, typed, spreading-activation, and "
        "session-level lanes, merging results with Reciprocal Rank Fusion. Best for "
        "complex queries.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "query": {"type": "string", "description": "Natural language query"},
                "limit": {"type": "integer", "description": "Max results", "default": 10},
                "realm": {"type": "string", "description": "Memory realm"},
                "skip_llm_plan": {
                    "type": "boolean",
                    "description": "Skip LLM planning step (faster)",
                    "default": False,
                },
            },
            "required": ["query"],
        },
    },
    {
        "name": "run_hint_enricher",
        "description": "Generate retrieval hints for unprocessed memories using chitta-hint-tuned. Reads "
        "memories without a retrieval_hint tag, calls the local hint model, stores each "
        "hint as a derived memory (kind=hint, tags=retrieval_hint), and marks the source "
        "memory with hint:done. Run after a session to enrich new memories for better "
        "recall.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "limit": {
                    "type": "integer",
                    "description": "Max memories to enrich per run (default: 100)",
                    "default": 100,
                },
                "model": {
                    "type": "string",
                    "description": "Ollama model to use (default: chitta-hint-tuned)",
                    "default": "chitta-hint-tuned",
                },
                "dry_run": {
                    "type": "boolean",
                    "description": "Preview hints without writing to memory (default: false)",
                    "default": False,
                },
            },
        },
    },
]
