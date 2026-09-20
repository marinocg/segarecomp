#!/usr/bin/env python3
"""Black-box validation for TST.L (xxx).L's shared M68k pipeline contract
(SEG-007-T008, docs/references/tst-l-absolute-long-contract.md).

Drives tests/tools/tst_l_test_harness.cpp, a test-only executable (not the
production `segarecomp` CLI) that exercises the real shared
decode_m68k_instruction/lift_m68k_instruction/m68k_resolve_absolute_test_operand/
m68k_operation_effect/emit_m68k_operation_c production pipeline directly, with
no second, instruction-specific production API.

Reuses the exact accepted-vector and fail-closed images/hashes the contract
already recorded (see tests/fixtures/tst-l-fixtures.json's "ownership"
field), and independently exercises the same accepted lowering path against a
project-authored synthetic_work_ram operand, which the contract requires to
exist but does not itself pin fixed bytes for.
"""
import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "tests/fixtures/tst-l-fixtures.json"


def fixed_hex(value, width):
    return isinstance(value, str) and len(value) == width and value == value.upper() and all(
        character in "0123456789ABCDEF" for character in value)


def command(executable, image, entry, seed_d, seed_sr, extra=()):
    return [str(executable), str(image), "00000100",
            "0000000000000100", seed_sr, *seed_d, *extra]


def main():
    executable, compiler = map(Path, sys.argv[1:3])
    manifest = json.loads(FIXTURES.read_text())
    assert manifest["schema"] == 1 and "project-authored" in manifest["ownership"].lower()
    seed_d = manifest["seed_d"]
    seed_sr = manifest["seed_sr"]

    with tempfile.TemporaryDirectory() as temporary:
        temporary = Path(temporary)

        for entry in manifest["accepted"]:
            image_bytes = bytes.fromhex(entry["image_hex"])
            assert hashlib.sha256(image_bytes).hexdigest() == entry["sha256"], entry["id"]
            image = temporary / f"{entry['id']}.bin"
            image.write_bytes(image_bytes)
            extra = []
            for seed in entry.get("ram_seed", []):
                extra += [seed["address"], seed["value"]]
            result = subprocess.run(command(executable, image, entry, seed_d, seed_sr, extra),
                                    text=True, capture_output=True, check=False)
            assert result.returncode == 0 and result.stderr == "", (entry["id"], result.stderr, result.stdout)
            repeated = subprocess.run(command(executable, image, entry, seed_d, seed_sr, extra),
                                      text=True, capture_output=True, check=False)
            assert repeated.returncode == 0 and repeated.stdout == result.stdout and repeated.stderr == ""
            # The translator retains source provenance as metadata, but never embeds the ROM data
            # image bytes as an opaque blob, nor re-fetches/decodes them at generated-program runtime.
            assert "segarecomp_tst_l_source_address" in result.stdout
            assert "segarecomp_tst_l_image_offset" in result.stdout
            assert all(token not in result.stdout for token in ("fopen", "fread", "open(", "getchar", "scanf"))
            source = temporary / f"{entry['id']}.c"
            binary = temporary / entry["id"]
            source.write_text(result.stdout)
            compiled = subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", source, "-o", binary],
                text=True, capture_output=True, check=False)
            assert compiled.returncode == 0, compiled.stderr
            executed = subprocess.run([binary], text=True, capture_output=True, check=False)
            expected = {"schema": 1, "d": entry["expected_d"], "pc": "00000106",
                        "sr": entry["expected_sr"], "stop_reason": "instruction_budget_exhausted"}
            assert executed.returncode == 0 and executed.stderr == "" and json.loads(executed.stdout) == expected, (
                entry["id"], executed)
            repeated_execution = subprocess.run([binary], text=True, capture_output=True, check=False)
            assert repeated_execution.returncode == 0 and json.loads(repeated_execution.stdout) == expected

        for entry in manifest["rejections"]:
            image_bytes = bytes.fromhex(entry["image_hex"])
            assert hashlib.sha256(image_bytes).hexdigest() == entry["sha256"], entry["id"]
            image = temporary / f"{entry['id']}.bin"
            image.write_bytes(image_bytes)
            result = subprocess.run(command(executable, image, entry, seed_d, seed_sr),
                                    text=True, capture_output=True, check=False)
            if entry.get("expected_policy"):
                assert result.returncode == 0 and result.stderr == "", (entry["id"], result.stderr)
                assert "SEG-007-T020" not in result.stdout
                assert "resolved static read" in result.stdout
                source = temporary / f"{entry['id']}.c"
                binary = temporary / entry["id"]
                source.write_text(result.stdout)
                compiled = subprocess.run(
                    [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", source, "-o", binary],
                    text=True, capture_output=True, check=False)
                assert compiled.returncode == 0, compiled.stderr
                expected = {"schema": 1, "d": seed_d, "pc": "00000106", "sr": "2714",
                            "stop_reason": "instruction_budget_exhausted"}
                executed = subprocess.run([binary], text=True, capture_output=True, check=False)
                assert executed.returncode == 0 and json.loads(executed.stdout) == expected
                continue
            assert result.returncode == 1 and result.stdout == "", (entry["id"], result)
            payload = json.loads(result.stderr)
            assert payload["category"] == entry["expected_category"], (entry["id"], payload)
            assert payload["stage"] == entry["expected_stage"], (entry["id"], payload)
            if "expected_unsupported_instruction_form" in entry:
                assert payload["unsupported_instruction_form"] == entry["expected_unsupported_instruction_form"], (
                    entry["id"], payload)
            if entry["expected_stage"] == "effective_address":
                # A rejected operand emits no data read: the instruction record itself is complete
                # (six verified bytes), but no ROM/RAM byte beyond the primary+extension was ever
                # read or embedded.
                assert payload["provenance"]["length"] == 6
                assert payload["complete_raw_bytes"] is not None
                assert payload["effective_address"] is not None
                if "expected_effective_address" in entry:
                    assert payload["effective_address"] == entry["expected_effective_address"]
                    assert payload["complete_raw_bytes"] == entry["expected_complete_raw_bytes"]
            else:
                assert payload["complete_raw_bytes"] is None

    print("validated static TST.L C translation, execution, source-provenanced rejections, "
          "and the untouched genesis-rom-startup route")


if __name__ == "__main__":
    main()
