#!/usr/bin/env python3
"""SEG-007-T180 / ADR-0026: the offline-inventory stitch metrics line is parsed
from the emitter's stderr string fully in-process. No artifact is written into
the compare-runs out-dir surface, so the expected-artifacts allowlist is
unaffected, and the normalized counts still reach ONE_SHOT_SUMMARY /
EPHEMERAL_FRONTIER when the inventory is non-empty.
"""
from __future__ import annotations

import importlib.util
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "genesis_startup_bridge", ROOT / "tools" / "genesis_startup_bridge.py")
assert SPEC and SPEC.loader
bridge = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bridge)


class OfflineInventoryStitchMetricsTest(unittest.TestCase):
    def test_parse_extracts_counts_only(self) -> None:
        stderr = (
            "some unrelated warning\n"
            "segarecomp: offline inventory stitch: candidates=7 admitted_units=4 "
            "rejected_entry_decode=1 stitched_direct_edges=9 overlap_agree=3 "
            "overlap_conflict=0 local_discovery=12 aggregate_discovery=20\n"
        )
        metrics = bridge.parse_offline_inventory_stitch_metrics(stderr)
        self.assertEqual(metrics["candidates"], 7)
        self.assertEqual(metrics["admitted_units"], 4)
        self.assertEqual(metrics["overlap_conflict"], 0)
        self.assertEqual(metrics["aggregate_discovery"], 20)
        # No token here is a hex address; every value round-trips as an int.
        self.assertTrue(all(isinstance(v, int) for v in metrics.values()))

    def test_parse_returns_empty_without_marker(self) -> None:
        self.assertEqual(bridge.parse_offline_inventory_stitch_metrics("nothing here\n"), {})
        self.assertEqual(bridge.parse_offline_inventory_stitch_metrics(""), {})

    def test_parse_adr0038_retained_block_delta_growth_zero_and_shrinkage(self) -> None:
        marker = "segarecomp: offline inventory partition: "
        for spelling, expected in (("7", 7), ("0", 0), ("-3", -3)):
            with self.subTest(spelling=spelling):
                self.assertEqual(
                    bridge.parse_offline_inventory_partition_metrics(
                        f"{marker}adr0038_retained_block_delta={spelling} candidates=1\n"),
                    {"adr0038_retained_block_delta": expected, "candidates": 1})

        # Production merges stitch, partition, and emission metrics into one
        # canonical inventory dictionary in that order. A shrinking ADR-0038
        # result must survive that exact merge rather than disappearing because
        # the signed field was looked for on the wrong marker.
        merged = bridge.parse_offline_inventory_stitch_metrics(
            "segarecomp: offline inventory stitch: candidates=1\n")
        merged.update(bridge.parse_offline_inventory_partition_metrics(
            f"{marker}adr0038_retained_block_delta=-3 retained_block_count_after_pruning=2\n"))
        merged.update(bridge.parse_offline_inventory_emission_metrics(
            "segarecomp: offline inventory emission: emitted_block_count=2\n"))
        self.assertEqual(merged["adr0038_retained_block_delta"], -3)

    def test_parse_rejects_malformed_or_signed_unsigned_metrics(self) -> None:
        marker = "segarecomp: offline inventory partition: "
        for spelling in ("-", "+1", "--1", "-1.0", "-0", "00", "-01"):
            with self.subTest(spelling=spelling):
                self.assertEqual(
                    bridge.parse_offline_inventory_partition_metrics(
                        f"{marker}adr0038_retained_block_delta={spelling} candidates=1\n"),
                    {"candidates": 1})
        self.assertEqual(
            bridge.parse_offline_inventory_partition_metrics(
                f"{marker}candidates=-1 adr0038_retained_block_delta=-2\n"),
            {"adr0038_retained_block_delta": -2})
        self.assertEqual(
            bridge.parse_offline_inventory_stitch_metrics(
                "segarecomp: offline inventory stitch: candidates=-1 stitched_direct_edges=2\n"),
            {"stitched_direct_edges": 2})
        self.assertEqual(
            bridge.parse_offline_inventory_emission_metrics(
                "segarecomp: offline inventory emission: emitted_block_count=-1 "
                "emitted_code_address_count=2\n"),
            {"emitted_code_address_count": 2})

    def test_parse_enforces_integer_widths_before_conversion(self) -> None:
        marker = "segarecomp: offline inventory partition: "
        int64_max = (1 << 63) - 1
        int64_min = -(1 << 63)
        uint32_max = (1 << 32) - 1
        self.assertEqual(
            bridge.parse_offline_inventory_partition_metrics(
                f"{marker}adr0038_retained_block_delta={int64_max} candidates={uint32_max}\n"),
            {"adr0038_retained_block_delta": int64_max, "candidates": uint32_max})
        self.assertEqual(
            bridge.parse_offline_inventory_partition_metrics(
                f"{marker}adr0038_retained_block_delta={int64_min} values=[0,{uint32_max}]\n"),
            {"adr0038_retained_block_delta": int64_min, "values": [0, uint32_max]})
        huge = "9" * 100_000
        self.assertEqual(
            bridge.parse_offline_inventory_partition_metrics(
                f"{marker}adr0038_retained_block_delta={int64_max + 1} "
                f"candidates={uint32_max + 1} huge={huge} values=[1,{huge}] valid=2\n"),
            {"valid": 2})
        self.assertEqual(
            bridge.parse_offline_inventory_partition_metrics(
                f"{marker}adr0038_retained_block_delta={int64_min - 1}\n"),
            {})

    def test_cache_lookup_by_resolved_dir_no_file_touch(self) -> None:
        out_dir = pathlib.Path("/tmp/seg007-t180-stitch-cache-probe").resolve()
        key = str(out_dir)
        bridge._OFFLINE_INVENTORY_STITCH_METRICS_BY_DIR[key] = {"candidates": 2, "admitted_units": 1}
        try:
            got = bridge.offline_inventory_stitch_metrics(pathlib.Path("/tmp/seg007-t180-stitch-cache-probe"))
            self.assertEqual(got, {"candidates": 2, "admitted_units": 1})
            self.assertFalse((out_dir / "emitter.stderr.txt").exists())
        finally:
            bridge._OFFLINE_INVENTORY_STITCH_METRICS_BY_DIR.pop(key, None)

    def test_cache_miss_is_empty(self) -> None:
        self.assertEqual(
            bridge.offline_inventory_stitch_metrics(pathlib.Path("/tmp/seg007-t180-absent-dir")), {})


if __name__ == "__main__":
    # CMake passes the segarecomp binary / C compiler / source-root as trailing
    # args (shared add_test pattern); this module-only test ignores them.
    unittest.main(argv=[sys.argv[0]])
