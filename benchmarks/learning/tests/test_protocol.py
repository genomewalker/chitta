"""Behavioral tests for provenance, freezes, exclusions, telemetry and isolation."""

from __future__ import annotations

import sys
import tempfile
import unittest
from copy import deepcopy
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import digest, git, initial_hashes, seal, task_worktree
from freeze import classify, enumerate_memories, task_payload, validate_freeze, validate_task
from runner import aggregate, guard_environment, telemetry, verify_exclusion


def triplet(mid, predicate, obj):
    return {"subject": str(mid), "predicate": predicate, "object": str(obj)}


class ClassificationTests(unittest.TestCase):
    def classify(self, kind="wisdom", trips=(), parents=None):
        return classify({"id": "18446744073709551614", "kind": kind}, list(trips), parents or {})

    def test_queue_requires_source_not_tag(self):
        mid = "18446744073709551614"
        row = self.classify(trips=[triplet(mid, "source", "distillation")])
        self.assertEqual((row["classification"], row["writer"]), ("included", "queue_distillation"))
        row = self.classify(trips=[triplet(mid, "tagged", "distillation")])
        self.assertEqual(row["classification"], "unresolved")

    def test_native_all_kinds_and_value_facts(self):
        mid = "18446744073709551614"
        for kind in ("wisdom", "belief", "preference", "milestone", "operational"):
            with self.subTest(kind=kind):
                row = self.classify(
                    kind, [triplet(mid, "derived_from", "20")], {"20": {"type": "episode"}}
                )
                self.assertEqual(
                    row["classification"], "excluded" if kind == "operational" else "included"
                )
                self.assertTrue(row["parents"])
        self.assertEqual(self.classify("operational")["classification"], "unresolved")

    def test_preserved_corrections_episodes_and_explicit(self):
        mid = "18446744073709551614"
        for kind in ("correction", "episode"):
            self.assertEqual(
                self.classify(kind, [triplet(mid, "source", "distillation")])["classification"],
                "excluded",
            )
        self.assertEqual(
            self.classify(trips=[triplet(mid, "source", "mcp_tool")])["classification"], "excluded"
        )

    def test_ingester_missing_parent_and_conflicts(self):
        mid = "18446744073709551614"
        derived = triplet(mid, "derived_from", 20)
        self.assertEqual(self.classify(trips=[derived])["classification"], "unresolved")
        row = self.classify(
            trips=[derived, triplet(mid, "source", "mcp_tool")], parents={"20": {"type": "episode"}}
        )
        self.assertEqual(row["classification"], "unresolved")
        row = self.classify(trips=[derived, triplet(mid, "ingested_from", "readme")])
        self.assertEqual(row["writer"], "ingester")

    def test_capped_pagination_runs_to_empty(self):
        class RPC:
            def call(self, name, **args):
                return {
                    "memories": [
                        {"id": args["offset"] + i} for i in range(min(100, 203 - args["offset"]))
                    ]
                }

        self.assertEqual(len(enumerate_memories(RPC(), "project:cc-soul")), 203)


class TaskTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="learning-test-")
        self.root = Path(self.tmp.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        git(self.repo, "init", "--quiet", "-b", "fixture")
        git(self.repo, "config", "user.email", "test@example.invalid")
        git(self.repo, "config", "user.name", "Test")
        (self.repo / "answer").write_text("bad")
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-qm", "initial")
        initial = git(self.repo, "rev-parse", "HEAD")
        (self.repo / "answer").write_text("good")
        git(self.repo, "commit", "-qam", "good")
        good = git(self.repo, "rev-parse", "HEAD")
        self.task = {
            "id": "task1",
            "prompt": "Repair answer",
            "repo": str(self.repo),
            "cwd_sha": initial,
            "known_good": good,
            "initial_state_hashes": initial_hashes(self.repo, initial),
            "grader": {
                "command": ["/bin/sh", ".learning-grader/check.sh"],
                "files": {"check.sh": 'test "$(cat answer)" = good\n'},
            },
            "transcript_sha256": "a" * 64,
            "prompt_timestamp_ms": 2,
            "selection_note": "next self-contained graded prompt",
            "selection_rule": "next_eligible_graded_prompt",
            "recall_inspected": False,
            "dependencies": [{"path": "/bin/sh", "sha256": digest("/bin/sh")}],
        }
        self.task["validation"] = validate_task(self.task)

    def tearDown(self):
        self.tmp.cleanup()

    def panel(self):
        tasks = [deepcopy(self.task) for _ in range(20)]
        for n, task in enumerate(tasks):
            task["id"] = f"t{n}"
            task["prompt_timestamp_ms"] += n
            task["validation"]["payload_sha256"] = seal(task_payload(task))
        cohort = {
            "ids": [],
            "unresolved": [],
            "preview": False,
            "enumeration_stable": True,
            "realm": "project:cc-soul",
            "store": {"fully_hashed": True},
            "cut_timestamp_ms": 1,
        }
        config = {
            "model": "pinned-model",
            "claude_version": "1",
            "max_turns": 1,
            "budget_usd": 1,
            "timeout_s": 1,
            "claude_bin": "/bin/true",
            "chitta_bin": "/bin/true",
            "chittad_bin": "/bin/true",
            "embed_model": "/bin/true",
            "seed": 1,
        }
        return tasks, cohort, config

    def test_real_pre_fail_post_pass(self):
        self.assertNotEqual(self.task["validation"]["initial"]["exit_code"], 0)
        self.assertEqual(self.task["validation"]["known_good"]["exit_code"], 0)

    def test_always_pass_or_fail_grader_refused(self):
        for body in ("exit 0", "exit 1"):
            task = deepcopy(self.task)
            task["grader"]["files"]["check.sh"] = body
            with self.assertRaises(ValueError):
                validate_task(task)

    def test_missing_known_good_refused(self):
        task = deepcopy(self.task)
        task.pop("known_good")
        with self.assertRaisesRegex(ValueError, "known_good"):
            validate_task(task)

    def test_freeze_refusals(self):
        tasks, cohort, config = self.panel()
        validate_freeze(tasks, cohort, config)
        cases = [
            ("unresolved", lambda t, c, p: c.update(unresolved=["99"])),
            ("preview", lambda t, c, p: c.update(preview=True)),
            ("prospective", lambda t, c, p: c.update(cut_timestamp_ms=10)),
            ("validation", lambda t, c, p: t[0].pop("validation")),
            ("validation", lambda t, c, p: t[0].update(prompt="Changed after validation")),
            ("runner pin", lambda t, c, p: p.pop("model")),
        ]
        for reason, mutate in cases:
            with self.subTest(reason=reason):
                ts, cs, ps = deepcopy((tasks, cohort, config))
                mutate(ts, cs, ps)
                with self.assertRaisesRegex(ValueError, reason):
                    validate_freeze(ts, cs, ps)

    def test_no_future_git_objects_and_grader_absent(self):
        parent = self.root / "trial"
        parent.mkdir()
        with task_worktree(self.task, parent) as work:
            self.assertFalse((work / ".learning-grader").exists())
            self.assertFalse((parent / "objects.git/objects/info/alternates").exists())
            from common import command

            probe = command(
                ["git", "-C", work, "cat-file", "-e", self.task["known_good"]], check=False
            )
            self.assertNotEqual(probe.returncode, 0)
            self.assertEqual(git(work, "rev-parse", "HEAD"), self.task["cwd_sha"])


class ExclusionTests(unittest.TestCase):
    def lanes(self):
        return {
            "hybrid": {"results": []},
            "graph": {"results": []},
            "correction": {"results": []},
            "correction_check": {"matches": []},
        }

    def test_all_lanes_and_by_id_required(self):
        verify_exclusion(["42"], {"42": None}, self.lanes())
        with self.assertRaisesRegex(ValueError, "still resolves"):
            verify_exclusion(["42"], {"42": {"id": "42"}}, self.lanes())
        for lane in self.lanes():
            lanes = self.lanes()
            lanes[lane] = {"matches": [{"memory_id": "42"}]}
            with self.subTest(lane=lane), self.assertRaisesRegex(ValueError, lane):
                verify_exclusion(["42"], {"42": None}, lanes)
        with self.assertRaisesRegex(ValueError, "incomplete"):
            verify_exclusion(["42"], {}, self.lanes())
        with self.assertRaisesRegex(ValueError, "missing"):
            verify_exclusion(["42"], {"42": None}, {})

    def test_transport_error_is_not_absence(self):
        with self.assertRaises(ValueError):
            verify_exclusion(["42"], {"42": {"error": "timeout"}}, self.lanes())


class OutcomeTests(unittest.TestCase):
    def data(self):
        tasks = [{"id": f"t{i}"} for i in range(20)]
        events = [
            {
                "task": t["id"],
                "trial": n,
                "arm": arm,
                "success": int(arm == "A" and i < 3),
                "exclusion_verified": arm == "B",
                "telemetry": {
                    "complete": True,
                    "cohort_injections": int(arm == "A"),
                    "total_injections": 1,
                    "empty_turns": 0,
                    "recall_ms": [1, 3],
                    "hook_ms": [10],
                    "lane_failures": 0,
                },
            }
            for i, t in enumerate(tasks)
            for n in range(1, 4)
            for arm in ("A", "B")
        ]
        return tasks, events

    def test_three_trial_threshold_nine_net_successes(self):
        tasks, events = self.data()
        summary = aggregate(tasks, events, 3)
        self.assertEqual(summary["delta"], 3)
        self.assertEqual(summary["net_successes"], 9)
        self.assertTrue(summary["verdict"].startswith("RETAIN"))
        events[0]["success"] = 0
        self.assertTrue(aggregate(tasks, events, 3)["verdict"].startswith("RETIRE"))

    def test_invalid_runs_never_verdict(self):
        tasks, events = self.data()
        variants = [events[:-1], events + [events[0]]]
        for field, value in (("error", "timeout"), ("success", None)):
            other = deepcopy(events)
            other[0][field] = value
            variants.append(other)
        other = deepcopy(events)
        other[1]["telemetry"]["cohort_injections"] = 1
        variants.append(other)
        other = deepcopy(events)
        for event in other:
            event["telemetry"]["cohort_injections"] = 0
        variants.append(other)
        for variant in variants:
            self.assertTrue(aggregate(tasks, variant, 3)["verdict"].startswith("NO VERDICT"))
        for args in ({"dry_run": True}, {"isolation_ok": False}, {"unresolved": ["1"]}):
            self.assertTrue(aggregate(tasks, events, 3, **args)["verdict"].startswith("NO VERDICT"))

    def test_hook_output_ledger_join_and_genuine_empty(self):
        hook = {
            "event": "UserPromptSubmit",
            "session_id": "s",
            "started_ms": 10,
            "ended_ms": 20,
            "exit_code": 0,
            "stdout": '{"context":"[kw]#42 [90%] wisdom"}',
        }
        ledger = {
            "event": "injected",
            "session_id": "s",
            "ts": 15,
            "ids": ["42"],
            "lane_ms": {"kw": 2},
            "lane_timeout": {"kw": False},
            "hook_ms": 3,
        }
        self.assertEqual(telemetry([hook], [ledger], ["42"])["cohort_injections"], 1)
        bad = deepcopy(ledger)
        bad["ids"] = ["41"]
        with self.assertRaisesRegex(ValueError, "disagree"):
            telemetry([hook], [bad], ["42"])
        with self.assertRaisesRegex(ValueError, "join"):
            telemetry([hook], [], ["42"])
        hook["stdout"] = "{}"
        ledger.update(event="recall_empty", ids=[])
        self.assertEqual(telemetry([hook], [ledger], ["42"])["empty_turns"], 1)


class LeakageTests(unittest.TestCase):
    def test_live_home_socket_and_symlinks_refused(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            live = {
                "home": str(root / "live"),
                "socket": str(root / "live.sock"),
                "mind": str(root / "live/mind"),
            }
            private = root / "private"
            private.mkdir()
            env = {
                "HOME": str(private / "home"),
                "CHITTA_SOCKET_PATH": str(private / "sock"),
                "CHITTA_DB_PATH": str(private / "db"),
                "CHITTA_QUEUE": str(private / "queue"),
                "XDG_RUNTIME_DIR": str(private / "run"),
                "CHITTA_REALM": "project:cc-soul",
                "CHITTA_UTILITY_RECALL": "0",
            }
            guard_environment(env, live, private)
            for key, path in (("HOME", live["home"]), ("CHITTA_SOCKET_PATH", live["socket"])):
                bad = {**env, key: path}
                with self.assertRaises(ValueError):
                    guard_environment(bad, live, private)
                alias = private / ("alias-" + key)
                alias.symlink_to(path)
                bad[key] = str(alias)
                with self.assertRaises(ValueError):
                    guard_environment(bad, live, private)
            for alias in ("CHITTA_HEADLESS", "CC_SOUL_HEADLESS"):
                with self.assertRaises(ValueError):
                    guard_environment({**env, alias: "1"}, live, private)


class BrokerTests(unittest.TestCase):
    def test_host_file_and_execution_tools_denied(self):
        from broker import allowed_request

        for name in (
            "read_transcript",
            "predicate_run",
            "import_soul",
            "read_symbol",
            "compact_wal",
            "forget",
        ):
            self.assertFalse(allowed_request({"method": "tools/call", "params": {"name": name}}))
        self.assertTrue(allowed_request({"method": "tools/call", "params": {"name": "recall"}}))
        self.assertFalse(allowed_request({"method": "eval", "params": {"name": "recall"}}))

    def test_truncated_header_cannot_fabricate_an_id(self):
        hook = {
            "event": "UserPromptSubmit",
            "session_id": "s",
            "started_ms": 10,
            "ended_ms": 20,
            "exit_code": 0,
            "stdout": "[kw]#42 [90%] fact\n[hyb]#4",
        }
        row = {
            "event": "injected",
            "session_id": "s",
            "ts": 15,
            "ids": ["42"],
            "lane_ms": {"kw": 2},
            "lane_timeout": {"kw": False},
            "hook_ms": 3,
        }
        self.assertEqual(telemetry([hook], [row], ["42"])["cohort_injections"], 1)

    def test_runtime_cannot_mount_live_parent_or_future_repo(self):
        from common import validate_runtime_roots

        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            live = root / "live"
            live.mkdir()
            with self.assertRaises(ValueError):
                validate_runtime_roots([root], [live])
