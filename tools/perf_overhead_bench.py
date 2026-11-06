#!/usr/bin/env python3
"""Measures what the SEG-125 profilers cost (ADR 0273), on a project-authored synthetic Genesis spin image. Nothing is tuned: the same program is run interleaved
(off, summary, detailed, off, ...) so host drift hits every variant alike, and the median and spread of each variant are reported.

  perf_overhead_bench.py --cli <segarecomp> --cc <compiler> --root <product root> [--reps N] [--budget N]
                         [--sdl3-include DIR --sdl3-lib DIR]   (adds the SDL3 viewer variants under the dummy video driver)
                         [--build-reps N] [-o result.json]

Variants: build (profiling off / --performance-report), headless runtime (off / summary / detailed), viewer unthrottled (off / report summary / report detailed /
overlay player / overlay developer). The workload is a spinning guest, so the numbers are the collector's cost relative to the emulation of a trivial program: a
conservative (worst-case) ratio, since real games spend more time per frame in the same emulation.
"""
import argparse
import json
import os
import pathlib
import statistics
import subprocess
import sys
import tempfile
import time


def spin_rom(root):
    sys.path.insert(0, str(root / "tools"))
    import genesis_z80_fixture_rom as fx
    spin = fx.M68k()
    spin.w(0x7000)
    spin.branch(0x67, "real")
    spin.w(0x4E70)
    spin.label("real")
    spin.label("spin")
    spin.branch(0x60, "spin")
    return fx.build_rom(spin.resolve())


def timed(cmd, env=None, timeout=600):
    t0 = time.perf_counter()
    r = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=timeout)
    return time.perf_counter() - t0, r


def stats(values):
    values = sorted(values)
    return {"n": len(values), "median_s": statistics.median(values), "min_s": values[0], "max_s": values[-1],
            "spread_pct": (values[-1] - values[0]) / statistics.median(values) * 100 if len(values) > 1 else 0.0}


def interleaved(variants, reps, runner):
    """variants: {name: arg}; runs every variant once per round, `reps` rounds, in rotating order."""
    names = list(variants)
    times = {n: [] for n in names}
    for rep in range(reps):
        order = names[rep % len(names):] + names[:rep % len(names)]
        for n in order:
            times[n].append(runner(n, variants[n]))
    return {n: stats(v) for n, v in times.items()}


def overhead_table(result, baseline="off"):
    base = result[baseline]["median_s"]
    return {n: {**s, "overhead_pct_vs_off": (s["median_s"] / base - 1.0) * 100.0} for n, s in result.items()}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cli", required=True)
    ap.add_argument("--cc", required=True)
    ap.add_argument("--root", required=True)
    ap.add_argument("--reps", type=int, default=7)
    ap.add_argument("--build-reps", type=int, default=5)
    ap.add_argument("--budget", type=int, default=40_000_000, help="guest instructions per run")
    ap.add_argument("--sdl3-include")
    ap.add_argument("--sdl3-lib")
    ap.add_argument("-o", "--output")
    args = ap.parse_args()
    root = pathlib.Path(args.root).resolve()
    out = {"workload": "synthetic spin image", "guest_instructions": args.budget, "host_os": sys.platform}
    with tempfile.TemporaryDirectory(prefix="perf-bench-") as d:
        tmp = pathlib.Path(d)
        rom = tmp / "spin.bin"
        rom.write_bytes(spin_rom(root))
        common = ["--rom", str(rom), "--cc", args.cc, "--runtime-dir", str(root / "platforms" / "genesis"), "--optimize", "2", "--runtime-optimize", "2"]

        # ---- build
        def build_run(name, _):
            cmd = [args.cli, "build", *common, "--output", str(tmp / f"b-{name}-{time.monotonic_ns()}")]
            if name == "profiled":
                cmd += ["--performance-report", str(tmp / "build.json")]
            t, r = timed(cmd)
            if r.returncode != 0:
                raise SystemExit("build failed: " + r.stdout + r.stderr)
            return t
        out["build"] = overhead_table(interleaved({"off": None, "profiled": None}, args.build_reps, build_run))

        # ---- headless runtime
        game_dir = tmp / "game"
        r = subprocess.run([args.cli, "build", *common, "--output", str(game_dir)], capture_output=True, text=True)
        if r.returncode != 0:
            raise SystemExit("game build failed: " + r.stdout + r.stderr)
        game = game_dir / "game"
        base_env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin")}

        def headless_run(name, _):
            env = dict(base_env)
            if name != "off":
                env.update(SEGARECOMP_PERF_REPORT=str(tmp / "run.json"), SEGARECOMP_PERF_MODE=name)
            t, _r = timed([str(game), "--instruction-budget", str(args.budget)], env=env)
            return t
        out["headless"] = overhead_table(interleaved({"off": None, "summary": None, "detailed": None}, args.reps, headless_run))
        # the collector's own per-boundary cost, from the report of a detailed run
        rep = json.loads((tmp / "run.json").read_text())
        out["headless_frames_per_run"] = rep["runtime"]["frames"]["virtual"]
        out["headless_slices_per_run"] = rep["runtime"]["counters"]["runner_slices"]

        # ---- viewer (optional)
        if args.sdl3_include and args.sdl3_lib:
            vdir = tmp / "viewer"
            r = subprocess.run([args.cli, "build", *common, "--sdl3-include", args.sdl3_include, "--sdl3-lib", args.sdl3_lib, "--output", str(vdir)], capture_output=True, text=True)
            if r.returncode != 0:
                raise SystemExit("viewer build failed: " + r.stdout + r.stderr)
            settings = {"overlay1": "video.perf_overlay=1\n", "overlay2": "video.perf_overlay=2\n"}
            env0 = dict(base_env, SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", SDL_RENDER_DRIVER="software", SEGARECOMP_VIEWER_UNTHROTTLED="1", SEGARECOMP_VIEWER_MUTE="1",
                        LD_LIBRARY_PATH=args.sdl3_lib)

            def viewer_run(name, _):
                env = dict(env0, SEGARECOMP_VIEWER_SETTINGS=str(tmp / f"settings-{name}.cfg"))
                (tmp / f"settings-{name}.cfg").write_text(settings.get(name, ""))
                if name in ("summary", "detailed"):
                    env.update(SEGARECOMP_PERF_REPORT=str(tmp / "viewer.json"), SEGARECOMP_PERF_MODE=name)
                t, _r = timed([str(vdir / "game"), "--instruction-budget", str(args.budget // 4)], env=env)
                return t
            out["viewer_unthrottled"] = overhead_table(interleaved({"off": None, "summary": None, "detailed": None, "overlay1": None, "overlay2": None}, args.reps, viewer_run))
    text = json.dumps(out, indent=1)
    if args.output:
        pathlib.Path(args.output).write_text(text + "\n")
    print(text)


if __name__ == "__main__":
    main()
