#!/usr/bin/env python3
"""SEG-044-T005: the diagnostic H = C ∩ U plan builder (synthetic inputs only)."""
import hashlib
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import segarecomp_source_universe_plan as plan  # noqa: E402

ROM = "a" * 64


def universe(entries, rom=ROM):
    return "\n".join(["segarecomp.m68k_source_universe.v1", f"rom_sha256 {rom}", "producer p", "source_revision " + "1" * 40,
                      "source_config none", f"entries {len(entries)}"] + [f"{e:08x}" for e in entries] + ["end", ""])


class PlanTest(unittest.TestCase):
    def test_intersection_ranges_and_digest(self):
        broad = [0x200, 0x202, 0x204, 0x206, 0x300]
        text, summary = plan.build_plan(universe([0x200, 0x204, 0x206, 0x300, 0x400]), " ".join(f"{a:x}" for a in broad), ROM)
        digest = hashlib.sha256("".join(f"{a:08x}\n" for a in broad).encode()).hexdigest()
        self.assertEqual(text.splitlines()[:5], ["segarecomp.m68k_hybrid_admission_plan.v1", f"rom_sha256 {ROM}", f"universe_sha256 {digest}",
                                                  "strategy hybrid", "range 00000200 00000202"])
        # runs are over consecutive BROAD identities, so 0x204, 0x206 and 0x300 form one run (the planner's own range semantics)
        self.assertIn("range 00000204 00000302", text)
        self.assertEqual(summary, {"broad_u": 5, "source_c": 5, "admitted_h": 4, "source_outside_broad": 1, "ranges": 2})

    def test_rejects_malformed_or_mismatched_universe(self):
        for bad in (universe([0x200], "b" * 64), universe([0x204, 0x200]), universe([0x201]), universe([0x200]).replace("entries 1", "entries 2"),
                    universe([0x200]).replace("end\n", "")):
            with self.assertRaises(SystemExit):
                plan.build_plan(bad, "200 202", ROM)

    def test_empty_admission_rejected(self):
        with self.assertRaises(SystemExit):
            plan.build_plan(universe([0x900]), "200 202", ROM)


if __name__ == "__main__":
    unittest.main()
