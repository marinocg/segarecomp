#!/usr/bin/env python3
"""SEG-020-T007: unified failure-diagnosis assembly on project-authored synthetic evidence.

Proves the combined ephemeral report (stop + frontier + first divergence), the durable-safe class
projection (no address/value/pc/image/byte material), determinism, graceful handling of an
unavailable divergence input, and CLI gating. No commercial input is used."""
from __future__ import annotations

import importlib.util
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def load(name: str, rel: str):
    spec = importlib.util.spec_from_file_location(name, ROOT / rel)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


bridge = load("genesis_startup_bridge", "tools/genesis_startup_bridge.py")
dd = load("genesis_device_divergence", "tools/genesis_device_divergence.py")

SANITIZED = {"result": "stop", "stop_class": "unsupported_instruction", "diagnostic_category": "decode"}
FRONTIER = {"report_kind": "ephemeral_frontier", "runtime_pc": "0x00001234", "debug_binary": "/x/bridge"}
SECRET_PC, SECRET_ADDR, SECRET_VALUE = 0xDEADBEE, 0x00FF1234, 0xC0FFEE1


def cpu(boundary, pc, **over):
    record = {"boundary": boundary, "pc": pc, "sr": 0x2700, "usp": 0, "d": [0] * 8, "a": [0] * 7 + [0xFF8000],
              "unsupported": 0, "effects": []}
    record.update(over)
    return record


def report_for_cpu_divergence() -> dict:
    generated = [cpu(1, 0x202), cpu(2, SECRET_PC, effects=[{"k": 1, "a": SECRET_ADDR, "w": 2, "v": SECRET_VALUE}])]
    expected = [cpu(1, 0x202), cpu(2, 0x204, effects=[{"k": 1, "a": SECRET_ADDR, "w": 2, "v": 1}])]
    devices = [{"boundary": n, "unsupported": 0, "components": {"vdp_vram": "%016x" % 1}, "events": []}
               for n in (1, 2)]
    return dd.compare(generated, expected, devices, devices, 8, 0x200, "secret-image-identity")


class FailureDiagnosisWorkflowTest(unittest.TestCase):
    def write(self, directory: str, report) -> str:
        path = pathlib.Path(directory) / "divergence.json"
        path.write_text(json.dumps(report) if report is not None else "{not json")
        return str(path)

    def capture(self, path: str | None) -> list[str]:
        import contextlib
        import io
        buffer = io.StringIO()
        with contextlib.redirect_stderr(buffer):
            bridge.write_combined_diagnosis(path, SANITIZED, dict(FRONTIER))
        return buffer.getvalue().splitlines()

    def test_combined_report_carries_stop_frontier_and_divergence(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            lines = self.capture(self.write(tmp, report_for_cpu_divergence()))
        self.assertEqual([l.split(" ", 1)[0] for l in lines], ["EPHEMERAL_DIAGNOSIS", "DIAGNOSIS_CLASSES"])
        combined = json.loads(lines[0].split(" ", 1)[1])
        self.assertEqual(combined["stop"]["stop_class"], "unsupported_instruction")
        self.assertEqual(combined["frontier"]["runtime_pc"], "0x00001234")
        self.assertEqual(combined["divergence"]["domain"], "cpu")
        self.assertEqual(combined["divergence"]["first_differing_boundary"], 2)

    def test_durable_projection_is_non_reconstructable(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            lines = self.capture(self.write(tmp, report_for_cpu_divergence()))
        durable_text = lines[1].split(" ", 1)[1]
        durable = json.loads(durable_text)
        self.assertEqual(durable["divergence"]["domain"], "cpu")
        self.assertEqual(durable["divergence"]["differing_fields"], ["effect:write/w2", "pc"])
        for secret in ("%08X" % SECRET_ADDR, "%X" % SECRET_PC, "%X" % SECRET_VALUE, "secret-image", "0x00001234",
                       "/x/bridge", str(SECRET_PC), str(SECRET_ADDR), str(SECRET_VALUE)):
            self.assertNotIn(secret.lower(), durable_text.lower())

    def test_device_command_field_reduction(self) -> None:
        for raw, expected in (("event:write@vdp/00C00004/w2", "event:write@vdp/w2"),
                              ("effect:write@00FF1234/w4#2", "effect:write/w4"),
                              ("state:vdp_vram", "state:vdp_vram"), ("d3", "d3"),
                              ("weird:00FF1234", "other"), (None, "other")):
            self.assertEqual(bridge.durable_field_class(raw), expected)

    def test_deterministic_and_unavailable_input(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = self.write(tmp, report_for_cpu_divergence())
            self.assertEqual(self.capture(path), self.capture(path))
            bad = self.write(tmp, None)
            lines = self.capture(bad)
        self.assertIsNone(json.loads(lines[0].split(" ", 1)[1])["divergence"])
        self.assertIsNone(json.loads(lines[1].split(" ", 1)[1])["divergence"])
        self.assertEqual(self.capture(None), [])

    def test_pathological_and_oversized_inputs_degrade(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "deep.json"
            path.write_text("[" * 100000)
            self.assertIsNone(bridge.load_divergence_report(path))
        self.assertEqual(bridge.durable_field_class("state:" + "a" * 500), "other")
        self.assertEqual(bridge.durable_field_class("pc\n"), "other")

    def test_unknown_fields_collapse_to_other_and_never_echo(self) -> None:
        raw = ("state:secret_image_identity", "state:not_a_real_component",
               "event:write@secret_region/00C00004/w2", "event:write@vdp/00C00004/w3",
               "effect:write@00FF1234/w9", "effect:write@00FF1234/w4#x", "event:write@vdp/zz/w2",
               "d8", "a-1", "pcx", "sr\n", "event:vblank_raise2", "effect:trap ", "state:", "event:write@vdp/w2")
        for name in raw:
            self.assertEqual(bridge.durable_field_class(name), "other", name)
        report = {"schema": 1, "result": "diverged", "domain": "device", "classification": "device_state",
                  "first_differing_boundary": 3, "fields": [{"field": n} for n in raw]}
        with tempfile.TemporaryDirectory() as tmp:
            lines = self.capture(self.write(tmp, report))
        durable_text = lines[1]
        self.assertEqual(json.loads(durable_text.split(" ", 1)[1])["divergence"]["differing_fields"], ["other"])
        for name in raw:
            if len(name) > 3:
                self.assertNotIn(name, durable_text)
        for secret in ("secret_image_identity", "not_a_real_component", "secret_region"):
            self.assertNotIn(secret, durable_text)

    def test_known_vocabulary_is_preserved(self) -> None:
        for name in ("state:vdp_registers", "state:vdp_dma", "state:vdp_vram", "state:vdp_cram", "state:vdp_vsram",
                     "state:interrupt", "state:psg", "state:z80_bus", "state:z80_ram", "state:controller_io",
                     "effect:trap", "event:order", "d0", "a7", "usp", "sr", "pc", "boundary_ordinal"):
            self.assertEqual(bridge.durable_field_class(name), name)
        self.assertEqual(bridge.durable_field_class("event:write@z80_ram_window/00A00000/w1"),
                         "event:write@z80_ram_window/w1")

    def test_cli_requires_diagnose_frontier(self) -> None:
        completed = subprocess.run(
            [sys.executable, str(ROOT / "tools/genesis_startup_bridge.py"), "--rom", "x.md", "--mode", "synthetic",
             "--divergence-report", "d.json"], capture_output=True, text=True)
        self.assertEqual(completed.returncode, 8)


if __name__ == "__main__":
    unittest.main()
