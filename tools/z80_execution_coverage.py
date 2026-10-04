#!/usr/bin/env python3
"""SEG-031 (ADR 0080): Z80 execution-PC coverage for a Master System title or a Genesis title's materialized Z80 images.

Measurement only. The program is built through the unchanged production route (`segarecomp build`) with
`--cc-arg -DSEGARECOMP_Z80_EXECUTION_COVERAGE` (the observer call is compiled only then; the default program never calls it):

  * Master System: the headless program runs `--frames N` twice with `--execution-coverage <dir>` and once without it; the machine
    state digest must be identical in all three runs (zero semantic effect) and the private coverage list identical in both observed
    runs (deterministic);
  * Genesis: the build keeps its work directory (`--keep-work 1`) and the materialization pass program (the full game, headless, with
    the final Z80 image registry) runs twice more with SEGARECOMP_Z80_COVERAGE_DIR; the coverage lists must be identical. Use the
    build's own observation window (600 frames, gz80::kObservationFrames): a longer run may meet an image the build never materialized,
    which is a materialization-window limit (typed `unknown_image`), reported here as a Z80 error rather than hidden.

The falsification claim is broad Z80 AOT: every observed (code-image identity, PC) executed through an exact generated entry. That holds
by construction for a run that did not stop with a Z80 error or an unknown identity; the tool fails if either occurs (an escape would
surface exactly there). Output: one sanitized JSON object (counts, booleans and the outcome class; the private PC lists and their
digests stay in the ignored --work directory).

usage: z80_execution_coverage.py --segarecomp <cli> --rom <image> --platform master-system|genesis --work <ignored-dir>
       [--mapper sega|rom_only] [--frames N] [--cc <c-compiler>] [--label NAME]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time

MACRO = "-DSEGARECOMP_Z80_EXECUTION_COVERAGE"


def summary_of(stderr: str) -> dict | None:
    line = next((l for l in stderr.splitlines() if l.startswith("Z80_COVERAGE_SUMMARY ")), None)
    return json.loads(line.split(" ", 1)[1]) if line else None


def digest_file(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--segarecomp", required=True)
    parser.add_argument("--rom", required=True, type=pathlib.Path)
    parser.add_argument("--platform", required=True, choices=("master-system", "genesis"))
    parser.add_argument("--work", required=True, type=pathlib.Path)
    parser.add_argument("--mapper", default="sega")
    parser.add_argument("--frames", type=int, default=600)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--label", default="title")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    work = args.work.resolve()
    if subprocess.run(["git", "check-ignore", "-q", "--", str(work)], cwd=root).returncode != 0:
        raise SystemExit("--work must be an ignored directory inside the product root")
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    out = work / "build"
    command = [args.segarecomp, "build", "--rom", str(args.rom), "--output", str(out), "--cc", args.cc, "--cc-arg", MACRO,
               "--runtime-dir", str(root / "platforms" / args.platform), "--platform", args.platform]
    if args.platform == "master-system":
        command += ["--mapper", args.mapper]
    else:
        command += ["--keep-work", "1"]
    started = time.monotonic()
    built = subprocess.run(command, capture_output=True, text=True)
    build_seconds = time.monotonic() - started
    if built.returncode != 0:
        raise SystemExit(f"build failed ({built.returncode}): {built.stdout[-1500:]}")
    report: dict = {"schema": "segarecomp.z80_execution_coverage.v1", "label": args.label, "platform": args.platform,
                    "build_seconds": round(build_seconds, 1)}
    runs = []
    if args.platform == "master-system":
        exe = next(p for p in (out / "game", out / "game.exe") if p.exists())
        states = []
        for index, observed in enumerate((True, True, False)):
            cov = work / f"cov{index}"
            cov.mkdir()
            extra = ["--execution-coverage", str(cov)] if observed else []
            result = subprocess.run([str(exe), "--frames", str(args.frames)] + extra, capture_output=True, text=True, timeout=1800)
            match = re.search(r"^stop (\S+) cycles \d+ frames (\d+) digest ([0-9a-f]{64})", result.stdout, re.M)
            if match is None:
                raise SystemExit(f"run {index} printed no stop line (rc {result.returncode})")
            states.append(match.group(3))
            if observed:
                summary = summary_of(result.stderr)
                if summary is None:
                    raise SystemExit("an observed run printed no Z80_COVERAGE_SUMMARY (was the macro compiled in?)")
                runs.append({"stop": match.group(1), "frames": int(match.group(2)), "summary": summary,
                             "list": digest_file(cov / "z80-coverage.txt"), "z80_error": "z80_outcome" in result.stdout})
        report["zero_semantic_effect"] = len(set(states)) == 1
    else:
        obj = out / "obj"
        pass_program = next(p for p in (obj / "materialize-pass", obj / "materialize-pass.exe") if p.exists())
        for index in range(2):
            cov = work / f"cov{index}"
            cov.mkdir()
            env = dict(os.environ, SEGARECOMP_MATERIALIZE_DIR=str(cov), SEGARECOMP_MATERIALIZE_FRAMES=str(args.frames),
                       SEGARECOMP_Z80_COVERAGE_DIR=str(cov))
            result = subprocess.run([str(pass_program), "--instruction-budget", str(max(50_000_000, args.frames * 200_000))],
                                    capture_output=True, text=True, env=env, timeout=1800)
            summary = summary_of(result.stderr)
            outcome = next((l.split()[1] for l in (cov / "pass.report").read_text().splitlines() if l.startswith("outcome ")), "missing")
            if summary is None:
                raise SystemExit("the pass printed no Z80_COVERAGE_SUMMARY (was the macro compiled in?)")
            runs.append({"stop": outcome, "summary": summary, "list": digest_file(cov / "z80-coverage.txt"),
                         "z80_error": outcome in ("unknown_image", "z80_code_mismatch", "z80_stop")})
    first = runs[0]
    report["deterministic"] = all(run["list"] == first["list"] and run["summary"] == first["summary"] for run in runs)
    report["outcome"] = first["stop"]
    report["images"] = first["summary"]["images"]
    report["distinct_pcs"] = first["summary"]["distinct_pcs"]
    report["retirements"] = first["summary"]["retirements"]
    report["unknown_identity"] = first["summary"]["unknown_identity"] + first["summary"]["identity_overflow"]
    report["z80_error"] = first["z80_error"]
    report["escapes"] = 0 if not first["z80_error"] and report["unknown_identity"] == 0 else None
    print(json.dumps(report, sort_keys=True))
    ok = report["deterministic"] and report["escapes"] == 0 and report.get("zero_semantic_effect", True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
