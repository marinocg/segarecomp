#!/usr/bin/env python3
"""SEG-057: runtime equivalence of a selective build with the broad AOT on one real image (ADR 0118). Measurement tool, not a product feature.

  z80_admission_equivalence.py --emit <sms_admission_emit> --rom <rom> --frames N [--cc gcc] [--work DIR]

1. builds the BROAD program with the SEG-031 execution-coverage observer (a measurement build) and runs it for N frames, collecting
   which (code-image identity, PC) starts begin executing;
2. turns exactly those starts (plus the architectural roots) into an admission plan, builds the SELECTIVE program (a production build) and
   runs it for the same N frames;
3. compares the exit status and the machine state digest.

Identical digests show that, for this deterministic run, executing only admitted starts is observably equivalent to the broad AOT. It is
NOT evidence of execution completeness for any other run: the plan is the trace itself. Everything private (plan, coverage, ROM) stays under
--work; only aggregates are printed (counts and fractions, never an address or a byte).
"""
import argparse
import json
import pathlib
import shutil
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import z80_admission_economics as econ  # noqa: E402

SLOT_BASES = (0x0000, 0x4000, 0x8000)


def compile_program(args, rom, work, plan, cflags):
    shutil.rmtree(work, ignore_errors=True)
    shard = work / "generated"
    shard.mkdir(parents=True)
    command = [args.emit, str(rom), str(shard), "sms"] + (["--plan", str(plan)] if plan else [])
    code, _wall, _cpu, _rss, output = econ.run_timed(command)
    if code != 0:
        raise SystemExit("emit failed: " + output[-300:])
    stats = json.loads(output.strip().splitlines()[-1])
    units = [shard / n for n in (shard / "sms.units").read_text().split()]
    base = [args.cc, "-std=c11", "-O1", "-w", *cflags, "-D_CRT_SECURE_NO_WARNINGS", "-I", str(econ.ROOT / "libs/codegen/c11/include"),
            "-I", str(econ.RUNTIME), "-I", str(econ.PSG / "include"), "-I", str(shard), "-I", str(econ.HEADLESS)]
    sources = units + [econ.RUNTIME / n for n in econ.RUNTIME_SOURCES] + [econ.PSG / "src/sn76489.c"] + [econ.HEADLESS / n for n in econ.HEADLESS_SOURCES]
    processes = []
    for i, src in enumerate(sources):
        obj = work / ("%d.o" % i)
        processes.append((subprocess.Popen(base + ["-c", "-o", str(obj), str(src)]), obj))
        while sum(1 for p, _ in processes if p.poll() is None) >= args.jobs:
            processes[[p.poll() is None for p, _ in processes].index(True)][0].wait()
    for process, _ in processes:
        if process.wait() != 0:
            raise SystemExit("compile failed")
    exe = work / "program"
    if subprocess.run([args.cc, *[str(o) for _, o in processes], "-o", str(exe)]).returncode != 0:
        raise SystemExit("link failed")
    return exe, stats


def run(exe, frames, work, coverage=False):
    artifacts = work / "artifacts"
    shutil.rmtree(artifacts, ignore_errors=True)
    artifacts.mkdir()
    command = ["timeout", "900", str(exe), "--frames", str(frames), "--artifacts", str(artifacts)]
    if coverage:
        (work / "coverage").mkdir(exist_ok=True)
        command += ["--execution-coverage", str(work / "coverage")]
    code, wall, cpu, _rss, _out = econ.run_timed(command)
    digest = (artifacts / "state.sha256").read_text().strip() if (artifacts / "state.sha256").exists() else ""
    return {"exit": code, "wall": wall, "cpu": cpu, "state_sha256": digest}


def trace_plan(coverage_file, plan_file):
    """(identity, PC) -> `identity lo hi` lines. Fixed image offset = PC; bank slot windows expose offset = PC - slot base."""
    offsets = {1: {0x0000, 0x0038, 0x0066}}
    count = 0
    for line in coverage_file.read_text().splitlines():
        identity, pc = int(line.split()[0], 16), int(line.split()[1], 16)
        count += 1
        if identity == 1:
            offsets.setdefault(1, set()).add(pc)
        else:
            base = max(b for b in SLOT_BASES if b <= pc)
            offsets.setdefault(identity, set()).add(pc - base)
    lines = []
    for identity, values in sorted(offsets.items()):
        run_lo = prev = None
        for value in sorted(values):
            if run_lo is None:
                run_lo = prev = value
            elif value == prev + 1:
                prev = value
            else:
                lines.append("%d %d %d" % (identity, run_lo, prev + 1))
                run_lo = prev = value
        if run_lo is not None:
            lines.append("%d %d %d" % (identity, run_lo, prev + 1))
    plan_file.write_text("\n".join(lines) + "\n")
    return count, sum(len(v) for v in offsets.values())


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--emit", required=True)
    parser.add_argument("--rom", required=True)
    parser.add_argument("--frames", type=int, default=120)
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--work", default="/tmp/z80equiv")
    args = parser.parse_args()
    work, rom = pathlib.Path(args.work), pathlib.Path(args.rom)
    covered_exe, broad_stats = compile_program(args, rom, work / "broad", None, ["-DSEGARECOMP_Z80_EXECUTION_COVERAGE"])
    traced = run(covered_exe, args.frames, work / "broad", coverage=True)
    starts, offsets = trace_plan(work / "broad" / "coverage" / "z80-coverage.txt", work / "plan.txt")
    plain_exe, _ = compile_program(args, rom, work / "broadplain", None, [])
    reference = run(plain_exe, args.frames, work / "broadplain")
    selective_exe, stats = compile_program(args, rom, work / "selective", work / "plan.txt", [])
    result = run(selective_exe, args.frames, work / "selective")
    report = {
        "frames": args.frames, "rom_bytes": rom.stat().st_size, "traced_starts": starts, "plan_offsets_with_roots": offsets,
        "broad_entries": broad_stats["entries"], "selective_entries": stats["entries"],
        "selective_owners": stats["owners"], "admitted_fraction_of_rom": offsets / rom.stat().st_size,
        "broad_exit": reference["exit"], "selective_exit": result["exit"],
        "state_digest_equal": bool(reference["state_sha256"]) and reference["state_sha256"] == result["state_sha256"],
        "coverage_build_digest_equal": traced["state_sha256"] == reference["state_sha256"],
        "run_wall_broad": reference["wall"], "run_wall_selective": result["wall"],
    }
    print(json.dumps(report, sort_keys=True))
    return 0 if report["state_digest_equal"] and report["broad_exit"] == report["selective_exit"] else 1


if __name__ == "__main__":
    sys.exit(main())
