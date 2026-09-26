#!/usr/bin/env python3
"""SEG-021-T031: hermetic checks for the bridge's --capture-frames plumbing (no ROM)."""
import argparse
import importlib.util
import pathlib
import struct
import subprocess
import sys
import zlib


def load_bridge(root: pathlib.Path):
    sys.path.insert(0, str(root / "tools"))
    spec = importlib.util.spec_from_file_location("genesis_startup_bridge", root / "tools" / "genesis_startup_bridge.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    root = pathlib.Path(sys.argv[1]).resolve()
    bridge = load_bridge(root)
    assert bridge.capture_frames_value("40:16") == (40, 16, 1)
    assert bridge.capture_frames_value("40:16:80") == (40, 16, 80)
    for bad in ("0:1", "1:0", "1:65", "1:2:0", "x:1", "1", "1:2:3:4", "-1:2"):
        try:
            bridge.capture_frames_value(bad)
        except argparse.ArgumentTypeError:
            continue
        raise AssertionError(f"accepted {bad!r}")
    # PPM -> PNG: valid signature/IHDR, pixels round-trip exactly, deterministic bytes.
    width, height = 3, 2
    pixels = bytes(range(width * height * 3))
    ppm = b"P6\n%d %d\n255\n" % (width, height) + pixels
    png = bridge.ppm_to_png(ppm)
    assert png == bridge.ppm_to_png(ppm)
    assert png.startswith(b"\x89PNG\r\n\x1a\n")
    ihdr_len, ihdr_kind = struct.unpack(">I4s", png[8:16])
    assert ihdr_kind == b"IHDR" and struct.unpack(">IIBBBBB", png[16:16 + ihdr_len]) == (width, height, 8, 2, 0, 0, 0)
    offset, idat = 8, b""
    while offset < len(png):
        length, kind = struct.unpack(">I4s", png[offset:offset + 8])
        data = png[offset + 8:offset + 8 + length]
        assert struct.unpack(">I", png[offset + 8 + length:offset + 12 + length])[0] == zlib.crc32(kind + data) & 0xFFFFFFFF
        if kind == b"IDAT":
            idat += data
        offset += 12 + length
    raw = zlib.decompress(idat)
    rows = [raw[i * (width * 3 + 1):(i + 1) * (width * 3 + 1)] for i in range(height)]
    assert all(row[0] == 0 for row in rows) and b"".join(row[1:] for row in rows) == pixels
    for bad in (b"P3\n1 1\n255\n\x00\x00\x00", b"P6\n2 2\n255\n\x00"):
        try:
            bridge.ppm_to_png(bad)
        except ValueError:
            continue
        raise AssertionError("accepted malformed PPM")
    # Incompatible flag combinations are rejected before any generation (exit 8).
    for extra in (["--capture-slice", "10"], ["--capture-frames", "1:1", "--viewer"],
                  ["--capture-frames", "1:1", "--compare-runs"]):
        result = subprocess.run([sys.executable, str(root / "tools" / "genesis_startup_bridge.py"),
                                 "--rom", str(root / "README.md"), "--mode", "commercial"] + extra, capture_output=True, text=True)
        assert result.returncode == 8, (extra, result.returncode, result.stderr)
    print("genesis_frame_capture_bridge_test: OK")


if __name__ == "__main__":
    main()
