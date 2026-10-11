#!/usr/bin/env python3
"""SEG-050-T003/T004 (ADR 0112): hermetic tests for the materialized-image E0/E1 tool. Synthetic data only (no ROM, no truth, no CLI)."""
import hashlib
import pathlib
import subprocess
import tempfile
import os
import sys
import unittest

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools")
sys.path.insert(0, TOOLS)
import seg050_materialized_e1 as e1  # noqa: E402


class E1(unittest.TestCase):
    def test_rank_select_is_the_frozen_policy(self):
        self.assertEqual(e1.rank_select([0.0] * 100, 35), list(range(35)))  # ties break by ordinal ascending; N = ceil(100*35/100)
        self.assertEqual(len(e1.rank_select([0.0] * 1024, 35)), 359)
        self.assertEqual(e1.rank_select([1.0, 3.0, 2.0, 0.5], 50), [1, 2])

    def test_frf_and_density_limit(self):
        scores = [0.9, 0.8, 0.1, 0.7, 0.2]
        self.assertEqual(e1.frf(scores, [1, 0, 0, 1, 0])["cells_needed"], 3)
        self.assertIsNone(e1.frf(scores, [0] * 5)["frf"])
        a = e1.arm("x", scores, [1, 1, 1, 1, 0], [0, 128, 256, 384], 35)
        self.assertTrue(a["density_limited"] and not a["full_recall"])
        b = e1.arm("x", scores, [1, 0, 0, 0, 0], [0], 35)
        self.assertTrue(b["full_recall"] and not b["density_limited"])

    def test_spearman(self):
        self.assertAlmostEqual(e1.spearman([1, 2, 3, 4], [10, 20, 30, 40]), 1.0)
        self.assertAlmostEqual(e1.spearman([1, 2, 3, 4], [4, 3, 2, 1]), -1.0)

    def test_truth_must_belong_to_the_rom_and_be_sorted(self):
        with tempfile.TemporaryDirectory() as d:
            p = pathlib.Path(d) / "t.u"
            sha = hashlib.sha256(b"rom").hexdigest()
            p.write_text("segarecomp.m68k_source_universe.v1\nrom_sha256 %s\nproducer x\nsource_revision y\nsource_config z\nentries 2\n00000200\n00000202\nend\n" % sha)
            self.assertEqual(e1.load_truth(p, sha), [0x200, 0x202])
            with self.assertRaises(SystemExit):
                e1.load_truth(p, "0" * 64)
            p.write_text("segarecomp.m68k_source_universe.v1\nrom_sha256 %s\nproducer x\nsource_revision y\nsource_config z\nentries 2\n00000202\n00000200\nend\n" % sha)
            with self.assertRaises(SystemExit):
                e1.load_truth(p, sha)

    def test_budget_is_not_a_parameter(self):
        done = subprocess.run([sys.executable, os.path.join(TOOLS, "seg050_materialized_e1.py"), "--segarecomp", "x", "--rom", "x", "--truth", "x", "--source", "0",
                               "--length", "80", "--work", "x", "--budget-percent", "40"], capture_output=True, text=True)
        self.assertNotEqual(done.returncode, 0)
        self.assertIn("frozen", done.stderr)


if __name__ == "__main__":
    unittest.main()
