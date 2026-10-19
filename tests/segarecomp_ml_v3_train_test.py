#!/usr/bin/env python3
"""SEG-072 (ADR 0151/0153): hermetic tests for the v3 training/evaluation helpers. Synthetic data only. The numpy/PyTorch checks run only
where those private research packages are importable (they are not product dependencies)."""
import importlib.util
import os
import sys
import unittest

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools")
sys.path.insert(0, TOOLS)
import segarecomp_ml_v3_eval as ev  # noqa: E402

HAVE_TORCH = all(importlib.util.find_spec(m) is not None for m in ("numpy", "torch"))


class Eval(unittest.TestCase):
    def test_regions_text_merges_and_guards(self):
        text = ev.regions_text("0" * 64, 300, {0, 1}, 128)
        self.assertIn("range 00000000 00000100", text)
        self.assertIn(ev.regions_text("0" * 64, 300, {2}, 128).splitlines()[2], "range 00000100 0000012c")
        with self.assertRaises(SystemExit):
            ev.regions_text("0" * 64, 300, {3}, 128)

    def test_public_strips_private_regions(self):
        self.assertEqual(ev.public({"a": 1, "_region": [1, 2], "b": [{"_region": [3], "c": 2}]}), {"a": 1, "b": [{"c": 2}]})

    def test_lineage_aggregate(self):
        recs = [{"lineage": "D01", "frf": 0.2, "residual_frf": 0.1, "starts": 5, "policies": {"B35": {"retained": 0.4, "c_outside_r": 0, "abstained": False}}},
                {"lineage": "D01", "frf": 0.3, "residual_frf": 0.2, "starts": 1, "policies": {"B35": {"retained": 0.6, "c_outside_r": 2, "abstained": False}}},
                {"lineage": "D02", "frf": 0.1, "residual_frf": 0.0, "starts": 3, "policies": {"B35": {"retained": 0.35, "c_outside_r": 0, "abstained": False}}}]
        agg = ev.lineage_aggregate(recs)
        self.assertAlmostEqual(agg["D01"]["policies"]["B35"]["mean_retained"], 0.5)
        self.assertEqual(agg["D01"]["policies"]["B35"]["misses"], 2)
        self.assertEqual(agg["D01"]["max_frf"], 0.3)


@unittest.skipUnless(HAVE_TORCH and importlib.util.find_spec("sklearn") is not None, "private research environment only")
class FreezeHandoff(unittest.TestCase):
    def test_frozen_record_has_every_key_the_sealed_gate_needs(self):
        import json
        import pathlib
        import tempfile
        import segarecomp_ml_v3 as v3
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "results").mkdir()
            (root / "results" / "selection.json").write_text(json.dumps({"selected": "M3_FROZEN+B50", "rule": "selection_order", "incumbent_retained": False,
                                                                          "table": {}}))
            (root / "results" / "table.M3_FROZEN.json").write_text(json.dumps({"records": [{"residual_frf": 0.2, "density_e": 0.1}]}))
            (root / "results" / "hardware.json").write_text(json.dumps({"selected": "CPU", "improvement": 0.09, "rule": ">= 25%"}))
            old = (ev.RESULTS, ev.ROOT, v3.FROZEN_PATH)
            ev.RESULTS, ev.ROOT, v3.FROZEN_PATH = root / "results", root, root / "frozen.json"
            try:
                ev.cmd_freeze(None)
                rec = json.loads((root / "frozen.json").read_text())
            finally:
                ev.RESULTS, ev.ROOT, v3.FROZEN_PATH = old
        for key in ("configuration", "policy", "environment", "plan_sha256", "status"):
            self.assertIn(key, rec)
        self.assertEqual((rec["status"], rec["configuration"], rec["policy"]), ("FROZEN", "M3_FROZEN", "B50"))


@unittest.skipUnless(HAVE_TORCH, "numpy/torch not installed (private research environment)")
class Dense(unittest.TestCase):
    def test_cell_max(self):
        import numpy as np
        import segarecomp_ml_v3_train as tr
        w = np.array([0, 5, 1, 2, -1, -2, -3, -4, 9], dtype=np.float32)
        self.assertEqual(tr.cell_max(w, 18, 8).tolist(), [5.0, -1.0, 9.0])

    def test_receptive_field_within_margin(self):
        import torch
        import segarecomp_ml_v3_train as tr
        torch.manual_seed(0)
        model = tr.build_m4(torch).eval()
        m = tr.M4["margin"]
        length = 2 * m + 64
        hi = torch.randint(0, 256, (1, length))
        lo = torch.randint(0, 256, (1, length))
        tok = torch.zeros(1, length, 8, dtype=torch.long)
        base = model(hi, lo, tok)[0, 0].item()  # output word 0 sits at input position m
        hi2 = hi.clone()
        hi2[0, 0:m - 506] = (hi2[0, 0:m - 506] + 1) % 256  # beyond the 504-word radius (+ stride alignment) on the left
        self.assertAlmostEqual(model(hi2, lo, tok)[0, 0].item(), base, places=5)
        hi3 = hi.clone()
        hi3[0, m - 8] = (hi3[0, m - 8] + 1) % 256  # inside the radius: the logit must react
        self.assertNotAlmostEqual(model(hi3, lo, tok)[0, 0].item(), base, places=6)


if __name__ == "__main__":
    unittest.main()
