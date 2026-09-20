#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "genesis_frontier_debug", ROOT / "tools" / "genesis_frontier_debug.py")
assert SPEC and SPEC.loader
debug = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(debug)


class GenesisFrontierDebugTest(unittest.TestCase):
    def test_lldb_command_stops_at_access_failure_and_inspects_vdp_state(self) -> None:
        command = debug.command("/usr/bin/lldb", pathlib.Path("/ignored/bridge"))
        joined = " ".join(command)
        self.assertIn("genesis_access_stop", joined)
        self.assertIn("addressed_pointer", joined)
        self.assertIn("registers", joined)
        self.assertIn("dma", joined)

    def test_gdb_command_has_same_state_targets(self) -> None:
        command = debug.command("/usr/bin/gdb", pathlib.Path("/ignored/bridge"))
        joined = " ".join(command)
        self.assertIn("genesis_access_stop", joined)
        self.assertIn("routed_value", joined)
        self.assertIn("auto_increment_value", joined)


if __name__ == "__main__":
    unittest.main()
