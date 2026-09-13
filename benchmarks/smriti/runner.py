#!/usr/bin/env python3
"""SMRITI-Bench runner: executes tasks under memory conditions and records outcomes.

For each task x condition (off | on | ablate:<lane>) x trial:
  1. materialize the repo fixture into a scratch workdir -- tasks/<id>/hidden/
     is NOT part of this; the agent never sees it (see step 4)
  2. plant the task's memories via a MemoryAdapter (no-op under off)
  3. invoke the agent via an AgentAdapter, letting the agent's own memory
     pipeline (chitta's hooks, for ClaudeCodeAdapter) do the recall -- see
     "How memory reaches the agent" below
  4. copy tasks/<id>/hidden/ over the workdir (apply_hidden_checks) -- only
     now, after the agent is done -- then run check_cmd; record pass/fail.
     Any test that would reveal the planted convention lives in hidden/, not
     repo_fixture/, so the agent can't just read the expected value out of a
     visible test (see README "Threats to validity" > "Fixture leakage via a
     visible test" -- this is what a 2026-09-01 live pilot caught: both off
     and on passed because the agent read the answer out of a visible test).
  5. append one JSON record to results/<run_id>.jsonl, including
     injected_confirmed (did an outcome-ledger 'injected' event during this
     exact agent call actually name one of the ids we planted? -- see
     read_injected_ids_in_window)
  6. tear down the workdir and the planted memories (by id, not by realm --
     see ChittaAdapter.teardown)

How memory reaches the agent
-----------------------------
ChittaAdapter does NOT hand the agent a hand-built memory string. For
ClaudeCodeAdapter, memory reaches the agent the same way it would in real
use: cc-soul's own hooks (hooks/prompt-core.sh's UserPromptSubmit handler,
plus session-start-hook.sh / resume-inject-hook.sh) recall from the live
daemon when `claude -p` runs. Two adapter-level env vars steer that:

- ChittaAdapter.agent_env sets CHITTA_REALM=<realm_prefix>. detect_realm()
  (chitta/src/rpc_server.cpp:1416) checks CHITTA_REALM before the
  .cc-soul-realm-file and git-repo-name fallbacks, so this pins the agent's
  own hooks to exactly this trial's planted realm without touching the
  materialized fixture's files.
- When CHITTA_EVAL_SOCKET is set, ChittaAdapter pins its own CLI calls with
  --socket-path and passes CHITTA_SOCKET_PATH to the agent subprocess. The
  latter takes effect for hooks inside "claude -p" once the chitta CLI honors
  that environment variable.
- NullAdapter.agent_env sets CHITTA_HEADLESS=1 (and CC_SOUL_HEADLESS=1, the
  pre-rename name). Every memory-injecting hook (prompt-core.sh,
  session-start-hook.sh, resume-inject-hook.sh, post-bash-hook.sh,
  pre-tool-hook.sh -- `grep _HEADLESS hooks/*.sh`) no-ops immediately when
  either is set. It is chitta's existing global kill switch (used today to
  keep multi-agent "room" participants quiet), not a benchmark-specific
  workaround -- memory=off gets zero injection through any lane,
  deterministically.

ChittaAdapter.context_for still makes a real `chitta recall` call and
records what it returns, but only as a diagnostic (results/*.jsonl
"recalled_context_preview") -- it is never prepended to the agent's prompt.
Prepending it there as well as letting the hooks inject it would
double-inject and would bypass the real recall-routing path (relevant to
ablation) with a hand-rolled proxy of it.

Ablation
--------
Wired via env, not a daemon flag: ablate:<lane> sets
CHITTA_ABLATE_LANES=<short-lane-name> (and the pre-rename
CC_SOUL_ABLATE_LANES) on the agent subprocess (ChittaAdapter.agent_env);
hooks/prompt-core.sh's recall call skips any lane named there. ablate:all
disables every lane at once (CHITTA_ABLATE_LANES=sem,ctx,hyb,kw,corr,xr)
-- in effect this should
match `off`, and scorer.py reports the gap as ablate_all_vs_off_sr_gap as
a sanity check on the wiring itself. ABLATE_LANE_ALIASES maps
benchmark-facing lane names (semantic, keyword, graph, hybrid, context,
corrections) to chitta's short hook-lane names (sem, kw, hyb, ctx, corr)
-- see parse_condition().
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
import uuid
from dataclasses import dataclass
from pathlib import Path

SCRATCH_ROOT = os.environ.get("SMRITI_SCRATCH", "/projects/caeg/scratch/kbd606/tmp")

# Benchmark-facing ablation lane name -> chitta's short hook-lane name
# (hooks/prompt-core.sh's CHITTA_ABLATE_LANES contract). graph and hybrid
# both land on "hyb" -- chitta has no separate triplet/graph lane distinct
# from the hybrid-recall path (see README "Ablation matrix"). Passing a
# short name directly (sem/kw/hyb/ctx/corr/xr) also works -- see
# parse_condition().
ABLATE_LANE_ALIASES = {
    "semantic": "sem",
    "keyword": "kw",
    "graph": "hyb",
    "hybrid": "hyb",
    "context": "ctx",
    "corrections": "corr",
}
ABLATE_ALL_LANES = "sem,ctx,hyb,kw,corr,xr"
ABLATE_SHORT_LANES = set(ABLATE_ALL_LANES.split(","))


# ---- task loading + minimal schema validation (stdlib only) ----


def load_task(task_dir: Path) -> dict:
    task = json.loads((task_dir / "task.json").read_text())
    validate_task(task)
    return task


def validate_task(task: dict) -> None:
    required = ["id", "repo_fixture", "prompt", "check_cmd", "planted_memories", "tags"]
    missing = [k for k in required if k not in task]
    if missing:
        raise ValueError(f"task missing required fields: {missing}")
    if not isinstance(task["planted_memories"], list):
        raise ValueError("planted_memories must be a list")
    for m in task["planted_memories"]:
        for k in ("content", "realm", "type"):
            if k not in m:
                raise ValueError(f"planted memory missing '{k}': {m}")
        if m["type"] not in ("wisdom", "belief", "episode", "correction"):
            raise ValueError(f"planted memory has invalid type: {m['type']}")
    if not task["tags"]:
        raise ValueError("tags must be non-empty")


# ---- memory adapters ----


class MemoryAdapter:
    """Interface between the runner and a memory system under test."""

    def plant(self, memories: list, realm_prefix: str) -> list:
        """Store `memories` and return the ids that were created (empty for
        no-op adapters). The runner passes these ids back to teardown()."""
        raise NotImplementedError

    def context_for(self, prompt: str, realm_prefix: str) -> str:
        """Diagnostic text: what this condition's memory system would surface
        for this prompt. Recorded in results/*.jsonl; not injected into the
        agent's prompt (see module docstring)."""
        raise NotImplementedError

    def agent_env(self, realm_prefix: str) -> dict:
        """Extra environment variables to set on the agent subprocess for
        this condition."""
        return {}

    def teardown(self, realm_prefix: str, planted_ids: list) -> None:
        pass


class NullAdapter(MemoryAdapter):
    """memory=off: no planting, no retrieval, and the agent's own
    memory-injecting hooks are disabled outright (see module docstring)."""

    def plant(self, memories, realm_prefix):
        return []

    def context_for(self, prompt, realm_prefix):
        return ""

    def agent_env(self, realm_prefix):
        return {"CHITTA_HEADLESS": "1", "CC_SOUL_HEADLESS": "1"}


class ChittaAdapter(MemoryAdapter):
    """memory=on / memory=ablate:<lane> via the live chitta store.

    Plants each task memory into a realm unique to this task x condition x
    trial (`project:smriti-<task>-<run_id>-<condition>-t<trial>`, built by
    run_one()), never reused across conditions or tasks (README "Threats to
    validity"). Canonical `project:*` shape is required, not cosmetic -- see
    the comment at realm_prefix's construction in run_one(). Cleanup is by
    memory id (`chitta forget --id`), not by realm
    delete -- there is no bulk "forget this realm" RPC, only forget-by-id
    and forget-by-pattern (field_memory_ops.cpp:tool_batch_forget), and
    per-id is the precise, order-independent choice.

    dry_run: print the exact `chitta` CLI commands instead of running them
    (see main's --dry-run).
    """

    CHITTA_BIN = os.path.expanduser("~/.claude/bin/chitta")

    def __init__(self, ablate_lane: str | None = None, dry_run: bool = False):
        self.ablate_lane = ablate_lane
        self.dry_run = dry_run

    @staticmethod
    def _with_eval_socket(cmd: list) -> list:
        socket = os.environ.get("CHITTA_EVAL_SOCKET")
        return [*cmd, "--socket-path", socket] if socket else cmd

    def _run_cli(self, cmd: list, timeout: int = 20) -> dict:
        if self.dry_run:
            print(f"[dry-run] would run: {' '.join(shlex.quote(c) for c in cmd)}")
            return {}
        try:
            result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            print(f"[ChittaAdapter] timeout running: {' '.join(cmd)}", file=sys.stderr)
            return {}
        try:
            data = json.loads(result.stdout)
            return data if isinstance(data, dict) else {}
        except json.JSONDecodeError:
            print(
                f"[ChittaAdapter] non-JSON response from '{cmd[1]}': "
                f"stdout={result.stdout!r} stderr={result.stderr!r}",
                file=sys.stderr,
            )
            return {}

    def plant(self, memories, realm_prefix):
        ids = []
        for m in memories:
            cmd = [
                self.CHITTA_BIN,
                "remember",
                "--json",
                "--content",
                m["content"],
                "--type",
                m["type"],
                "--realm",
                realm_prefix,
                "--tags",
                "smriti-bench",
            ]
            data = self._run_cli(self._with_eval_socket(cmd))
            if "id" in data:
                ids.append(data["id"])
        return ids

    def context_for(self, prompt, realm_prefix):
        cmd = [self.CHITTA_BIN, "recall", "--json", "--query", prompt, "--realm", realm_prefix]
        data = self._run_cli(self._with_eval_socket(cmd))
        results = data.get("results", [])
        return "\n".join(r.get("text", "") for r in results)

    def agent_env(self, realm_prefix):
        env = {"CHITTA_REALM": realm_prefix}
        if socket := os.environ.get("CHITTA_EVAL_SOCKET"):
            env["CHITTA_SOCKET_PATH"] = socket
        if self.ablate_lane == "all":
            env["CHITTA_ABLATE_LANES"] = env["CC_SOUL_ABLATE_LANES"] = ABLATE_ALL_LANES
        elif self.ablate_lane:
            env["CHITTA_ABLATE_LANES"] = env["CC_SOUL_ABLATE_LANES"] = self.ablate_lane
        return env

    def teardown(self, realm_prefix, planted_ids):
        for mem_id in planted_ids:
            cmd = [self.CHITTA_BIN, "forget", "--json", "--id", mem_id]
            self._run_cli(self._with_eval_socket(cmd))


# ---- agent adapters ----


@dataclass
class AgentResult:
    transcript: str
    tokens_used: int  # uncached input+output tokens; see README "Token accounting"
    input_tokens: int = 0
    output_tokens: int = 0
    cache_read_tokens: int = 0


class AgentAdapter:
    def run(
        self, prompt: str, memory_context: str, workdir: Path, env: dict | None = None
    ) -> AgentResult:
        raise NotImplementedError


class EchoAdapter(AgentAdapter):
    """Deterministic fake agent for exercising the harness. Makes no repo edits --
    check_cmd's outcome reflects only the fixture's starting state. That's the point:
    it proves materialize -> plant -> invoke -> check -> score end to end without
    an LLM call or a live memory store. tokens_used is a word-count proxy, not real
    token accounting -- input/output/cache_read_tokens stay 0."""

    def run(self, prompt, memory_context, workdir, env=None):
        transcript = f"PROMPT:\n{prompt}\n\nMEMORY:\n{memory_context}"
        tokens_used = len(transcript.split())
        return AgentResult(transcript=transcript, tokens_used=tokens_used)


class ClaudeCodeAdapter(AgentAdapter):
    """Drives Claude Code headless (`claude -p ... --output-format json`) as
    the agent under test, in the materialized fixture dir. See module
    docstring for how memory reaches it (via env, not a hand-built prompt
    prefix).

    Usage/token parsing is against the real `claude -p --output-format json`
    envelope (verified live: `type":"result"` with a top-level `usage` object
    containing `input_tokens`, `output_tokens`, `cache_read_input_tokens`,
    `cache_creation_input_tokens`; the agent's final text is the top-level
    `result` field). tokens_used = input_tokens + output_tokens deliberately
    excludes cache_read_tokens: a memory-injection prompt that's mostly a warm
    cache hit shouldn't count against `on`'s delta-tokens the way a cold
    read would (README "Open design questions" #4).
    """

    BIN = "claude"

    def __init__(self, timeout_s: int = 600, dry_run: bool = False):
        self.timeout_s = timeout_s
        self.dry_run = dry_run

    def build_cmd(self, prompt: str) -> list:
        return [self.BIN, "-p", prompt, "--output-format", "json"]

    def run(self, prompt, memory_context, workdir, env=None):
        cmd = self.build_cmd(prompt)
        if self.dry_run:
            print(
                f"[dry-run] would run in {workdir} (env additions: {env or {}}): "
                f"{cmd[0]} -p <prompt: {len(prompt)} chars> --output-format json "
                f"(timeout {self.timeout_s}s)"
            )
            return AgentResult(transcript="[dry-run]", tokens_used=0)

        full_env = dict(os.environ)
        full_env.update(env or {})
        try:
            proc = subprocess.run(
                cmd,
                cwd=workdir,
                env=full_env,
                capture_output=True,
                text=True,
                timeout=self.timeout_s,
            )
        except subprocess.TimeoutExpired:
            return AgentResult(transcript=f"[timeout after {self.timeout_s}s]", tokens_used=0)

        try:
            payload = json.loads(proc.stdout)
        except json.JSONDecodeError:
            transcript = (
                f"[unparseable claude -p output] stdout={proc.stdout!r} stderr={proc.stderr!r}"
            )
            return AgentResult(transcript=transcript, tokens_used=0)

        usage = payload.get("usage", {})
        input_tokens = usage.get("input_tokens", 0)
        output_tokens = usage.get("output_tokens", 0)
        cache_read_tokens = usage.get("cache_read_input_tokens", 0)
        return AgentResult(
            transcript=payload.get("result", ""),
            tokens_used=input_tokens + output_tokens,
            input_tokens=input_tokens,
            output_tokens=output_tokens,
            cache_read_tokens=cache_read_tokens,
        )


AGENT_ADAPTERS = {"echo": EchoAdapter, "claude-code": ClaudeCodeAdapter}


# ---- run orchestration ----


def parse_condition(condition: str, dry_run: bool = False):
    """'off' | 'on' | 'ablate:<lane>' | 'ablate:all' -> (memory_adapter, label).

    <lane> accepts either a benchmark-facing name (semantic, keyword,
    graph, hybrid, context, corrections -- see ABLATE_LANE_ALIASES) or one
    of chitta's own short hook-lane names directly (sem, kw, hyb, ctx,
    corr, xr). 'all' disables every lane -- see ABLATE_ALL_LANES and
    scorer.py's ablate_all_vs_off_sr_gap."""
    if condition == "off":
        return NullAdapter(), condition
    if condition == "on":
        return ChittaAdapter(dry_run=dry_run), condition
    if condition.startswith("ablate:"):
        lane = condition.split(":", 1)[1]
        if lane == "all":
            return ChittaAdapter(ablate_lane="all", dry_run=dry_run), condition
        short = ABLATE_LANE_ALIASES.get(lane, lane)
        if short not in ABLATE_SHORT_LANES:
            raise ValueError(
                f"unknown ablation lane: {lane!r} -- known benchmark names: "
                f"{sorted(ABLATE_LANE_ALIASES)}, known hook names: "
                f"{sorted(ABLATE_SHORT_LANES)}, or 'all'"
            )
        return ChittaAdapter(ablate_lane=short, dry_run=dry_run), condition
    raise ValueError(f"unknown condition: {condition}")


def materialize_fixture(task: dict, task_dir: Path) -> Path:
    os.makedirs(SCRATCH_ROOT, exist_ok=True)
    fixture = task["repo_fixture"]
    workdir = Path(tempfile.mkdtemp(prefix=f"smriti-{task['id']}-", dir=SCRATCH_ROOT))
    if fixture.startswith("git:"):
        url, _, ref = fixture[len("git:") :].partition("#")
        subprocess.run(["git", "clone", "--quiet", url, str(workdir)], check=True)
        if ref:
            subprocess.run(["git", "-C", str(workdir), "checkout", "--quiet", ref], check=True)
    else:
        src = task_dir / fixture
        shutil.copytree(src, workdir, dirs_exist_ok=True)
    return workdir


def run_check(check_cmd: str, workdir: Path) -> bool:
    result = subprocess.run(check_cmd, shell=True, cwd=workdir, capture_output=True, text=True)
    return result.returncode == 0


def apply_hidden_checks(task_dir: Path, workdir: Path) -> None:
    """Copy tasks/<id>/hidden/ over the materialized workdir, AFTER the agent
    has finished and BEFORE check_cmd runs. hidden/ holds every test that
    would reveal the planted convention (exact expected strings, exit codes,
    etc.) -- it never reaches the agent's workdir during its turn, only
    check-time. This is what actually fixed the 2026-09-01 pilot failure
    (both off and on passed because the agent read the literal expected
    value straight out of a VISIBLE test file); see README "Threats to
    validity" > "Fixture leakage via a visible test"."""
    hidden_dir = task_dir / "hidden"
    if hidden_dir.is_dir():
        shutil.copytree(hidden_dir, workdir, dirs_exist_ok=True)


MIND_PATH = Path(os.environ.get("CHITTA_DB_PATH", os.path.expanduser("~/.claude/mind")))


def read_injected_ids_in_window(start_ms: int, end_ms: int) -> set:
    """Scan $CHITTA_DB_PATH/outcome_ledger.jsonl (default ~/.claude/mind) for
    'injected' events (hooks/prompt-core.sh:660-678 -- {"event":"injected",
    "ids":[...],"lanes":{...},"ts":<epoch ms>,"session_id":...}, appended
    live during the child claude session's UserPromptSubmit hook) with ts in
    [start_ms, end_ms], and returns the union of their ids. There's no
    reliable session_id to join on here (the child claude -p session's id
    isn't known to the runner in advance), so the join is purely by the
    wall-clock window the agent subprocess ran in -- see run_one."""
    ledger_path = MIND_PATH / "outcome_ledger.jsonl"
    ids = set()
    if not ledger_path.exists():
        return ids
    try:
        with open(ledger_path) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    evt = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if evt.get("event") != "injected":
                    continue
                ts = evt.get("ts", 0)
                if start_ms <= ts <= end_ms:
                    ids.update(evt.get("ids", []))
    except OSError:
        pass
    return ids


def run_one(
    task: dict,
    task_dir: Path,
    condition: str,
    agent: AgentAdapter,
    run_id: str,
    trial: int,
    dry_run: bool = False,
) -> dict | None:
    """Runs one task x condition x trial and returns its scored record.
    ablate:<lane> is a real, scored condition -- see module docstring's
    "Ablation" section."""
    memory, label = parse_condition(condition, dry_run=dry_run)

    # Canonical `project:*` shape -- required, not cosmetic. detect_realm()
    # (chitta/src/rpc_server.cpp:1416) only ever produces `project:<repo>` or
    # `brahman`; hooks/prompt-core.sh's realm_detect_once greps the *output*
    # of `chitta realm_detect` for a `word:token` shape before trusting it
    # (an explicit CHITTA_REALM is now taken verbatim ahead of that grep --
    # see prompt-core.sh's own comment -- but nothing else downstream is
    # guaranteed to have been audited the same way). A live pilot on the
    # non-canonical `smriti-<task>-<run>-<cond>-t<n>` form (no colon)
    # actually failed this way: the child session's injected ids were 3
    # unrelated memories, not the 2 planted ones -- see README "Threats to
    # validity" > "Non-canonical realm names".
    realm_prefix = f"project:smriti-{task['id']}-{run_id}-{label.replace(':', '_')}-t{trial}"

    planted_ids = memory.plant(task["planted_memories"], realm_prefix)
    workdir = materialize_fixture(task, task_dir)
    try:
        memory_context = memory.context_for(task["prompt"], realm_prefix)
        agent_env = memory.agent_env(realm_prefix)
        call_start_ms = int(time.time() * 1000)
        agent_result = agent.run(task["prompt"], memory_context, workdir, env=agent_env)
        call_end_ms = int(time.time() * 1000)
        apply_hidden_checks(task_dir, workdir)
        passed = run_check(task["check_cmd"], workdir)
    finally:
        memory.teardown(realm_prefix, planted_ids)
        shutil.rmtree(workdir, ignore_errors=True)

    # injected_confirmed is None when nothing was planted (off) -- "was it
    # injected" doesn't apply. Otherwise: did an outcome-ledger 'injected'
    # event during this exact agent call name one of the ids we planted? A
    # False here means this is not a valid memory-on sample -- the memory
    # never actually reached the child session (README "Threats to validity").
    if planted_ids:
        injected_ids = read_injected_ids_in_window(call_start_ms, call_end_ms)
        injected_confirmed = bool(set(planted_ids) & injected_ids)
    else:
        injected_confirmed = None

    return {
        "run_id": run_id,
        "task_id": task["id"],
        "condition": label,
        "trial": trial,
        "passed": passed,
        "tokens_used": agent_result.tokens_used,
        "input_tokens": agent_result.input_tokens,
        "output_tokens": agent_result.output_tokens,
        "cache_read_tokens": agent_result.cache_read_tokens,
        "transcript": agent_result.transcript,
        "recalled_context_preview": memory_context,
        "planted_memory_contents": [m["content"] for m in task["planted_memories"]],
        "injected_confirmed": injected_confirmed,
        "timestamp": time.time(),
        "dry_run": dry_run,
    }


def load_resume_state(resume_path: Path | None):
    """Reads an existing results/<run_id>.jsonl (if given and present) and
    returns (run_id_to_reuse_or_None, {(task_id, condition, trial) already
    recorded}). Used by run_all to skip work an earlier, interrupted
    invocation already did and to keep appending to the same run_id/file
    rather than starting a fresh one."""
    if resume_path is None or not resume_path.exists():
        return None, set()
    with open(resume_path) as f:
        existing = [json.loads(line) for line in f if line.strip()]
    done = {(r["task_id"], r["condition"], r["trial"]) for r in existing}
    run_id = existing[0]["run_id"] if existing else None
    return run_id, done


def run_all(
    tasks_dir: Path,
    task_ids: list,
    conditions: list,
    agent: AgentAdapter,
    output_dir: Path,
    trials: int,
    dry_run: bool = False,
    resume_path: Path | None = None,
) -> Path | None:
    if trials < 1:
        raise ValueError(
            "trials must be >= 1 -- a single trial is a coin flip, not a result "
            "(README 'Threats to validity': repeated-trial default)"
        )

    resumed_run_id, done = load_resume_state(resume_path)
    run_id = resumed_run_id or uuid.uuid4().hex[:8]
    records = []
    for trial in range(trials):
        for task_id in task_ids:
            task_dir = tasks_dir / task_id
            task = load_task(task_dir)
            for condition in conditions:
                if (task_id, condition, trial) in done:
                    print(f"{task_id} [{condition}] trial {trial}: SKIP (already in {resume_path})")
                    continue
                record = run_one(task, task_dir, condition, agent, run_id, trial, dry_run=dry_run)
                if record is None:
                    continue
                records.append(record)
                status = "PASS" if record["passed"] else "FAIL"
                tag = " [dry-run]" if dry_run else ""
                print(
                    f"{task_id} [{condition}] trial {trial}{tag}: {status} "
                    f"({record['tokens_used']} tokens)"
                )

    if dry_run:
        print(
            f"\n[dry-run] {len(records)} trial(s) would be recorded; no results file "
            f"written, no external chitta/claude calls made."
        )
        return None

    os.makedirs(output_dir, exist_ok=True)
    out_path = resume_path if resumed_run_id else output_dir / f"{run_id}.jsonl"
    if records:
        with open(out_path, "a" if resumed_run_id else "w") as f:
            for record in records:
                f.write(json.dumps(record) + "\n")
    return out_path


def main():
    parser = argparse.ArgumentParser(description="SMRITI-Bench runner")
    parser.add_argument("--tasks-dir", default=str(Path(__file__).parent / "tasks"))
    parser.add_argument(
        "--task", action="append", help="task id to run (default: all under tasks-dir)"
    )
    parser.add_argument(
        "--condition",
        action="append",
        default=None,
        help="off|on|ablate:<lane>, repeatable (default: off,on)",
    )
    parser.add_argument("--agent", choices=sorted(AGENT_ADAPTERS), default="echo")
    parser.add_argument(
        "--trials",
        type=int,
        required=True,
        help="repeat each task x condition this many times (no default -- "
        "README recommends >=5 before trusting a per-task delta_sr; "
        "a single trial is a coin flip, not a result)",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=600,
        help="wall-clock seconds before a claude-code agent run is killed (default 600)",
    )
    parser.add_argument("--output", default=str(Path(__file__).parent / "results"))
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print the chitta/claude commands each trial would run, without "
        "executing them or the daemon-mutating chitta calls; writes no results file",
    )
    parser.add_argument(
        "--resume",
        default=None,
        help="path to an existing results/<run_id>.jsonl -- skip any "
        "task x condition x trial already present in it and append "
        "the rest to the same file (same run_id), instead of starting "
        "a fresh run from scratch",
    )
    args = parser.parse_args()

    if args.trials < 1:
        parser.error("--trials must be >= 1")

    tasks_dir = Path(args.tasks_dir)
    task_ids = args.task or [p.name for p in sorted(tasks_dir.iterdir()) if p.is_dir()]
    conditions = args.condition or ["off", "on"]

    if args.agent == "claude-code":
        agent = ClaudeCodeAdapter(timeout_s=args.timeout, dry_run=args.dry_run)
    else:
        agent = AGENT_ADAPTERS[args.agent]()

    out_path = run_all(
        tasks_dir,
        task_ids,
        conditions,
        agent,
        Path(args.output),
        trials=args.trials,
        dry_run=args.dry_run,
        resume_path=Path(args.resume) if args.resume else None,
    )
    if out_path:
        print(f"results: {out_path}")


if __name__ == "__main__":
    main()
