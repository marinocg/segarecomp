#!/usr/bin/env python3
"""SEG-061: the frozen label-free score features are deterministic, rank based and respond to ties (stdlib only, no ROM/truth)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import segarecomp_m68k_adaptive_budget as A  # noqa: E402


class Features(unittest.TestCase):
    def test_deterministic_and_order_invariant(self):
        s = [((i * 37) % 1000 + 0.5) / 1000 for i in range(1000)]
        self.assertEqual(A.features(s), A.features(list(reversed(s))))

    def test_tie_group_detected(self):
        flat = [0.5] * 600 + [0.9] * 400
        self.assertGreater(A.features(flat)["tie_frac25"], 0.39)
        spread = [(i + 1) / 1001 for i in range(1000)]
        self.assertLess(A.features(spread)["tie_frac25"], 0.01)

    def test_no_label_or_identity_input(self):
        self.assertEqual(set(A.features([0.1, 0.9, 0.5, 0.2]).keys()),
                         {"L10", "L20", "L30", "shape_L10_L30", "tie_frac25", "max_gap_10_40"})

    def test_frozen_family_c_is_not_evaluated(self):
        self.assertIn("NOT EVALUATED", A.FEATURE_SPEC["C_structural"])


if __name__ == "__main__":
    unittest.main()
