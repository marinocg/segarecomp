#!/usr/bin/env python3
"""SEG-031 (ADR 0080): broad-versus-hybrid measurement of one Genesis title (measurement only; production default unchanged).

For one image it:
  1. plans the hybrid admission with the report-only planner (`--hybrid-plan`; wall time and peak RSS from its metrics output);
  2. emits generated C three ways with the generation-only CLI route: broad, broad from a reference (pre-SEG-031) CLI when given,
     and with the plan (`--immutable-rom-aot-admission`), and compares the trees (broad isolation and the candidate's effect);
  3. builds broad and planned production programs (`segarecomp build`, optionally `--admission-plan`), measuring wall time, child CPU
     time, peak child RSS, generated C bytes/files and executable size, and comparing the executables;
  4. optionally runs both through the bridge's headless execution-coverage route for a fixed frame count and explicit instruction
     budget (final-state and frame-stream digests must be equal; wall time is the runtime figure);
  5. optionally checks a private complete execution-PC oracle (`--coverage-dir`, a `coverage.bitmap`): every observed broad identity
     must be admitted by the plan (an escape is a BLOCKER; not observed proves nothing).
Output: one sanitized JSON object (counts, bytes, seconds, booleans and generic classes; never an address or a digest). Every
intermediate artifact stays in the ignored `--work` directory, which must be inside the product root.

usage: genesis_hybrid_admission_compare.py --segarecomp <cli> --driver <analysis-report> --rom <image> --work <ignored-dir>
       [--reference-segarecomp <cli>] [--cc <c-compiler>] [--coverage-dir <private-oracle-dir>] [--run-frames N] [--label NAME]
       [--skip-build]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import sys
import time

try:
    import resource
except ImportError:  # Windows: CPU time and RSS are not reported
    resource = None


def tree_digest(paths: list[pathlib.Path], base: pathlib.Path) -> str:
    digest = hashlib.sha256()
    for path in sorted(paths):
        digest.update(str(path.relative_to(base)).encode() + b"\0" + path.read_bytes() + b"\0")
    return digest.hexdigest()


def generated_files(directory: pathlib.Path) -> list[pathlib.Path]:
    return [p for p in directory.rglob("*") if p.is_file() and p.suffix in (".c", ".h")]


def timed(command: list[str], log: pathlib.Path) -> tuple[int, float, float | None, int | None]:
    before = resource.getrusage(resource.RUSAGE_CHILDREN) if resource else None
    started = time.monotonic()
    with log.open("w") as sink:
        rc = subprocess.run(command, stdout=sink, stderr=subprocess.STDOUT).returncode
    wall = time.monotonic() - started
    if resource is None:
        return rc, wall, None, None
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    cpu = (after.ru_utime - before.ru_utime) + (after.ru_stime - before.ru_stime)
    rss = after.ru_maxrss if sys.platform == "darwin" else after.ru_maxrss * 1024  # max over all children so far
    return rc, wall, cpu, rss


def load_bitmap(path: pathlib.Path) -> set[int]:
    data = path.read_bytes()
    if len(data) != 1 << 20:
        raise SystemExit("coverage bitmap has the wrong size")
    observed = set()
    for index, byte in enumerate(data):
        if byte:
            for bit in range(8):
                if byte >> bit & 1:
                    observed.add(((index << 3) | bit) << 1)
    return observed


def parse_plan(text: str) -> tuple[str, list[tuple[int, int]]]:
    strategy, ranges = "broad", []
    for line in text.splitlines():
        if line.startswith("strategy "):
            strategy = line.split()[1]
        elif line.startswith("range "):
            _, begin, end = line.split()
            ranges.append((int(begin, 16), int(end, 16)))
    return strategy, ranges


def admitted(strategy: str, ranges: list[tuple[int, int]], pc: int) -> bool:
    return strategy == "broad" or any(begin <= pc < end for begin, end in ranges)


def coverage_run(root: pathlib.Path, cli: str, rom: pathlib.Path, out: pathlib.Path, frames: int, plan: pathlib.Path | None) -> dict:
    command = [sys.executable, str(root / "tools/genesis_startup_bridge.py"), "--segarecomp", cli, "--rom", str(rom), "--mode",
               "commercial", "--immutable-rom-aot", "--execution-coverage", str(frames), "--coverage-epoch-frames", str(frames),
               "--coverage-no-render", "--instruction-budget", str(max(10_000_000, frames * 100_000)), "--out-dir", str(out)]
    if plan is not None:
        command += ["--admission-plan", str(plan)]
    started = time.monotonic()
    result = subprocess.run(command, capture_output=True, text=True)
    wall = time.monotonic() - started
    summary = next((line for line in result.stderr.splitlines() if line.startswith("COVERAGE_SUMMARY ")), None)
    if summary is None:
        return {"ok": False, "returncode": result.returncode}
    data = json.loads(summary.split(" ", 1)[1])
    return {"ok": True, "wall_seconds": round(wall, 2), "outcome": data["outcome"], "frames": data["frames_published"],
            "final_state": data["final_state_digest"], "frame_stream": data["frame_stream_digest"], "coverage": data["coverage_digest"]}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--segarecomp", required=True)
    parser.add_argument("--driver", required=True)
    parser.add_argument("--rom", required=True, type=pathlib.Path)
    parser.add_argument("--work", required=True, type=pathlib.Path)
    parser.add_argument("--reference-segarecomp")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--coverage-dir", type=pathlib.Path)
    parser.add_argument("--run-frames", type=int, default=0)
    parser.add_argument("--label", default="title")
    parser.add_argument("--skip-build", action="store_true")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    work = args.work.resolve()
    if subprocess.run(["git", "check-ignore", "-q", "--", str(work)], cwd=root).returncode != 0:
        raise SystemExit("--work must be an ignored directory inside the product root")
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    data = args.rom.read_bytes()
    sha = hashlib.sha256(data).hexdigest()
    report: dict = {"schema": "segarecomp.m68k_hybrid_compare.v1", "label": args.label}

    plan = work / "hybrid.plan"
    planned = subprocess.run([args.driver, "--rom", str(args.rom), "--rom-sha256", sha, "--reset-entry", "--private-output",
                              str(work / "plan.private.json"), "--hybrid-plan", str(plan), "--metrics-output", str(work / "plan.metrics.json")],
                             capture_output=True, text=True)
    if planned.returncode != 0:
        raise SystemExit(f"planner failed: {planned.returncode}")
    report["plan"] = json.loads(planned.stdout)
    metrics = json.loads((work / "plan.metrics.json").read_text())
    report["plan_cost"] = {"wall_seconds": metrics["wall_seconds"], "peak_rss_mib": round(metrics["peak_rss_bytes"] / 1048576, 1)}
    strategy, ranges = parse_plan(plan.read_text())

    # Generation only: broad, reference broad, planned.
    def emit(cli: str, name: str, extra: list[str]) -> tuple[str, int, int]:
        out = work / f"emit-{name}"
        out.mkdir()
        command = [cli, "emit-general-startup-bridge-c", "--rom", str(args.rom), "--reset-entry", "--rom-sha256", sha, "--immutable-rom-aot",
                   "--generated-c-shard-dir", str(out / "generated")] + extra
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode != 0:
            raise SystemExit(f"emission {name} failed: {result.returncode}")
        files = generated_files(out)
        return tree_digest(files, out), sum(p.stat().st_size for p in files), len(files)

    address_report = work / "u.txt"
    broad_digest, broad_bytes, broad_files = emit(args.segarecomp, "broad", ["--immutable-aot-address-report", str(address_report)])
    universe = {int(line, 16) for line in address_report.read_text().split()}
    address_report.unlink()
    hybrid_digest, hybrid_bytes, hybrid_files = emit(args.segarecomp, "hybrid", ["--immutable-rom-aot-admission", str(plan)])
    report["generation"] = {"broad_c_bytes": broad_bytes, "broad_c_files": broad_files, "planned_c_bytes": hybrid_bytes,
                            "planned_c_files": hybrid_files, "planned_identical_to_broad": hybrid_digest == broad_digest,
                            "c_bytes_change_percent": round(100.0 * (hybrid_bytes - broad_bytes) / broad_bytes, 2)}
    if args.reference_segarecomp:
        reference_digest, _, _ = emit(args.reference_segarecomp, "reference", [])
        report["generation"]["broad_identical_to_reference"] = reference_digest == broad_digest

    if not args.skip_build:
        def build(name: str, extra: list[str]) -> dict:
            out = work / f"build-{name}"
            command = [args.segarecomp, "build", "--rom", str(args.rom), "--output", str(out), "--cc", args.cc,
                       "--runtime-dir", str(root / "platforms/genesis")] + extra
            rc, wall, cpu, rss = timed(command, work / f"build-{name}.log")
            if rc != 0:
                raise SystemExit(f"build {name} failed: {rc}")
            exe = next(p for p in (out / "game", out / "game.exe") if p.exists())
            status = json.loads((out / "status.json").read_text())
            return {"wall_seconds": round(wall, 2), "cpu_seconds": None if cpu is None else round(cpu, 2),
                    "peak_child_rss_mib": None if rss is None else round(rss / 1048576, 1),
                    "executable_bytes": exe.stat().st_size, "executable_digest": hashlib.sha256(exe.read_bytes()).hexdigest(),
                    "admission": status.get("m68k_admission", "broad")}
        broad_build = build("broad", [])
        planned_build = build("planned", ["--admission-plan", str(plan)])
        same_exe = broad_build.pop("executable_digest") == planned_build.pop("executable_digest")
        report["build"] = {"broad": broad_build, "planned": planned_build, "executable_identical": same_exe}

    if args.run_frames:
        broad_run = coverage_run(root, args.segarecomp, args.rom, work / "run-broad", args.run_frames, None)
        planned_run = coverage_run(root, args.segarecomp, args.rom, work / "run-planned", args.run_frames, plan)
        same = broad_run.get("ok") and planned_run.get("ok") and all(broad_run[k] == planned_run[k] for k in ("final_state", "frame_stream", "coverage"))
        for run in (broad_run, planned_run):
            for key in ("final_state", "frame_stream", "coverage"):
                run.pop(key, None)
        report["runtime"] = {"broad": broad_run, "planned": planned_run, "identical_behaviour": bool(same)}

    if args.coverage_dir:
        observed = load_bitmap(args.coverage_dir / "coverage.bitmap")
        observed_broad = {pc for pc in observed if pc in universe}
        escapes = sum(1 for pc in observed_broad if not admitted(strategy, ranges, pc))
        report["oracle"] = {"observed_pcs": len(observed), "observed_broad_identities": len(observed_broad),
                            "observed_outside_broad_identities": len(observed) - len(observed_broad), "escapes": escapes}
    print(json.dumps(report, sort_keys=True))
    return 1 if report.get("oracle", {}).get("escapes", 0) else 0


if __name__ == "__main__":
    sys.exit(main())
