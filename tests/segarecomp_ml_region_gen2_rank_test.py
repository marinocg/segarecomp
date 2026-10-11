#!/usr/bin/env python3
"""SEG-049-T002 (ADR 0101): hermetic tests for the rank-budget calibration of the frozen gen2 M3 model.

Synthetic data only (no ROM, no truth, no commercial data, no numpy/torch). The research-environment artifacts (final model / frozen record) are
checked when the committed files exist.
"""
import inspect
import json
import os
import random
import shutil
import sys
import tempfile
import unittest

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools")
ROOT = os.path.join(TOOLS, "..")
sys.path.insert(0, TOOLS)
import segarecomp_ml_region_gen2 as g  # noqa: E402
import segarecomp_ml_region_gen2_rank as r  # noqa: E402

PLAN = os.path.join(TOOLS, "segarecomp_ml_region_gen2_rank.plan.json")
T001_PLAN = os.path.join(TOOLS, "segarecomp_ml_region_gen2.plan.json")


class Budget(unittest.TestCase):
    def test_exact_cell_count_integer_arithmetic(self):
        self.assertEqual(r.budget_cell_count(100, 30), 30)  # 100*0.30 in floating point is 30.000000000000004 -> would ceil to 31
        self.assertEqual(r.budget_cell_count(1024, 30), 308)
        self.assertEqual(r.budget_cell_count(1024, 35), 359)
        self.assertEqual(r.budget_cell_count(16384, 50), 8192)
        self.assertEqual(r.budget_cell_count(1, 30), 1)
        self.assertEqual(r.budget_cell_count(7, 50), 4)
        for n in (1, 3, 10, 99, 100, 101, 4096, 12288, 16384):
            for p in r.BUDGET_PERCENT.values():
                self.assertEqual(r.budget_cell_count(n, p), -(-n * p // 100))

    def test_invalid_inputs_rejected(self):
        for n, p in ((0, 30), (-1, 30), (10, 0), (10, 101), (10, 30.5), (True, 30), (10, True)):
            with self.assertRaises(SystemExit, msg=(n, p)):
                r.budget_cell_count(n, p)

    def test_exactly_five_preregistered_budgets(self):
        self.assertEqual(r.BUDGET_PERCENT, {"B30": 30, "B35": 35, "B40": 40, "B45": 45, "B50": 50})


class Ranking(unittest.TestCase):
    def test_stable_descending_then_ordinal_ascending(self):
        scores = [0.5, 0.9, 0.5, 0.1, 0.9, 0.5]
        self.assertEqual(r.rank_order(scores), [1, 4, 0, 2, 5, 3])

    def test_same_scores_same_selection_on_any_input_order_of_ties(self):
        rng = random.Random(7)
        scores = [rng.choice([0.0, 0.25, 0.5, 1.0]) for _ in range(257)]
        first = r.rank_select(scores, 35)
        for _ in range(5):
            self.assertEqual(r.rank_select(list(scores), 35), first)
        self.assertEqual(len(first), r.budget_cell_count(257, 35))

    def test_tie_break_prefers_lower_ordinal_only_among_equal_scores(self):
        scores = [1.0] * 10
        self.assertEqual(r.rank_select(scores, 30), {0, 1, 2})
        scores[9] = 2.0
        self.assertEqual(r.rank_select(scores, 30), {9, 0, 1})

    def test_selection_is_monotone_in_budget(self):
        rng = random.Random(3)
        scores = [rng.random() for _ in range(500)]
        prev = set()
        for p in sorted(r.BUDGET_PERCENT.values()):
            cur = r.rank_select(scores, p)
            self.assertTrue(prev <= cur)
            prev = cur

    def test_nan_and_non_numbers_rejected(self):
        for bad in ([0.1, float("nan")], [0.1, "x"], [0.1, True], []):
            with self.assertRaises(SystemExit):
                r.rank_order(bad)

    def test_rank_is_invariant_to_monotone_rescaling(self):
        rng = random.Random(11)
        scores = [rng.random() for _ in range(300)]
        squashed = [s ** 9 for s in scores]  # a different 'probability scale', same order
        self.assertEqual(r.rank_select(scores, 40), r.rank_select(squashed, 40))


class PolicyIsolation(unittest.TestCase):
    def test_policy_signature_is_scores_and_budget_only(self):
        self.assertEqual(r.policy_signature(), ["scores", "percent"])
        self.assertEqual(list(inspect.signature(r.rank_order).parameters), ["scores"])
        self.assertEqual(list(inspect.signature(r.budget_cell_count).parameters), ["cell_count", "percent"])

    def test_policy_source_cannot_reach_title_rom_truth_or_runtime(self):
        src = "".join(inspect.getsource(f) for f in (r.budget_cell_count, r.rank_order, r.rank_select))
        for word in ("sha", "rom", "truth", "label", "coverage", "runtime", "title", "path", "open(", "import ", "environ"):
            self.assertNotIn(word, src.lower().replace("rank_order", "").replace("rank_select", ""), msg=word)

    def test_ordinal_is_never_a_model_input(self):
        for name in ("cell_ordinal", "ordinal", "cell_id", "cell_index", "title", "truth", "runtime_pc", "address", "rom_sha"):
            with self.assertRaises(SystemExit, msg=name):
                g.assert_input_channels(list(g.BYTE_CHANNELS) + [name])
        g.assert_input_channels(list(g.BYTE_CHANNELS) + list(g.TOKEN_CHANNELS))

    def test_labels_cannot_influence_selection(self):
        scores = [0.9, 0.1, 0.5, 0.7, 0.2]
        a = r.rank_select(scores, 50)
        b = r.rank_select(scores, 50)  # selection has no label parameter at all
        self.assertEqual(a, b)
        self.assertNotIn("labels", r.policy_signature())


class Arithmetic(unittest.TestCase):
    def test_rank_headroom(self):
        self.assertAlmostEqual(r.rank_headroom(30, 0.24), 1.25)
        self.assertAlmostEqual(r.rank_headroom(35, 0.269), 0.35 / 0.269)
        self.assertLess(r.rank_headroom(30, 0.269), 1.25)
        with self.assertRaises(SystemExit):
            r.rank_headroom(30, 0.0)

    def synthetic(self, positives, seeds=()):
        # 100 cells x 128 B; truth = one start in each positive cell
        scores = [0.0] * 100
        for rank, c in enumerate(range(100)):
            scores[c] = 1.0 - c / 100.0
        truth = {c * 128 for c in positives}
        labels = g.cell_labels(truth, 12800)
        return scores, labels, truth, set(seeds), 12800

    def test_c_outside_r_attribution_ml_only_vs_seeds(self):
        scores, labels, truth, seeds, size = self.synthetic([0, 29, 31])
        rec = r.r_stage(scores, labels, truth, {31}, size, 30)
        self.assertEqual(rec["selected_cells"], 30)
        self.assertEqual(rec["c_outside_ml_only"], 1)  # cell 31 is below the budget
        self.assertEqual(rec["c_outside_r"], 0)  # ... but a certain-code seed covers it
        self.assertEqual(rec["r_cells"], 31)
        self.assertAlmostEqual(rec["r_over_rom"], 0.31)
        self.assertAlmostEqual(rec["frf"], 0.32)
        self.assertAlmostEqual(rec["rank_headroom"], 0.30 / 0.32)
        rec2 = r.r_stage(scores, labels, truth, set(), size, 30)
        self.assertEqual(rec2["c_outside_r"], 1)

    def test_r_union_seeds_is_deterministic(self):
        scores, labels, truth, seeds, size = self.synthetic([0, 5])
        a = r.r_stage(scores, labels, truth, {77, 3}, size, 30)["_region"]
        b = r.r_stage(scores, labels, truth, {3, 77}, size, 30)["_region"]
        self.assertEqual(a, b)
        self.assertEqual(a, set(range(30)) | {77})

    def test_r_gate_requires_all_three(self):
        base = {"c_outside_r": 0, "r_over_rom": 0.5, "rank_headroom": 1.3}
        self.assertTrue(r.r_gate(base))
        self.assertFalse(r.r_gate({**base, "c_outside_r": 1}))
        self.assertFalse(r.r_gate({**base, "r_over_rom": 0.61}))
        self.assertTrue(r.r_gate({**base, "r_over_rom": 0.60}))
        self.assertFalse(r.r_gate({**base, "rank_headroom": 1.2499}))
        self.assertTrue(r.r_gate({**base, "rank_headroom": 1.25}))

    def test_post_ml_loss_and_k_attribution(self):
        truth = {0, 128, 256, 4096}
        region = {0, 1}  # cells 0,1 -> starts 0 and 128; 256 (cell 2) and 4096 (cell 32) are outside R
        att = g.attribute(truth, region, {0}, True)
        self.assertEqual((att["c_outside_r"], att["c_outside_k"], att["post_ml_losses"]), (2, 3, 1))
        self.assertEqual(att["post_ml_status"], "POST_ML_PRUNING_BLOCKER")
        clean = g.attribute({0, 128}, {0, 1}, {0, 128}, True)
        self.assertEqual((clean["c_outside_k"], clean["post_ml_losses"], clean["post_ml_status"]), (0, 0, "OK"))
        rej = g.attribute(truth, region, None, False)
        self.assertEqual(rej["post_ml_status"], "VALIDATOR_REJECTED_PROPOSAL")
        self.assertIsNone(rej["c_outside_k"])


class Selection(unittest.TestCase):
    def folds(self, passing_from):
        out = {}
        for t in g.TITLE_ORDER:
            out[t] = {}
            for b, p in r.BUDGET_PERCENT.items():
                ok = p >= passing_from
                out[t][b] = {"c_outside_r": 0 if ok else 1, "r_over_rom": 0.4, "rank_headroom": 1.3,
                             "validator": "accepted", "attribution": {"c_outside_k": 0}}
        return out

    def test_smallest_passing_budget_is_chosen(self):
        self.assertEqual(r.choose_budget(self.folds(30)), "B30")
        self.assertEqual(r.choose_budget(self.folds(40)), "B40")
        self.assertEqual(r.choose_budget(self.folds(50)), "B50")
        self.assertIsNone(r.choose_budget(self.folds(51)))

    def test_one_failing_title_disqualifies_the_budget(self):
        f = self.folds(30)
        f["sk"]["B30"]["rank_headroom"] = 1.1
        self.assertEqual(r.choose_budget(f), "B35")
        f["ps2"]["B35"]["attribution"]["c_outside_k"] = 1
        self.assertEqual(r.choose_budget(f), "B40")
        f["s1"]["B40"]["validator"] = "rejected"
        self.assertEqual(r.choose_budget(f), "B45")

    def test_missing_k_stage_never_selects(self):
        f = self.folds(30)
        for t in f:
            for b in f[t]:
                del f[t][b]["validator"]
        self.assertIsNone(r.choose_budget(f))

    def test_classification(self):
        self.assertEqual(r.classify(selected=None, runtime_ok=None, economics_ok=None, frozen_ok=False), "GEN2_CALIBRATION_UNSOLVED")
        self.assertEqual(r.classify(selected="B35", runtime_ok=False, economics_ok=True, frozen_ok=True), "GEN2_CALIBRATION_VALID_RUNTIME_BLOCKED")
        self.assertEqual(r.classify(selected="B35", runtime_ok=True, economics_ok=False, frozen_ok=True), "GEN2_CALIBRATION_VALID_ECONOMICS_BLOCKED")
        self.assertEqual(r.classify(selected="B35", runtime_ok=True, economics_ok=True, frozen_ok=True), "GEN2_CALIBRATION_READY_FOR_NATIVE_PORT")
        self.assertEqual(r.classify(selected="B35", runtime_ok=True, economics_ok=True, frozen_ok=False), "GEN2_CALIBRATION_UNSOLVED")


class Plan(unittest.TestCase):
    def test_plan_frozen_before_results_and_matches_code(self):
        plan = r.verify_plan(PLAN, T001_PLAN)
        self.assertEqual(plan["status"], "FROZEN BEFORE ANY B30-B50 EVALUATION")
        self.assertEqual(list(plan["policy"]["budgets_percent"]), ["B30", "B35", "B40", "B45", "B50"])
        self.assertNotIn("results", plan)
        self.assertEqual(plan["frozen_from"]["representation_sha256"], g.representation_sha256())
        self.assertEqual(plan["gates"]["r_stage"]["rank_headroom_min"], 1.25)
        self.assertEqual(plan["gates"]["r_stage"]["r_over_rom_max"], 0.60)

    def test_plan_cannot_gain_a_sixth_budget(self):
        with tempfile.TemporaryDirectory() as tmp:
            with open(PLAN) as handle:
                plan = json.load(handle)
            plan["policy"]["budgets_percent"]["B32"] = 32
            path = os.path.join(tmp, "p.json")
            with open(path, "w") as handle:
                json.dump(plan, handle)
            with self.assertRaises(SystemExit):
                r.verify_plan(path, T001_PLAN)

    def test_plan_is_derived_from_the_t001_plan(self):
        self.assertEqual(r.t001_plan_digest(T001_PLAN), "84ab1134fe5ac5d1fa7c2af17749bd2d00854aae1350e0ad746e34eeab733af9")
        with tempfile.TemporaryDirectory() as tmp:
            crlf = os.path.join(tmp, "t001.json")
            with open(T001_PLAN, "rb") as src, open(crlf, "wb") as dst:
                dst.write(src.read().replace(b"\n", b"\r\n"))
            self.assertEqual(r.t001_plan_digest(crlf), r.t001_plan_digest(T001_PLAN))  # line-ending independent

    def test_m3_architecture_and_recipe_unchanged(self):
        with open(T001_PLAN) as handle:
            t001 = json.load(handle)
        self.assertEqual(t001["models"]["M3"]["trainable_parameters"], 77441)
        self.assertEqual(t001["recipe"]["epochs"], 30)
        self.assertEqual(t001["recipe"]["seed"], 49)
        self.assertEqual(t001["representation"]["sha256"], g.representation_sha256())


class V1Guard(unittest.TestCase):
    def test_v1_identity_guard_is_line_ending_independent(self):
        self.assertEqual(g.v1_artifacts_unchanged(ROOT), [])
        with tempfile.TemporaryDirectory() as tmp:
            os.makedirs(os.path.join(tmp, "tools"))
            for name in g.V1_ARTIFACT_SHA256:
                with open(os.path.join(ROOT, name), "rb") as src:
                    data = src.read().replace(b"\r\n", b"\n")
                with open(os.path.join(tmp, name), "wb") as dst:
                    dst.write(data.replace(b"\n", b"\r\n"))  # a Windows autocrlf checkout
            self.assertEqual(g.v1_artifacts_unchanged(tmp), [])
            with open(os.path.join(tmp, "tools", "segarecomp_ml_region.model.json"), "ab") as sink:
                sink.write(b" ")
            self.assertEqual(g.v1_artifacts_unchanged(tmp), ["tools/segarecomp_ml_region.model.json"])


if __name__ == "__main__":
    unittest.main()
