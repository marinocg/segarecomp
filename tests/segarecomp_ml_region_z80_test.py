#!/usr/bin/env python3
"""SEG-051 (ADR 0114): hermetic tests for the Z80 executable-region experiment tooling and the Z80 decode-token report.

Synthetic data only: no ROM, no truth, no commercial bytes. Stdlib tests always run; the numpy/PyTorch smoke test runs only
where those private research packages are importable. The token-report goldens need the CLI binary (argv[1]); without it
they are skipped (the CMake registration always passes it).
"""
import hashlib
import importlib.util
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools")
sys.path.insert(0, TOOLS)
import segarecomp_ml_region_z80 as z  # noqa: E402

CLI = sys.argv[1] if len(sys.argv) > 1 and os.path.exists(sys.argv[1]) else None
HAVE_TORCH = all(importlib.util.find_spec(m) is not None for m in ("numpy", "torch"))


def report(image: bytes):
    with tempfile.TemporaryDirectory() as d:
        open(d + "/i", "wb").write(image)
        subprocess.check_call([CLI, "z80-decode-token-report", "--rom", d + "/i", "--output", d + "/o"])
        blob = open(d + "/o", "rb").read()
    kind, length, form = z.parse_token_report(blob)
    return list(zip(kind, length, form))


@unittest.skipUnless(CLI, "segarecomp CLI binary not supplied")
class TokenReport(unittest.TestCase):
    def test_prefix_families_and_lengths(self):
        cases = {
            "base nop": ([0x00], 1),
            "ddcb displacement op": ([0xDD, 0xCB, 0x05, 0x06], 4),
            "fd immediate16": ([0xFD, 0x21, 0x34, 0x12], 4),
            "ed neg": ([0xED, 0x44], 2),
            "ed undefined is still two bytes": ([0xED, 0x00, 0, 0], 2),
            "cb undocumented sll": ([0xCB, 0x37], 2),
            "repeated dd counts the superseded prefix": ([0xDD, 0xDD, 0x21, 0x00, 0x00], 5),
            "ignored dd before nop": ([0xDD, 0x00, 0, 0], 2),
        }
        for name, (data, length) in cases.items():
            kind, got, form = report(bytes(data))[0]
            self.assertEqual((kind, got), (0, length), name)
            self.assertNotEqual(form, 0xFFFF, name)

    def test_inner_positions_decode_independently(self):
        # offset 1 of a DDCB sequence is CB 05 06 -> a two-byte CB instruction: boundary behaviour is the decoder's
        tokens = report(bytes([0xDD, 0xCB, 0x05, 0x06]))
        self.assertEqual([t[1] for t in tokens], [4, 2, 1, 0])

    def test_truncated_and_prefix_run_fail_closed(self):
        self.assertEqual(report(bytes([0xC3]))[0], (3, 0, 0xFFFF))
        for t in report(bytes([0xDD, 0xDD, 0xDD])):
            self.assertEqual(t, (3, 0, 0xFFFF))

    def test_instruction_continues_across_a_16k_block_edge(self):
        image = bytearray(0x4000 + 2)
        image[0x3FFF] = 0xC3
        tokens = report(bytes(image))
        self.assertEqual(tokens[0x3FFF][:2], (0, 3))

    def test_deterministic_and_size(self):
        data = bytes((i * 31 + 7) & 0xFF for i in range(40000))
        with tempfile.TemporaryDirectory() as d:
            open(d + "/i", "wb").write(data)
            outs = []
            for k in range(2):
                subprocess.check_call([CLI, "z80-decode-token-report", "--rom", d + "/i", "--output", d + f"/o{k}"])
                outs.append(open(d + f"/o{k}", "rb").read())
        self.assertEqual(outs[0], outs[1])
        self.assertEqual(len(outs[0]), 12 + 4 * len(data))

    def test_token_consistency_with_lengths(self):
        data = bytes((i * 131 + 17) & 0xFF for i in range(2048))
        for i, (kind, length, form) in enumerate(report(data)):
            if kind == 0:
                self.assertTrue(1 <= length <= 255)
                self.assertTrue(form < z.FORM_CARDINALITY)
            else:
                self.assertEqual((length, form), (0, 0xFFFF))


class Cells(unittest.TestCase):
    def test_labels(self):
        cls = "I" + "c" * 31 + "D" * 32 + "A" * 32 + "D" * 31 + "U" + "Q" * 8
        self.assertEqual(z.cell_labels(cls), [z.POSITIVE, z.NEGATIVE, z.MASKED, z.MASKED, z.MASKED])
        self.assertEqual(z.cell_count(33), 2)

    def test_form_token_bounds(self):
        self.assertEqual(z.form_token(0xFFFF), 0)
        self.assertEqual(z.form_token(0), 1)

    def test_parse_rejects_bad_reports(self):
        with self.assertRaises(ValueError):
            z.parse_token_report(b"XXXXXXXX" + struct.pack("<I", 0))
        with self.assertRaises(ValueError):
            z.parse_token_report(z.TOKEN_MAGIC + struct.pack("<I", 2) + bytes(4))


class RankingAndPolicies(unittest.TestCase):
    def test_ties_break_to_lower_cell_and_are_deterministic(self):
        self.assertEqual(z.ranking([1.0, 1.0, 2.0, 1.0]), [2, 0, 1, 3])

    def test_non_finite_scores_fail_closed(self):
        for bad in (float("nan"), float("inf"), float("-inf")):
            with self.assertRaises(ValueError):
                z.ranking([0.0, bad])

    def test_selection_size_and_budget_range(self):
        scores = list(range(10))
        self.assertEqual(z.selected_cells(scores, 0.3), {9, 8, 7})
        self.assertEqual(len(z.selected_cells(scores, 1.0)), 10)
        for b in (0.0, -0.1, 1.01):
            with self.assertRaises(ValueError):
                z.selected_cells(scores, b)

    def test_frf_and_recall(self):
        scores = [0.9, 0.1, 0.8, 0.2, 0.7]
        labels = [1, 0, 0, 1, -1]
        self.assertAlmostEqual(z.frf(scores, labels), 4 / 5)  # cell 3 ranks fourth
        self.assertEqual(z.recall_at(scores, labels, 0.6), (1, 2))
        self.assertEqual(z.recall_at(scores, labels, 0.8), (2, 2))
        self.assertEqual(z.frf([0.5, 0.4], [0, 0]), 0.0)  # no positive: nothing is required

    def test_fixed_budget_is_max_plus_margin_rounded_up(self):
        self.assertAlmostEqual(z.fixed_budget([0.31, 0.22]), 0.40)
        self.assertEqual(z.fixed_budget([0.97]), 1.0)

    def test_adaptive_budget_contract(self):
        probs = [0.9] * 10 + [0.1] * 90
        self.assertAlmostEqual(z.confident_fraction(probs), 0.10)
        self.assertAlmostEqual(z.adaptive_budget(probs, 2.0), 0.25)
        self.assertEqual(z.adaptive_budget([0.0] * 10, 1.0), z.ADAPT_FLOOR)  # floor
        self.assertEqual(z.adaptive_budget([1.0] * 10, 4.0), 1.0)  # cap
        self.assertEqual(z.calibrate_adaptive([(probs, 0.2)]), 1.0)  # the 0.20 floor already covers it
        self.assertEqual(z.calibrate_adaptive([(probs, 0.3)]), 2.5)
        self.assertIsNone(z.calibrate_adaptive([([0.0] * 10, 0.99)]))  # not calibratable -> fail closed


class SplitAndLeakage(unittest.TestCase):
    IDS = ["a", "b", "c", "d", "e", "f", "g", "h", "i"]

    def test_split_is_deterministic_and_disjoint(self):
        dev, sealed = z.split_lineages(self.IDS)
        self.assertEqual((dev, sealed), z.split_lineages(list(reversed(self.IDS))))
        self.assertEqual(len(sealed), 3)
        self.assertFalse(set(dev) & set(sealed))

    def test_guards(self):
        lineage = {i: i for i in self.IDS}
        lineage["a2"] = "a"  # a revision of lineage a
        self.assertTrue(z.assert_split(["b", "c"], ["d"], ["e"], lineage))
        with self.assertRaises(ValueError):
            z.assert_split(["a", "b"], ["a2"], [], lineage)  # revision of a trained lineage held out
        with self.assertRaises(ValueError):
            z.assert_split(["a", "b"], ["c"], ["a2"], lineage)  # sealed revision of a training lineage


class FrozenPlan(unittest.TestCase):
    PLAN = os.path.join(TOOLS, "segarecomp_ml_region_z80.plan.json")
    DIGEST = os.path.join(TOOLS, "segarecomp_ml_region_z80.plan.digest")

    def setUp(self):
        self.plan = json.load(open(self.PLAN))

    def test_digest_pins_the_plan(self):
        self.assertEqual(z.canonical_digest(self.plan), open(self.DIGEST).read().strip())

    def test_sealed_assignment_follows_the_rule(self):
        ids = [l["id"] for l in self.plan["lineages"]]
        dev, sealed = z.split_lineages(ids)
        self.assertEqual(sorted(l["id"] for l in self.plan["lineages"] if l["role"] == "sealed"), sealed)
        self.assertEqual(sorted(l["id"] for l in self.plan["lineages"] if l["role"] == "development"), dev)

    def test_pilot_is_not_an_adoption_candidate(self):
        self.assertLess(len(self.plan["lineages"]) - 3, self.plan["adoption_minimums"]["development_lineages"])
        self.assertIn("EXPLORATORY_PILOT", self.plan["stage"])

    def test_public_plan_contains_only_aggregates(self):
        text = open(self.PLAN).read()
        for forbidden in ('"classes"', "/root/", "games/", "0x"):
            self.assertNotIn(forbidden, text)
        for l in self.plan["lineages"]:
            self.assertEqual(len(l["image_sha256"]), 64)
            self.assertEqual(set(l) & {"offsets", "classes", "starts"}, set())


@unittest.skipUnless(HAVE_TORCH, "numpy/torch research packages absent (not product dependencies)")
class TrainerSmoke(unittest.TestCase):
    def test_fit_and_score_are_deterministic_on_synthetic_data(self):
        import numpy as np
        import torch
        import segarecomp_ml_region_z80_train as t
        torch.set_num_threads(1)
        torch.use_deterministic_algorithms(True)
        rng = np.random.default_rng(0)
        n = 64 * z.CELL_BYTES
        item = {
            "byte": rng.integers(0, 256, n).astype(np.int64), "kind": np.zeros(n, dtype=np.int64),
            "len": np.ones(n, dtype=np.int64), "form": np.ones(n, dtype=np.int64),
            "labels": np.array([1 if i % 3 == 0 else 0 for i in range(64)], dtype=np.int64), "n": n, "name": "x",
        }
        t.EPOCHS = 1
        outs = []
        for _ in range(2):
            m = [t.fit(np, torch, [item], True, 1)]
            outs.append(t.score(np, torch, m, item))
        self.assertEqual(outs[0].shape, (64,))
        self.assertTrue(np.array_equal(outs[0], outs[1]))
        self.assertTrue(np.isfinite(outs[0]).all())


if __name__ == "__main__":
    sys.argv = [sys.argv[0]] + sys.argv[2:]
    unittest.main()
