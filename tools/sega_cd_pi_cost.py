#!/usr/bin/env python3
"""SEG-097-T002 (ADR 0207): measured cost of the per-mapping emitter against the source-relative (position-independent) emitter. Stdlib only.

Builds the project-authored `unvisited` fixture disc (case `pi`), maps module C at k distinct bases (k = 1, 2, 4, 8), and for each emitter reports:
generated bodies and C bytes, emission CPU time, compile CPU time and peak memory of the program translation unit, object and executable
size, the run time of the same route, and the incremental cost of one more mapping (delta of generated C). Nothing here is a threshold: the
numbers are evidence; the caller decides what to assert.

usage: sega_cd_pi_cost.py <segarecomp> <c-compiler> <source-root> [--ks 1,2,4,8] [--repeats 3]   ->   JSON on stdout
"""
import argparse
import json
import pathlib
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("segarecomp")
    parser.add_argument("cc")
    parser.add_argument("root")
    parser.add_argument("--ks", default="1,2,4,8")
    parser.add_argument("--repeats", type=int, default=3)
    args = parser.parse_args()
    root = pathlib.Path(args.root).resolve()
    sys.path.insert(0, str(root / "tests"))
    import sega_cd_common as common
    measured = common.measured
    ks = [int(k) for k in args.ks.split(",")]
    result = {"schema": 1, "fixture": "unvisited/pi", "rows": []}
    with tempfile.TemporaryDirectory(prefix="scd-pi-cost-") as tmp:
        tmp = pathlib.Path(tmp)
        builder = common.Builder(args.segarecomp, args.cc, tmp)
        builder.runtime_objects()
        disc = tmp / "disc"
        expected = common.build_disc(disc, "unvisited", "cooked", "--unvisited-case", "pi")
        found = json.loads(subprocess.run([args.segarecomp, "sega-cd", "discover-modules", "--cue", str(disc / "disc.cue"), "--out", str(tmp / "base.txt")],
                                          capture_output=True, text=True).stdout)
        base_text = (tmp / "base.txt").read_text()
        previous = {}
        for k in ks:
            # module C (a sector discovered at $090000 and $0A0000) gets k bases: the two it runs at, then disjoint PRG RAM bases it never runs at.
            c_block = expected["lba"]["C"]
            extra = [0x040000 + 0x400 * i for i in range(max(0, k - 2))]
            bases = ([0x090000, 0x0A0000] + extra)[:k] if k >= 2 else [0x090000]
            text = re.sub(r"(module lba%d\n(?:.*\n)*?)(map 0x[0-9a-f]+\n)+" % c_block,
                          lambda m: m.group(1) + "".join("map 0x%x\n" % b for b in bases), base_text, count=1)
            (tmp / ("k%d.txt" % k)).write_text(text, newline="\n")
            row = {"mappings": k}
            for mode, flags in (("per_mapping", []), ("source_relative", ["--position-independent"])):
                c_path = tmp / ("k%d-%s.c" % (k, mode))
                rc, out, cpu, rss, wall, _ = measured([args.segarecomp, "sega-cd", "emit", "--cue", disc / "disc.cue", "--out", c_path, "--modules",
                                                       tmp / ("k%d.txt" % k), *flags])
                report = json.loads(out)
                sub = report["processors"][1]
                text_c = c_path.read_text()
                obj = tmp / ("k%d-%s.o" % (k, mode))
                crc, _, ccpu, crss, cwall, err = measured([args.cc, *common.CFLAGS, "-I", common.SCD_RUNTIME, "-I", common.GENESIS_RUNTIME, "-c", c_path,
                                                           "-o", obj])
                assert crc == 0, err
                exe = tmp / ("k%d-%s%s" % (k, mode, common.EXE))
                subprocess.run([args.cc, str(obj), *[str(o) for o in builder.runtime_objects()], "-o", str(exe)], check=True)
                runs = []
                for _ in range(args.repeats):
                    t = time.time()
                    rcode, rep = common.run(exe, disc, 3_000_000, "--set", "main:d7=%d" % (3 if k >= 2 else 1))  # route C needs both of its bases
                    runs.append(time.time() - t)
                assert rcode == 0, rep
                entry = {"bodies": sub["bodies"], "shared_bodies": sub["shared_bodies"], "shared_instances": sub["shared_instances"],
                         "fallback_instances": sub["fallback_instances"], "functions": len(re.findall(r"^static int ", text_c, re.M)),
                         "c_bytes": len(text_c.encode()), "translation_units": 1, "emit_cpu_s": round(cpu, 3), "emit_peak_kib": rss,
                         "compile_cpu_s": round(ccpu, 3), "compile_peak_kib": crss, "object_bytes": obj.stat().st_size,
                         "executable_bytes": exe.stat().st_size, "run_wall_s_median": round(statistics.median(runs), 4),
                         "retired": rep["cpus"][1]["retired"], "trace": rep["cpus"][1]["trace"]}
                row[mode] = entry
                if mode in previous:
                    entry["incremental_c_bytes_vs_previous_k"] = entry["c_bytes"] - previous[mode]["c_bytes"]
                previous[mode] = entry
            result["rows"].append(row)
    json.dump(result, sys.stdout, indent=1, sort_keys=True)
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
