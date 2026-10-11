#!/usr/bin/env python3
"""SEG-049 (ADR 0100): hermetic tests for the second-generation ML region experiment tooling.

Synthetic data only: no ROM, no commercial data, no truth. The stdlib tests always run; the numpy/torch/scikit-learn tests run only where
those private research packages are importable (they are NOT product dependencies and are absent from hermetic CI).
"""
import hashlib
import importlib.util
import json
import os
import shutil
import sys
import tempfile
import unittest

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools")
ROOT = os.path.join(TOOLS, "..")
sys.path.insert(0, TOOLS)
import segarecomp_ml_region_gen2 as g  # noqa: E402

HAVE_TORCH = all(importlib.util.find_spec(m) is not None for m in ("numpy", "torch", "sklearn"))


class Representation(unittest.TestCase):
    def test_cell_labelling(self):
        labels = g.cell_labels({0, 126, 128, 254, 1024}, 2048)
        self.assertEqual(len(labels), 16)
        self.assertEqual([i for i, y in enumerate(labels) if y], [0, 1, 8])
        self.assertEqual(g.cell_labels(set(), 256), [0, 0])
        with self.assertRaises(SystemExit):
            g.cell_labels({3}, 256)  # odd start
        with self.assertRaises(SystemExit):
            g.cell_labels({256}, 256)  # outside

    def test_partial_last_cell(self):
        self.assertEqual(g.cell_count(130), 2)
        self.assertEqual(g.cell_labels({128}, 130), [0, 1])

    def test_context_construction(self):
        rom = bytes((i * 7 + 3) & 0xFF for i in range(8192))
        words = g.context_word_indices(20, len(rom) // 2)
        self.assertEqual(len(words), g.CONTEXT_WORDS)
        self.assertEqual(g.CONTEXT_BYTES, 1024)
        self.assertEqual(words[g.CONTEXT_PAD_WORDS:g.CONTEXT_PAD_WORDS + g.CELL_WORDS], list(range(20 * 64, 21 * 64)))
        self.assertEqual(words[0], 20 * 64 - 224)
        self.assertEqual(words[-1], 20 * 64 + 64 + 224 - 1)
        self.assertEqual(g.context_bytes_of(rom, 20), rom[20 * 128 - 448:20 * 128 + 128 + 448])

    def test_rom_edge_padding_is_reflection_not_a_sentinel(self):
        n = 4096  # words
        first = g.context_word_indices(0, n)
        self.assertEqual(first[g.CONTEXT_PAD_WORDS - 1], 0)  # word -1 reflects to word 0
        self.assertEqual(first[g.CONTEXT_PAD_WORDS - 2], 1)
        self.assertEqual(first[0], 223)
        last = g.context_word_indices(n // 64 - 1, n)
        self.assertEqual(last[-1], n - 224)
        self.assertEqual(last[-224], n - 1)
        self.assertEqual(last[-225], n - 1)  # symmetric reflection duplicates the edge word
        # tiny ROMs stay defined (periodic reflection)
        self.assertEqual([g.reflect_index(i, 3) for i in range(-4, 8)], [2, 2, 1, 0, 0, 1, 2, 2, 1, 0, 0, 1])

    def test_context_is_translation_invariant_in_the_interior(self):
        block = bytes((i * 31 + 5) & 0xFF for i in range(2048))
        rom_a = bytes(1024) + block + bytes(1024)
        rom_b = bytes(3072) + block + bytes(4096)
        # the same 2 KiB interior content at different ROM positions yields identical contexts for the cell in the middle
        cell_a = (1024 + 1024) // 128
        cell_b = (3072 + 1024) // 128
        self.assertEqual(g.context_bytes_of(rom_a, cell_a), g.context_bytes_of(rom_b, cell_b))

    def test_no_address_ordinal_or_position_input(self):
        g.assert_input_channels(list(g.BYTE_CHANNELS) + list(g.TOKEN_CHANNELS))
        g.assert_input_channels(list(g.BYTE_CHANNELS))
        for bad in ("title_id", "rom_offset", "cell_ordinal", "abs_target", "is_first_cell", "rom_size", "absolute_address", "window_start",
                    "runtime_count", "truth_label", "tok_target", "source_symbol", "rom_sha", "relative_position", "is_last_cell"):
            with self.assertRaises(SystemExit, msg=bad):
                g.assert_input_channels(list(g.BYTE_CHANNELS) + [bad])
        with self.assertRaises(SystemExit):
            g.assert_input_channels(["byte_hi", "byte_hi"])
        with self.assertRaises(SystemExit):
            g.assert_input_channels(["byte_hi", "mystery"])

    def test_mutation_title_offset_target_injection_rejected(self):
        for injected in ("title_identity", "rom_offset_words", "decoded_target_address"):
            with self.assertRaises(SystemExit):
                g.assert_input_channels(list(g.BYTE_CHANNELS) + list(g.TOKEN_CHANNELS) + [injected])

    def test_token_schema_deterministic_and_target_free(self):
        self.assertEqual(g.token_schema_sha256(), g.token_schema_sha256())
        self.assertRegex(g.token_schema_sha256(), r"^[0-9a-f]{64}$")
        g.assert_token_schema_clean()
        self.assertEqual(g.TOKEN_FIELDS, ("status", "length", "family", "control", "ea_src", "ea_dst", "width", "flags"))
        self.assertTrue(all("target" not in f and "addr" not in f for f in g.TOKEN_FIELDS))
        self.assertIn("decoded target address", g.TOKEN_DEFINITION["never_emitted"])
        # changing the schema changes the hash (mutation)
        saved = dict(g.TOKEN_CARDINALITY)
        try:
            g.TOKEN_CARDINALITY["status"] = 4
            self.assertNotEqual(g.token_schema_sha256(), hashlib.sha256(b"").hexdigest())
            mutated = g.token_schema_sha256()
        finally:
            g.TOKEN_CARDINALITY.clear()
            g.TOKEN_CARDINALITY.update(saved)
        self.assertNotEqual(mutated, g.token_schema_sha256())
        self.assertRegex(g.representation_sha256(), r"^[0-9a-f]{64}$")

    def test_token_report_parser(self):
        n = 6
        body = bytes([2, 1, 1, 0, 0, 0, 2, 0] * n)
        data = (g.TOKEN_HEADER.format(n=n) + "\n").encode() + body + b"end\n"
        self.assertEqual(g.parse_token_report(data, n * 2), body)
        for bad in (data[:-1], data.replace(b"positions 6", b"positions 7"), data[:-6] + b"zend\n",
                    (g.TOKEN_HEADER.format(n=n) + "\n").encode() + bytes([9] + [0] * 7) * n + b"end\n"):
            with self.assertRaises(SystemExit):
                g.parse_token_report(bad, n * 2)


class Splits(unittest.TestCase):
    def test_five_way_loto_identity(self):
        folds = g.loto_folds()
        self.assertEqual([h for h, _ in folds], ["s1", "flicky", "sf1", "ps2", "sk"])
        for held, train in folds:
            self.assertEqual(len(train), 4)
            self.assertNotIn(held, train)
            self.assertEqual(sorted(train + [held]), sorted(g.TITLE_ORDER))
        self.assertEqual(dict(folds)["s1"], ["flicky", "sf1", "ps2", "sk"])
        self.assertEqual(dict(folds)["sk"], ["s1", "flicky", "sf1", "ps2"])

    def test_title_split_isolation(self):
        g.assert_split(["a", "b"], "c", calibration_ids=["a"], scaler_ids=["a", "b"], mining_ids=["b"])
        with self.assertRaises(SystemExit):
            g.assert_split(["a", "c"], "c")
        with self.assertRaises(SystemExit):
            g.assert_split([], "c")
        for kw in ("calibration_ids", "scaler_ids", "mining_ids", "early_stop_ids"):
            with self.assertRaises(SystemExit, msg=kw):
                g.assert_split(["a", "b"], "c", **{kw: ["c"]})
            with self.assertRaises(SystemExit, msg=kw):
                g.assert_split(["a", "b"], "c", **{kw: ["z"]})

    def test_inner_folds(self):
        inner = g.inner_folds(["a", "b", "c", "d"])
        self.assertEqual(len(inner), 4)
        for scored, train in inner:
            self.assertNotIn(scored, train)
            self.assertEqual(len(train), 3)
        with self.assertRaises(SystemExit):
            g.inner_folds(["a", "b"])

    def test_corpus_identities(self):
        self.assertEqual(sorted(g.CORPUS), sorted(g.TITLE_ORDER))
        self.assertEqual({k: v["truth_starts"] for k, v in g.CORPUS.items()},
                         {"s1": 24180, "flicky": 5707, "sf1": 44740, "ps2": 19261, "sk": 119319})
        for meta in g.CORPUS.values():
            self.assertRegex(meta["rom_sha256"], r"^[0-9a-f]{64}$")
            self.assertRegex(meta["truth_sha256"], r"^[0-9a-f]{64}$")
            self.assertRegex(meta["revision"], r"^[0-9a-f]{40}$")


class Metrics(unittest.TestCase):
    def test_frf_arithmetic(self):
        r = g.frf([0.9, 0.8, 0.1, 0.5], [1, 0, 0, 1])
        self.assertEqual((r["cells_needed"], r["frf"], r["min_positive_score"]), (3, 0.75, 0.5))
        self.assertEqual(g.frf([0.9, 0.8, 0.1, 0.5], [1, 0, 1, 0])["frf"], 1.0)
        self.assertEqual(g.frf([0.9, 0.2, 0.1, 0.05], [1, 0, 0, 0])["frf"], 0.25)
        # ties with the lowest positive are all counted (pessimistic)
        self.assertEqual(g.frf([0.5, 0.5, 0.5, 0.1], [1, 0, 0, 0])["cells_needed"], 3)
        self.assertEqual(g.frf([0.7] * 10, [1] + [0] * 9)["frf"], 1.0)
        with self.assertRaises(SystemExit):
            g.frf([0.1, 0.2], [0, 0])
        with self.assertRaises(SystemExit):
            g.frf([0.1], [1, 0])

    def test_c_outside_r_and_attribution(self):
        truth = [0, 130, 260, 400]  # cells 0, 1, 2, 3
        self.assertEqual(g.c_outside_cells(truth, {0, 1, 3}), [260])
        self.assertEqual(g.c_outside_cells(truth, {0, 1, 2, 3}), [])
        # R contains everything, K lost one instruction: a POST-ML loss, never an ML false negative
        att = g.attribute(truth, {0, 1, 2, 3}, [0, 130, 400], True)
        self.assertEqual((att["ml_false_negatives"], att["c_outside_r"], att["c_outside_k"], att["post_ml_losses"]), (0, 0, 1, 1))
        self.assertEqual(att["post_ml_status"], "POST_ML_PRUNING_BLOCKER")
        # R misses one instruction (an ML false negative) and K additionally lost another one
        att = g.attribute(truth, {0, 1, 3}, [0, 400], True)
        self.assertEqual((att["ml_false_negatives"], att["c_outside_k"], att["post_ml_losses"]), (1, 2, 1))
        att = g.attribute(truth, {0, 1, 2, 3}, truth, True)
        self.assertEqual((att["c_outside_k"], att["post_ml_losses"], att["post_ml_status"]), (0, 0, "OK"))
        # validator rejection of an otherwise complete proposal is a post-ML failure, not an ML miss
        att = g.attribute(truth, {0, 1, 2, 3}, None, False)
        self.assertEqual((att["ml_false_negatives"], att["post_ml_status"]), (0, "VALIDATOR_REJECTED_PROPOSAL"))

    def test_selection_and_region_bytes(self):
        self.assertEqual(g.selected_cells([0.1, 0.5, 0.9], 0.5), {1, 2})
        self.assertEqual(g.region_bytes({0, 1}, 130), 130)
        self.assertEqual(g.region_bytes({0}, 1000), 128)
        text = g.regions_text("0" * 64, 1024, {0, 1, 5})
        self.assertIn("range 00000000 00000100", text)
        self.assertIn("range 00000280 00000300", text)
        self.assertEqual(g.seed_cells("0 7e 80 ffffff", 1024), {0, 1})

    def test_prediction_digest_is_deterministic(self):
        a = g.prediction_digest({"x": [0.1, 0.2], "y": [0.3]})
        self.assertEqual(a, g.prediction_digest({"y": [0.3], "x": [0.1, 0.2]}))
        self.assertNotEqual(a, g.prediction_digest({"x": [0.1, 0.2000001], "y": [0.3]}))


class Calibration(unittest.TestCase):
    TRAIN = ["a", "b", "c", "d"]

    def scores(self):
        return ({"a": [0.9, 0.05, 0.8], "b": [0.02, 0.5, 0.6], "c": [0.7, 0.01, 0.3], "d": [0.4, 0.4, 0.001]},
                {"a": [1, 0, 1], "b": [1, 0, 1], "c": [1, 0, 1], "d": [1, 0, 0]})

    def test_inner_minimum_positive(self):
        s, y = self.scores()
        self.assertEqual(g.inner_oof_min_positive(s, y, train_ids=self.TRAIN, held_out="H"), 0.02)

    def test_held_out_title_cannot_calibrate(self):
        s, y = self.scores()
        s["H"], y["H"] = [0.0001, 0.9], [1, 0]
        with self.assertRaises(SystemExit):
            g.inner_oof_min_positive(s, y, train_ids=self.TRAIN, held_out="H")
        s, y = self.scores()
        with self.assertRaises(SystemExit):
            g.inner_oof_min_positive(s, y, train_ids=self.TRAIN + ["H"], held_out="H")
        s, y = self.scores()
        del s["d"]
        with self.assertRaises(SystemExit):  # an incomplete inner score set is rejected rather than silently shrinking the calibration
            g.inner_oof_min_positive(s, y, train_ids=self.TRAIN, held_out="H")

    def test_mutation_outer_scores_for_threshold_rejected(self):
        s, y = self.scores()
        s["H"], y["H"] = [0.5], [1]
        with self.assertRaises(SystemExit):
            g.inner_oof_min_positive(s, y, train_ids=self.TRAIN, held_out="H")

    def test_policies_q0_to_q3(self):
        self.assertEqual(g.POLICY_FACTORS, {"Q0": 1.0, "Q1": 0.5, "Q2": 0.25, "Q3": 0.1})
        self.assertEqual([g.threshold_for(0.02, f) for f in g.POLICY_FACTORS.values()], [0.02, 0.01, 0.005, 0.002])
        with self.assertRaises(SystemExit):
            g.threshold_for(0.02, 0.0)
        with self.assertRaises(SystemExit):
            g.threshold_for(0.0, 0.5)
        with self.assertRaises(SystemExit):
            g.threshold_for(0.02, 1.5)

    def fold(self, **per_policy):
        return {"policies": {n: {"c_outside_r": per_policy.get(n, (0, 0.4))[0], "r_over_rom": per_policy.get(n, (0, 0.4))[1]} for n in g.POLICY_FACTORS}}

    def test_choose_highest_passing_factor(self):
        ok = self.fold()
        bad_q0 = self.fold(Q0=(3, 0.2))
        self.assertEqual(g.choose_policy([ok, ok]), "Q0")
        self.assertEqual(g.choose_policy([ok, bad_q0]), "Q1")
        two = self.fold(Q0=(1, 0.2), Q1=(1, 0.3), Q2=(0, 0.7))
        self.assertEqual(g.choose_policy([ok, two]), "Q3")
        none = self.fold(Q0=(1, 0.2), Q1=(1, 0.2), Q2=(1, 0.2), Q3=(0, 0.61))
        self.assertIsNone(g.choose_policy([ok, none]))


class Selection(unittest.TestCase):
    COMPLEXITY = {"M1": (0, 0), "M2": (1, 0), "M3": (1, 1), "M4": (1, 2)}

    @staticmethod
    def table(**rows):
        return {n: {"frf": dict(zip(g.TITLE_ORDER, v))} for n, v in rows.items()}

    def test_viability_filter(self):
        t = self.table(M1=[0.2, 0.2, 0.2, 0.2, 0.61], M3=[0.3, 0.3, 0.3, 0.3, 0.3])
        self.assertEqual(g.select_model(t, self.COMPLEXITY), "M3")
        t = self.table(M1=[0.2, 0.2, 0.2, 0.2, 0.61])
        self.assertIsNone(g.select_model(t, self.COMPLEXITY))

    def test_simpler_wins_unless_materially_better(self):
        t = self.table(M1=[0.30] * 5, M3=[0.29] * 5)
        self.assertEqual(g.select_model(t, self.COMPLEXITY), "M1")
        t = self.table(M1=[0.40] * 5, M3=[0.30] * 5)
        self.assertEqual(g.select_model(t, self.COMPLEXITY), "M3")
        t = self.table(M1=[0.20, 0.20, 0.20, 0.20, 0.40], M3=[0.25, 0.25, 0.25, 0.25, 0.25])
        self.assertEqual(g.select_model(t, self.COMPLEXITY), "M3")  # worst-title materially lower

    def test_classification(self):
        c = g.classify
        self.assertEqual(c(representation_viable=False, policy=None, r_gate_all=False, k_clean=False, validator_all=False), "GEN2_ML_INSUFFICIENT")
        self.assertEqual(c(representation_viable=True, policy=None, r_gate_all=False, k_clean=False, validator_all=False),
                         "GEN2_REPRESENTATION_VALID_CALIBRATION_UNSOLVED")
        self.assertEqual(c(representation_viable=True, policy="Q1", r_gate_all=True, k_clean=False, validator_all=True),
                         "GEN2_ML_VALID_POSTPROCESS_BLOCKED")
        self.assertEqual(c(representation_viable=True, policy="Q1", r_gate_all=True, k_clean=True, validator_all=False),
                         "GEN2_ML_VALID_POSTPROCESS_BLOCKED")
        self.assertEqual(c(representation_viable=True, policy="Q0", r_gate_all=True, k_clean=True, validator_all=True),
                         "GEN2_ML_READY_FOR_NATIVE_SPIKE")


class HardPositives(unittest.TestCase):
    def test_mining_uses_training_titles_only(self):
        scores = {"a": [0.9, 0.1, 0.5, 0.2], "b": [0.8, 0.05, 0.6, 0.7]}
        labels = {"a": [1, 0, 1, 1], "b": [1, 0, 1, 1]}
        hard = g.hard_positive_cells(scores, labels, train_ids=["a", "b"], held_out="H", quantile=0.25)
        self.assertEqual(hard, {"a": {3}, "b": set()})
        with self.assertRaises(SystemExit):
            g.hard_positive_cells({**scores, "H": [0.0]}, {**labels, "H": [1]}, train_ids=["a", "b"], held_out="H", quantile=0.25)
        with self.assertRaises(SystemExit):
            g.hard_positive_cells({"a": scores["a"]}, {"a": labels["a"]}, train_ids=["a", "b"], held_out="H", quantile=0.25)
        with self.assertRaises(SystemExit):
            g.hard_positive_cells(scores, labels, train_ids=["a", "b"], held_out="H", quantile=1.0)


class Guards(unittest.TestCase):
    def test_runtime_coverage_cannot_be_a_label_source(self):
        g.refuse_runtime_label_paths(["/x/s1.1.u"])
        for name in ("coverage.bitmap", "s1.oracle.txt", "run.trace", "runtime_pcs.u", "executed_pcs.txt"):
            with self.assertRaises(SystemExit, msg=name):
                g.refuse_runtime_label_paths([f"/x/{name}"])

    def test_production_v1_artifacts_unchanged(self):
        self.assertEqual(g.v1_artifacts_unchanged(ROOT), [])
        with tempfile.TemporaryDirectory() as tmp:
            os.makedirs(os.path.join(tmp, "tools"))
            for name in g.V1_ARTIFACT_SHA256:
                shutil.copy(os.path.join(ROOT, name), os.path.join(tmp, name))
            self.assertEqual(g.v1_artifacts_unchanged(tmp), [])
            with open(os.path.join(tmp, "tools", "segarecomp_ml_region.model.json"), "a") as sink:
                sink.write(" ")
            self.assertEqual(g.v1_artifacts_unchanged(tmp), ["tools/segarecomp_ml_region.model.json"])
            os.remove(os.path.join(tmp, "tools", "segarecomp_ml_region.py"))
            self.assertIn("tools/segarecomp_ml_region.py", g.v1_artifacts_unchanged(tmp))

    def test_frozen_artifact_digest_verification(self):
        with tempfile.TemporaryDirectory() as tmp:
            model, plan = os.path.join(tmp, "m.json"), os.path.join(tmp, "p.json")
            open(model, "w").write("{}")
            open(plan, "w").write('{"a":1}')
            frozen = {"model_artifact": {"file": "m.json", "sha256": g.sha256_file(model)},
                      "plan_artifact": {"file": "p.json", "sha256": g.sha256_file(plan)}}
            path = os.path.join(tmp, "f.json")
            open(path, "w").write(json.dumps(frozen))
            self.assertEqual(g.verify_frozen_artifact(path), frozen)
            open(model, "w").write("{ }")
            with self.assertRaises(SystemExit):
                g.verify_frozen_artifact(path)
            open(model, "w").write("{}")
            frozen["plan_artifact"]["sha256"] = "0" * 64
            open(path, "w").write(json.dumps(frozen))
            with self.assertRaises(SystemExit):
                g.verify_frozen_artifact(path)

    def test_frozen_plan_file(self):
        plan = json.load(open(os.path.join(TOOLS, "segarecomp_ml_region_gen2.plan.json")))
        self.assertEqual(sorted(plan["models"]), ["M0", "M1", "M2", "M3", "M4"])
        self.assertEqual(plan["representation"]["sha256"], g.representation_sha256())
        self.assertEqual(plan["representation"]["token_schema_sha256"], g.token_schema_sha256())
        self.assertEqual(plan["v1_artifact_sha256"], g.V1_ARTIFACT_SHA256)
        self.assertEqual(plan["corpus"], g.CORPUS)
        self.assertEqual([f["held_out"] for f in plan["loto"]], list(g.TITLE_ORDER))
        self.assertLessEqual(plan["recipe"]["epochs"], 30)
        for name in ("M2", "M3", "M4"):
            self.assertLessEqual(plan["models"][name]["trainable_parameters"], 500000)
        self.assertEqual(plan["calibration"]["policies"], g.POLICY_FACTORS)
        self.assertEqual(plan["recipe"]["hard_positive"]["loss_multiplier"], 4.0)
        self.assertLessEqual(plan["recipe"]["hard_positive"]["loss_multiplier"], 4)


@unittest.skipUnless(HAVE_TORCH, "numpy/torch/scikit-learn (private research environment) not installed")
class PrivateEnvironment(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import numpy
        import segarecomp_ml_region_gen2_train as t
        cls.t, cls.np = t, numpy

    def fake_title(self, tid, cells, seed):
        t, np = self.t, self.np
        rng = np.random.RandomState(seed)
        title = t.Title.__new__(t.Title)
        title.id = tid
        title.n_cells, title.n_words, title.size = cells, cells * 64, cells * 128
        title.hi = rng.randint(0, 256, title.n_words).astype(np.uint8)
        title.lo = rng.randint(0, 256, title.n_words).astype(np.uint8)
        title.rom = bytes(np.stack([title.hi, title.lo], axis=1).reshape(-1))
        title.tokens = np.stack([rng.randint(0, g.TOKEN_CARDINALITY[f], title.n_words) for f in g.TOKEN_FIELDS], axis=1).astype(np.uint8)
        title.labels = rng.randint(0, 2, cells).astype(np.uint8)
        title.labels[0] = 1
        title.seeds, title.universe, title.truth, title._padded = set(), [], [], None
        return title

    def test_padded_arrays_match_reference_context(self):
        title = self.fake_title("x", 12, 1)
        hi, lo, tok = title.padded()
        for cell in (0, 3, 11):
            words = g.context_word_indices(cell, title.n_words)
            lo_i = cell * 64
            self.assertEqual(list(hi[lo_i:lo_i + 512]), [title.hi[w] for w in words])
            self.assertEqual(list(lo[lo_i:lo_i + 512]), [title.lo[w] for w in words])
            self.assertEqual([tuple(r) for r in tok[lo_i:lo_i + 512]], g.context_tokens_of(bytes(title.tokens.reshape(-1)), title.size, cell))

    def test_architecture_and_parameter_bound(self):
        plan = json.load(open(os.path.join(TOOLS, "segarecomp_ml_region_gen2.plan.json")))
        for name, tokens in (("M2", False), ("M3", True)):
            self.assertEqual(self.t.parameter_count(self.t.build_model(tokens)), plan["models"][name]["trainable_parameters"])

    def test_segment_evaluation_equals_per_cell_evaluation_and_is_deterministic(self):
        import torch
        t = self.t
        torch.manual_seed(5)
        title = self.fake_title("x", 70, 2)
        outputs = []
        for _ in range(2):
            torch.manual_seed(5)
            model = t.build_model(True)
            outputs.append(t.score_logits(model, title, True))
        self.assertTrue((outputs[0] == outputs[1]).all())  # same seed -> identical weights and logits
        model.eval()
        with torch.no_grad():
            for cell in (0, 1, 33, 69):
                h, l, tk = t._batch(title, [cell], 1, True)
                self.assertAlmostEqual(float(model(h, l, tk)[0, 0]), outputs[0][cell], places=4)

    def test_byte_embedding_input_is_deterministic(self):
        import torch
        t = self.t
        torch.manual_seed(7)
        a = t.build_model(False).emb_hi.weight.detach().clone()
        torch.manual_seed(7)
        b = t.build_model(False).emb_hi.weight.detach().clone()
        self.assertTrue(bool((a == b).all()))
        self.assertEqual(tuple(a.shape), (256, 8))

    def test_m1_features_follow_the_representation(self):
        t, np = self.t, self.np
        title = self.fake_title("x", 20, 3)
        feats = t.m1_features(title)
        names = t.m1_feature_names()
        self.assertEqual(feats.shape, (20, len(names)))
        for cell in (0, 7, 19):
            ctx = g.context_bytes_of(title.rom, cell)
            mid = ctx[448 - 192:448 + 128 + 192]
            self.assertAlmostEqual(feats[cell, names.index("ctx_zero")], ctx.count(0) / 1024, places=12)
            self.assertAlmostEqual(feats[cell, names.index("mid_zero")], mid.count(0) / 512, places=12)
            self.assertAlmostEqual(feats[cell, names.index("cell_ff")], title.rom[cell * 128:(cell + 1) * 128].count(255) / 128, places=12)

    def test_m1_feature_names_carry_no_forbidden_information(self):
        for name in self.t.m1_feature_names():
            self.assertFalse(any(f in name for f in g.FORBIDDEN_FRAGMENTS), name)

    def test_stdlib_reference_evaluator_reproduces_the_torch_model(self):
        import torch
        t, np = self.t, self.np
        import segarecomp_ml_region_gen2_reference as ref
        torch.manual_seed(3)
        model = t.build_model(True)
        for mod in model.modules():  # non-trivial eval-mode batch-norm statistics exercise the folding arithmetic
            if isinstance(mod, torch.nn.BatchNorm1d):
                mod.running_mean.normal_(0, 0.3)
                mod.running_var.uniform_(0.5, 1.5)
                mod.weight.data.uniform_(0.5, 1.5)
                mod.bias.data.normal_(0, 0.2)
        model.eval()
        rng = np.random.RandomState(1)
        hi, lo = rng.randint(0, 256, 512), rng.randint(0, 256, 512)
        tok = np.stack([rng.randint(0, g.TOKEN_CARDINALITY[f], 512) for f in g.TOKEN_FIELDS], axis=1)
        with torch.no_grad():
            want = float(model(torch.tensor(hi)[None].long(), torch.tensor(lo)[None].long(), torch.tensor(tok)[None].long())[0, 0])
        got = ref.cnn_logit(t.export_weights(model), hi.tolist(), lo.tolist(), tok.tolist())
        self.assertAlmostEqual(got, want, places=5)

    def test_training_set_must_not_contain_the_held_out_title(self):
        with self.assertRaises(SystemExit):
            g.assert_split(["s1", "flicky"], "s1")


if __name__ == "__main__":
    unittest.main()
