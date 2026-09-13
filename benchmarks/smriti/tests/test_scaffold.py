#!/usr/bin/env python3
"""Scaffold + corpus checks.

TestSchema / TestScaffoldEndToEnd: runner.py x scorer.py plumbing on
example-001 with EchoAdapter (fake agent, no cost) and the real ChittaAdapter
(live local daemon, self-cleaning via forget-by-id -- no cost, no external
network).

TestTaskCorpus: every task under tasks/ validates against schema/task.json,
has a hidden/ dir (the convention-revealing test, applied only at check
time -- see runner.apply_hidden_checks), no visible repo_fixture file states
the planted convention's answer, and check_cmd (with hidden/ applied, same
as a real run) FAILS on the untouched fixture and PASSES once solution/ is
copied over it. A task whose check_cmd passes untouched is broken -- see
README "Threats to validity" > "Fixture leakage via a visible test", the
failure mode a 2026-09-01 live pilot actually caught (both off and on
passed because the agent read the expected value out of a visible test
file -- fixed by moving every convention-revealing test into hidden/).

TestDryRun: one smoke run of runner.run_all with ClaudeCodeAdapter(dry_run)
across the whole corpus, proving the runner prints the exact commands it
would execute without making any live chitta/claude calls or writing a
results file. The real ClaudeCodeAdapter is never invoked against the
corpus here -- that costs real LLM calls and belongs to a deliberate,
manually-launched benchmark run, not the test suite.
"""

import io
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from contextlib import contextmanager, redirect_stdout
from pathlib import Path

SMRITI_DIR = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(SMRITI_DIR))

import runner  # noqa: E402
import scorer  # noqa: E402


@contextmanager
def temporary_env(name, value):
    previous = os.environ.get(name)
    os.environ[name] = value
    try:
        yield
    finally:
        if previous is None:
            os.environ.pop(name, None)
        else:
            os.environ[name] = previous


def validate_against_schema(instance, schema, path="$"):
    """Minimal stdlib JSON-Schema check covering exactly the keywords
    schema/task.json uses (type, required, properties, additionalProperties,
    pattern, minLength, items, minItems, enum). Raises AssertionError with
    a path on any violation; raises NotImplementedError on a schema keyword
    it doesn't know, so a schema change fails loudly instead of silently
    passing unchecked."""
    known = {
        "$schema",
        "$id",
        "title",
        "description",
        "type",
        "required",
        "properties",
        "additionalProperties",
        "pattern",
        "minLength",
        "items",
        "minItems",
        "enum",
        "minimum",
    }
    unknown = set(schema) - known
    if unknown:
        raise NotImplementedError(f"{path}: unsupported schema keyword(s): {unknown}")

    stype = schema.get("type")
    if stype == "object":
        assert isinstance(instance, dict), f"{path}: expected object, got {type(instance).__name__}"
        for req in schema.get("required", []):
            assert req in instance, f"{path}: missing required property '{req}'"
        props = schema.get("properties", {})
        if schema.get("additionalProperties") is False:
            extra = set(instance) - set(props)
            assert not extra, f"{path}: additionalProperties not allowed: {extra}"
        for key, subschema in props.items():
            if key in instance:
                validate_against_schema(instance[key], subschema, f"{path}.{key}")
    elif stype == "array":
        assert isinstance(instance, list), f"{path}: expected array, got {type(instance).__name__}"
        if "minItems" in schema:
            assert len(instance) >= schema["minItems"], (
                f"{path}: fewer than minItems={schema['minItems']}"
            )
        if "items" in schema:
            for i, item in enumerate(instance):
                validate_against_schema(item, schema["items"], f"{path}[{i}]")
    elif stype == "string":
        assert isinstance(instance, str), f"{path}: expected string, got {type(instance).__name__}"
        if "minLength" in schema:
            assert len(instance) >= schema["minLength"], (
                f"{path}: shorter than minLength={schema['minLength']}"
            )
        if "pattern" in schema:
            assert re.match(schema["pattern"], instance), (
                f"{path}: {instance!r} doesn't match {schema['pattern']!r}"
            )
        if "enum" in schema:
            assert instance in schema["enum"], f"{path}: {instance!r} not in {schema['enum']}"
    elif stype == "integer":
        assert isinstance(instance, int) and not isinstance(instance, bool), (
            f"{path}: expected integer"
        )
        if "minimum" in schema:
            assert instance >= schema["minimum"], f"{path}: below minimum={schema['minimum']}"
    elif stype is None:
        pass  # top-level $schema/$id-only wrapper, nothing to check itself
    else:
        raise NotImplementedError(f"{path}: unsupported schema type: {stype}")


class TestSchema(unittest.TestCase):
    def test_example_001_satisfies_schema(self):
        task = runner.load_task(SMRITI_DIR / "tasks" / "example-001")
        self.assertEqual(task["id"], "example-001")
        self.assertGreaterEqual(len(task["planted_memories"]), 2)


class TestScaffoldEndToEnd(unittest.TestCase):
    def setUp(self):
        self.output_dir = Path(tempfile.mkdtemp(prefix="smriti-test-results-"))

    def tearDown(self):
        shutil.rmtree(self.output_dir, ignore_errors=True)

    def test_runner_produces_results_and_scorer_computes_metrics(self):
        out_path = runner.run_all(
            tasks_dir=SMRITI_DIR / "tasks",
            task_ids=["example-001"],
            conditions=["off", "on"],
            agent=runner.EchoAdapter(),
            output_dir=self.output_dir,
            trials=1,
        )

        self.assertTrue(out_path.exists())
        records = scorer.load_records(out_path)
        self.assertEqual(len(records), 2)
        conditions = {r["condition"] for r in records}
        self.assertEqual(conditions, {"off", "on"})
        for r in records:
            self.assertIn("passed", r)
            self.assertIn("tokens_used", r)
            self.assertIn("input_tokens", r)
            self.assertIn("cache_read_tokens", r)
            self.assertIn("injected_confirmed", r)
            self.assertGreater(r["tokens_used"], 0)

        by_condition = {r["condition"]: r for r in records}
        # off plants nothing -- "was it injected" doesn't apply.
        self.assertIsNone(by_condition["off"]["injected_confirmed"])
        # on plants 2 real memories via ChittaAdapter, but EchoAdapter never
        # runs a real claude session (no hooks fire, nothing writes to the
        # outcome ledger) -- confirmation must correctly come back False, not
        # silently True.
        self.assertFalse(by_condition["on"]["injected_confirmed"])

        report = scorer.score(records)
        self.assertIn("off", report["per_condition"])
        self.assertIn("on", report["per_condition"])
        self.assertIn("delta_sr", report["headline"])
        self.assertIn("delta_tokens", report["headline"])
        self.assertIn("mui", report["headline"])
        self.assertIn("sr_lift_credit", report["headline"])
        self.assertIn("cost_credit", report["headline"])
        self.assertIn("echo_rate", report["headline"])

        # EchoAdapter edits nothing, so the fixture's own failing test still fails
        # under both conditions -- this is expected, not a benchmark result.
        self.assertTrue(all(r["passed"] is False for r in records))

    def test_ablate_condition_is_now_a_real_scored_run(self):
        """Ablation is wired via CHITTA_ABLATE_LANES (an env var read by
        hooks/prompt-core.sh, not a chittad daemon flag) -- ablate:<lane>
        is a real, scored condition, not the earlier skip-and-drop
        placeholder."""
        out_path = runner.run_all(
            tasks_dir=SMRITI_DIR / "tasks",
            task_ids=["example-001"],
            conditions=["ablate:semantic"],
            agent=runner.EchoAdapter(),
            output_dir=self.output_dir,
            trials=1,
        )
        records = scorer.load_records(out_path)
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0]["condition"], "ablate:semantic")
        self.assertIn("passed", records[0])

    def test_resume_skips_recorded_trials_and_appends_the_rest(self):
        first = runner.run_all(
            tasks_dir=SMRITI_DIR / "tasks",
            task_ids=["example-001"],
            conditions=["off", "on"],
            agent=runner.EchoAdapter(),
            output_dir=self.output_dir,
            trials=2,
        )
        before = scorer.load_records(first)
        self.assertEqual(len(before), 4)  # 2 conditions x 2 trials
        run_id = before[0]["run_id"]

        second = runner.run_all(
            tasks_dir=SMRITI_DIR / "tasks",
            task_ids=["example-001"],
            conditions=["off", "on"],
            agent=runner.EchoAdapter(),
            output_dir=self.output_dir,
            trials=3,  # one more trial than the first run
            resume_path=first,
        )
        self.assertEqual(second, first)  # same file, appended in place
        after = scorer.load_records(second)
        self.assertEqual(len(after), 6)  # 4 kept + 2 new (trial 2, off+on)
        self.assertTrue(all(r["run_id"] == run_id for r in after))
        seen = {(r["task_id"], r["condition"], r["trial"]) for r in after}
        self.assertEqual(
            seen,
            {("example-001", c, t) for c in ("off", "on") for t in (0, 1, 2)},
        )

    def test_resume_with_nothing_new_leaves_the_file_untouched(self):
        out_path = runner.run_all(
            tasks_dir=SMRITI_DIR / "tasks",
            task_ids=["example-001"],
            conditions=["off"],
            agent=runner.EchoAdapter(),
            output_dir=self.output_dir,
            trials=1,
        )
        before_mtime = out_path.stat().st_mtime_ns
        before_records = scorer.load_records(out_path)

        result = runner.run_all(
            tasks_dir=SMRITI_DIR / "tasks",
            task_ids=["example-001"],
            conditions=["off"],
            agent=runner.EchoAdapter(),
            output_dir=self.output_dir,
            trials=1,
            resume_path=out_path,
        )
        self.assertEqual(result, out_path)
        self.assertEqual(out_path.stat().st_mtime_ns, before_mtime)
        self.assertEqual(scorer.load_records(out_path), before_records)


# Per-task terms that would reveal the planted convention's actual answer if
# they appeared in any VISIBLE fixture file (repo_fixture/ or the prompt --
# hidden/ and solution/ are allowed to contain them, that's their job). Not
# generic domain vocabulary (e.g. "csv"/"json" are legitimate to mention as
# the two valid *options* in 005) -- specifically the answer-revealing terms.
LEAK_CHECK_TERMS = {
    "example-001": ["hello_world", "hello-world", "underscore", "hyphen"],
    "example-002": ["pickle"],
    "example-003": ["bare valueerror", "never a bare", "never use valueerror"],
    "example-004": ["migrat"],
    "example-005": ["defaults to csv", "must default", "json is opt-in"],
    "example-006": ["tab-separat", "awk pipeline", "downstream ingest"],
    "example-007": ["known bug", "silently drops", "cap the parallel", "always cap"],
    "example-008": ["100k+", "batch job", "o(n)", "o(n^2)", "o(n²)"],
    "example-009": ["exit code 3", "monitoring dashboard", "pages differently", "on-call"],
    "example-010": ["generated-by: forge-cli", "forge-cli v2", "artifact scanner"],
    "example-011": ["evt_ping", "evt_<x>", "handle_ping", "on_ping"],
    "example-012": ["forge_api_token"],
    "example-013": ["evt|", "pipe-delimited", "log-shipper's regex"],
    "example-014": ["list, add, remove", "generate-docs", "runbook"],
    "example-015": ["0.2 * attempt", "5 total attempts", "linear backoff", "rate limiter enforces"],
}


class TestTaskCorpus(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.schema = json.loads((SMRITI_DIR / "schema" / "task.json").read_text())
        cls.task_dirs = sorted(
            (p for p in (SMRITI_DIR / "tasks").iterdir() if p.is_dir()),
            key=lambda p: p.name,
        )

    def test_corpus_has_at_least_nine_tasks(self):
        self.assertGreaterEqual(len(self.task_dirs), 9)

    def test_every_task_has_a_solution_dir(self):
        for task_dir in self.task_dirs:
            with self.subTest(task=task_dir.name):
                self.assertTrue(
                    (task_dir / "solution").is_dir(), f"{task_dir.name} has no solution/"
                )

    def test_every_task_has_a_hidden_dir(self):
        for task_dir in self.task_dirs:
            with self.subTest(task=task_dir.name):
                hidden_dir = task_dir / "hidden"
                self.assertTrue(hidden_dir.is_dir(), f"{task_dir.name} has no hidden/")
                hidden_files = list(hidden_dir.rglob("*.py"))
                self.assertTrue(hidden_files, f"{task_dir.name}'s hidden/ has no .py files")

    def test_every_task_validates_against_schema(self):
        for task_dir in self.task_dirs:
            with self.subTest(task=task_dir.name):
                task = json.loads((task_dir / "task.json").read_text())
                validate_against_schema(task, self.schema)
                self.assertEqual(task["id"], task_dir.name)
                self.assertNotIn(
                    task["planted_memories"][0]["content"][:20].lower(),
                    task["prompt"].lower(),
                    f"{task_dir.name}: prompt appears to quote the planted memory verbatim",
                )

    def test_no_visible_fixture_file_states_the_convention(self):
        """Grep every file under repo_fixture/ (what the agent actually sees
        during its turn -- hidden/ and solution/ are never materialized into
        its workdir) plus the prompt itself for each task's answer-revealing
        terms, and assert none appear. This is the automated regression guard
        for the 2026-09-01 pilot's leakage failure."""
        for task_dir in self.task_dirs:
            terms = LEAK_CHECK_TERMS.get(task_dir.name)
            with self.subTest(task=task_dir.name):
                self.assertIsNotNone(terms, f"{task_dir.name} has no entry in LEAK_CHECK_TERMS")
                task = json.loads((task_dir / "task.json").read_text())
                haystacks = {"prompt": task["prompt"]}
                fixture_dir = task_dir / task["repo_fixture"]
                for f in fixture_dir.rglob("*"):
                    if f.is_file():
                        try:
                            haystacks[str(f.relative_to(task_dir))] = f.read_text()
                        except UnicodeDecodeError:
                            continue
                for where, text in haystacks.items():
                    low = text.lower()
                    for term in terms:
                        self.assertNotIn(
                            term,
                            low,
                            f"{task_dir.name}: leaked term {term!r} found in visible {where}",
                        )

    def test_no_visible_test_file_defines_a_test(self):
        """repo_fixture/ may contain no test, or only a convention-agnostic
        one -- never one that reveals the answer (that's what hidden/ is
        for). Concretely for this corpus: no visible test_*.py defines any
        test method at all."""
        for task_dir in self.task_dirs:
            with self.subTest(task=task_dir.name):
                task = json.loads((task_dir / "task.json").read_text())
                fixture_dir = task_dir / task["repo_fixture"]
                for f in fixture_dir.rglob("test_*.py"):
                    text = f.read_text()
                    self.assertNotIn(
                        "def test",
                        text,
                        f"{task_dir.name}: visible {f.relative_to(task_dir)} defines a test "
                        f"-- move it to hidden/ instead",
                    )

    def test_check_cmd_fails_before_fix_and_passes_after_solution(self):
        """Mirrors runner.run_one's real order: materialize, THEN apply
        hidden/ (as the runner does right before check_cmd, never before),
        THEN check. Without applying hidden/ this test would be checking
        nothing -- a fixture with no visible test trivially 'passes'
        (unittest discovers zero tests and exits 0)."""
        for task_dir in self.task_dirs:
            with self.subTest(task=task_dir.name):
                task = runner.load_task(task_dir)
                workdir = runner.materialize_fixture(task, task_dir)
                try:
                    runner.apply_hidden_checks(task_dir, workdir)
                    pre = subprocess.run(
                        task["check_cmd"], shell=True, cwd=workdir, capture_output=True, text=True
                    )
                    self.assertNotEqual(
                        pre.returncode,
                        0,
                        f"{task_dir.name}: check_cmd passed on the UNTOUCHED fixture -- "
                        f"broken invariant (README 'Threats to validity'). "
                        f"stdout={pre.stdout!r} stderr={pre.stderr!r}",
                    )

                    shutil.copytree(task_dir / "solution", workdir, dirs_exist_ok=True)
                    post = subprocess.run(
                        task["check_cmd"], shell=True, cwd=workdir, capture_output=True, text=True
                    )
                    self.assertEqual(
                        post.returncode,
                        0,
                        f"{task_dir.name}: check_cmd still fails after applying solution/. "
                        f"stdout={post.stdout!r} stderr={post.stderr!r}",
                    )
                finally:
                    shutil.rmtree(workdir, ignore_errors=True)


class TestDryRun(unittest.TestCase):
    """Smoke test: --dry-run prints the real commands (chitta plant/recall,
    claude -p invocation) for every task without executing any of them --
    no live daemon calls, no LLM calls, no results file."""

    def test_dry_run_prints_commands_and_writes_nothing(self):
        output_dir = Path(tempfile.mkdtemp(prefix="smriti-test-dryrun-"))
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                out_path = runner.run_all(
                    tasks_dir=SMRITI_DIR / "tasks",
                    task_ids=[
                        p.name for p in sorted((SMRITI_DIR / "tasks").iterdir()) if p.is_dir()
                    ],
                    conditions=["off", "on"],
                    agent=runner.ClaudeCodeAdapter(dry_run=True),
                    output_dir=output_dir,
                    trials=1,
                    dry_run=True,
                )
            output = buf.getvalue()

            self.assertIsNone(out_path)
            self.assertEqual(list(output_dir.iterdir()), [])
            self.assertIn("[dry-run] would run:", output)  # ChittaAdapter plant/recall
            self.assertIn("[dry-run] would run in", output)  # ClaudeCodeAdapter
            self.assertIn("chitta", output)
            self.assertIn("claude", output)
            self.assertNotIn("Traceback", output)
        finally:
            shutil.rmtree(output_dir, ignore_errors=True)


class TestRunnerRefusesSilentSingleTrial(unittest.TestCase):
    def test_cli_requires_explicit_trials(self):
        result = subprocess.run(
            [
                sys.executable,
                str(SMRITI_DIR / "runner.py"),
                "--task",
                "example-001",
                "--agent",
                "echo",
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--trials", result.stderr)

    def test_run_all_rejects_zero_trials(self):
        with self.assertRaises(ValueError):
            runner.run_all(
                tasks_dir=SMRITI_DIR / "tasks",
                task_ids=["example-001"],
                conditions=["off"],
                agent=runner.EchoAdapter(),
                output_dir=Path(tempfile.mkdtemp(prefix="smriti-test-zerotrials-")),
                trials=0,
            )


class TestAblationWiring(unittest.TestCase):
    """No daemon/subprocess calls -- parse_condition and agent_env are pure
    (dry_run=True short-circuits the only network-touching bits, plant/
    context_for, neither of which agent_env calls)."""

    def test_benchmark_lane_names_map_to_the_hook_short_names(self):
        cases = {
            "semantic": "sem",
            "keyword": "kw",
            "graph": "hyb",
            "hybrid": "hyb",
            "context": "ctx",
            "corrections": "corr",
        }
        for benchmark_name, hook_name in cases.items():
            memory, _ = runner.parse_condition(f"ablate:{benchmark_name}", dry_run=True)
            env = memory.agent_env("project:smriti-test")
            self.assertEqual(env["CHITTA_ABLATE_LANES"], hook_name)
            self.assertEqual(env["CC_SOUL_ABLATE_LANES"], hook_name)

    def test_hook_short_names_pass_through_unchanged(self):
        for hook_name in ("sem", "kw", "hyb", "ctx", "corr", "xr"):
            memory, _ = runner.parse_condition(f"ablate:{hook_name}", dry_run=True)
            env = memory.agent_env("project:smriti-test")
            self.assertEqual(env["CHITTA_ABLATE_LANES"], hook_name)
            self.assertEqual(env["CC_SOUL_ABLATE_LANES"], hook_name)

    def test_ablate_all_disables_every_lane(self):
        memory, _ = runner.parse_condition("ablate:all", dry_run=True)
        env = memory.agent_env("project:smriti-test")
        self.assertEqual(env["CHITTA_ABLATE_LANES"], "sem,ctx,hyb,kw,corr,xr")
        self.assertEqual(env["CC_SOUL_ABLATE_LANES"], "sem,ctx,hyb,kw,corr,xr")

    def test_unknown_lane_is_rejected(self):
        with self.assertRaises(ValueError):
            runner.parse_condition("ablate:nonsense")

    def test_off_and_on_set_no_ablate_lanes_env(self):
        off_memory, _ = runner.parse_condition("off")
        on_memory, _ = runner.parse_condition("on", dry_run=True)
        off_env = off_memory.agent_env("project:smriti-test")
        on_env = on_memory.agent_env("project:smriti-test")
        self.assertNotIn("CHITTA_ABLATE_LANES", off_env)
        self.assertNotIn("CC_SOUL_ABLATE_LANES", off_env)
        self.assertNotIn("CHITTA_ABLATE_LANES", on_env)
        self.assertNotIn("CC_SOUL_ABLATE_LANES", on_env)

    def test_eval_socket_reaches_cli_and_agent_environment(self):
        socket = "/tmp/chitta-eval-test.sock"
        with temporary_env("CHITTA_EVAL_SOCKET", socket):
            memory = runner.ChittaAdapter(dry_run=True)
            cmd = memory._with_eval_socket([memory.CHITTA_BIN, "recall", "--json"])
            self.assertEqual(cmd[-2:], ["--socket-path", socket])
            self.assertEqual(memory.agent_env("project:smriti-test")["CHITTA_SOCKET_PATH"], socket)


class TestMUI(unittest.TestCase):
    """scorer.mui_credit / scorer.score's MUI section -- synthetic records,
    no daemon calls."""

    @staticmethod
    def _rec(task_id, condition, trial, passed, tokens, injected_confirmed):
        return {
            "task_id": task_id,
            "condition": condition,
            "trial": trial,
            "passed": passed,
            "tokens_used": tokens,
            "injected_confirmed": injected_confirmed,
            "transcript": "",
            "planted_memory_contents": [],
        }

    def test_credit_requires_confirmed_injection_and_a_pass(self):
        paired_off = [self._rec("t", "off", 0, False, 100, None)]
        unconfirmed = self._rec("t", "on", 0, True, 50, False)
        self.assertEqual(scorer.mui_credit(unconfirmed, paired_off), (False, False))

        failed_on = self._rec("t", "on", 0, False, 50, True)
        self.assertEqual(scorer.mui_credit(failed_on, paired_off), (False, False))

    def test_sr_lift_credit_when_a_paired_off_trial_failed(self):
        on = self._rec("t", "on", 0, True, 800, True)
        paired_off = [self._rec("t", "off", 0, False, 1000, None)]
        self.assertEqual(scorer.mui_credit(on, paired_off), (True, False))

    def test_cost_credit_when_paired_off_used_1_5x_tokens_and_still_passed(self):
        on = self._rec("t", "on", 0, True, 900, True)
        paired_off = [self._rec("t", "off", 0, True, 2000, None)]
        self.assertEqual(scorer.mui_credit(on, paired_off), (False, True))

    def test_no_credit_when_off_passed_and_tokens_are_close(self):
        on = self._rec("t", "on", 0, True, 900, True)
        paired_off = [self._rec("t", "off", 0, True, 1000, None)]  # only 1.11x
        self.assertEqual(scorer.mui_credit(on, paired_off), (False, False))

    def test_score_pairs_by_matching_trial_and_reports_the_two_components(self):
        records = [
            self._rec("t1", "off", 0, False, 1000, None),
            self._rec("t1", "on", 0, True, 800, True),  # sr_lift
            self._rec("t1", "off", 1, True, 2000, None),
            self._rec("t1", "on", 1, True, 900, True),  # cost (2000 >= 1.5*900)
            self._rec("t2", "off", 0, False, 500, None),
            self._rec("t2", "on", 0, True, 400, False),  # unconfirmed -> no credit
            self._rec("t3", "off", 0, True, 500, None),
            self._rec("t3", "on", 0, False, 400, True),  # on failed -> no credit
        ]
        report = scorer.score(records)
        headline = report["headline"]
        self.assertEqual(headline["mui_n"], 4)
        self.assertAlmostEqual(headline["mui"], 0.5)
        self.assertAlmostEqual(headline["sr_lift_credit"], 0.25)
        self.assertAlmostEqual(headline["cost_credit"], 0.25)
        self.assertIn("echo_rate", headline)  # old proxy still reported, for reference

    def test_ablate_all_vs_off_sr_gap_present_when_ablate_all_in_results(self):
        records = [
            self._rec("t1", "off", 0, True, 100, None),
            self._rec("t1", "on", 0, True, 100, True),
            self._rec("t1", "ablate:all", 0, True, 100, True),
        ]
        report = scorer.score(records)
        self.assertIn("ablate_all_vs_off_sr_gap", report["headline"])
        self.assertAlmostEqual(report["headline"]["ablate_all_vs_off_sr_gap"], 0.0)


if __name__ == "__main__":
    unittest.main()
