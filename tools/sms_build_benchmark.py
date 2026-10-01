#!/usr/bin/env python3
"""SEG-033-T001: measures the production `segarecomp build` route (Master System target) for one image and configuration.

usage: sms_build_benchmark.py --cli <segarecomp> --root <product-root> --image <shape> [--optimize 2] [--jobs 4]
                              [--cc cc] [--work DIR] [--json OUT] [--keep] [--label TEXT] [--run-cycles N]

Shapes: `random:<KiB>`, `dense:<KiB>` (back-to-back legal instructions, ADR 0058 reference shape) and `rom:<path>` (an
authorized local image: bytes, names and paths are never recorded). Everything runs through the real consumer route
(`segarecomp build`: analyze, generate, compile, link) with its real flags, so the figures are what a user pays.

Aggregates only: generation/compile/link wall time, compiler CPU, per-process and aggregate peak compiler RSS (sampled
process tree), generated C bytes/files, host functions, exact entries, unique owners, TU size distribution, executable size,
startup and generated-native throughput (fixed T-state budget). The same driver is used by T001, T004, T006 and T009.
"""
import argparse
import json
import os
import pathlib
import random
import re
import resource
import shutil
import statistics
import subprocess
import sys
import tempfile
import threading
import time

OWNER_DEF = re.compile(r"^(?:static )?struct Z80OwnerRef (z80_\w+)\(struct Z80Runtime \*rt, uint16_t window_base\) \{$", re.M)
BODY_DEF = re.compile(r"^void (z80_fb_\d+)\(struct Z80Runtime \*rt\) \{$", re.M)
KEY_LINE = re.compile(r"^  UINT32_C\(0x[0-9A-Fa-f]{8}\),$", re.M)
KEY_ARRAY = re.compile(r"static const uint32_t z80_entry_keys(?!_chunk_first)\w*\[\] = \{\n(.*?\n)\};", re.S)


def make_image(shape, root):
    sys.path.insert(0, str(root / "tests"))
    sys.path.insert(0, str(root / "tools"))
    import sms_fixture_rom as builder  # noqa: E402

    if shape.startswith("rom:"):
        return pathlib.Path(shape[4:]).read_bytes(), "rom"
    kind, kib = shape.split(":")
    size = int(kib) * 1024
    rng = random.Random(33000 + size)
    if kind == "random":
        rom = bytearray(rng.randrange(256) for _ in range(size))
    elif kind == "dense":
        import z80_static_budget as budget  # noqa: E402

        rom = bytearray(budget.dense_bytes(rng, size, budget.legal_encoding_pool()))
    else:
        raise SystemExit("unknown shape " + shape)
    builder.write_header(rom, size)
    return bytes(rom), kind


def process_table():
    out = subprocess.run(["ps", "-axo", "pid=,ppid=,rss=,comm="], capture_output=True, text=True).stdout
    table = {}
    for line in out.splitlines():
        parts = line.split(None, 3)
        if len(parts) == 4 and parts[0].isdigit():
            table[int(parts[0])] = (int(parts[1]), int(parts[2]), parts[3])
    return table


class Sampler(threading.Thread):
    """Samples the descendants of `root_pid`: per-process and aggregate RSS, per build stage."""

    def __init__(self, root_pid):
        super().__init__(daemon=True)
        self.root, self.stage, self.stop, self.peaks = root_pid, "generate", False, {}

    def run(self):
        while not self.stop:
            table = process_table()
            tree = {self.root}
            grew = True
            while grew:
                grew = False
                for pid, (ppid, _, _) in table.items():
                    if ppid in tree and pid not in tree:
                        tree.add(pid)
                        grew = True
            rss = [table[p][1] for p in tree if p in table and p != self.root]
            peak = self.peaks.setdefault(self.stage, {"proc_kib": 0, "aggregate_kib": 0})
            if rss:
                peak["proc_kib"] = max(peak["proc_kib"], max(rss))
                peak["aggregate_kib"] = max(peak["aggregate_kib"], sum(rss))
            time.sleep(0.2)


def generated_stats(directory):
    files = sorted(p for p in directory.iterdir() if p.is_file())
    c_files = [p for p in files if p.suffix == ".c"]
    owners, entries, defs, bodies = set(), 0, 0, 0
    sizes = []
    for p in c_files:
        text = p.read_text(errors="replace")
        sizes.append(p.stat().st_size)
        for m in OWNER_DEF.finditer(text):
            defs += 1
            owners.add(m.group(1))
        bodies += len(BODY_DEF.findall(text))
        for m in KEY_ARRAY.finditer(text):
            entries += len(KEY_LINE.findall(m.group(1)))
    sizes.sort()
    return {"generated_c_bytes": sum(p.stat().st_size for p in files), "files": len(files), "c_files": len(c_files),
            "host_functions": defs + bodies, "owner_functions": defs, "shared_body_functions": bodies, "unique_owners": len(owners), "exact_entries": entries,
            "entries_per_owner": round(entries / max(1, len(owners)), 3),
            "tu_bytes": {"min": sizes[0], "median": int(statistics.median(sizes)), "max": sizes[-1]}}


def run_game(exe, args, timeout=900):
    start = time.monotonic()
    proc = subprocess.run([str(exe), *args], capture_output=True, text=True, timeout=timeout)
    return time.monotonic() - start, proc.returncode


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--cli", required=True)
    ap.add_argument("--root", required=True)
    ap.add_argument("--image", required=True)
    ap.add_argument("--optimize", default="", help="empty: the build route's default policy")
    ap.add_argument("--jobs", type=int, default=0)
    ap.add_argument("--cc", default="cc")
    ap.add_argument("--mapper", default="sega")
    ap.add_argument("--work")
    ap.add_argument("--json")
    ap.add_argument("--label", default="")
    ap.add_argument("--run-cycles", type=int, default=20000000)
    ap.add_argument("--platform", default="master-system", choices=("master-system", "genesis"),
                    help="genesis: only the build is measured (policy checks, SEG-033-T004); the image shape is `rom:<path>`")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--run-repeats", type=int, default=3, help="throughput runs; the median is reported")
    ap.add_argument("--exe-out", help="copy the linked executable here (for a separate runtime comparison)")
    ap.add_argument("--extra", action="append", default=[], help="extra `segarecomp build` option pair, e.g. --extra=--cc-arg=-O1")
    args = ap.parse_args()
    root = pathlib.Path(args.root).resolve()
    rom, kind = make_image(args.image, root)
    cleanup = None
    if args.work:
        work = pathlib.Path(args.work)
        shutil.rmtree(work, ignore_errors=True)
        work.mkdir(parents=True)
    else:
        cleanup = tempfile.TemporaryDirectory()
        work = pathlib.Path(cleanup.name)
    (work / "image.sms").write_bytes(rom)  # the file name is irrelevant to the route (classified by content)
    out = work / "out"
    cmd = [args.cli, "build", "--rom", str(work / "image.sms"), "--output", str(out), "--cc", args.cc,
           "--runtime-dir", str(root / "platforms" / args.platform)]
    if args.platform == "master-system":
        cmd += ["--mapper", args.mapper]
    if args.optimize:
        cmd += ["--optimize", args.optimize]
    if args.jobs:
        cmd += ["--jobs", str(args.jobs)]
    for pair in args.extra:
        cmd += pair.split("=", 1)
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    t0 = time.monotonic()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    sampler = Sampler(proc.pid)
    sampler.start()
    stamps = {}
    for line in proc.stdout:
        if line.startswith("@stage "):
            _, name, state = line.split()
            stamps[(name, state)] = time.monotonic() - t0
            sampler.stage = name
        elif line.startswith("@result"):
            result_line = line.strip()
    code = proc.wait()
    sampler.stop = True
    sampler.join()
    total = time.monotonic() - t0
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    cpu = (after.ru_utime + after.ru_stime) - (before.ru_utime + before.ru_stime)
    if code != 0:
        print("build failed", code, (out / "build.log").read_text()[-600:] if (out / "build.log").exists() else "")
        return 1
    span = lambda a, b: round(stamps[(b, "done")] - stamps[(a, "begin")], 2) if (a, "begin") in stamps else None  # noqa: E731
    exe = out / "game"
    gen = generated_stats(out / "generated")
    if args.platform == "genesis":  # build-only figures
        result = {"label": args.label, "platform": "genesis", "optimize": args.optimize or "default", "jobs": args.jobs or "default",
                  "total_wall_s": round(total, 2), "generate_wall_s": span("analyze", "generate"), "compile_wall_s": span("compile", "compile"),
                  "link_wall_s": span("link", "link"), "build_cpu_s": round(cpu, 1),
                  "rss_mib": {stage: {k.replace("_kib", "_mib"): round(v / 1024) for k, v in peak.items()} for stage, peak in sampler.peaks.items()},
                  "executable_mib": round(exe.stat().st_size / 1048576, 2), "generated_c_bytes": gen["generated_c_bytes"], "c_files": gen["c_files"]}
        text = json.dumps(result, indent=1, sort_keys=True)
        if args.json:
            pathlib.Path(args.json).write_text(text + "\n")
        print(text)
        return 0
    startup, _ = run_game(exe, ["--cycle-budget", "1000"])
    # Throughput: fixed T-state budget (real images run it; a synthetic image may stop earlier on a typed outcome).
    art = work / "art"
    art.mkdir()
    times = []
    for _ in range(max(1, args.run_repeats)):
        run_s, run_rc = run_game(exe, ["--cycle-budget", str(args.run_cycles), "--artifacts", str(art)])
        times.append(run_s)
    run_s = statistics.median(times)
    if args.exe_out:
        shutil.copy2(exe, args.exe_out)
    digest = (art / "state.sha256").read_text().strip() if (art / "state.sha256").exists() else None
    status = json.loads((art / "status.json").read_text()) if (art / "status.json").exists() else {}
    result = {
        "label": args.label, "image": "rom" if args.image.startswith("rom:") else args.image, "rom_kib": len(rom) // 1024,
        "optimize": args.optimize or "default", "jobs": args.jobs or "default", "cc": args.cc, "host_cpus": os.cpu_count(),
        "total_wall_s": round(total, 2), "generate_wall_s": span("analyze", "generate"), "compile_wall_s": span("compile", "compile"),
        "link_wall_s": span("link", "link"), "build_cpu_s": round(cpu, 1),
        "rss_mib": {stage: {k.replace("_kib", "_mib"): round(v / 1024) for k, v in peak.items()} for stage, peak in sampler.peaks.items()},
        "executable_mib": round(exe.stat().st_size / 1048576, 2), "startup_s": round(startup, 3),
        "run": {"cycle_budget": args.run_cycles, "wall_s": round(run_s, 3), "exit": run_rc, "wall_s_runs": [round(x, 3) for x in times],
                "mcycles_per_s": round((status.get("cycles") or args.run_cycles) / run_s / 1e6, 2) if run_s else None,
                "state_sha256": digest},
        **gen,
    }
    text = json.dumps(result, indent=1, sort_keys=True)
    if args.json:
        pathlib.Path(args.json).write_text(text + "\n")
    print(text)
    if args.keep and cleanup:
        keep = pathlib.Path(tempfile.mkdtemp(prefix="sms-bench-"))
        shutil.copytree(work, keep / "w")
        print("kept", keep)
    return 0


if __name__ == "__main__":
    sys.exit(main())
