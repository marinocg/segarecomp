#!/usr/bin/env python3
"""Focused black-box regressions for T023 review corrections."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VECTORS = ROOT / "tests/fixtures/m68k-batch-a-review-corrections.json"

def environment():
    result = os.environ.copy()
    if not result.get("SDKROOT") and shutil.which("xcrun"):
        sdk = subprocess.run(["xcrun", "--show-sdk-path"], text=True, capture_output=True)
        if sdk.returncode == 0:
            result["SDKROOT"] = sdk.stdout.strip()
    return result

def image(recipe):
    result = bytearray(recipe["image_length"])
    result[0x100:0x100 + len(bytes.fromhex(recipe["instruction"]))] = bytes.fromhex(recipe["instruction"])
    for offset, data in recipe.get("writes", {}).items():
        at = int(offset, 16)
        result[at:at + len(bytes.fromhex(data))] = bytes.fromhex(data)
    return result

def args(harness, path, a7="00FF0100", seeds=()):
    values = [str(harness), str(path), "00000100", "0000000000000100", "2714",
              "11111111", "22222222", "33333333", "44444444", "55555555", "66666666", "77777777", "88888888",
              "00000000", "00000000", "00000000", "00000000", "00000000", "00000000", "00000000", a7]
    for seed in seeds:
        values += [seed["address"], seed["value"]]
    return values

def compile_and_run(compiler, source, temporary, name):
    binary = temporary / name
    built = subprocess.run([str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(source), "-o", str(binary)], text=True, capture_output=True, env=environment())
    assert built.returncode == 0, built.stderr
    run = subprocess.run([str(binary)], text=True, capture_output=True)
    assert run.returncode == 0 and run.stderr == "", run
    return json.loads(run.stdout)

def successful(harness, compiler, temporary, name, recipe, a7="00FF0100"):
    path = temporary / f"{name}.bin"
    path.write_bytes(image(recipe))
    lowered = subprocess.run(args(harness, path, a7, recipe.get("ram_seed", [])), text=True, capture_output=True)
    assert lowered.returncode == 0 and lowered.stderr == "", lowered
    source = temporary / f"{name}.c"
    source.write_text(lowered.stdout)
    return compile_and_run(compiler, source, temporary, name)

def main():
    harness, compiler = map(Path, sys.argv[1:3])
    vectors = json.loads(VECTORS.read_text())
    assert vectors["schema"] == 1 and "project-authored" in vectors["ownership"]
    absolute = vectors["absolute_word"]
    with tempfile.TemporaryDirectory() as name:
        temporary = Path(name)
        lower = successful(harness, compiler, temporary, "lower", absolute["lower_rom"])
        assert lower["d"][0] == absolute["lower_rom"]["expected_d0"] and lower["pc"] == absolute["lower_rom"]["expected_pc"] and lower["sr"] == absolute["lower_rom"]["expected_sr"]
        read = successful(harness, compiler, temporary, "upper-read", absolute["upper_ram_read"])
        assert read["d"][2] == absolute["upper_ram_read"]["expected_d2"] and read["pc"] == "00000104" and read["sr"] == "2718"
        write = successful(harness, compiler, temporary, "upper-write", absolute["upper_ram_write"])
        assert write["pc"] == absolute["upper_ram_write"]["expected_pc"] and write["sr"] == absolute["upper_ram_write"]["expected_sr"]
        rejected_path = temporary / "absolute-long.bin"
        rejected_path.write_bytes(image(absolute["absolute_long_rejected"]))
        rejected = subprocess.run(args(harness, rejected_path), text=True, capture_output=True)
        assert rejected.returncode == 1 and rejected.stdout == "", rejected
        assert json.loads(rejected.stderr)["category"] == absolute["absolute_long_rejected"]["category"]
        for name, recipe in vectors["a7_byte_ordering"].items():
            observed = successful(harness, compiler, temporary, name, recipe, recipe["a7"])
            assert observed["d"][0] == recipe["expected_d0"] and observed["a"][7] == recipe["expected_a7"], (name, observed)
    print("validated absolute.w Genesis-bus canonicalization and generated A7 byte ordering")

if __name__ == "__main__":
    main()
