"""Checks for the quality gate's relation accounting."""
import unittest

from think_ablation import agreement


class AgreementTest(unittest.TestCase):
    def test_empty_is_not_evidence(self):
        self.assertEqual(agreement([([], [])], True)["f1"], 0)

    def test_relaxed_normalizes_not_semantics(self):
        pairs = [([['Build_Cache', 'leads_to', 'Fast-build']],
                  [['build cache', 'leads_to', 'fast build']])]
        self.assertEqual(agreement(pairs, False)["f1"], 0)
        self.assertEqual(agreement(pairs, True)["f1"], 1)
        self.assertEqual(agreement([([['a', 'uses', 'b']], [['a', 'causes', 'b']])], True)["f1"], 0)

    def test_dedup_is_per_memory(self):
        t = ['a', 'uses', 'b']
        self.assertEqual(agreement([([t, t], [t]), ([t], [])], False),
                         {"matched": 1, "on": 2, "off": 1, "f1": 2 / 3})


if __name__ == '__main__':
    unittest.main()
