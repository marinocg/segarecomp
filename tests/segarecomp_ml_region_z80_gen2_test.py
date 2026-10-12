#!/usr/bin/env python3
"""SEG-056 (ADR 0117): hermetic tests for the second-generation Z80 experiment definitions. Synthetic data only (no ROM, no truth).
The numpy/PyTorch checks run only where those private research packages are importable."""
import importlib.util
import json
import os
import sys
import unittest

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools")
sys.path.insert(0, TOOLS)
import segarecomp_ml_region_z80 as z1  # noqa: E402
import segarecomp_ml_region_z80_gen2 as g  # noqa: E402
import segarecomp_ml_region_z80_sdcc_truth as sd  # noqa: E402

HAVE_TORCH = all(importlib.util.find_spec(m) is not None for m in ("numpy", "torch"))


class Roots(unittest.TestCase):
    def test_architectural_cells(self):
        self.assertEqual(g.root_cells(1 << 15, 32), [0, 1, 3])
        self.assertEqual(g.root_cells(1 << 15, 16), [0, 3, 6])
        self.assertEqual(g.root_cells(0x50, 32), [0, 1])  # the NMI vector lies outside a tiny image

    def test_roots_do_not_depend_on_content(self):
        self.assertEqual(g.root_cells(4096, 32), g.root_cells(4096, 32))


class Labels(unittest.TestCase):
    def test_byte_labels_mask_unknown(self):
        self.assertEqual(g.byte_labels("IcDAUQ"), [1, 1, 0, -1, -1, -1])

    def test_cell_labels_match_seg051(self):
        classes = "I" + "c" * 31 + "D" * 32 + "A" * 32 + "D" * 31 + "U"
        self.assertEqual(g.cell_labels(classes, 32), z1.cell_labels(classes))
        self.assertEqual(g.cell_labels(classes, 16)[:2], [1, -1] if False else [z1.POSITIVE, z1.MASKED])


class Aggregation(unittest.TestCase):
    def test_reductions_are_deterministic(self):
        s = [0.0, 3.0, 1.0, -1.0, 2.0, 2.0, 0.5, 0.5]
        self.assertEqual(g.aggregate(s, 4, "max"), [3.0, 2.0])
        self.assertEqual(g.aggregate(s, 4, "top2"), [2.0, 2.0])
        self.assertEqual(g.aggregate(s, 4, "mean"), [0.75, 1.25])
        with self.assertRaises(ValueError):
            g.aggregate(s, 4, "median")


class Metrics(unittest.TestCase):
    def test_roots_cover_their_positive_cells(self):
        labels = [1, 1, -1, 1, 0, 0, 1, 0]
        scores = [0.1, 0.2, 0.0, 0.0, 0.5, 0.6, 0.9, 0.7]
        plain = g.metrics(scores, labels, [])
        rooted = g.metrics(scores, labels, [0, 1, 3])
        self.assertEqual(plain["frf"], 1.0)           # cell 3 (score 0.0) ranks last among positives
        self.assertEqual(rooted["residual_frf"], 0.125)  # only cell 6 remains: rank 1
        self.assertEqual(rooted["positives_in_roots"], 3)
        self.assertEqual(g.missed_at_budget(scores, labels, [0, 1, 3], 0.125), (0, 4, 0.5))
        self.assertEqual(g.missed_at_budget(scores, labels, [], 0.125)[0], 3)

    def test_ties_break_by_lower_cell(self):
        self.assertEqual(z1.ranking([1.0, 1.0, 2.0]), [2, 0, 1])


class SdccTruth(unittest.TestCase):
    LISTING = (
        "      000000 3E 01            [ 7]   10 \tld\ta, #0x01\n"
        "      000002 C9               [10]   11 \tret\n"
        "      000003 01 02                   12 \t.db #0x01, #0x02\n"
        "      000005 CD 00 00         [17]   13 \tcall\t_x\n"
        "      000008 05 06                   14 \t.db #0x05, #0x06\n"
        "      00000A C9               [10]   15 \tret\n")
    ROM = bytes([0x3E, 1, 0xC9, 1, 2, 0xCD, 0, 0, 5, 6, 0xC9])

    def test_classes(self):
        out, bad = sd.classify_listing(self.LISTING, self.ROM, True)
        self.assertEqual(bad, 0)
        s = "".join(out.get(i, "U") for i in range(len(self.ROM)))
        # a single .db row wedged between two instructions could be inline-assembled code: ambiguous, never a negative
        self.assertEqual(s, "IcIAAIccAAI")

    def test_multi_row_table_is_data(self):
        listing = ("      000000 C9               [10]    1 \tret\n"
                   "      000001 01 02                    2 \t.db #0x01, #0x02\n"
                   "      000003 03 04                    3 \t.db #0x03, #0x04\n"
                   "      000005 C9               [10]    4 \tret\n")
        out, bad = sd.classify_listing(listing, bytes([0xC9, 1, 2, 3, 4, 0xC9]), True)
        self.assertEqual("".join(out.get(i, "U") for i in range(6)), "IDDDDI")

    def test_identity_mismatch_fails_closed(self):
        out, bad = sd.classify_listing(self.LISTING, bytes(11), True)
        self.assertEqual(bad, 6)
        self.assertEqual(out, {})

    def test_handwritten_db_is_ambiguous(self):
        out, _ = sd.classify_listing(self.LISTING, self.ROM, False)
        self.assertEqual(out[3], "A")


class Plan(unittest.TestCase):
    def test_digest_and_split(self):
        plan = json.load(open(os.path.join(TOOLS, "segarecomp_ml_region_z80_gen2.plan.json")))
        self.assertEqual(g.canonical_digest(plan), open(os.path.join(TOOLS, "segarecomp_ml_region_z80_gen2.plan.digest")).read().strip())
        roles = {l["id"]: l["role"] for l in plan["lineages"]}
        self.assertEqual(sorted(i for i, r in roles.items() if r == "sealed"), ["alexkidd", "lots", "sonic1"])
        self.assertEqual(plan["split"]["sealed"], ["alexkidd", "lots", "sonic1"])
        self.assertEqual(sorted(plan["split"]["development"]), sorted(i for i, r in roles.items() if r == "development"))
        hist = json.load(open(os.path.join(TOOLS, "segarecomp_ml_region_z80.plan.json")))
        # the sealed cohort of SEG-051 is carried over unchanged
        self.assertEqual({l["id"] for l in hist["lineages"] if l["role"] == "sealed"}, set(plan["split"]["sealed"]))
        self.assertEqual(plan["historic_plan"]["digest"], open(os.path.join(TOOLS, "segarecomp_ml_region_z80.plan.digest")).read().strip())


@unittest.skipUnless(HAVE_TORCH, "numpy/PyTorch research packages not installed")
class Model(unittest.TestCase):
    def test_receptive_field_fits_margin_and_pad_edges(self):
        import numpy as np
        import torch
        import segarecomp_ml_region_z80_gen2_train as T
        torch.manual_seed(0)
        length = T.CENTRAL + 2 * T.MARGIN
        for name, radius in (("B1", 30), ("D256", 252), ("D512", 508)):
            m = T.build_model(torch, name).eval()
            x = torch.randint(0, 256, (1, length))
            base = m(x)[0]
            c = T.CENTRAL // 2
            for d in (radius + 4, T.MARGIN):  # beyond the radius nothing may influence the centre logit
                for sign in (1, -1):
                    pos = T.MARGIN + c + sign * d
                    if 0 <= pos < length:
                        x2 = x.clone()
                        x2[0, pos] = (x2[0, pos] + 9) % 256
                        self.assertLess(float((m(x2)[0][c] - base[c]).abs()), 1e-6, (name, d, sign))
        item = {"byte": np.arange(100) % 256, "y": np.ones(100, dtype=np.int64)}
        x, y = T.window(np, item, -64, 128)
        self.assertEqual(int((x[:T.MARGIN - 64] == T.PAD).all()), 1)  # left of the image is the PAD token
        self.assertTrue((y[:64] == -1).all())  # no label outside the image


@unittest.skipUnless(HAVE_TORCH, "numpy/PyTorch research packages not installed")
class Device(unittest.TestCase):
    def setUp(self):
        import numpy as np
        import torch
        import segarecomp_ml_region_z80_gen2_train as T
        self.np, self.torch, self.T = np, torch, T

    def _item(self, n=6000):
        rng = self.np.random.default_rng(3)
        return {"name": "s", "byte": rng.integers(0, 256, n).astype(self.np.int64),
                "y": rng.choice([-1, 0, 1], n).astype(self.np.int64), "n": n}

    def test_device_selection(self):
        T, torch = self.T, self.torch
        self.assertEqual(T.pick_device(torch, "cpu").type, "cpu")
        self.assertEqual(T.pick_device(torch, "auto").type, "cuda" if torch.cuda.is_available() else "cpu")
        if not torch.cuda.is_available():
            with self.assertRaises(SystemExit):  # an explicit GPU request never falls back silently
                T.pick_device(torch, "cuda")
        with self.assertRaises(SystemExit):
            T.pick_device(torch, "tpu")

    def _run(self, device):
        np, torch, T = self.np, self.torch, self.T
        old = T.STEPS
        T.STEPS = 3
        try:
            train = [self._item(), self._item(5000)]
            m = T.fit(np, torch, train, "D256", 1, device)
            self.assertTrue(all(p.device.type == device.type for p in m.parameters()))
            out = T.infer(np, torch, m, train[0])
        finally:
            T.STEPS = old
        self.assertEqual((out.dtype, out.shape), (np.float32, (train[0]["n"],)))
        self.assertTrue(np.isfinite(out).all())
        return out

    def test_cpu_training_is_seed_deterministic(self):
        torch = self.torch
        torch.use_deterministic_algorithms(True)
        a = self._run(torch.device("cpu"))
        b = self._run(torch.device("cpu"))
        self.assertTrue((a == b).all())

    def test_gpu_training_and_inference_when_available(self):
        if not self.torch.cuda.is_available():
            self.skipTest("no CUDA/ROCm device")
        self._run(self.torch.device("cuda"))

    def test_resume_rejects_invalid_results_and_publishes_atomically(self):
        import tempfile, pathlib
        np, T = self.np, self.T
        with tempfile.TemporaryDirectory() as d:
            p = pathlib.Path(d) / "logit.npy"
            self.assertFalse(T.valid_result(np, p, 10))                      # missing
            p.write_bytes(b"not an npy")
            self.assertFalse(T.valid_result(np, p, 10))                      # corrupt
            T.publish(np, p, np.zeros(10, dtype=np.float32))
            self.assertTrue(T.valid_result(np, p, 10))
            self.assertFalse(T.valid_result(np, p, 11))                      # wrong shape
            T.publish(np, p, np.zeros(10, dtype=np.float64))
            self.assertFalse(T.valid_result(np, p, 10))                      # wrong dtype
            T.publish(np, p, np.full(10, np.nan, dtype=np.float32))
            self.assertFalse(T.valid_result(np, p, 10))                      # non-finite
            self.assertFalse(list(pathlib.Path(d).glob("*.tmp")))            # no temp residue


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
