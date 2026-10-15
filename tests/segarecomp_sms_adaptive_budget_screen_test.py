#!/usr/bin/env python3
"""SEG-060: the oracle-bound screen is deterministic and its recorded conclusions hold (stdlib only, no ROM/truth)."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import segarecomp_sms_adaptive_budget_screen as S  # noqa: E402


class Screen(unittest.TestCase):
    def setUp(self):
        self.rep = S.run()

    def test_deterministic(self):
        self.assertEqual(self.rep, S.run())
        self.assertEqual(S.svg(self.rep["rows"]), S.svg(S.run()["rows"]))

    def test_fixed_65_covers_every_development_lineage(self):
        self.assertTrue(all(r["full"] <= S.CAP for r in self.rep["rows"].values()))
        self.assertEqual(self.rep["max_headroom_lineage"], "supertennis")
        self.assertLess(self.rep["min_headroom"], 0.06)

    def test_oracle_density_rule_misses_solomon(self):
        for name in ("density_x_max_ratio", "density_x_max_ratio_x1.2"):
            self.assertEqual(self.rep["rules"][name]["summary"]["misses"], ["solomon"])

    def test_only_passing_rule_needs_label_derived_tail_quantile(self):
        passing = [n for n, v in self.rep["rules"].items() if not v["summary"]["misses"]]
        self.assertEqual(passing, ["r99_x_max_ratio_x1.1"])
        self.assertEqual(self.rep["rules"]["r99_x_max_ratio_x1.1"]["label_derived_input"], "retained_for_recall_0.99")

    def test_hard_and_easy_lineages_are_not_separated_by_coarse_recall(self):
        r = self.rep["rows"]
        self.assertLess(abs(r["solomon"]["r90"] - r["kots"]["r90"]), 0.02)
        self.assertGreater(r["solomon"]["full"] - r["kots"]["full"], 0.30)


if __name__ == "__main__":
    unittest.main()
