"""Compatibility schemas for daemon tools historically repeated in COMPOSITE_TOOLS.

These are schema overrides, not local handlers. Keep their existing discovery
precedence and contract rows until a separately reviewed API migration.
"""

DAEMON_OVERRIDES = [
    {
        "name": "skill_upload",
        "description": "Upload a new version of a reusable skill. Skills are immutable versioned text "
        "blobs (prompts, templates, procedures).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "skill_id": {"type": "string", "description": "Unique skill identifier"},
                "content": {
                    "type": "string",
                    "description": "Skill content (prompt, template, procedure)",
                },
                "uploaded_by": {"type": "string", "description": "Agent or user who uploaded"},
                "tags": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Tags for search",
                },
            },
            "required": ["skill_id", "content"],
        },
    },
    {
        "name": "skill_read",
        "description": "Read a skill version. version=0 means latest.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "skill_id": {"type": "string", "description": "Skill identifier"},
                "version": {"type": "integer", "description": "Version number (0=latest)"},
            },
            "required": ["skill_id"],
        },
    },
    {
        "name": "skill_list",
        "description": "List all registered skills with their latest version number.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "skill_search",
        "description": "Search skills by text match on id, tags, or content.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "query": {"type": "string", "description": "Search query"},
                "limit": {"type": "integer", "description": "Max results (default: 20)"},
            },
            "required": ["query"],
        },
    },
    {
        "name": "skill_deprecate",
        "description": "Deprecate a skill (marks latest version as deprecated).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "skill_id": {"type": "string", "description": "Skill identifier to deprecate"}
            },
            "required": ["skill_id"],
        },
    },
    {
        "name": "agent_upsert",
        "description": "Register or update an agent identity in the multi-agent registry.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "agent_id": {"type": "string", "description": "Unique agent identifier"},
                "display_name": {"type": "string", "description": "Human-readable name"},
                "description": {"type": "string", "description": "What this agent does"},
            },
            "required": ["agent_id"],
        },
    },
    {
        "name": "agent_get",
        "description": "Get an agent's identity record.",
        "inputSchema": {
            "type": "object",
            "properties": {"agent_id": {"type": "string", "description": "Agent identifier"}},
            "required": ["agent_id"],
        },
    },
    {
        "name": "agent_list",
        "description": "List all registered agents.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "agent_disable",
        "description": "Disable (revoke) an agent.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "agent_id": {"type": "string", "description": "Agent identifier to disable"}
            },
            "required": ["agent_id"],
        },
    },
]

# Existing MCP wording is part of the frozen discovery contract.
DESCRIPTION_OVERRIDES = {
    "correction_check": "Deterministic durable-correction check (capability #2): does any stored "
    "[correction] trigger recur in this turn? Exact keyed bigram probe of the "
    "turn text against corrected-mistake phrases — bypasses fuzzy recall (which "
    "loses corrections on cosine similarity ~99% of the time). Returns found + "
    "the correction(s), newest first (latest-wins).",
    "find_symbol": "Search for symbols by name. Scope with path (substring the file path must "
    "contain, e.g. a repo dir) and lang (cpp|c|python|rust|js|go|java|ruby).",
}
