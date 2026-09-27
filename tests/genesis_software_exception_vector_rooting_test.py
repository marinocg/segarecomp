#!/usr/bin/env python3
"""SEG-021-T019 correction: an unadmittable software-exception vector slot is "not installed", not a build error.

A project-authored synthetic image (no commercial data) whose reset program never raises a software exception must
build even when a TRAP / line-A / line-F / CHK / TRAPV / ILLEGAL vector slot points at work RAM or an odd address.
Vectors 5 and 8 keep their established fail-closed build rule.

usage: genesis_software_exception_vector_rooting_test.py <segarecomp>
"""
import json
import pathlib
import subprocess
import sys
import tempfile


def image(vector: int, value: int) -> bytes:
    img = bytearray(0x400)
    img[0:4] = (0x00FFFE00).to_bytes(4, "big")      # initial SSP
    img[4:8] = (0x00000200).to_bytes(4, "big")      # reset PC
    img[0x100:0x110] = b"SEGA GENESIS    "
    img[0x200:0x202] = bytes([0x60, 0xFE])           # BRA.S * (never raises a software exception)
    img[vector * 4:vector * 4 + 4] = value.to_bytes(4, "big")
    return bytes(img)


def analyze(segarecomp: str, data: bytes) -> dict:
    with tempfile.TemporaryDirectory() as tmp:
        path = pathlib.Path(tmp) / "synthetic.bin"
        path.write_bytes(data)
        result = subprocess.run([segarecomp, "genesis-general-startup", str(path)], capture_output=True, text=True)
        lines = (result.stdout + "\n" + result.stderr).strip().splitlines()
        reports = [line for line in lines if line.startswith("{")]
        assert reports, (result.returncode, result.stdout, result.stderr)
        return json.loads(reports[-1])


def main():
    segarecomp = sys.argv[1]
    assert analyze(segarecomp, image(47, 0))["result"] == "accepted", "baseline image builds"
    for vector in (4, 6, 7, 10, 11, 32, 47):
        for value in (0x00FF0000, 0x00000301, 0x00800000):  # work RAM, odd, unmapped
            report = analyze(segarecomp, image(vector, value))
            assert report["result"] == "accepted", (vector, hex(value), report)
    # Established vectors 5 and 8 keep their fail-closed build rule.
    for vector in (5, 8):
        report = analyze(segarecomp, image(vector, 0x00000301))
        assert report["result"] == "rejected", (vector, report)
    print("genesis_software_exception_vector_rooting_test: OK")


if __name__ == "__main__":
    main()
