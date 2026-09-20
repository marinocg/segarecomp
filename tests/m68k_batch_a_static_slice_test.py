#!/usr/bin/env python3
"""Black-box validation for SEG-007-T023's common MC68000 startup/
data-movement batch (docs/references/
m68k-common-startup-data-movement-batch-contract.md).

Drives tests/tools/m68k_batch_a_test_harness.cpp, a test-only executable
(not the production `segarecomp` CLI) that exercises the real shared
decode_m68k_instruction / lift_m68k_instruction / (resolution) /
m68k_operation_effect / emit_m68k_operation_c production pipeline directly,
with no second, instruction-specific production API. Modeled directly on
tests/tst_l_static_slice_test.py.

Reads tests/fixtures/m68k-batch-a-fixtures.json's three vector kinds:
  * "accepted": statically-foldable and runtime-only positive forms whose
    generated C compiles, executes, and reaches the fixture's declared
    post-state deterministically.
  * "rejections" with kind == "decode"/"effective_address": the harness
    itself rejects before any C is printed (illegal-EA-for-instruction,
    size-illegal, truncation, alignment, mapping/controller-IO).
  * "rejections" with kind == "runtime": the harness succeeds (the EA mode
    is legal; no address exists to validate statically), but the COMPILED
    program's own runtime bounds-check guard fails closed (`return 1;`, no
    partial mutation, no stdout).
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "tests/fixtures/m68k-batch-a-fixtures.json"


def compiler_environment():
    """Supply macOS's SDK to generated-C compilation, matching the tst_l
    precedent's own compiler_environment() helper."""
    environment = os.environ.copy()
    if not environment.get("SDKROOT") and shutil.which("xcrun"):
        sdk = subprocess.run(["xcrun", "--show-sdk-path"], text=True, capture_output=True, check=False)
        if sdk.returncode == 0:
            environment["SDKROOT"] = sdk.stdout.strip()
    return environment


def command(executable, image, entry):
    args = [str(executable), str(image), entry["entry_pc"], f"{int(entry['entry_pc'], 16):016X}",
            entry["seed_sr"], *entry["seed_d"], *entry["seed_a"]]
    for seed in entry.get("ram_seed", []):
        args += [seed["address"], seed["value"]]
    return args


def main():
    executable, compiler = map(Path, sys.argv[1:3])
    manifest = json.loads(FIXTURES.read_text())
    assert manifest["schema"] == 1
    ownership = manifest["ownership"].lower()
    assert "project-authored" in ownership and "no commercial content" in ownership

    with tempfile.TemporaryDirectory() as temporary_name:
        temporary = Path(temporary_name)

        for entry in manifest["accepted"]:
            image_bytes = bytes.fromhex(entry["image_hex"])
            assert hashlib.sha256(image_bytes).hexdigest() == entry["sha256"], entry["id"]
            image = temporary / f"{entry['id']}.bin"
            image.write_bytes(image_bytes)
            args = command(executable, image, entry)
            first = subprocess.run(args, text=True, capture_output=True, check=False)
            repeated = subprocess.run(args, text=True, capture_output=True, check=False)
            assert first.returncode == 0 and first.stderr == "", (entry["id"], first)
            assert repeated.stdout == first.stdout, entry["id"]
            assert all(token not in first.stdout for token in
                       ("fopen", "fread", "argv", "decode_m68k_instruction", "opcode")), entry["id"]

            source = temporary / f"{entry['id']}.c"
            binary = temporary / entry["id"]
            source.write_text(first.stdout)
            compiled = subprocess.run(
                [str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(source), "-o", str(binary)],
                text=True, capture_output=True, check=False, env=compiler_environment())
            assert compiled.returncode == 0, (entry["id"], compiled.stderr)

            expected = {"schema": 1, "d": entry["expected_d"], "a": entry["expected_a"],
                        "pc": entry["expected_pc"], "sr": entry["expected_sr"],
                        "stop_reason": "instruction_budget_exhausted"}
            executed = subprocess.run([str(binary)], text=True, capture_output=True, check=False)
            executed_again = subprocess.run([str(binary)], text=True, capture_output=True, check=False)
            assert executed.returncode == 0 and executed.stderr == "", (entry["id"], executed)
            observed = json.loads(executed.stdout)
            assert observed == expected, (entry["id"], observed, expected)
            assert executed_again.stdout == executed.stdout, entry["id"]

        for entry in manifest["rejections"]:
            image_bytes = bytes.fromhex(entry["image_hex"])
            assert hashlib.sha256(image_bytes).hexdigest() == entry["sha256"], entry["id"]
            image = temporary / f"{entry['id']}.bin"
            image.write_bytes(image_bytes)

            if entry["kind"] in ("decode", "effective_address"):
                args = [str(executable), str(image), "00000100", "0000000000000100", "2714",
                        "11111111", "22222222", "33333333", "44444444",
                        "55555555", "66666666", "77777777", "88888888",
                        "00000000", "00000000", "00000000", "00000000",
                        "00000000", "00000000", "00000000", "00FF0100"]
                result = subprocess.run(args, text=True, capture_output=True, check=False)
                assert result.returncode == 1 and result.stdout == "", (entry["id"], result)
                assert result.stderr.count("\n") == 1 and result.stderr.endswith("}\n"), (entry["id"], result)
                payload = json.loads(result.stderr)
                assert payload["category"] == entry["expected_category"], (entry["id"], payload)
                assert payload["stage"] == entry["expected_stage"], (entry["id"], payload)
                assert payload["unsupported_instruction_form"] == entry["expected_unsupported_instruction_form"], (
                    entry["id"], payload)
            else:
                assert entry["kind"] == "runtime", entry["id"]
                args = [str(executable), str(image), "00000100", "0000000000000100", "2714",
                        "11111111", "22222222", "33333333", "44444444",
                        "55555555", "66666666", "77777777", "88888888",
                        *entry["seed_a"]]
                result = subprocess.run(args, text=True, capture_output=True, check=False)
                assert result.returncode == 0 and result.stderr == "", (entry["id"], result)
                source = temporary / f"{entry['id']}.c"
                binary = temporary / entry["id"]
                source.write_text(result.stdout)
                compiled = subprocess.run(
                    [str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                     str(source), "-o", str(binary)], text=True, capture_output=True, check=False,
                    env=compiler_environment())
                assert compiled.returncode == 0, (entry["id"], compiled.stderr)
                executed = subprocess.run([str(binary)], text=True, capture_output=True, check=False)
                # Fail-closed: nonzero exit, and (since the guard `return 1;`
                # fires before any printf) no observable output at all -- no
                # partial mutation is ever reported.
                assert executed.returncode == 1 and executed.stdout == "" and executed.stderr == "", (
                    entry["id"], executed)

    accepted_count = len(manifest["accepted"])
    rejection_count = len(manifest["rejections"])
    print(f"validated {accepted_count} accepted and {rejection_count} rejected common MC68000 "
          "startup/data-movement batch vectors (static-foldable, runtime-guarded, and decode-stage forms)")


if __name__ == "__main__":
    main()
