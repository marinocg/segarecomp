#!/usr/bin/env python3
"""SEG-049-T003 (ADR 0102): hermetic tests for the NATIVE gen2 M3 + B35 route (CLI seams + generator + identity pins).

Synthetic data only (a private 64-bit LCG): no ROM, no truth, no commercial data, no numpy/torch. Usage: <test.py> <segarecomp binary>.
"""
import hashlib
import json
import os
import pathlib
import re
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))
import segarecomp_ml_region_gen2 as g  # noqa: E402
import segarecomp_ml_region_gen2_native_data as nd  # noqa: E402
import segarecomp_ml_region_gen2_native_parity as P  # noqa: E402
import segarecomp_ml_region_gen2_rank as rk  # noqa: E402

BINARY = sys.argv.pop(1) if len(sys.argv) > 1 and not sys.argv[1].startswith("-") else None
ARTIFACT = json.loads((TOOLS / "segarecomp_ml_region_gen2.native_parity.json").read_text())
FROZEN = json.loads((TOOLS / "segarecomp_ml_region_gen2.frozen.json").read_text())
SRC = ROOT / "platforms" / "genesis" / "machine"


def run(*args, check=True):
    return subprocess.run([BINARY, *args], capture_output=True, text=True, check=check, timeout=600)


class Identity(unittest.TestCase):
    def test_generated_table_matches_frozen_model(self):
        self.assertEqual(nd.render((TOOLS / "segarecomp_ml_region_gen2.model.json").read_bytes(), FROZEN),
                         (SRC / "src" / "ml_region_gen2_model_data.inc").read_text().replace("\r\n", "\n"))

    def test_cpp_pins_equal_frozen_record(self):
        text = (SRC / "src" / "ml_region_gen2_producer.cpp").read_text()
        pins = dict(re.findall(r'pinned_(\w+) = "([^"]+)"', text))
        self.assertEqual(pins["model_sha256"], FROZEN["model_artifact"]["sha256"])
        self.assertEqual(pins["representation_sha256"], FROZEN["representation_sha256"])
        self.assertEqual(pins["token_schema_sha256"], FROZEN["token_schema_sha256"])
        self.assertEqual(pins["rank_plan_sha256"], FROZEN["rank_plan_sha256"])
        self.assertEqual(pins["representation_version"], FROZEN["representation_version"])
        inc = (SRC / "src" / "ml_region_gen2_model_data.inc").read_text()
        self.assertIn(f'g2_weight_table_sha256 = "{pins["weight_table_sha256"]}"', inc)
        self.assertEqual(g.representation_sha256(), FROZEN["representation_sha256"])
        self.assertEqual(g.token_schema_sha256(), FROZEN["token_schema_sha256"])

    def test_frozen_research_artifacts_and_v1_unchanged(self):
        self.assertEqual(g.sha256_bytes((TOOLS / "segarecomp_ml_region_gen2.model.json").read_bytes().replace(b"\r\n", b"\n")),
                         "11258708285e4427a3e38e02732e8f93b4c5d497883101f0629223d2366a98e1")
        self.assertEqual(g.v1_artifacts_unchanged(ROOT), [])
        self.assertEqual(rk.BUDGET_PERCENT["B35"], 35)
        self.assertEqual(ARTIFACT["model_sha256"], FROZEN["model_artifact"]["sha256"])

    def test_no_forbidden_runtime_dependency(self):
        for name in ("src/ml_region_gen2_producer.cpp", "include/segarecomp/machine/genesis/ml_region_gen2_producer.hpp"):
            body = (SRC / name).read_text().lower()
            code = "\n".join(line for line in body.splitlines() if not line.lstrip().startswith("//"))
            for token in ("torch", "onnx", "tensorflow", "python", "numpy", "sklearn", "dlopen", "popen", "system("):
                self.assertNotIn(token, code, f"{name}: {token}")

    def test_selection_signature_has_no_label_or_title_input(self):
        header = (SRC / "include" / "segarecomp" / "machine" / "genesis" / "ml_region_gen2_producer.hpp").read_text()
        select = re.search(r"genesis_ml2_rank_select\(([^)]*)\)", header).group(1)
        self.assertEqual(select.replace("\n", " ").split(","), ["std::span<const float> logits", " unsigned percent"])


@unittest.skipIf(BINARY is None, "needs the segarecomp binary")
class NativeCli(unittest.TestCase):
    def test_synthetic_probes_within_documented_tolerance(self):
        lines = run("ml-gen2-probe-report", "--synthetic").stdout.strip().splitlines()
        self.assertEqual(len(lines), 3)
        for line, expected in zip(lines, ARTIFACT["synthetic_probes"]["probes"]):
            m = re.fullmatch(r"probe (\d) logit (\S+) bits ([0-9a-f]{8})", line)
            value = float(m.group(2))
            self.assertLessEqual(abs(value - expected["reference_logit_double"]), ARTIFACT["numeric_contract"]["logit_tolerance_abs_vs_reference"])
            self.assertEqual(m.group(3), expected["native_bits"], "bitwise cross-platform determinism of the native binary32 logit")
            self.assertEqual(float("%.9e" % struct.unpack("<f", bytes.fromhex(m.group(3))[::-1])[0]), value, "printed logit is the exact binary32 value")

    def test_synthetic_images_logits_tokens_and_selection(self):
        with tempfile.TemporaryDirectory() as tmp_name:
            tmp = pathlib.Path(tmp_name)
            for name, spec in ARTIFACT["synthetic_images"].items():
                rom = P.lcg_rom(spec["words"], spec["seed"])
                rom_path, tok_path, logit_path = tmp / f"{name}.rom", tmp / f"{name}.tok", tmp / f"{name}.logits"
                rom_path.write_bytes(rom)
                run("m68k-decode-token-report", "--rom", str(rom_path), "--output", str(tok_path))
                tokens = g.parse_token_report(tok_path.read_bytes(), len(rom))
                self.assertEqual(g.sha256_bytes(tokens), spec["tokens_sha256"], f"{name}: CPU decode-token contract drift")
                err = run("ml-gen2-cell-logits", "--rom", str(rom_path), "--output", str(logit_path)).stderr
                raw = logit_path.read_bytes()
                self.assertEqual(hashlib.sha256(raw).hexdigest(), spec["logits_sha256"], f"{name}: bitwise logits")
                logits = P.read_logits(logit_path)
                self.assertEqual(len(logits), spec["cells"])
                selected = sorted(rk.rank_select(logits, 35))  # independent stdlib selection from the native logits
                self.assertEqual(P.selected_digest(selected), spec["selected_cells_sha256"])
                self.assertIn(f"selected_cells_sha256={spec['selected_cells_sha256']}", err)
                self.assertEqual(len(selected), rk.budget_cell_count(spec["cells"], 35))

    def test_region_proposal_output_on_synthetic_image(self):
        sys.path.insert(0, str(HERE))
        from genesis_ml_admission_cli_test import rom_image  # the project-authored synthetic Genesis image of the SEG-047 admission test
        body = rom_image()
        with tempfile.TemporaryDirectory() as tmp_name:
            tmp = pathlib.Path(tmp_name)
            rom = tmp / "t.rom"
            rom.write_bytes(bytes(body))
            sha = hashlib.sha256(bytes(body)).hexdigest()
            outs = []
            for tag in ("a", "b"):
                proc = run("emit-general-startup-bridge-c", "--rom", str(rom), "--reset-entry", "--rom-sha256", sha, "--immutable-rom-aot",
                           "--ml-gen2-region-proposal-output", str(tmp / f"{tag}.regions"), "--ml-gen2-logits-output", str(tmp / f"{tag}.logits"), check=False)
                self.assertEqual(proc.returncode, 0, proc.stderr[-400:])
                match = P.LINE.search(proc.stderr)
                self.assertIsNotNone(match)
                outs.append((match.groups(), (tmp / f"{tag}.regions").read_bytes(), (tmp / f"{tag}.logits").read_bytes()))
            self.assertEqual(outs[0], outs[1], "repeated runs are identical")
            groups = outs[0][0]
            self.assertEqual(int(groups[1]), rk.budget_cell_count(int(groups[0]), 35))
            logits = P.read_logits(tmp / "a.logits")
            selected = sorted(rk.rank_select(logits, 35))
            self.assertEqual(P.selected_digest(selected), groups[7])
            self.assertEqual(g.sha256_bytes(g.regions_text(sha, len(body), selected).encode()), groups[10], "ml-only region serialization equals the stdlib one")
            self.assertEqual(int(groups[3]) >= int(groups[1]), True)

    def test_help_lists_the_report_commands(self):
        usage = run("--help", check=False)
        text = usage.stdout + usage.stderr
        self.assertIn("ml-gen2-cell-logits", text)
        self.assertIn("--ml-gen2-region-proposal-output", text)


if __name__ == "__main__":
    unittest.main()
