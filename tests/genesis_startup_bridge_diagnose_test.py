#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "genesis_startup_bridge", ROOT / "tools" / "genesis_startup_bridge.py")
assert SPEC and SPEC.loader
bridge = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bridge)


class GenesisStartupBridgeDiagnoseTest(unittest.TestCase):
    def test_ephemeral_frontier_is_bounded(self) -> None:
        full = {
            "runtime": {"pc": "0x00001234", "work_ram_base64": "do-not-copy"},
            "provenance": {
                "has_instruction_provenance": True,
                "instruction": {
                    "cpu_variant": "mc68000",
                    "source_address": "0x00001230",
                    "image_offset": 1,
                    "primary_bytes": "abcd",
                    "length": 2,
                },
                "has_access": True,
                "access_address": "0x00c00000",
                "access_width": "word",
                "access_direction": "write",
            },
        }
        result = bridge.ephemeral_frontier(full, pathlib.Path("/ignored/bridge"))
        self.assertEqual(
            list(result),
            ["report_kind", "runtime_pc", "instruction", "access", "debug_binary"],
        )
        self.assertEqual(result["access"]["width"], "word")
        self.assertNotIn("runtime", result)
        self.assertNotIn("work_ram_base64", str(result))

    def test_ephemeral_frontier_surfaces_recent_pc_history_for_runner_resource_limit(self) -> None:
        # SEG-007-T252 / ADR-0040 correction regression: `provenance` is always
        # null for a runner_resource_limit full report (see
        # genesis_write_full_report), so the history must NOT be discarded by
        # the `isinstance(provenance, dict)` early-return path that guards the
        # unrelated instruction/access fields above. `full` itself never
        # carries `recent_pc_history` any more (it travels only through the
        # separate ephemeral-report-fd transport); it is passed explicitly as
        # this function's own third argument instead.
        full = {
            "result": "runner_resource_limit",
            "runtime": {"pc": "0x00001234", "work_ram_base64": "do-not-copy"},
            "provenance": None,
        }
        result = bridge.ephemeral_frontier(full, pathlib.Path("/ignored/bridge"),
                                           ["0x00000100", "0x00000104"])
        self.assertEqual(result["recent_pc_history"], ["0x00000100", "0x00000104"])
        self.assertNotIn("runtime", result)
        self.assertNotIn("work_ram_base64", str(result))

    def test_ephemeral_frontier_tolerates_missing_recent_pc_history(self) -> None:
        full = {"result": "runner_resource_limit", "provenance": None}
        result = bridge.ephemeral_frontier(full, pathlib.Path("/ignored/bridge"))
        self.assertNotIn("recent_pc_history", result)
        self.assertEqual(list(result), ["report_kind", "debug_binary"])

    def test_ephemeral_frontier_tolerates_empty_recent_pc_history_list(self) -> None:
        full = {"result": "runner_resource_limit", "provenance": None}
        result = bridge.ephemeral_frontier(full, pathlib.Path("/ignored/bridge"), [])
        self.assertNotIn("recent_pc_history", result)


if __name__ == "__main__":
    unittest.main()
