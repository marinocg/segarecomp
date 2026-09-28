#!/usr/bin/env python3
"""SEG-024-T001: synthetic scaling series for the report-only executable-support experiment.

Generates project-authored Genesis images of increasing size (vector table, a small known code loop at 0x200,
and a deterministic pseudo-random high-entropy filler standing in for compressed/graphics data), runs the
existing general-startup route with ``--immutable-rom-aot --executable-support-report`` once per image, and
prints one JSON object with, per size: candidate count, frontend-analysis time (discovery + immutable-ROM
candidate enumeration), support-matrix time, total generation wall time, generated-C bytes and peak RSS.
No commercial input is read. Times/RSS are machine-dependent; counts are deterministic.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import resource
import subprocess
import sys
import tempfile
import time

# The existing fixtures' genuine partial-program prefix (so emission takes the ordinary C4 route real titles use):
CODE = bytes.fromhex(
    "3051"      # 0x200 MOVEA.W (A1),A0
    "4E90"      # 0x202 JSR (A0)
    "4E71"      # 0x204 NOP
    "60F8"      # 0x206 BRA.S 0x200
)


def synthetic_image(size: int, seed: int = 0x5E6024) -> bytes:
    image = bytearray(size)
    state = seed
    for offset in range(0x100, size, 4):  # xorshift32 filler
        state ^= (state << 13) & 0xFFFFFFFF
        state ^= state >> 17
        state ^= (state << 5) & 0xFFFFFFFF
        image[offset:offset + 4] = state.to_bytes(4, "big")[: min(4, size - offset)]
    image[0:4] = (0x00FF0100).to_bytes(4, "big")
    image[4:8] = (0x00000200).to_bytes(4, "big")
    image[8:0x100] = bytes(0xF8)  # every other vector uninstalled
    image[0x200:0x200 + len(CODE)] = CODE
    return bytes(image)


def peak_child_rss_bytes() -> int:
    rss = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    return rss if sys.platform == "darwin" else rss * 1024


def measure(segarecomp: pathlib.Path, size: int, work: pathlib.Path) -> dict:
    image = synthetic_image(size)
    rom = work / f"synthetic-{size}.bin"
    rom.write_bytes(image)
    report = work / f"synthetic-{size}.json"
    generated = work / f"synthetic-{size}.c"
    command = [str(segarecomp), "emit-general-startup-bridge-c", "--rom", str(rom), "--reset-entry",
               "--rom-sha256", hashlib.sha256(image).hexdigest(), "--immutable-rom-aot",
               "--executable-support-report", str(report), "--generated-c-output", str(generated)]
    started = time.monotonic()
    completed = subprocess.run(command, capture_output=True, text=True, timeout=1800)
    wall = time.monotonic() - started
    if completed.returncode != 0:
        raise SystemExit(f"segarecomp failed for {size}: {completed.stderr[-2000:]}")
    timing = re.search(r"frontend_analysis_ms=(\d+) support_matrix_ms=(\d+)", completed.stderr)
    data = json.loads(report.read_text())
    runs = {run["label"]: run for run in data["runs"]}
    result = {
        "image_bytes": size,
        "aligned_starts": data["universe"]["aligned_starts"],
        "candidates": data["universe"]["candidates"],
        "live_current_facts": runs["current_facts"]["live"],
        "live_fixed_flow_only": runs["fixed_flow_only"]["live"],
        "frontend_analysis_ms": int(timing.group(1)) if timing else None,
        "support_matrix_ms": int(timing.group(2)) if timing else None,
        "support_matrix_runs": len(data["runs"]),
        "generation_wall_s": round(wall, 2),
        "generated_c_bytes": generated.stat().st_size,
        "peak_child_rss_bytes_so_far": peak_child_rss_bytes(),
    }
    generated.unlink()
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--segarecomp", required=True, type=pathlib.Path)
    parser.add_argument("--sizes", default="524288,1048576,2097152,4194304")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="seg024-scaling-") as scratch:
        work = pathlib.Path(scratch)
        rows = [measure(args.segarecomp, int(size), work) for size in args.sizes.split(",")]
    json.dump({"schema": "segarecomp.executable-support-scaling.v1", "rows": rows}, sys.stdout, indent=1)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
