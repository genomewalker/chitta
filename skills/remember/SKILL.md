---
name: remember
description: Save a memory to chitta
execution: direct
args: content to remember
---

# Remember

Quickly store something in soul memory.

## Usage

```
/remember <what to remember>
```

## Process

1. **Parse input** - Extract the content after `/remember`
2. **Format as SSL** - Wrap in appropriate domain tag if not already formatted
3. **Store** - Call `remember` tool
4. **Confirm** - Show what was stored

## SSL v0.3 Formatting

If input doesn't start with `[domain]`, auto-detect, wrap, and add affect:

| Pattern | Domain | Default Affect |
|---------|--------|----------------|
| "prefer..." / "always..." / "never..." | `[preference]` | A:+0.2,0.1 |
| "when X, do Y" | `[pattern]` | A:+0.3,0.2 |
| "watch out..." / "gotcha..." | `[gotcha]` | A:-0.3,0.5 |
| "decided..." / "chose..." | `[decision]` | A:+0.3,0.3 |
| "fixed..." / "solved..." | `[solution]` | A:+0.5,0.4 |
| Other | `[note]` | A:0.0,0.1 |

Add `F:FLAG` when structurally significant (ORIGIN, CORE, PIVOT, GENESIS, TURNING).
Add `→@ref` when referencing a known topic.

## Tool Call

```json
{
  "tool": "remember",
  "args": {
    "content": "[domain] formatted content here A:v,a",
    "tags": ["user-added"]
  }
}
```

## Examples

```
/remember always run tests before committing
→ [preference] always→run-tests-before-commit A:+0.2,0.1 F:CORE

/remember when debugging, check logs first
→ [pattern] debugging→check-logs-first A:+0.3,0.2

/remember gotcha: the API returns 200 even on errors
→ [gotcha] API→returns-200-on-errors A:-0.3,0.5

/remember decided to use sqlite over postgres for metadata
→ [decision] sqlite>postgres|metadata|simpler A:+0.4,0.3 F:PIVOT
```

## Output

```
Remembered: [domain] content A:v,a
```
