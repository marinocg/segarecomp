#!/usr/bin/env python3
"""SEG-047 (ADR 0096): bounded broad-vs-ML-selective differential of one Genesis image (measurement only).

Runs the unchanged bridge twice for a fixed frame count and an explicit instruction budget: broad, and with the native ML admission
(`--ml-admission`). Equal final-state / frame-stream / coverage digests and outcome are required. Output: one sanitized JSON object
(counts, bytes, seconds, booleans; never an address or a digest value). Intermediate artifacts stay in the ignored `--work` directory.

usage: genesis_ml_admission_differential.py --segarecomp <cli> --rom <image> --work <ignored-dir> --frames N [--mode synthetic|commercial]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]


def generated_bytes(out: pathlib.Path) -> int:
    files = [p for p in (out / "generated").rglob("*") if p.is_file()] if (out / "generated").exists() else []
    files += [p for p in out.glob("*.generated.c") if p.is_file()]
    return sum(p.stat().st_size for p in files)


def run(cli: str, rom: pathlib.Path, out: pathlib.Path, frames: int, mode: str, ml: bool, cc: str) -> dict:
    command = [sys.executable, str(ROOT / "tools/genesis_startup_bridge.py"), "--segarecomp", cli, "--rom", str(rom), "--mode", mode,
               "--immutable-rom-aot", "--execution-coverage", str(frames), "--coverage-epoch-frames", str(frames), "--coverage-no-render",
               "--instruction-budget", str(max(10_000_000, frames * 100_000)), "--out-dir", str(out), "--cc", cc]
    if ml:
        command.append("--ml-admission")
    started = time.monotonic()
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    wall = time.monotonic() - started
    summary = next((line for line in result.stderr.splitlines() if line.startswith("COVERAGE_SUMMARY ")), None)
    admission = [line for line in result.stderr.splitlines() if "m68k admission: requested=optimized" in line]
    if ml:  # the bridge does not forward the emitter report: ask the generation-only route (seconds) for the sanitized line
        digest = hashlib.sha256(rom.read_bytes()).hexdigest()
        probe = subprocess.run([cli, "emit-general-startup-bridge-c", "--rom", str(rom), "--reset-entry", "--rom-sha256", digest,
                                "--immutable-rom-aot", "--immutable-rom-aot-ml-admission"] +
                               ["--generated-c-output", str(out / "probe.c")], capture_output=True, text=True, check=False)
        admission = [l for l in probe.stderr.splitlines() if "m68k admission: requested=optimized" in l]
    if summary is None:
        return {"ok": False, "returncode": result.returncode}
    data = json.loads(summary.split(" ", 1)[1])
    return {"ok": True, "wall_seconds": round(wall, 1), "outcome": data["outcome"], "frames": data["frames_published"],
            "final_state": data["final_state_digest"], "frame_stream": data["frame_stream_digest"], "coverage": data["coverage_digest"],
            "generated_bytes": generated_bytes(out), "producer": "ml_region" if any("producer=ml_region" in l for l in admission) else "broad"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--segarecomp", required=True)
    parser.add_argument("--rom", required=True, type=pathlib.Path)
    parser.add_argument("--work", required=True, type=pathlib.Path)
    parser.add_argument("--frames", required=True, type=int)
    parser.add_argument("--cc", default="clang", help="C compiler for the generated programs (clang: gcc -Werror rejects constant division in generated C)")
    parser.add_argument("--mode", default="commercial", choices=("synthetic", "commercial"))
    args = parser.parse_args()
    work = args.work.resolve()
    if subprocess.run(["git", "check-ignore", "-q", "--", str(work)], cwd=ROOT).returncode != 0:
        raise SystemExit("--work must be an ignored directory inside the product root")
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    broad = run(args.segarecomp, args.rom, work / "broad", args.frames, args.mode, False, args.cc)
    ml = run(args.segarecomp, args.rom, work / "ml", args.frames, args.mode, True, args.cc)
    equal = bool(broad.get("ok") and ml.get("ok") and all(broad[k] == ml[k] for k in ("outcome", "frames", "final_state", "frame_stream", "coverage")))
    report = {"schema": "segarecomp.ml_admission_differential.v1", "frames": args.frames, "equal_behaviour": equal,
              "broad": {k: v for k, v in broad.items() if k in ("ok", "wall_seconds", "outcome", "frames", "generated_bytes")},
              "ml": {k: v for k, v in ml.items() if k in ("ok", "wall_seconds", "outcome", "frames", "generated_bytes", "producer")}}
    if equal and broad["generated_bytes"] and ml["generated_bytes"]:
        report["generated_bytes_reduction"] = round(1 - ml["generated_bytes"] / broad["generated_bytes"], 4)
    print(json.dumps(report, indent=1, sort_keys=True))
    return 0 if equal else 1


if __name__ == "__main__":
    sys.exit(main())
