#!/usr/bin/env python3
"""Synthetic black-box coverage for the production general-startup ingress."""
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import tempfile


def build_image(vector):
    image = bytearray(vector["image_length"])
    for offset, data in vector["writes"].items():
        start = int(offset, 16)
        raw = bytes.fromhex(data)
        image[start:start + len(raw)] = raw
    image[0x120:0x150] = vector["title"].encode("ascii").ljust(48, b" ")
    return image


def main() -> None:
    (executable,) = sys.argv[1:]
    root = pathlib.Path(__file__).resolve().parent
    vector = json.loads((root / "fixtures/genesis-general-startup-vectors.json").read_text())
    image = build_image(vector)
    assert hashlib.sha256(image).hexdigest() == vector["sha256"]

    with tempfile.TemporaryDirectory() as directory:
        rom = pathlib.Path(directory) / "general-startup.bin"
        rom.write_bytes(image)

        general = subprocess.run([executable, "genesis-general-startup", str(rom)],
                                 text=True, capture_output=True, check=False)
        assert general.returncode == 0 and general.stderr == "", general
        report = json.loads(general.stdout)
        expected = vector["expected"]
        assert report["result"] == expected["general_result"], general
        assert report["profile"] == "general_startup", general
        for field in ("decoded", "blocks", "edges", "frames"):
            assert report[field] == expected[field], general

        # This five-instruction, two-call-frame graph begins with JSR rather
        # than the fixed route's required MOVEQ. The same reset-valid image
        # must therefore be rejected by that retained route.
        fixed = subprocess.run([executable, "genesis-rom-startup", str(rom)],
                               text=True, capture_output=True, check=False)
        assert fixed.returncode == 1 and fixed.stdout == "", fixed
        assert json.loads(fixed.stderr)["category"] == vector["expected"]["fixed_category"], fixed

        # The public AOT source has no caller-selected extent. Its normalized
        # accounting exposes counts only, while the removed range spelling is
        # rejected as an unknown option.
        aot = subprocess.run([
            executable, "emit-general-startup-bridge-c", "--rom", str(rom),
            "--reset-entry", "--rom-sha256", vector["sha256"], "--immutable-rom-aot",
        ], text=True, capture_output=True, check=False)
        assert aot.returncode == 0, aot.stderr
        aggregate = re.search(
            r"immutable-rom AOT enumeration: aligned_start_count=(\d+) accepted_count=(\d+) rejected_count=(\d+)",
            aot.stderr,
        )
        assert aggregate is not None, aot.stderr
        assert "0x" not in aot.stderr
        total, accepted, rejected = map(int, aggregate.groups())
        assert total > 0 and accepted > 0 and accepted + rejected == total
        old_range = subprocess.run([
            executable, "emit-general-startup-bridge-c", "--rom", str(rom),
            "--reset-entry", "--rom-sha256", vector["sha256"],
            "--immutable-rom-aot-range", "00000000", "00000002",
        ], text=True, capture_output=True, check=False)
        assert old_range.returncode == 2 and old_range.stdout == ""

    print("genesis general startup CLI: ok")


if __name__ == "__main__":
    main()
