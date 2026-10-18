#!/usr/bin/env python3
"""SEG-072 (ADR 0151): hermetic tests for the M68K ML v3 protocol definitions. Synthetic data only (no ROM, no truth, no model)."""
import os
import sys
import unittest

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools")
sys.path.insert(0, TOOLS)
import segarecomp_ml_v3 as v3  # noqa: E402


class Plan(unittest.TestCase):
    def test_committed_plan_matches_constants_and_digest(self):
        plan = v3.verify_plan()
        self.assertEqual(plan["plan_id"], v3.PLAN_ID)

    def test_fixed_manifest_counts(self):
        self.assertEqual(len(v3.DEVELOPMENT), 12)
        self.assertEqual(len(v3.SEALED), 7)
        self.assertEqual(v3.development_lineages(), [f"D{i:02d}" for i in range(1, 13)])
        d01 = [m["id"] for m in v3.DEVELOPMENT[0]["members"]]
        self.assertEqual(d01, ["s1", "sk", "scd", "s2"])  # every Sonic title in one fold
        d03 = [m["id"] for m in v3.DEVELOPMENT[2]["members"]]
        self.assertEqual(d03, ["sf1", "sf2"])

    def test_pinned_revisions_are_full_shas(self):
        revs = [m["revision"] for d in v3.DEVELOPMENT for m in d["members"]] + [e["revision"] for e in v3.SEALED] + [v3.DIAGNOSTIC["revision"]]
        self.assertEqual(len(revs), 24)  # 16 development members + 7 sealed + TMSS
        for r in revs:
            self.assertRegex(r, r"^[0-9a-f]{40}$")

    def test_tmss_never_counted(self):
        ids = {m["id"] for d in v3.DEVELOPMENT for m in d["members"]} | {e["id"] for e in v3.SEALED}
        self.assertNotIn("tmss", ids)


class Folds(unittest.TestCase):
    def test_leave_one_lineage_out(self):
        folds = v3.lolo_folds(["D01", "D02", "D03"])
        self.assertEqual(folds, [("D01", ["D02", "D03"]), ("D02", ["D01", "D03"]), ("D03", ["D01", "D02"])])

    def test_merged_lineages_leave_together(self):
        folds = dict(v3.lolo_folds(["D05", "D06", "D07"], merges=[["D06", "D05"]]))
        self.assertEqual(folds["D05+D06"], ["D07"])
        self.assertEqual(folds["D07"], ["D05", "D06"])
        self.assertEqual(len(folds), 2)

    def test_lineage_of_images(self):
        self.assertEqual(v3.lineage_of("s2"), "D01")
        self.assertEqual(v3.lineage_of("scd/m07"), "D01")
        self.assertEqual(v3.lineage_of("sf2"), "D03")
        self.assertEqual(v3.lineage_of("d12"), "D12")
        with self.assertRaises(SystemExit):
            v3.lineage_of("e01")

    def test_sealed_barrier(self):
        with self.assertRaises(SystemExit):
            v3.assert_no_sealed(["d05", "e03"])
        v3.assert_no_sealed(["d05", "s1"])
        with self.assertRaises(SystemExit):
            v3.refuse_sealed_path(v3.PRIVATE_ROOT / "sealed" / "x" / "truth.json")
        v3.refuse_sealed_path(v3.PRIVATE_ROOT / "truth" / "d05.json")


class Labels(unittest.TestCase):
    def test_cell_labels_mask_and_positive(self):
        cls = b"D" * 128 + b"U" * 64 + b"D" * 64 + b"I" + b"c" + b"U" * 126 + b"D" * 100
        self.assertEqual(v3.cell_labels(cls), [0, -1, 1, 0])

    def test_word_labels(self):
        self.assertEqual(v3.word_labels(b"IcDDUUDUcD"), [1, 0, -1, -1, 1])

    def test_starts(self):
        self.assertEqual(v3.starts_of(b"DDIcIcU"), [2, 4])


class Metrics(unittest.TestCase):
    def test_frf_ties_counted_and_masked_never_positive(self):
        scores = [0.9, 0.5, 0.5, 0.1, 0.95]
        labels = [1, 1, -1, 0, -1]
        r = v3.frf(scores, labels)
        self.assertEqual(r["cells_needed"], 4)  # 0.95, 0.9, 0.5, 0.5
        self.assertAlmostEqual(r["frf"], 0.8)

    def test_residual_frf_seeds_excluded(self):
        scores = [0.1, 0.9, 0.8, 0.2]
        labels = [1, 1, 0, 0]
        self.assertAlmostEqual(v3.residual_frf(scores, labels, seeds={0}), 0.25)
        self.assertAlmostEqual(v3.residual_frf(scores, labels, seeds=set()), 1.0)

    def test_budget_integer_ceil(self):
        self.assertEqual(v3.budget_cell_count(4096, 35), 1434)
        self.assertEqual(v3.budget_cell_count(3, 35), 2)

    def test_fixed_policy_includes_seeds_and_rounding(self):
        scores = [0.1 * i for i in range(10)]
        r = v3.policy_region("B35", scores, scores, seeds={0})
        self.assertEqual(r["region"], {9, 8, 7, 6, 0})
        self.assertAlmostEqual(r["retained"], 0.5)

    def test_adaptive_policy_is_label_free_and_abstains(self):
        probs = [0.9] * 20 + [0.1] * 80
        self.assertAlmostEqual(v3.density_estimate(probs), 0.2)
        b, abst = v3.adaptive_budget(probs, 0.1)
        self.assertFalse(abst)
        self.assertAlmostEqual(b, 0.3)
        b, abst = v3.adaptive_budget(probs, 0.45)
        self.assertTrue(abst)
        self.assertEqual(b, 1.0)
        r = v3.policy_region("ADAPT", probs, probs, seeds=set(), margin=0.45)
        self.assertEqual(r["retained"], 1.0)
        self.assertTrue(r["abstained"])

    def test_adaptive_margin_fit(self):
        self.assertAlmostEqual(v3.fit_adaptive_margin([(0.30, 0.20), (0.15, 0.20)]), 0.15)
        self.assertAlmostEqual(v3.fit_adaptive_margin([(0.10, 0.30)]), 0.0)
        with self.assertRaises(SystemExit):
            v3.fit_adaptive_margin([])

    def test_misses(self):
        self.assertEqual(v3.misses([0, 130, 300], {0, 2}), 1)


class Selection(unittest.TestCase):
    def row(self, complete, worst, mean, ku=None, misses=0):
        return {"complete": complete, "worst_frf": worst, "mean_retained": mean, "mean_ku": ku, "misses": misses}

    def test_incumbent_retained_without_material_gain(self):
        t = {("M3_FROZEN", "B35"): self.row(True, 0.3, 0.36), ("M4_DENSE@128", "B35"): self.row(True, 0.2, 0.34)}
        s = v3.select_pair(t)
        self.assertEqual(s["pair"], ("M3_FROZEN", "B35"))
        self.assertTrue(s["incumbent_retained"])

    def test_alternative_when_incumbent_incomplete(self):
        t = {("M3_FROZEN", "B35"): self.row(False, 0.5, 0.36, misses=3), ("M3_R", "B45"): self.row(True, 0.3, 0.46)}
        self.assertEqual(v3.select_pair(t)["pair"], ("M3_R", "B45"))

    def test_simplest_in_tie_set(self):
        t = {("M3_FROZEN", "B35"): self.row(False, 0.5, 0.36, misses=1), ("M1_HGB", "ADAPT"): self.row(True, 0.21, 0.25),
             ("M4_DENSE@64", "ADAPT"): self.row(True, 0.20, 0.24)}
        self.assertEqual(v3.select_pair(t)["pair"], ("M1_HGB", "ADAPT"))

    def test_no_complete_pair(self):
        t = {("M3_FROZEN", "B35"): self.row(False, 0.5, 0.36, misses=3), ("M3_R", "B60"): self.row(False, 0.6, 0.61, misses=1)}
        s = v3.select_pair(t)
        self.assertEqual((s["pair"], s["rule"]), (("M3_R", "B60"), "no_complete_pair"))

    def test_classification_precedence(self):
        kw = dict(dev_complete_exists=True, sealed_misses=0, dev_qualified=12, sealed_qualified=7, pair=("M3_R", "B45"),
                  incumbent_retained=False, economics_ok=True, review_concurs=True)
        self.assertEqual(v3.classify(**kw), "M3_RETRAIN_SUFFICIENT")
        self.assertEqual(v3.classify(**(kw | {"sealed_misses": 1, "sealed_qualified": 5})), "RECALL_FAILED")
        self.assertEqual(v3.classify(**(kw | {"sealed_qualified": 6})), "CORPUS_INSUFFICIENT")
        self.assertEqual(v3.classify(**(kw | {"incumbent_retained": True, "pair": ("M3_FROZEN", "B35")})), "ECONOMICS_NOT_JUSTIFIED")
        self.assertEqual(v3.classify(**(kw | {"pair": ("M3_FROZEN", "ADAPT")})), "ADAPTIVE_ONLY_JUSTIFIED")
        self.assertEqual(v3.classify(**(kw | {"pair": ("M4_DENSE@128", "B35")})), "V3_READY_FOR_OPT_IN")
        self.assertEqual(v3.classify(**(kw | {"review_concurs": False})), "ECONOMICS_NOT_JUSTIFIED")
        self.assertEqual(v3.classify(**(kw | {"dev_complete_exists": False})), "RECALL_FAILED")


if __name__ == "__main__":
    unittest.main()
