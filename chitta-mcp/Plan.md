# MCP fixes — fix/mcp

Scope: chitta-mcp only. No deployment or live state writes. Commit after gates.

1. Complete: instrumented real stdio baseline; native tokenizer reduced cold recall from 10.848 to 1.082 s.
2. Complete: bounded HTTP sessions, idle/capacity eviction, health counters, table and real SDK tests.
3. Complete: final-output detection, closed stdin, budget timeout and private process-group cleanup; sleeping-stub tests.
4. Gates passed: 110 MCP tests, 46 SMRITI tests, seven isolated hook suites, MCP-wide ruff and PyPy import checks. See Documentation.md and tests/evidence/.
5. Final diff reviewed; branch delivery is the commit containing this plan on fix/mcp.
