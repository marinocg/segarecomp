#!/usr/bin/env python3
"""SEG-050-T001 (ADR 0110): hermetic tests for the materialized-image screening tool. Synthetic data only (no ROM, no CLI)."""
import os
import sys
import unittest

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools")
sys.path.insert(0, TOOLS)
import seg050_materialized_screen as screen  # noqa: E402

LOG = """rom_sha256=aa
segarecomp: immutable-rom AOT enumeration: aligned_start_count=65536 accepted_count=1000 rejected_count=5
m68k alias preparation round: aliases=1
segarecomp: immutable-rom AOT enumeration: aligned_start_count=65536 accepted_count=1250 rejected_count=4
m68k alias preparation round: aliases=1 ram_thunks=1
segarecomp: m68k admission: requested=optimized producer=ml_region_gen2 fallback=0 reason=none universe=1000 k0=400 k=390 pruned=10 validator=accepted
segarecomp: m68k admission: requested=optimized producer=broad fallback=1 reason=prune_rejected detail=pruned validator=not_run
m68k alias preparation: rounds=2 aliases=1 ram_thunks=1 alias_bytes=64 termination=route_advanced
"""


class Screen(unittest.TestCase):
    def test_log_aggregates_are_counts_only(self):
        d = screen.parse_log(LOG)
        self.assertEqual(d["universe_first_emission"], 1000)
        self.assertEqual(d["universe_final_emission"], 1250)
        self.assertEqual(d["materialized_starts"], 250)
        self.assertEqual(d["materialized_share_ppm"], 200000)
        self.assertEqual(d["preparation"], {"rounds": 2, "aliases": 1, "ram_thunks": 1, "alias_bytes": 64, "termination": "route_advanced"})
        self.assertEqual(d["admission_fallback_reasons"], ["prune_rejected"])
        self.assertEqual(d["admission_first"]["k"], "390")

    def test_no_alias_log(self):
        d = screen.parse_log("segarecomp: immutable-rom AOT enumeration: aligned_start_count=8 accepted_count=7 rejected_count=1\n")
        self.assertNotIn("materialized_share_ppm", d)
        self.assertEqual(d["emissions"], 1)


if __name__ == "__main__":
    unittest.main()
