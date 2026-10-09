"""SEG-045: the frozen FLOW8-D region-proposal producer is deterministic, bounded and fail-closed (synthetic data only)."""
import importlib.util
import pathlib
import unittest

spec = importlib.util.spec_from_file_location("region_proposal", pathlib.Path(__file__).resolve().parents[1] / "tools" / "segarecomp_region_proposal.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
SHA = "a" * 64


def report(rows):
    return "segarecomp.m68k_page_structure.v1 bin_bytes 2048\n" + "".join(f"{start:08x} {i} {t}\n" for start, i, t in rows)


class RegionProposalTest(unittest.TestCase):
    def test_constants_are_frozen(self):
        self.assertEqual((module.PAGE_BYTES, module.FLOW_DENSITY_MIN), (8192, 0.010))

    def test_density_threshold_and_seed_pages_merge(self):
        # page 0: 41 terminators / 4096 words >= 0.010 ; page 1 below ; page 2 selected only by a seed ; page 3 empty.
        bins = module.parse_bins(report([(0, 1000, 41), (0x800, 1000, 0), (0x2000, 1000, 5), (0x4000, 1000, 0)]))
        pages = module.select_pages(bins, [0x4100], 0x8000)
        self.assertEqual(pages, [0, 2])
        text = module.build_regions(SHA, 0x8000, pages)
        self.assertEqual(text.count("range "), 2)
        self.assertIn("range 00000000 00002000\n", text)
        self.assertIn("range 00004000 00006000\n", text)
        self.assertEqual(module.build_regions(SHA, 0x8000, [0, 1, 2]).count("range "), 1)

    def test_just_below_threshold_is_not_selected(self):
        bins = module.parse_bins(report([(0, 1000, 40)]))
        self.assertEqual(module.select_pages(bins, [], 0x2000), [])

    def test_last_partial_page_is_clamped_to_rom_size(self):
        text = module.build_regions(SHA, 0x3000, [1])
        self.assertIn("range 00002000 00003000\n", text)

    def test_fail_closed_inputs(self):
        for bad in ("segarecomp.m68k_page_structure.v1 bin_bytes 4096\n", report([(0, 1, 2)]), report([(1, 1, 0)]), report([(0, 1, 0), (0, 1, 0)])):
            with self.assertRaises(SystemExit):
                module.parse_bins(bad)
        with self.assertRaises(SystemExit):
            module.build_regions(SHA, 0x2000, [])
        with self.assertRaises(SystemExit):
            module.build_regions("x", 0x2000, [0])


if __name__ == "__main__":
    unittest.main()
