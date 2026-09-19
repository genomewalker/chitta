# Build cache: one environment, isolated outputs

Status as of 2026-09-19: **IMPLEMENTATION IN PROGRESS — lead SIGN-OFF 2026-09-19 09:55.**
Design ad378f04 accepted with per-node sccache, a 600 s primed-node target,
separate unavailable/drift contract results, and p22 ownership of baseline.py.
Nightly scheduling is documented but has not been enabled.

## Baseline and measurement definitions

The supplied `feat/build-cache` worktree was clean except for its launcher lock,
with HEAD and origin/main both `a820c9675b9ca0567e22d776a90121264455388d`.
Its initialized chitta-field submodule was
`ec22685fc7cc83632f193b0c3ceb79823d342fa0`. Neither `chitta/build` nor
`chitta-field/target` existed; neither ccache nor sccache was installed.
This is the fresh origin/main checkout used for the baseline, not a reused build.

Measurements run through `scripts/on-compute.sh -c 16 -m 64G`, with full logs in
`/projects/caeg/scratch/kbd606/tmp/p23-build-cache-62pl7ypq`.
The retained `measure.sh`, `environment.txt`, and `timings.tsv` record commands,
node/toolchain identity, seconds, exit status, and maximum RSS in KiB.
Native builds use the bioinfo conda g++, Rust 1.93 through the existing wrapper,
16 build workers, single-threaded BLAS, Release, 768 dimensions, and the existing
FetchContent source cache. llama.cpp remains off for gate-equivalent builds.
Rust flags retain `target-cpu=x86-64-v4`, release LTO, and one codegen unit.
Configure follows the Rust build because CMake requires its static library.

“Cold” below means empty worktree outputs, with compiler caches disabled.
The Cargo registry/toolchain and FetchContent sources are already available;
this does not measure downloads or first-time tool installation. “Incremental”
means the identical command repeated in the SAME worktree, with outputs retained.
It is not evidence for compiler-cache reuse across fresh worktrees. Queue delay
is excluded from step timings; scheduled runtime and queue delay must be reported
separately for the later end-to-end acceptance run.

| Step | Fresh outputs, no compiler cache (s) | Same-worktree incremental (s) | Fresh outputs, warm compiler cache (s) |
| --- | ---: | ---: | ---: |
| CMake configure, FetchContent sources cached | 29.07 | not measured | step 3 |
| Rust release build | 274.91 | 2.10 | step 3 |
| C++ release build | 152.94 | 2.02 | step 3 |
| ctest (37/37 passed) | 25.85 | not measured | step 3 |
| Complete gate-full (FAIL: contract service unavailable) | not measured as a cold aggregate | 793.62 | step 3 |

The lead's earlier observations (not re-labelled as measurements here) are:
configure 2247 s before the FetchContent fix and 14 s after; Rust release
1800–2220 s and approximately 8 GB of target outputs; C++ about 150 s;
ctest 37 tests about 30 s. A sum of isolated build timings is not a cold full-gate
measurement: Rust test compilation, quick checks and hook suites add work.
Both fresh-worktree cold-cache and warm-cache gate totals belong to step 3.

Measured node: `dandycmpn22fl.unicph.domain`; GCC 14.3.0, Rust 1.93.0,
CMake 3.26.5. Every native stage exited zero. The Rust log records compilation
and a 4m34s release build with initially absent target outputs. Its 274.91 s
cold time is substantially below the lead's earlier 30–37 minute observation.
The lead clarified at sign-off that the earlier 30–37 minute figure was
FetchContent cloning, not Rust compilation; 274.91 s replaces that estimate. Do not substitute the earlier estimate for this measurement or
attribute the improvement to an uninstalled compiler cache. Native stages total
482.77 s, excluding the incremental probes and all additional full-gate work.

Two full-gate attempts were rejected and cancelled after the measurement
wrapper interfered with fixtures. The first used a deeply nested TMPDIR that
exceeded Unix socket path limits. The second fixed that path but inherited a
placeholder CHITTA_SOCKET_PATH, causing the daemon-isolation fixture's health
check to query the wrong socket. Both also had a nonexistent CHITTA_BIN; the
verified local CLI is `bin/chitta`, not `chitta/build/chitta`. Their elapsed
times are not performance evidence and neither failure is attributed to caches.

The final runner uses the short scratch root for ctest fixtures, `bin/chitta`,
single-threaded BLAS, private HOME/runtime state, and no inherited socket,
mind or queue override. Its exact command is retained in `run-gates-verified.sh`;
output and timing files have `verified` in their names. Rust test artifacts
compiled during the rejected attempts are retained, so the final complete-gate
result is incremental for both release and test outputs, not a fresh
test-compilation measurement. The final full gate exited 1 after **793.62 s**
(13m13.62s): Rust tests, all 37 ctests, and all 41 shell hook suites passed;
the nested quick gate failed only its live-daemon contract probe. This is a
completed failing gate, not a gate-clean timing. The separate quick gate passed
in 48.99 s with that unavailable probe skipped; the daemon-node contract check
reported `contracts unchanged`. Logs are retained outside the repository.

The final baseline full gate runs inside a compute allocation with
`CHITTA_ON_COMPUTE=0` for its inner dispatches, avoiding nested allocations.
Test HOME is private under the log directory; runtime/socket paths are short
and private, and individual fixtures own their mind directories;
Rust and hook fixtures use node-local /tmp. The generic janitor's age threshold
is raised to prevent this measurement from deleting unrelated scratch state.
The isolated quick gate skips its unavailable CLI contract probe; a separate
`contract-snapshot.sh check` against the live read-only schema reports
`contracts unchanged`. These isolation controls do not change native build flags.
The full gate reports a contract failure in that isolated compute environment:
the local CLI exists, but the private HOME has no live daemon or daemon-node
routing marker. The contract script requires a reachable daemon and exits 1
without one; gate-quick labels any such failure "contract drift". The separate
read-only daemon-node check passes. Do not report the full gate as passing or
infer schema drift from that generic label. Step 2 must distinguish unavailable
contract service from an actual snapshot mismatch and retain the diagnostic.

The existing full gate suppresses some detailed test output and uses grepped
success markers; its reported verdict is the current script's result, with those
known diagnostic limits. Step 2 replaces these with actual stage exit statuses.

## One sourced environment and one native recipe

Add `scripts/build-env.sh`, sourced using a path derived from the caller's
location rather than its current directory. Sourcing is idempotent: deduplicate
PATH/library entries, preserve explicit valid overrides, and do not configure,
compile, install, prune, or stop processes. Export resolved values and provide
small environment/step-runner functions. The native recipe lives once in
`scripts/build.sh`: Rust release build, Rust release tests, CMake configure,
C++ build and ctest. Gate-full calls that recipe; development callers can select
its build-only stages. CMake is reconfigured every time (cheap with cached
sources), avoiding half-configured directories and stale launcher settings.

| Setting | Resolution and policy |
| --- | --- |
| PATH / CHITTA_PY | Preserve caller choices; prefer the verified bioinfo CPython and conda tools, user cache executables, and the pinned Rust toolchain. Retain python-with-mcp as the single Python capability probe. No installation while sourcing. |
| CXX / CC | Explicit override first; otherwise conda x86_64-conda-linux-gnu-g++ and matching gcc here, system compiler fallback elsewhere. Pass CXX to CMake explicitly. |
| LIBRARY_PATH / LD_LIBRARY_PATH | Add the existing conda lib directory once when present; preserve caller entries. Record the resolved compiler and library paths in the gate log. |
| Rust linker / Python | Preserve the existing /usr/bin/gcc Rust linker, ar, pinned Rust 1.93 selection and PYO3_PYTHON. The submodule wrapper sources the parent recipe when present, retaining its standalone fallback outside this repository. |
| CHITTA_SCRATCH_TMP | Persistent scratch root, default /projects/caeg/scratch/kbd606/tmp here; portable user-selected fallback off this cluster. |
| TMPDIR | A private writable directory beneath the compute allocation's node-local temporary root for compiler scratch and unit fixtures. Never use it to select replica storage or durable logs. Respect explicit valid TMPDIR and make on-compute preserve it. |
| GATE_TMP | An explicitly supplied or newly created private directory under CHITTA_SCRATCH_TMP; exported across compute dispatch. Every build/test/gate log lives here. |
| CHITTA_DEPS_CACHE | Preserve the a820c967 FetchContent mechanism; default to scratch/cmake-deps only if present. Missing sources follow normal FetchContent behavior, including possible offline download failure. |
| RUSTC_WRAPPER | Absolute sccache executable when available and its cache probe succeeds; leave a valid explicit caller wrapper alone. CARGO_INCREMENTAL=0 in the cached profile. |
| SCCACHE_DIR / SCCACHE_CACHE_SIZE | Proposed scratch/build-cache/sccache/<node>/v1, 20G per node. The node suffix is a required review decision explained below; the unsuffixed multi-server directory is unsafe. |
| CMAKE_CXX_COMPILER_LAUNCHER | Absolute ccache when available, passed explicitly by the recipe and defaulted from the environment by CMake launcher wiring. Respect an explicit custom launcher. Also apply ccache to C grammar compilation through CMAKE_C_COMPILER_LAUNCHER. |
| CCACHE_DIR / CCACHE_MAXSIZE | Shared scratch/build-cache/ccache, 10G. Keep cache temp files node-local. Compiler identity uses content checking. |
| Worktree outputs | chitta-field/target and chitta/build remain private to each worktree. Do not export a shared CARGO_TARGET_DIR. |

A shared Cargo target directory is rejected: Cargo's directory lock serializes
concurrent worktrees and a stream can poison another stream's artifacts. Cache
entries are reusable compiler results, not shared mutable build outputs. Explicit
CARGO_TARGET_DIR values pointing outside the worktree are rejected by managed
gates, not silently accepted; standalone development remains caller-controlled.
Neither compiler cache eliminates private output disk usage: the approximately
8 GB target cost remains a separate retention issue.

Wire gate-quick, gate-full, codex-stream worktree setup, dev-install and the native
recipe to source build-env. Source again on the compute node: hostname, available
scratch and server identity cannot be decided on the login node. codex-stream
only prepares its environment and checkout; it does not eagerly compile.
Dev-install currently links plugin assets and explicitly leaves binaries alone;
sourcing the environment must not turn it into an implicit deployment.

`benchmarks/storage/baseline.py` belongs to p22 and is not edited here.
`scripts/baseline-build-env.sbatch` supplies the common environment to that
harness; submit it from the harness worktree after p22 lands.

CLAUDE.md's Build & deploy section gets a two-line pointer with a “Status as of”
stamp. Document direct, gate, stream and baseline invocations in the build docs.
No hooks/*.sh changes; update docs/HOOKS.md only if the reviewed implementation
changes the environment actually inherited by hooks.

## Shared-cache correctness and the required review decision

### sccache local storage is not a multi-server NFS backend

Upstream explicitly restricts a local disk cache to one sccache server at a time;
multiple servers can race and produce spurious build failures. NFS visibility
does not supply the missing coordination. A filesystem flock held for an entire
build would serialize streams and defeat the stated concurrency objective.
See [sccache local storage](https://github.com/mozilla/sccache/blob/main/docs/Local.md).

**Proposed safe first implementation:** one node-specific cache under the required
scratch/build-cache/sccache root, owned by one per-user, per-node sccache server.
A stable node-local Unix socket isolates it from unrelated default sccache
servers. Scope path-normalization inputs per compiler request; a worktree-specific
server setting must not be captured by the first caller and reused for others.
All worktrees on that node use the same server/configuration; startup is
serialized briefly, compilation is not. Never stop another stream's server.
Set a finite idle timeout and make cache directory, size and tool version stable
for that socket. Detect incompatible existing server configuration and fall back
to uncached compilation with an explicit diagnostic rather than restarting it.
Cache/server startup belongs to a build invocation, never to merely sourcing env.

This preserves worktree concurrency and NFS-resident cache persistence, but a
newly used node is cold. Therefore a <600 s warm claim applies only to a node
whose shard was primed. **Lead review must accept this narrower placement rule,
or select a supported shared remote backend as additional scope.** A universally
warm arbitrary-node guarantee with one shared local SCCACHE_DIR is not claimed.
A remote backend requires separate operational ownership and measurements;
it is not smuggled into this stream as a service deployment.

### Paths, compiler identity and artifacts

Use worktree-relative compiler invocations and the pinned cache version's
supported base-directory normalization. For ccache use CCACHE_BASEDIR at the
canonical worktree root and preserve correctness checks. For Rust first verify
cross-worktree cache hits on dependencies and chitta-field independently; do not
assume SCCACHE_BASEDIRS necessarily fixes every Rust argument or captured path.
If remapping is needed, merge remap-path-prefix with existing target-cpu and
caller flags rather than replacing Cargo rustflags. Do not strip semantic path
inputs or disable correctness checks merely to manufacture hits. Debug/source
path remapping must be explicit and identical across callers.

The installed sccache version must support the selected normalization and socket
options; pin versions and download checksums in the install instructions after
review. The dependency fingerprint includes compiler version/content, target,
features, optimization settings and source/header inputs. Require misses after
source, header, feature or toolchain changes. Preserve embedding dimension/model
identity and compare the existing identity marker when one is produced.

Rust crates that invoke the system linker, including test executables, build
scripts and proc-macros, are not wholly cacheable; sccache also requires Rust
incremental compilation to be disabled. The staticlib/rlib root crate, native
build scripts, release LTO and test linking need measured hit/miss breakdowns;
none is assumed free. [Rust support limitations](https://github.com/mozilla/sccache/blob/main/docs/Rust.md)
and [configuration](https://github.com/mozilla/sccache/blob/main/docs/Configuration.md)
are implementation references, not performance evidence.

### NFS skew, simultaneous builds and failures

| Failure mode | Handling and proof |
| --- | --- |
| Fixture environment | Use a short runtime/socket path (within Unix socket limits), a real locally built CLI for replica startup, and isolated fixture state. Do not force global socket/mind/queue values into tests that create their own fixtures. A deliberately missing CLI may skip a schema probe but must not be exported across the whole native gate. |
| NFS mtime/ctime skew | Keep ccache's conservative fresh-file checks; disable inode-based hash reuse on NFS and avoid file_stat_matches/include-mtime sloppiness. Use node-local compiler temp files. Verify a changed header with preserved timestamp still invalidates. A conservative miss is acceptable. |
| sccache multiple servers / NFS locking | Node shards plus one stable server/socket per user/node; no shared-directory multi-server writes. Test simultaneous worktrees on one node and separate shards on two nodes. |
| Same crate compiled by two worktrees | Separate targets and intermediates; only completed cache entries are reusable. Duplicate misses may do duplicate work. Neither worktree waits for the other's Cargo target lock. Validate outputs independently. |
| Cache executable absent | Leave launcher/wrapper disabled; print cold mode once. Missing optional cache tools are not a build failure and trigger no installation. |
| Cache inaccessible/full or server unavailable | Probe before enabling; use documented cache-error fallback where supported. Otherwise retry the affected stage once uncached and retain both logs. Preserve genuine compiler/test failures. Performance acceptance fails if fallback occurs. |
| Existing CMake directory captures old launcher | Always rerun configure with resolved launcher, including an empty launcher when disabled. Never infer configuration success from CMakeCache.txt alone. |
| Shared FetchContent sources changed | Keep a820c967 behavior; streams only consume existing sources. Orchestrator refreshes when readers are quiescent and records dependency revisions. Existing cache pin-validation limitations remain visible. |
| Hidden compiler error in filtered logs | Preserve exit status before displaying a tail. Rust build, Rust tests, configure, C++ build, ctest and each hook suite have distinct verdicts. No success based on grepping “ok”. |

ccache documents possible NFS performance penalties and recommends local
temporary files; this layout must be benchmarked on the actual mount.
See the [ccache manual](https://ccache.dev/manual/4.14.html).

## Diagnostics, tests and hygiene

Each gate stage records command, start/end time, status and full stdout/stderr
under GATE_TMP; console output is bounded. Failure format:
`FAIL: <stage> (exit <code>; <absolute-log-path>)`.
No truncating tee /dev/stderr, discarded configuration log, pipeline-status
masking, or success inferred from a log tail. gate-quick uses the same logger.
Run Rust and hook fixtures with isolated state and node-local TMPDIR; keep
replica copies pinned under scratch with their existing eval-replica guard.
The contract snapshot remains a separate read-only check against the daemon's
schema when compute-node test isolation makes the live daemon unavailable.

Cache tools enforce the normal size limits: 20G per sccache node shard and 10G
ccache. Cap aggregate retained sccache shards at 80G as an operational target;
active shards may temporarily exceed it, which is reported rather than forcibly
pruned. The orchestrator owns periodic cleanup, tool upgrades and FetchContent
refresh. Streams never recursively delete a shared cache. tmp-janitor must
explicitly exclude build-cache roots from generic age-based deletion and expose
an opt-in cache maintenance mode that is dry-run by default. It may prune an
inactive node shard only after verifying its server is idle/stopped and no
active build lease refers to it; uncertainty means skip. Keep tool-native ccache
cleanup, and prune inactive shards older than 14 days to meet the aggregate cap.
Do not infer inactivity from /proc on a different host. Logs report sizes,
pruned/skipped entries, and the reason. No automatic removal of worktree targets.

After SIGN-OFF, install sccache via `cargo install sccache --locked --version <pin>`
into ~/.cargo/bin and a verified static ccache release into ~/.local/bin.
Build the installer on compute, use install rather than cp over a binary, and
record exact versions. The task's explicit step-2 permission applies only after
review; no system packages, sudo, live-daemon restarts, or ~/.claude installation.

## Acceptance and the later proof run

1. On 16 allocated CPU cores / 64 GB, checkout plus submodule preparation through
   a successful full gate takes **<600 s** with fresh private target/build
   directories and a primed cache on the same node. Include quick checks, Rust
   build AND tests, configure, native build, ctest and all hook suites. Record
   scheduler wait separately. A zero-exit verdict and retained full logs are
   required; skipped contracts must be covered by the separate daemon-node check.
2. Record both a fresh-worktree cold-cache full gate and a DIFFERENT fresh-worktree
   warm-cache full gate at the same branch/submodule commits. A second gate in
   the same checkout is only an incremental control. Never clear production
   shared caches to make a cold case; use an isolated benchmark namespace.
3. Provisional component budgets: configure <=30 s, Rust build plus test
   compilation/execution <=360 s, C++ build <=90 s, ctest <=40 s, remaining
   checkout/quick/hooks <=80 s. These sum to 600 s; the strict total must be below
   600, so component ceilings alone are insufficient. Revise allocations only
   with measured evidence, not by dropping tests or weakening release flags.
   These are targets, not a demonstrated feasible allocation: the incremental
   baseline takes 793.62 s (and fails only its contract probe). Compiler
   caches cannot shorten existing no-op builds or fixture waits. Review must
   account for that gate overhead before promising the end-to-end target; any
   required hook changes would need a separately authorized stream.
4. Show cache requests/hits/misses/non-cacheable reasons, cache bytes, target/build
   bytes, toolchain hashes and the node per run. Measure Rust and C++ separately;
   cache hit rates are diagnostic, elapsed full-gate success is the criterion.
5. Two simultaneous fresh worktrees must pass with no target-lock serialization
   or cross-worktree artifacts. A source/header edit must miss appropriately.
   Absent tools and unwritable cache tests must still produce correct cold
   builds and explicit cold-mode diagnostics. NFS/cache errors cannot be hidden
   by a successful-looking tail.
6. Run gate-quick per commit, contract-snapshot check at commit time, and gate-full
   once at completion. No replica is required for this documentation-only step.
   Later launcher/environment changes do not authorize live-mind experiments.

Step 3 adds scripts/nightly-gate.sbatch and documents a **disabled** systemd
user timer on the daemon node. Its submission job creates a unique fresh
origin/main worktree with the pinned submodule, sources the same environment,
runs full gate on CPU compute, and writes a complete dated verdict to
/projects/caeg/scratch/kbd606/tmp/nightly-gate/<date>.log (including commit, node,
queue/runtime, cache stats and exit status). Use a non-overlap lock and unique
per-run logs before publishing the dated result. Configure success and failure
verdicts explicitly; do not infer completion from a vanished PID. Scheduling,
server lifecycle, retention and removal of completed nightly worktrees remain
orchestrator responsibilities. This stream does not enable the timer.

## Review boundary

Step 1 ends after the baseline, gates, documentation commit and handoff.
The cache-directory safety/placement decision, 793.62 s incremental gate
overhead, unavailable compute contract probe, and missing storage baseline
integration are explicit review items. The lead must append SIGN-OFF to the
stream specification before implementation begins. No cold/warm cache result
or <10 minute guarantee is claimed before the step-3 experiment.

## Implementation and operations (2026-09-19)

`build-env.sh` is source-only until `chitta_log_init` or `chitta_build_init` is
called. `build.sh --tests` owns the native release recipe. Gate scripts,
`codex-stream.sh`, `dev-install.sh`, `on-compute.sh` and the Rust wrapper source
the environment. p22 owns `benchmarks/storage/baseline.py`; it is unchanged.
Submit `sbatch scripts/baseline-build-env.sbatch <baseline arguments>` from the
checkout to supply that harness with the same environment.

Installed for this evaluation: sccache 0.18.0 via `cargo install --locked` and
ccache 4.14 static musl release (SHA256
`985f575acf84cf6d70e0f4fe86903418df9e7bce11f35faaf8d415e5ff5a9478`).
The tools are user-local, outside the live plugin. No service was restarted.
[Rust caching restrictions](https://github.com/mozilla/sccache/blob/v0.18.0/docs/Rust.md)
mean final links and proc-macro crates still compile. `SCCACHE_BASEDIRS` is
server-scoped; do not set it separately per worktree sharing the stable socket.
Targets remain private and compiler flags are unchanged.

Every gate step writes its full diagnostic and exit status under `GATE_TMP`.
`timings.tsv` records seconds and status; each hook has its own log. Contract
unavailability exits 3 and is deferred by quick to full's required daemon-node
check; schema drift exits 1. Remote transport failures retain their diagnostic.
The actual full-gate result must pass before claiming acceptance.

`scripts/tmp-janitor.sh --cache-maintenance --dry-run` reports cache usage.
Only the orchestrator should invoke `--apply`. ccache performs its own bounded
cleanup. Unknown/active remote sccache shards are retained, even after 14 days;
an 80 GiB aggregate breach is reported for orchestrator action after quiescing
remote servers. The ordinary janitor excludes cache and compiler-temp roots.

### Nightly scheduling (not enabled)

Run from a daemon node with Slurm access, after merging the implementation.
The batch job fetches origin/main, creates a fresh detached worktree and runs
full gates. It retains the worktree, logs, node identity and exit status under
`/projects/caeg/scratch/kbd606/tmp/nightly-gate/<date>.log`. It primes the node
chosen by Slurm; warming one node makes no claim about another.

Example user units, to be installed and enabled by the orchestrator only:

```ini
# ~/.config/systemd/user/chitta-nightly-gate.service
[Unit]
Description=Fresh main checkout build and full gate
[Service]
Type=oneshot
WorkingDirectory=/projects/fernandezguerra/apps/repos/cc-soul
Environment=CHITTA_NIGHTLY_REPO=/projects/fernandezguerra/apps/repos/cc-soul
ExecStart=/usr/bin/env sbatch --wait --export=ALL scripts/nightly-gate.sbatch

# ~/.config/systemd/user/chitta-nightly-gate.timer
[Unit]
Description=Nightly build cache and gate check
[Timer]
OnCalendar=*-*-* 02:30:00
RandomizedDelaySec=15m
Persistent=true
[Install]
WantedBy=timers.target
```

On that node the orchestrator may run `systemctl --user daemon-reload` followed
by `systemctl --user enable --now chitta-nightly-gate.timer`. These commands have
not been executed by this stream. Retained nightly worktrees/logs require the
orchestrator's retention policy; do not delete a worktree running a gate.

### Additional baseline and proof status

The pre-change overhead runner measures quick checks, release Rust tests and
each hook suite independently on a compute node, before applying implementation.
Its stage timings and logs are under
`/projects/caeg/scratch/kbd606/tmp/p23-cache-impl.h8ERCJ`.
The first tracing attempt used an overlong Unix socket runtime path and is
excluded. The corrected runner uses a private short node-local runtime path.
Fresh cold-cache and fresh warm-cache full-gate proof remains pending; no
sub-600-second or gate-clean acceptance claim is made yet.

### Continuation diagnosis (2026-09-19)

The failed implementation run on dandycmpn22fl used Rust 1.93.0 and the
bioinfo conda C++ compiler, with TMPDIR=/tmp (confirmed by the failing Rust
store path). Its original HOME was not recorded. Logs are retained under
`/projects/caeg/scratch/kbd606/tmp/p23-cache-impl.h8ERCJ/full-implementation`.

| Failure | Evidence and correction |
|---|---|
| Eight chaos fixtures abort before daemon startup | Captured fixture stderr: `Invalid embedding dimension: expected 768, got 1024`. The gate passed 768 to CMake while Rust defaulted to 1024. Export CHITTA_EMBED_DIM=768 to both, preserving the gate dimension. |
| Shared/long runtime paths | build-env allocates private `/tmp/cb.XXXXXX` per node and reuses it in nested gates; durable test stores and logs remain on shared scratch. |
| Rust reopen lock race | `field.rs:1984` returned WouldBlock for `/tmp/chitta-chaos-351501-snapshot/.instance.lock`, recorded holder 351501 on dandycmpn22fl. The immediate uncached full rerun passed 303 tests. Three isolated reruns on the same node with Rust 1.93.0 and the original TMPDIR=/tmp all pass. A second set under private TMPDIR=/tmp/cb.hpHOGu also passed 3/3. The full suite then failed in `ffi::tests::test_ledger_session_snapshot_and_wal_suffix` at ffi.rs:2810 with the same self-holder WouldBlock (302 passed, one failed; 27.34 s). Root cause remains unproven and is not attributed to compiler caching. |

Pre-change overhead: 41 sequential hook suites total 620.93 s;
quick 59.46 s (failed contract/citation checks), corrected release Rust tests
34.10 s. The initial Rust test timing (5.10 s, exit 127) is invalid.
No tests, parallelism settings or optimization flags are removed to meet 600 s.

The six isolated passes do not explain or fix the intermittent full-suite lock
failure. Concurrent child creation temporarily retaining a parent flock is a
hypothesis from the two process-spawning tests, not an established cause.
This remains a store-test blocker outside the authorized build.sh-only Rust
scope. Test errors are no longer retried as cache failures. No gate-clean
acceptance claim is permitted while this remains unresolved.

### Saved fresh-worktree proof (job 22916137)

Both worktrees used parent 1dc664c3 and submodule 96bccb4 on dandycmpn22fl,
Rust 1.93.0, conda GCC 14.3.0, private runtime directories from build-env,
and a shared private compiler cache. Queue time was not captured. Results and
full stage logs: `/projects/caeg/scratch/kbd606/tmp/agent_7WR1yO/proof`.

| Stage | Cold compiler cache (s) | Warm compiler cache, fresh outputs (s) |
|---|---:|---:|
| Quick | 61 | 55 |
| Rust release | 252 | 246 |
| Rust tests, including compilation | 264 | 262 |
| Configure | 35 | 29 |
| C++ | 164 | 10 |
| CTest | 23 | 23 |
| 41 hook suites | 622 | 630 |
| Full gate, both FAIL | 1441 | 1273 |

Both Rust suites passed. Both CTest runs passed 36/37, failing only
code_navigation_test at line 97: CCACHE_BASEDIR made __FILE__ relative, breaking
its source-relative fixture lookup. The corrected environment leaves BASEDIR
empty to preserve compiler semantics. Cross-worktree hit rates must be measured
again; correctness takes priority over path normalization.

Subprocess_load_test passed in 7.32 s cold and 6.70 s warm. The earlier 300 s
failure occurred alongside the proven 768/1024 Rust/C++ dimension mismatch and
shared TMPDIR; it did not recur after the committed dimension/runtime correction.
These runs do not isolate which environment change cured the timeout. The Rust
self-holder lock race remains unexplained despite six isolated passes and these
two full-suite passes; it is not considered fixed by caching.

The hook CXX wrapper now invokes the real compiler through ccache and is also
available as g++ on PATH. CMake uses the real compiler plus its existing launcher
to avoid applying ccache twice. Full gate initializes caches before spawning
native and hook stages, and prints counters before and after hooks. No hook test
sources, optimization flags, or test parallelism are changed.


### Resume after main v5.73.0 (2026-09-19)

The saved 5c32c4be fresh-output warm gate passed in 1536 s on dandycmpn22fl:
quick 64, Rust release 272, Rust tests including compilation 301, configure 32,
C++ 160, CTest 24. The focused hook run still took 656 s warm. Logs are retained
under `/projects/caeg/scratch/kbd606/tmp/agent_vqexmuuq`.
This is a passing gate, but it fails the 600 s performance target.

The initial executable wrapper was insufficient: fixture invocations combine
compilation and linking, which ccache classifies as a link call. The wrapper now
splits supported single-source invocations into cached object compilation and
ordinary linking. It preserves source paths, flag values and link argument order;
unknown flags and multiple sources fall back unchanged. Temporary objects remain
in the private runtime directory. Hook sources and optimization flags are unchanged.
Job 22916937 completed all 41 suites on dandycmpn22fl with no failures:

| Hook measurement | Seconds | Result |
|---|---:|---|
| Previous wrapper, warm | 656 | PASS |
| Split compilation wrapper, priming pass | 283 | PASS |
| Split compilation wrapper, warm pass | 147 | PASS |

The warm reduction is 77.6%; the expected <90 s is not achieved. The largest
remaining suites are eval_replica (39 s), saddle_hook (16 s), saddle_parity
(13 s), and pretool_hardstop (8 s). These are suite totals, not isolated compile
times. No further performance floor is asserted without the full-gate results.
Focused logs and timings are under
`/projects/caeg/scratch/kbd606/tmp/agent_djZgaH/focused`.

A separate compute regression (job 22916938) passed: a repeated supported
compile produces a cache hit; source paths and define values are preserved;
unsupported flags and multiple sources fall back correctly; compiler errors
propagate. This regression uses a private cache, independent of the hook numbers.

The incoming main submodule adds InstanceLock::drop with explicit LOCK_UN, which
releases locks even while forked children retain an inherited descriptor. This
addresses a plausible cause of the previously observed self-holder reopen race;
a passing rerun alone is not proof that the historical failure had that cause.

Merging origin/main 47970ed7 exposes existing release-check failures: the tracked
plugin contract records 5.72.0 while the manifest records 5.73.0; the site footer
checker also rejects the release footer/version/date inconsistencies. These are
retained failures, not cache fallback or skipped coverage. The contract snapshot
is outside this stream's write scope and has not been regenerated.

A scratch-filesystem lock experiment reproduced the descriptor mechanism:
closing the original descriptor while a duplicate remains blocks reopen;
explicit LOCK_UN before closing permits reopen. The incoming Rust regression
`instance_lock_drop_unlocks_duplicate_descriptors_and_failed_open` exercises
that release path. This supports the mechanism, without proving the exact
historical trigger.

Detached proof job **22916939** is pinned to parent 35b16c86 and submodule
7ec4554 on dandycmpn22fl. It uses distinct cold/warm checkouts and build outputs,
one initially empty private compiler cache, and unchanged full-gate coverage.
After both gates it reruns the previously failing snapshot and ledger tests
three times each. Results append to
`/projects/caeg/scratch/kbd606/tmp/agent_djZgaH/results.txt`; full stage logs are
in cold-logs, warm-logs and isolated-logs beneath that directory. This job is
pending at this checkpoint; no new full-gate timing or acceptance is claimed.

The nightly sbatch definition passed Slurm's `--test-only` validation. Its
systemd timer remains a documented, disabled example; validation did not submit
or enable a nightly job.
