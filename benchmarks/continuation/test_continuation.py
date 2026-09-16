import hashlib
import json
import tempfile
import unittest
from pathlib import Path

import build
import score


class ContinuationTests(unittest.TestCase):
    def test_chronology_hash_and_visible_text(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for sid, date in [("z", "2026-09-01"), ("a", "2026-09-02"), ("b", "2026-09-03")]:
                rows = [
                    {
                        "type": "user",
                        "sessionId": sid,
                        "timestamp": date,
                        "message": {
                            "content": [{"type": "tool_result", "content": "not a prompt"}]
                        },
                    },
                    {
                        "type": "user",
                        "sessionId": sid,
                        "timestamp": date,
                        "message": {"content": "implement change"},
                    },
                    {
                        "type": "assistant",
                        "sessionId": sid,
                        "timestamp": date,
                        "message": {
                            "content": [
                                {"type": "thinking", "thinking": "Next: wrong"},
                                {
                                    "type": "tool_use",
                                    "name": "Read",
                                    "input": {"file_path": "docs/guide.md"},
                                },
                                {"type": "text", "text": "Next: edit docs/guide.md"},
                            ]
                        },
                    },
                ]
                (root / f"{sid}.jsonl").write_text("\n".join(map(json.dumps, rows)))
            result = build.build(root)
            self.assertEqual(result["available"], 2)
            first = result["pairs"][0]
            self.assertEqual((first["previous"]["id"], first["next"]["id"]), ("z", "a"))
            self.assertEqual(first["next"]["first_prompt"], "implement change")
            self.assertNotIn("first_tool", first["previous"])
            self.assertEqual(
                first["previous"]["sha256"],
                hashlib.sha256((root / "z.jsonl").read_bytes()).hexdigest(),
            )
            self.assertEqual(
                score.capsule(first["previous"])["next_action"], "Next: edit docs/guide.md"
            )
            self.assertTrue(score.score(score.capsule(first["previous"]), first["next"]))
            self.assertFalse(score.evaluate(result)["gate"])

    def test_strict_scoring_and_no_future_leak(self):
        following = {"first_tool": {"name": "Read", "input": {"file_path": "docs/guide.md"}}}
        for text in (
            "read guide.md",
            "read the documentation",
            "Read",
            "read docs/guide.md.backup",
        ):
            self.assertFalse(score.score({"next_action": text}, following))
        self.assertTrue(score.score({"next_action": "Read `docs/guide.md`"}, following))
        previous = {
            "last_assistant": "```\nNext: edit docs/guide.md\n```\n> Next: edit docs/guide.md",
            "last_ledger": None,
        }
        self.assertEqual(score.capsule(previous)["next_action"], "")
        previous["last_ledger"] = {"next_action": "Read docs/guide.md", "verified": False}
        self.assertEqual(score.capsule(previous)["next_action"], "")

    def test_shell_targets_are_literal_files(self):
        following = {
            "first_tool": {
                "name": "Bash",
                "input": {"command": "python3 scripts/check.py --mode strict"},
            }
        }
        self.assertTrue(score.score({"next_action": "run scripts/check.py"}, following))
        self.assertTrue(
            score.score({"next_action": "python3 scripts/check.py --mode strict"}, following)
        )
        self.assertFalse(score.score({"next_action": "run check.py"}, following))
        self.assertFalse(score.score({"next_action": "use python3"}, following))


if __name__ == "__main__":
    unittest.main()
