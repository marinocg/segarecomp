#!/usr/bin/env python3
"""SEG-008-T009: re-measures the ADR 0058 static-code budgets with the real Z80 lowerings on full-size synthetic images.

usage: z80_static_budget.py --emitter <z80_image_emitter> --shape <shape> [--cc cc] [--opt -O1] [--work DIR] [--json OUT]
       [--corpus-cache DIR]

Shapes: `dense64` (one invariant 64 KiB image of documented legal instructions with operands; sampled from the ignored
SST cache when present, else seeded uniform legal opcode bytes), `random64`, `zero64`, `ff64` (the ADR 0058 inputs) and
`banked512-dense` / `banked512-random` (SMS-shaped: a 1 KiB invariant image plus 32 banked 16 KiB banks each exposed in
three 16 KiB windows, window-relative owners). Measures generated C size, -jN/-j1 compile time (per-process peak RSS from
wait4), executable size (linked with the dispatch benchmark) and the dispatcher round-trip time (an upper bound of the
exact lookup), and reports pass/fail against the pre-declared budgets. Output is byte-identical across two emits (checked).
Nothing measured here is committed except the aggregate figures the caller records.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import pathlib
import random
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import z80_conformance as zc  # noqa: E402
import z80_sst_corpus as sst  # noqa: E402

BUDGETS_64K = {"generated_c_mib": 128.0, "j8_s": 90.0, "j1_s": 300.0, "rss_mib": 1536.0, "exe_mib": 48.0, "lookup_ns": 100.0}
BUDGETS_512K = {"generated_c_mib": 1024.0, "j8_s": 300.0, "j1_s": 1800.0, "rss_mib": 1536.0, "exe_mib": 256.0, "lookup_ns": 100.0}
BENCH = zc.TOOLS_DIR / "z80_dispatch_bench.c"


def dense_bytes(rng, size, cache):
    """Legal instruction encodings with operands, from the ignored corpus cache when available."""
    codes = []
    if cache and cache.is_dir():
        for path in sorted(cache.glob("*.json")):
            for case in json.loads(path.read_text())[:6]:
                ram = {a: v for a, v in case["initial"]["ram"]}
                pc, code = case["initial"]["pc"], bytearray()
                while len(code) < 4 and ((pc + len(code)) & 0xFFFF) in ram:
                    code.append(ram[(pc + len(code)) & 0xFFFF])
                if code:
                    codes.append(bytes(code[:instruction_length(code)]))
    out = bytearray()
    while len(out) < size:
        out += rng.choice(codes) if codes else bytes([rng.randrange(256)])
    return bytes(out[:size])


def instruction_length(code):
    first = code[0]
    if first in (0xDD, 0xFD):
        if len(code) > 1 and code[1] == 0xCB:
            return min(4, len(code))
        return min(len(code), 2 + (1 if len(code) > 2 else 0))
    if first == 0xED:
        return min(len(code), 2)
    return min(len(code), 1)


def rom_spec(path):
    """SMS-shaped banked spec from a local ROM image (ephemeral scale check; the path and bytes are never recorded)."""
    data = pathlib.Path(path).read_bytes()
    data += bytes(-len(data) % 0x4000)
    banks = len(data) // 0x4000
    lines = ["image 1 invariant\nwindow 1 0 0 400\nbytes 1 %s\n" % data[:0x400].hex()]
    for bank in range(banks):
        ident = 2 + bank
        lines.append("image %d banked\n" % ident)
        for base in ("4000", "8000", "C000"):
            lines.append("window %d %s 0 4000\n" % (ident, base))
        lines.append("bytes %d %s\n" % (ident, data[bank * 0x4000:(bank + 1) * 0x4000].hex()))
    return "".join(lines), 1, BUDGETS_512K


def shape_spec(shape, cache):
    if shape.startswith("rom:"):
        return rom_spec(shape[4:])
    rng = random.Random(88000)
    kind = shape.split("-")[-1] if "-" in shape else shape[:-2]
    if shape.endswith("64"):
        size = 0x10000
        data = {"dense": lambda: dense_bytes(rng, size, cache), "random": lambda: bytes(rng.randrange(256) for _ in range(size)),
                "zero": lambda: bytes(size), "ff": lambda: b"\xff" * size}[kind]()
        return "image 1 invariant\nwindow 1 0 0 10000\nbytes 1 %s\n" % data.hex(), 0, BUDGETS_64K
    lines = ["image 1 invariant\nwindow 1 0 0 400\nbytes 1 %s\n" % (
        dense_bytes(rng, 0x400, cache) if kind == "dense" else bytes(rng.randrange(256) for _ in range(0x400))).hex()]
    for bank in range(32):
        data = dense_bytes(rng, 0x4000, cache) if kind == "dense" else bytes(rng.randrange(256) for _ in range(0x4000))
        ident = 2 + bank
        lines.append("image %d banked\n" % ident)
        for base in ("4000", "8000", "C000"):
            lines.append("window %d %s 0 4000\n" % (ident, base))
        lines.append("bytes %d %s\n" % (ident, data.hex()))
    return "".join(lines), 1, BUDGETS_512K


def child_rss_mib(cmd):
    """Peak RSS of one child via wait4 (bytes on macOS, KiB on Linux)."""
    start = time.monotonic()
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    err = proc.stderr.read()
    _, status, usage = os.wait4(proc.pid, 0)
    scale = 1024 * 1024 if sys.platform == "darwin" else 1024
    return (os.waitstatus_to_exitcode(status), time.monotonic() - start, usage.ru_maxrss / scale, err.decode("utf-8", "replace"))


def compile_all(tc, workdir, stem, sources, workers):
    common = [tc.cc, *zc.STRICT, tc.opt, "-I", str(tc.include_dir), "-I", str(zc.INCLUDE_DIR), "-I", str(workdir), "-I", str(zc.TOOLS_DIR)]
    objs = [workdir / (s.stem + ".o") for s in sources]

    def one(pair):
        return child_rss_mib(common + ["-c", str(pair[0]), "-o", str(pair[1])])

    start = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
        results = list(pool.map(one, zip(sources, objs)))
    wall = time.monotonic() - start
    for code, _, _, err in results:
        if code != 0:
            raise AssertionError("compile failed: " + err[:400])
    return wall, max(r[2] for r in results), sum(r[1] for r in results), objs


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--emitter", required=True)
    parser.add_argument("--shape", required=True)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--opt", default="-O1")
    parser.add_argument("--work")
    parser.add_argument("--json")
    parser.add_argument("--corpus-cache")
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--skip-j1", action="store_true")
    args = parser.parse_args()
    cache = pathlib.Path(args.corpus_cache) if args.corpus_cache else sst.default_cache()
    tc = zc.Toolchain(args.cc, args.emitter, None, opt=args.opt)
    spec, mode, budgets = shape_spec(args.shape, cache)
    with tempfile.TemporaryDirectory() as tmp:
        work = pathlib.Path(args.work) if args.work else pathlib.Path(tmp)
        work.mkdir(parents=True, exist_ok=True)
        t0 = time.monotonic()
        stats, _, error = zc.emit_image(tc, spec, work, "budget")
        if error:
            print("emit failed:", error)
            return 1
        emit_s = time.monotonic() - t0
        again = work / "again"
        zc.emit_image(tc, spec, again, "budget")
        units = (work / "budget.units").read_text().split()
        identical = all((work / u).read_bytes() == (again / u).read_bytes() for u in units)
        c_bytes = sum((work / u).stat().st_size for u in units) + sum(p.stat().st_size for p in work.glob("*.h"))
        sources = [work / u for u in units] + [BENCH]
        j8, rss8, _, objs = compile_all(tc, work, "budget", sources, args.jobs)
        exe = work / "budget.exe"
        linked = subprocess.run([tc.cc, *[str(o) for o in objs], "-o", str(exe)], capture_output=True, text=True)
        if linked.returncode != 0:
            print("link failed", linked.stderr[:400])
            return 1
        j1 = rss1 = None
        if not args.skip_j1:
            j1, rss1, _, _ = compile_all(tc, work, "budget", sources, 1)
        roundtrip = subprocess.run([str(exe), str(mode), "2000000", "7"], capture_output=True, text=True, timeout=600)
        trip = dict(p.split("=") for p in roundtrip.stdout.split()[1:]) if roundtrip.returncode == 0 else {}
        # exact lookup alone: the bench includes the generated main TU so the static z80_entry_lookup is callable
        lookup_exe = work / "budget_lookup.exe"
        owner_objs = [str(o) for o, src in zip(objs, sources) if not src.name.endswith("_main.c") and src != BENCH]
        code, _, rss_l, err = child_rss_mib([tc.cc, "-std=c11", tc.opt, "-DZ80_BENCH_MAIN_TU=\"%s_main.c\"" % "budget", "-I", str(work),
                                             "-I", str(zc.INCLUDE_DIR), "-I", str(zc.TOOLS_DIR), str(BENCH), *owner_objs, "-o", str(lookup_exe)])
        if code != 0:
            print("lookup bench build failed", err[:400])
            return 1
        lookup = subprocess.run([str(lookup_exe), str(mode), "2000000", "7"], capture_output=True, text=True, timeout=600)
        parts = dict(p.split("=") for p in lookup.stdout.split()[1:]) if lookup.returncode == 0 else {}
        rss = max(rss8, rss1 or 0)  # the production build; the lookup bench re-includes the main TU and is reported apart
        result = {"shape": "rom" if args.shape.startswith("rom:") else args.shape, "opt": args.opt, "starts": {k: v for k, v in stats.items()}, "emit_s": round(emit_s, 1),
                  "generated_c_mib": round(c_bytes / 1048576, 1), "exe_mib": round(exe.stat().st_size / 1048576, 1),
                  "j8_s": round(j8, 1), "j1_s": None if j1 is None else round(j1, 1), "rss_mib": round(rss), "lookup_bench_rss_mib": round(rss_l),
                  "lookup_ns": float(parts.get("random_ns", "nan")), "lookup_seq_ns": float(parts.get("sequential_ns", "nan")),
                  "dispatch_round_trip_ns": float(trip.get("random_ns", "nan")),
                  "dispatch_round_trip_seq_ns": float(trip.get("sequential_ns", "nan")),
                  "identical": identical, "translation_units": len(units)}
        checks = {k: (result[k] is None or result[k] <= v) for k, v in budgets.items()}
        result["budgets"] = {k: {"limit": budgets[k], "pass": checks[k]} for k in budgets}
        result["all_pass"] = all(checks.values()) and identical
        text = json.dumps(result, indent=1, sort_keys=True)
        if args.json:
            pathlib.Path(args.json).write_text(text + "\n")
        print(text)
        return 0 if result["all_pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
