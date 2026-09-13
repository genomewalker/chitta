# fix/daemon

Scope: tag recall realm isolation; mind-local queues shared by hooks and daemon;
shadow-only MDL evidence pooling. Work only here; no deployment.

1. Cover tag/realm recall and close tag-selection bypasses.
2. Resolve queue overrides consistently; default <mind>/queue.jsonl with siblings.
3. Pool bounded recent nonoverlapping chunks by mind/transcript/session/realm;
   preserve compression boundaries and 64-byte margin.
4. Build Rust via build.sh and library exports; CMake from main cache with local
   field root. Run ctest, hooks, MCP and SMRITI gates.
5. Compare four recall JSON key sets with read-only live CLI. Prove scratch
   queue isolation, tag scoping and synthetic MDL outcomes. Commit after gates.

All implementation and validation steps complete. Deliver as commits in both repositories.
