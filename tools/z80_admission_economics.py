#!/usr/bin/env python3
"""SEG-057: Z80 selective-admission build economics (ADR 0118, frozen protocol). Measurement tool, not a product feature.

  z80_admission_economics.py gen-rom <KiB> <seed> <out.sms>
  z80_admission_economics.py measure --emit <sms_admission_emit> --rom <rom> --label <name> --config <cfg>... [--repeats N] [--jobs 8]
                                     [--cc gcc] [--out results.jsonl] [--keep] [--run-frames N]
  z80_admission_economics.py summarize <results.jsonl>

A configuration is `broad` (no plan: the merged immutable-image AOT) or `<shape>:<percent>` with shape `dist` (32-byte cells scattered by a
seeded hash), `clus` (4 KiB blocks scattered by the same hash) or `fine` (single bytes scattered), e.g. `dist:65`, `clus:35`,
`dist:100`. The architectural roots 0x0000, 0x0038 and 0x0066 are always admitted first (their cells), then cells/blocks/bytes in hash
order until `ceil(percent * units / 100)` units of the ROM are admitted, the integer rounding being part of the result.

Masks are DETERMINISTIC STRUCTURAL MASKS ONLY: they say nothing about whether a program executes only admitted starts. For a private image the
plan lives in the (ignored) work directory and only aggregate numbers are written out.

A "build" mirrors `segarecomp build --platform master-system --mapper sega -O2` for the headless link set: the same generated units, runtime
and headless sources and flags, compiled by a bounded pool (default 8 jobs, the product default), then linked. Every compiler process is
reaped with wait4, so user+system CPU and peak resident set size are per process.
"""
import argparse
import concurrent.futures
import hashlib
import json
import math
import os
import pathlib
import shutil
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
FIXED = 0x400
BANK = 0x4000
ROOTS = (0x0000, 0x0038, 0x0066)
RUNTIME = ROOT / "platforms" / "master-system" / "runtime"
HEADLESS = ROOT / "platforms" / "master-system" / "headless"
PSG = ROOT / "libs" / "device" / "sega" / "psg"
RUNTIME_SOURCES = ["sms_memory.c", "sms_sha256.c", "sms_input.c", "sms_machine.c", "sms_psg.c", "sms_pad.c", "sms_vdp.c", "sms_render.c"]
HEADLESS_SOURCES = ["sms_audio.c", "sms_headless.c", "sms_devices_vdp.c"]
GENERATED_FLAG = "-Wno-misleading-indentation"


def rng_bytes(size, seed):
    """Deterministic pseudo-random bytes (xorshift64*), the synthetic image content."""
    state = (seed * 0x9E3779B97F4A7C15 + 0x1234567) & (2**64 - 1) or 1
    out = bytearray()
    while len(out) < size:
        state ^= state >> 12
        state ^= (state << 25) & (2**64 - 1)
        state ^= state >> 27
        out += ((state * 0x2545F4914F6CDD1D) & (2**64 - 1)).to_bytes(8, "little")
    return bytes(out[:size])


def gen_rom(kib, seed):
    return rng_bytes(kib * 1024, seed)


def unit_hash(index, seed, tag):
    return hashlib.sha256(("%s:%d:%d" % (tag, seed, index)).encode()).digest()


def select_units(rom_size, shape, percent, seed):
    """Sorted list of admitted ROM-offset ranges [lo, hi) for a mask configuration. Returns (ranges, unit, wanted units, taken units)."""
    unit = {"dist": 32, "clus": 4096, "fine": 1}[shape]
    count = rom_size // unit
    wanted = min(count, math.ceil(percent * count / 100))
    chosen = []
    seen = set()
    for root in ROOTS:  # the architectural roots are admitted first, in every shape (the cell containing each)
        index = root // unit
        if index not in seen and len(chosen) < wanted:
            seen.add(index)
            chosen.append(index)
    for index in sorted(range(count), key=lambda i: unit_hash(i, seed, shape)):
        if len(chosen) >= wanted:
            break
        if index not in seen:
            seen.add(index)
            chosen.append(index)
    chosen.sort()
    ranges = []
    for index in chosen:
        lo, hi = index * unit, (index + 1) * unit
        if ranges and ranges[-1][1] == lo:
            ranges[-1][1] = hi
        else:
            ranges.append([lo, hi])
    return ranges, unit, wanted, len(chosen)


def plan_lines(ranges, rom_size):
    """ROM ranges -> `identity lo hi` lines: the fixed image holds ROM [0, 0x400); bank n (identity 2+n) holds ROM [n*0x4000, +0x4000)."""
    lines = []
    for lo, hi in ranges:
        if lo < FIXED:
            lines.append("1 %d %d" % (lo, min(hi, FIXED)))
        for bank in range(lo // BANK, (hi - 1) // BANK + 1):
            start, end = max(lo, bank * BANK), min(hi, (bank + 1) * BANK)
            lines.append("%d %d %d" % (2 + bank, start - bank * BANK, end - bank * BANK))
    return lines


def run_timed(command, cwd=None):
    """Runs one process, returns (returncode, wall, cpu seconds, peak RSS KiB, output)."""
    start = time.monotonic()
    proc = subprocess.Popen(command, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    output = proc.stdout.read()
    _pid, status, usage = os.wait4(proc.pid, 0)
    proc.returncode = os.waitstatus_to_exitcode(status)
    return proc.returncode, time.monotonic() - start, usage.ru_utime + usage.ru_stime, usage.ru_maxrss, output.decode(errors="replace")


def build(args, rom_path, config, work, repeat):
    shutil.rmtree(work, ignore_errors=True)
    shard = work / "generated"
    shard.mkdir(parents=True)
    emit = [args.emit, str(rom_path), str(shard), "sms"]
    plan_info = {}
    if config != "broad":
        shape, percent = config.split(":")
        rom_size = rom_path.stat().st_size
        ranges, unit, wanted, taken = select_units(rom_size, shape, int(percent), args.seed)
        plan = work / "plan.txt"
        plan.write_text("\n".join(plan_lines(ranges, rom_size)) + "\n")
        emit += ["--plan", str(plan)]
        plan_info = {"unit_bytes": unit, "units_wanted": wanted, "units_taken": taken,
                     "rom_bytes_admitted": sum(h - l for l, h in ranges), "rom_bytes": rom_size,
                     "rom_fraction": sum(h - l for l, h in ranges) / rom_size}
    code, emit_wall, emit_cpu, emit_rss, output = run_timed(emit)
    if code != 0:
        return {"error": "emit failed: " + output[-400:]}
    emit_stats = json.loads(output.strip().splitlines()[-1])
    units = [shard / line for line in (shard / "sms.units").read_text().split()]
    generated_bytes = sum(p.stat().st_size for p in shard.iterdir() if p.suffix in (".c", ".h") and p.name != "sms_rom.c")
    base = [args.cc, "-std=c11", "-Wall", "-Wextra", "-pedantic", "-O" + args.opt, "-D_CRT_SECURE_NO_WARNINGS",
            "-I", str(ROOT / "libs" / "codegen" / "c11" / "include"), "-I", str(RUNTIME), "-I", str(PSG / "include"),
            "-I", str(shard), "-I", str(HEADLESS)]
    jobs = [(u, [GENERATED_FLAG]) for u in units]
    jobs += [(RUNTIME / n, []) for n in RUNTIME_SOURCES] + [(PSG / "src" / "sn76489.c", [])] + [(HEADLESS / n, []) for n in HEADLESS_SOURCES]
    objects = work / "obj"
    objects.mkdir()
    commands = [(base + extra + ["-c", "-o", str(objects / ("%d.o" % i)), str(src)], i, src in units) for i, (src, extra) in enumerate(jobs)]
    start = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(lambda c: run_timed(c[0]), commands))
    compile_wall = time.monotonic() - start
    for (code, *_rest, text), _ in zip(results, commands):
        if code != 0:
            return {"error": "compile failed: " + text[-400:]}
    generated = [r for r, c in zip(results, commands) if c[2]]
    exe = work / "program"
    code, link_wall, link_cpu, _rss, text = run_timed([args.cc, *[str(objects / ("%d.o" % i)) for i in range(len(jobs))], "-o", str(exe)])
    if code != 0:
        return {"error": "link failed: " + text[-400:]}
    record = {
        "label": args.label, "config": config, "repeat": repeat, "cc": args.cc, "opt": args.opt, "jobs": args.jobs,
        "rom_bytes": rom_path.stat().st_size, **{"plan_" + k: v for k, v in plan_info.items()},
        "emit": emit_stats, "emit_wall": emit_wall, "emit_cpu": emit_cpu,
        "compile_wall": compile_wall, "link_wall": link_wall, "build_wall": emit_wall + compile_wall + link_wall,
        "compile_cpu": sum(r[2] for r in results), "generated_cpu": sum(r[2] for r in generated),
        "build_cpu": emit_cpu + sum(r[2] for r in results) + link_cpu,
        "peak_rss_kib": max(r[3] for r in results), "generated_peak_rss_kib": max(r[3] for r in generated),
        "generated_c_bytes": generated_bytes, "exe_bytes": exe.stat().st_size, "units": len(units),
    }
    if args.run_frames:
        record["run"] = run_program(exe, args.run_frames, work)
    return record


def run_program(exe, frames, work):
    artifacts = work / "artifacts"
    artifacts.mkdir(exist_ok=True)
    code, wall, cpu, _rss, text = run_timed(["timeout", "600", str(exe), "--frames", str(frames), "--artifacts", str(artifacts)])
    digest = (artifacts / "state.sha256").read_text().strip() if (artifacts / "state.sha256").exists() else ""
    status = {}
    if (artifacts / "status.json").exists():
        try:
            status = json.loads((artifacts / "status.json").read_text())
        except json.JSONDecodeError:
            pass
    return {"exit": code, "wall": wall, "cpu": cpu, "state_sha256": digest, "stop": status.get("stop") or status.get("stop_class")}


def summarize(path):
    rows = [json.loads(line) for line in pathlib.Path(path).read_text().splitlines() if line.strip()]
    groups = {}
    for r in rows:
        if "error" in r:
            print("ERROR", r)
            continue
        groups.setdefault((r["label"], r["rom_bytes"], r["config"]), []).append(r)
    print("%-14s %7s %-9s %5s %9s %9s %8s %8s %8s %9s %9s %7s %6s" % (
        "image", "KiB", "config", "n", "adm.frac", "entries", "owners", "bodies", "genC MB", "wall s", "cpu s", "rssMiB", "exeMB"))
    broad = {}
    for key in sorted(groups):
        label, size, config = key
        g = groups[key]
        med = lambda f: statistics.median(f(r) for r in g)  # noqa: E731
        wall, cpu = med(lambda r: r["build_wall"]), med(lambda r: r["build_cpu"])
        if config == "broad":
            broad[(label, size)] = (wall, cpu)
        spread = (max(r["build_wall"] for r in g) - min(r["build_wall"] for r in g)) / wall * 100
        fraction = g[0].get("plan_rom_fraction", 1.0)
        print("%-14s %7d %-9s %5d %9.4f %9d %8d %8d %8.1f %9.1f %9.1f %7.0f %6.2f  wall spread %.1f%%" % (
            label, size // 1024, config, len(g), fraction, g[0]["emit"]["entries"], g[0]["emit"]["owners"], g[0]["emit"]["shared_bodies"],
            med(lambda r: r["generated_c_bytes"]) / 1e6, wall, cpu, med(lambda r: r["peak_rss_kib"]) / 1024,
            med(lambda r: r["exe_bytes"]) / 1e6, spread))
    print()
    for key in sorted(groups):
        label, size, config = key
        if config == "broad" or (label, size) not in broad:
            continue
        g = groups[key]
        wall = statistics.median(r["build_wall"] for r in g)
        cpu = statistics.median(r["build_cpu"] for r in g)
        print("%-14s %7d %-9s wall %+6.1f%%  cpu %+6.1f%% vs broad" % (
            label, size // 1024, config, (wall / broad[(label, size)][0] - 1) * 100, (cpu / broad[(label, size)][1] - 1) * 100))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    g = sub.add_parser("gen-rom")
    g.add_argument("kib", type=int)
    g.add_argument("seed", type=int)
    g.add_argument("out")
    m = sub.add_parser("measure")
    m.add_argument("--emit", required=True)
    m.add_argument("--rom", required=True)
    m.add_argument("--label", required=True)
    m.add_argument("--config", action="append", required=True)
    m.add_argument("--repeats", type=int, default=3)
    m.add_argument("--jobs", type=int, default=8)
    m.add_argument("--cc", default="gcc")
    m.add_argument("--opt", default="2")
    m.add_argument("--seed", type=int, default=1)
    m.add_argument("--out", default="results.jsonl")
    m.add_argument("--workdir")
    m.add_argument("--keep", action="store_true")
    m.add_argument("--run-frames", type=int, default=0)
    s = sub.add_parser("summarize")
    s.add_argument("results")
    args = parser.parse_args()
    if args.command == "gen-rom":
        pathlib.Path(args.out).write_bytes(gen_rom(args.kib, args.seed))
        return 0
    if args.command == "summarize":
        summarize(args.results)
        return 0
    root = pathlib.Path(args.workdir or tempfile.mkdtemp(prefix="z80econ-"))
    for repeat in range(args.repeats):
        for config in args.config:  # interleave configurations across repeats so slow host drift hits all of them
            record = build(args, pathlib.Path(args.rom), config, root / ("%s-%s-%d" % (args.label, config.replace(":", "_"), repeat)), repeat)
            with open(args.out, "a", encoding="utf-8") as out:
                out.write(json.dumps(record, sort_keys=True) + "\n")
            print(json.dumps({k: record.get(k) for k in ("label", "config", "repeat", "build_wall", "build_cpu", "error")}), flush=True)
            if not args.keep:
                shutil.rmtree(root / ("%s-%s-%d" % (args.label, config.replace(":", "_"), repeat)), ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
