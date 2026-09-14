# Read-path locks validation — 2026-09-14

Branch `fix/read-path-locks`. All daemons and build products are private worktree artifacts; nothing was deployed.

## Method

The selected eval replica family was enumerated with `scripts/eval-replica-select.py` and copied into separate before/after stores. Baseline binary was built before edits and preserved. Both use the same 768-dimensional nomic model, four embedding contexts, eight RPC workers, `CHITTA_NO_QUEUE=1`, a private port/runtime, and a `.quiesce` file.

Concurrency query: `how does chitta semantic recall use the field store`, realm `brahman`, all six recall lanes. Three rounds each at 1, 6, and 12 simultaneous callers. Each 12-call round includes one `eu-stack` sample; wall measurements include that instrumentation and RPC queueing. Raw per-call data, schemas, logs, and complete stacks are under `.read-path/`.

## Baseline

| Concurrent callers | Calls | Median hyb ms | Median corr ms | Median lane total ms | Median RPC wall ms |
|---:|---:|---:|---:|---:|---:|
| 1 | 3 | 1044 | 976 | 1044 | 1046.9 |
| 6 | 18 | 1216.0 | 1076.5 | 1233.5 | 1236.1 |
| 12 | 36 | 1561.5 | 1512.5 | 1644.5 | 1747.3 |

Unchanged hook benchmark: `scripts/bench-recall-lanes.sh 5` (three queries; 15 runs/arm). Off: median/p95 1113/6031 ms; on: 1210/2094 ms; zero empty results in both arms. Runner exited successfully. The unchanged script suppresses individual hook exit statuses, so those are not separately observable.

| Stack sample | lock_shared_slow | fdatasync | ensure_turbo |
|---:|---:|---:|---:|
| 1 | 0 | 1 | 3 |
| 2 | 0 | 3 | 0 |
| 3 | 0 | 1 | 4 |

The supplied checkout already used `span_store.read()` in `span_for_memory`; the live daemon span-write convoy is absent in this baseline.

## Baseline per-call lane times

| Concurrency | Round | Call | hyb ms | corr ms | total ms | wall ms |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 1 | 1 | 1530 | 1507 | 1530 | 1533.2 |
| 1 | 2 | 1 | 1044 | 976 | 1044 | 1046.9 |
| 1 | 3 | 1 | 926 | 953 | 954 | 956.4 |
| 6 | 1 | 1 | 1569 | 901 | 1570 | 1573.4 |
| 6 | 1 | 2 | 1047 | 991 | 1048 | 1050.6 |
| 6 | 1 | 3 | 1023 | 840 | 1024 | 1026.0 |
| 6 | 1 | 4 | 1413 | 1579 | 1580 | 1582.1 |
| 6 | 1 | 5 | 1556 | 872 | 1556 | 1558.6 |
| 6 | 1 | 6 | 1301 | 1480 | 1481 | 1485.4 |
| 6 | 2 | 1 | 1158 | 942 | 1159 | 1162.5 |
| 6 | 2 | 2 | 1229 | 1252 | 1253 | 1255.1 |
| 6 | 2 | 3 | 1118 | 1154 | 1155 | 1157.9 |
| 6 | 2 | 4 | 1276 | 1058 | 1277 | 1280.1 |
| 6 | 2 | 5 | 1199 | 1126 | 1200 | 1203.5 |
| 6 | 2 | 6 | 1230 | 1094 | 1231 | 1234.5 |
| 6 | 3 | 1 | 1031 | 1059 | 1060 | 1062.6 |
| 6 | 3 | 2 | 1132 | 1235 | 1236 | 1237.7 |
| 6 | 3 | 3 | 1159 | 970 | 1160 | 1163.6 |
| 6 | 3 | 4 | 1249 | 1258 | 1259 | 1261.6 |
| 6 | 3 | 5 | 1276 | 1208 | 1277 | 1282.0 |
| 6 | 3 | 6 | 1203 | 935 | 1204 | 1207.2 |
| 12 | 1 | 1 | 1724 | 1425 | 1724 | 1726.4 |
| 12 | 1 | 2 | 1706 | 1387 | 1707 | 1711.4 |
| 12 | 1 | 3 | 1715 | 1575 | 1715 | 1720.8 |
| 12 | 1 | 4 | 1401 | 1453 | 1454 | 1459.9 |
| 12 | 1 | 5 | 659 | 809 | 810 | 2523.9 |
| 12 | 1 | 6 | 1739 | 1625 | 1740 | 1744.8 |
| 12 | 1 | 7 | 1735 | 1427 | 1736 | 1739.5 |
| 12 | 1 | 8 | 1287 | 1768 | 1768 | 1771.8 |
| 12 | 1 | 9 | 840 | 778 | 841 | 2558.4 |
| 12 | 1 | 10 | 887 | 802 | 888 | 2347.0 |
| 12 | 1 | 11 | 1534 | 1711 | 1712 | 1716.5 |
| 12 | 1 | 12 | 796 | 675 | 796 | 2518.4 |
| 12 | 2 | 1 | 1321 | 1736 | 1737 | 1740.4 |
| 12 | 2 | 2 | 1827 | 1892 | 1893 | 1897.2 |
| 12 | 2 | 3 | 1182 | 1629 | 1630 | 1633.9 |
| 12 | 2 | 4 | 1596 | 1607 | 1608 | 1613.2 |
| 12 | 2 | 5 | 851 | 680 | 851 | 2464.6 |
| 12 | 2 | 6 | 1589 | 1358 | 1589 | 1591.4 |
| 12 | 2 | 7 | 873 | 858 | 873 | 2465.4 |
| 12 | 2 | 8 | 793 | 748 | 794 | 2427.3 |
| 12 | 2 | 9 | 840 | 837 | 841 | 2488.8 |
| 12 | 2 | 10 | 1397 | 1702 | 1702 | 1706.1 |
| 12 | 2 | 11 | 1610 | 1644 | 1645 | 1646.7 |
| 12 | 2 | 12 | 1724 | 1528 | 1724 | 1726.2 |
| 12 | 3 | 1 | 1667 | 1647 | 1668 | 1673.1 |
| 12 | 3 | 2 | 1743 | 1497 | 1744 | 1749.8 |
| 12 | 3 | 3 | 1731 | 1593 | 1733 | 1736.7 |
| 12 | 3 | 4 | 1598 | 1644 | 1644 | 1648.2 |
| 12 | 3 | 5 | 862 | 803 | 862 | 2531.8 |
| 12 | 3 | 6 | 841 | 918 | 919 | 2567.2 |
| 12 | 3 | 7 | 1638 | 1813 | 1814 | 1818.1 |
| 12 | 3 | 8 | 880 | 827 | 880 | 2614.6 |
| 12 | 3 | 9 | 1680 | 2050 | 2051 | 2055.0 |
| 12 | 3 | 10 | 2063 | 1533 | 2063 | 2065.2 |
| 12 | 3 | 11 | 834 | 785 | 835 | 2461.4 |
| 12 | 3 | 12 | 1623 | 1528 | 1624 | 1628.0 |

## Final implementation and gates

- Deferred access batches: `get_memory` and content/metadata FFI reads do not acquire learner/state write guards or append to WAL. One maintenance batch preserves equal-millisecond counts and timestamps; snapshot and shutdown drain it. Completed drains sync off the read path.
- WAL append flushes to the OS. A separate 200 ms timer groups pending syncs; explicit durable boundaries remain synchronous. Detailed `health_check(details=true)` exposes pending and sync attempt counts.
- Span lookup was already read-only in this checkout and remains so; the new concurrency test holds its read guard while twelve readers complete.
- Turbo plans copy under a read guard, build off-lock, and atomically publish an Arc. Search uses the existing publication and scores changed vectors directly. Invalidated/stale plans cannot overwrite a newer embedding space.

Rust: full release command, **265 passed, 0 failed, 2 ignored**, 43.40 seconds. This includes an isolated child killed with SIGKILL, deterministic unsynced-tail truncation/repair, replay after append, exact-count V23 snapshot/restart, and concurrent read-lock tests. C++: **all 17 tests passed** across the full suite plus the initially skipped embedding-pool test rerun with the pinned model. Hooks: all 13 shell suites pass; MCP: 130 pass; SMRITI: 46 pass. Source hashes were frozen and verified unchanged.

Builds use reference CHITTA settings with worktree-local field/build paths, reference compilers, cached dependency sources, and pinned CPython. Generated identity matches the unchanged reference marker `768:nomic-embed-text-v1.5`; this checkout's wrapper does not create a local `.chitta-embed-identity` marker. The isolated MCP test HOME requires explicitly exposing the already-installed SDK 1.27.2; conda's 1.27.0 lacks the session ownership field. No SDK or binaries were installed.

All nested JSON key sets match before/after for recall, smart_recall, hybrid_recall, recall_keyword, and recall_lanes.

## After measurements

| Concurrent callers | Calls | Median hyb ms | Median corr ms | Median lane total ms | Median RPC wall ms |
|---:|---:|---:|---:|---:|---:|
| 1 | 3 | 524 | 473 | 572 | 575.2 |
| 6 | 18 | 94.0 | 90.0 | 96.5 | 99.1 |
| 12 | 36 | 82.5 | 83.0 | 94.5 | 105.0 |

The 12-caller lane median improved from 1644.5 to 94.5 ms (94.3%); the <1200 ms target passes. Single-call totals were 1530/1044/954 ms before and 587/572/380 ms after. Calls run in 1→6→12 order, so the first rounds include cache warming; these figures are not a pure scaling curve.

| Hook arm | Runs per build | Before median / p95 ms | After median / p95 ms | Empty outputs before / after |
|---|---:|---:|---:|---:|
| off | 15 | 1113 / 6031 | 788 / 1803 | 0 / 0 |
| on | 15 | 1210 / 2094 | 871 / 4268 | 0 / 0 |

Both unchanged benchmark invocations exited successfully. The on-arm p95 regressed in this run despite its improved median; no tail-latency gain is claimed. Individual hook failures are masked by the unchanged benchmark, as noted above.

## Active after stacks and syscall evidence

The original 100 ms delayed samples caught idle workers because after calls finished so quickly. They are retained as raw artifacts but are not evidence of active recall. A separate sustained run maintained twelve callers across three fresh samples (570 total completed calls).

| Active sample | lock_shared_slow | fdatasync | ensure_turbo | Recall-lane frames |
|---:|---:|---:|---:|---:|
| 1 | 1 | 0 | 0 | 15 |
| 2 | 0 | 0 | 0 | 7 |
| 3 | 1 | 0 | 0 | 24 |

Sustained run actually completed 570 calls. Two samples each contain one isolated shared-lock wait in recall_semantic_ctx, with no convoy; none is in span_for_memory. No active sample contains fdatasync or ensure_turbo.

A separate `strace -ff -e trace=fdatasync` run over twelve concurrent recalls observed **zero recall-worker syncs**. It observed one sync on `chitta-maint` and three on `chitta-wal`; periodic background work is kept visible. The status counter changed 28→29 around a single call plus detailed-status requests and 29→33 around the traced interval; pending counts were 0/1/1. These aggregate counters include background work and must not be interpreted as per-call sync counts. The unit/FFI tests independently assert zero WAL appends and syncs during getter calls.

## After per-call lane times

| Concurrency | Round | Call | hyb ms | corr ms | total ms | wall ms |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 1 | 1 | 524 | 586 | 587 | 590.1 |
| 1 | 2 | 1 | 572 | 473 | 572 | 575.2 |
| 1 | 3 | 1 | 379 | 368 | 380 | 382.6 |
| 6 | 1 | 1 | 116 | 174 | 183 | 185.4 |
| 6 | 1 | 2 | 172 | 99 | 176 | 178.4 |
| 6 | 1 | 3 | 176 | 171 | 181 | 186.2 |
| 6 | 1 | 4 | 213 | 170 | 214 | 216.7 |
| 6 | 1 | 5 | 103 | 172 | 178 | 181.8 |
| 6 | 1 | 6 | 106 | 170 | 176 | 186.0 |
| 6 | 2 | 1 | 74 | 87 | 89 | 92.4 |
| 6 | 2 | 2 | 75 | 108 | 109 | 111.4 |
| 6 | 2 | 3 | 87 | 87 | 88 | 91.2 |
| 6 | 2 | 4 | 75 | 74 | 75 | 77.8 |
| 6 | 2 | 5 | 108 | 86 | 109 | 111.0 |
| 6 | 2 | 6 | 74 | 74 | 75 | 77.3 |
| 6 | 3 | 1 | 91 | 87 | 94 | 96.5 |
| 6 | 3 | 2 | 95 | 96 | 97 | 99.2 |
| 6 | 3 | 3 | 86 | 82 | 88 | 90.9 |
| 6 | 3 | 4 | 73 | 87 | 91 | 93.2 |
| 6 | 3 | 5 | 94 | 93 | 96 | 99.0 |
| 6 | 3 | 6 | 94 | 87 | 96 | 99.0 |
| 12 | 1 | 1 | 68 | 67 | 69 | 157.4 |
| 12 | 1 | 2 | 81 | 93 | 96 | 98.2 |
| 12 | 1 | 3 | 97 | 96 | 97 | 102.8 |
| 12 | 1 | 4 | 85 | 98 | 99 | 102.0 |
| 12 | 1 | 5 | 84 | 96 | 97 | 99.6 |
| 12 | 1 | 6 | 69 | 68 | 70 | 156.4 |
| 12 | 1 | 7 | 85 | 84 | 86 | 89.1 |
| 12 | 1 | 8 | 83 | 70 | 84 | 182.3 |
| 12 | 1 | 9 | 83 | 82 | 84 | 88.5 |
| 12 | 1 | 10 | 84 | 82 | 84 | 185.0 |
| 12 | 1 | 11 | 84 | 96 | 97 | 101.7 |
| 12 | 1 | 12 | 82 | 109 | 111 | 116.6 |
| 12 | 2 | 1 | 93 | 76 | 94 | 100.1 |
| 12 | 2 | 2 | 96 | 109 | 110 | 113.6 |
| 12 | 2 | 3 | 110 | 86 | 111 | 113.7 |
| 12 | 2 | 4 | 64 | 90 | 90 | 187.1 |
| 12 | 2 | 5 | 93 | 74 | 95 | 99.3 |
| 12 | 2 | 6 | 68 | 67 | 68 | 162.1 |
| 12 | 2 | 7 | 73 | 99 | 100 | 109.5 |
| 12 | 2 | 8 | 75 | 75 | 76 | 78.5 |
| 12 | 2 | 9 | 66 | 66 | 67 | 160.8 |
| 12 | 2 | 10 | 75 | 81 | 83 | 92.6 |
| 12 | 2 | 11 | 67 | 67 | 68 | 146.0 |
| 12 | 2 | 12 | 76 | 93 | 95 | 105.4 |
| 12 | 3 | 1 | 78 | 97 | 98 | 104.6 |
| 12 | 3 | 2 | 80 | 80 | 81 | 83.8 |
| 12 | 3 | 3 | 67 | 73 | 74 | 174.2 |
| 12 | 3 | 4 | 79 | 78 | 80 | 83.8 |
| 12 | 3 | 5 | 93 | 95 | 106 | 112.4 |
| 12 | 3 | 6 | 97 | 96 | 98 | 100.7 |
| 12 | 3 | 7 | 91 | 70 | 91 | 172.5 |
| 12 | 3 | 8 | 96 | 94 | 97 | 100.5 |
| 12 | 3 | 9 | 71 | 95 | 97 | 177.6 |
| 12 | 3 | 10 | 94 | 94 | 96 | 101.0 |
| 12 | 3 | 11 | 67 | 69 | 70 | 167.4 |
| 12 | 3 | 12 | 94 | 76 | 95 | 102.1 |

Final scratch daemon shut down cleanly after SIGTERM; the baseline hit its existing shutdown watchdog. Rust submodule commit: `f113ea8`. Untracked `.read-path/` and `chitta/build-read-path/` retain local evidence/build products for review.
