#!/usr/bin/env python3
"""Strict-C11 execution coverage for project-authored synthetic NEG.W Dn."""
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def environment():
    result = os.environ.copy()
    if not result.get("SDKROOT") and shutil.which("xcrun"):
        sdk = subprocess.run(["xcrun", "--show-sdk-path"], text=True, capture_output=True)
        if sdk.returncode == 0:
            result["SDKROOT"] = sdk.stdout.strip()
    return result


def checked(command, **kwargs):
    result = subprocess.run(command, text=True, capture_output=True, **kwargs)
    assert result.returncode == 0, (command, result.stderr)
    return result


def main():
    harness, compiler = map(Path, sys.argv[1:])
    cases = [
        ("4440", 0, "00000000", "2704"),
        ("4443", 3, "12340001", "2719"),
        ("4447", 7, "ABCD8000", "271B"),
    ]
    with tempfile.TemporaryDirectory() as directory:
        temporary = Path(directory)
        for index, (opcode, register, initial, expected_sr) in enumerate(cases):
            image = temporary / f"neg-word-{index}.bin"
            image.write_bytes(bytes.fromhex(opcode))
            d = ["00000000"] * 8
            d[register] = initial
            a = ["00000000"] * 7 + ["00FF0100"]
            source = temporary / f"neg-word-{index}.c"
            generated = checked([str(harness), str(image), "00000100", "2710", *d, *a, "0"]).stdout
            source.write_text(generated)
            executable = temporary / f"neg-word-{index}"
            checked([str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                     str(source), "-o", str(executable)], env=environment())
            observed = json.loads(checked([str(executable)]).stdout)
            assert observed["d"][register] == ("00000000" if index == 0 else
                                               "1234FFFF" if index == 1 else "ABCD8000")
            assert observed["sr"] == expected_sr and observed["pc"] == "00000102", observed

        # A one-address RMW evaluates its auto-updating EA once: both accesses
        # target the seeded word, and An changes by exactly one word.
        for index, (opcode, initial_a0, seed_address, expected_a0) in enumerate([
                ("4458", "00FF0010", "00FF0010", "00FF0012"),  # NEG.W (A0)+
                ("4460", "00FF0012", "00FF0010", "00FF0010"),  # NEG.W -(A0)
        ]):
            image = temporary / f"neg-auto-{index}.bin"
            image.write_bytes(bytes.fromhex(opcode))
            d = ["00000000"] * 8
            a = [initial_a0] + ["00000000"] * 6 + ["00FF0100"]
            source = temporary / f"neg-auto-{index}.c"
            generated = checked([str(harness), str(image), "00000100", "2710", *d, *a, "0",
                                 seed_address, "00010000"]).stdout
            source.write_text(generated)
            executable = temporary / f"neg-auto-{index}"
            checked([str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                     str(source), "-o", str(executable)], env=environment())
            observed = json.loads(checked([str(executable)]).stdout)
            assert observed["a"][0] == expected_a0 and observed["ram"] == ["FFFF0000"], observed
            assert observed["sr"] == "2719" and observed["pc"] == "00000102", observed
    print("NEG.W Dn generated C11 synthetic checks: ok")


if __name__ == "__main__":
    main()
