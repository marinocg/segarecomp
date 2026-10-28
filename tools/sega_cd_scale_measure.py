#!/usr/bin/env python3
"""SEG-098-T004 (ADR 0213): measure the cost of an N-module inventory through the supported BIOS-image build (project-authored fixture, aggregate numbers only).

usage: sega_cd_scale_measure.py --segarecomp CLI --cc CC --root SOURCE_ROOT --work DIR N [N...]
Prints one JSON object per N: static status, extents, generated C bytes, compile units, build seconds, rounds, and a spot-check of the first/middle/last module.
"""
import argparse
import json
import pathlib
import subprocess
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--segarecomp", required=True)
    ap.add_argument("--cc", required=True)
    ap.add_argument("--root", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--cache", default=None, help="object cache shared between sizes (default: none, so compile cost is measured)")
    ap.add_argument("--jobs", default="4")
    ap.add_argument("--groups", default="4")
    ap.add_argument("--nops", default="0")
    ap.add_argument("sizes", nargs="+", type=int)
    args = ap.parse_args()
    root, work = pathlib.Path(args.root), pathlib.Path(args.work)
    for n in args.sizes:
        fx = work / ("fx%d" % n)
        out = work / ("out%d" % n)
        oracle = json.loads(subprocess.run([sys.executable, str(root / "tools" / "sega_cd_bios_unvisited_fixture.py"), "build", "--out", str(fx), "--many", str(n), "--many-groups", args.groups, "--many-nops", args.nops],
                                           check=True, capture_output=True, text=True).stdout)
        cmd = [args.segarecomp, "sega-cd", "build", "--cue", str(fx / "disc" / "disc.cue"), "--bios", str(fx / "bios.bin"), "--output", str(out), "--runtime-dir", str(root),
               "--cc", args.cc, "--budget", "6000000", "--jobs", args.jobs]
        if args.cache:
            cmd += ["--cache-dir", args.cache]
        t = time.time()
        done = subprocess.run(cmd, capture_output=True, text=True, timeout=3600)
        seconds = round(time.time() - t, 1)
        status = json.loads((out / "status.json").read_text()) if (out / "status.json").exists() else {}
        row = {"n": n, "exit": done.returncode, "status": status.get("status"), "message": status.get("message"), "build_seconds": seconds, "rounds": status.get("rounds"),
               "static": (status.get("static_discovery") or {}).get("status"), "static_extents": status.get("static_extents"), "resolved_extents": len(status.get("extents") or [])}
        src = out / "src"
        if src.exists():
            files = [p for p in src.iterdir() if p.suffix in (".c", ".h")]
            row["c_bytes"] = sum(p.stat().st_size for p in files)
            row["c_files"] = len(files)
        program = out / "program"
        if status.get("status") == "ok" and program.exists():
            ok = []
            for k in sorted({0, n // 2, n - 1}):
                run = subprocess.run([str(program), "--disc-dir", str(fx / "disc"), "--bios", str(fx / "bios.bin"), "--instruction-budget", "6000000", "--set", "main:d7=%d" % (k + 1)],
                                     capture_output=True, text=True, timeout=300)
                rep = json.loads(run.stdout)
                block = bytes.fromhex(rep.get("result_block", "")).ljust(16, b"\0")
                words = [int.from_bytes(block[2 * i:2 * i + 2], "big") for i in range(8)]
                ok.append(rep["result"] == "idle" and words[0] == 1 and words[3] == oracle["many"][k]["marker"])
            row["spot_checks_ok"] = all(ok)
        print(json.dumps(row, sort_keys=True), flush=True)


if __name__ == "__main__":
    main()
