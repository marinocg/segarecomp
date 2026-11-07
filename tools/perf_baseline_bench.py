#!/usr/bin/env python3
"""SEG-126 benchmark driver: an interleaved A/B runner over the SEG-125 reports. It adds no instrumentation; every number comes from a
`--performance-report` / `SEGARECOMP_PERF_REPORT` file (tools/perf_report.py format) or from the wall clock around the whole command.

  perf_baseline_bench.py build   --cli <segarecomp> --root <product root> --cc cc --image LABEL=PATH[:platform] ... [--synthetic] [--reps 5] [--jobs 8] -o out.json
  perf_baseline_bench.py runtime --cli <segarecomp> --root <product root> --cc cc --image LABEL=PATH ... --sdl3 PREFIX [--reps 5] [-o out.json]
  perf_baseline_bench.py compare before.json after.json            (medians, spread and the relative change per shared variant)

Variants are interleaved round-robin (v1 v2 v3 v1 v2 v3 ...) so host drift hits every variant alike; each result keeps every raw sample, the median,
min/max and the coefficient of variation. Image paths are used only to read the image; results carry the LABEL, never a path or a title.
`--variant NAME` (repeatable) restricts the run; `--cli` may differ between `before` and `after` runs of the same variant names.
"""
import argparse
import json
import os
import pathlib
import platform
import statistics
import subprocess
import sys
import tempfile
import time

PIN_CPU = os.environ.get("PERF_BENCH_CPU", "2")
PIN = ["taskset", "-c", PIN_CPU] if sys.platform.startswith("linux") and subprocess.run(["which", "taskset"], capture_output=True).returncode == 0 else []


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


def summarize(xs):
    xs = [x for x in xs if x is not None]
    if not xs:
        return None
    med = statistics.median(xs)
    return {"n": len(xs), "median": med, "min": min(xs), "max": max(xs), "cv_pct": (statistics.pstdev(xs) / statistics.mean(xs) * 100.0) if len(xs) > 1 and statistics.mean(xs) else 0.0,
            "samples": xs}


def environment(cc):
    ver = subprocess.run([cc, "--version"], capture_output=True, text=True).stdout.splitlines()[:1]
    cpu = ""
    try:
        cpu = next(l.split(":", 1)[1].strip() for l in open("/proc/cpuinfo") if l.startswith("model name"))
    except Exception:
        cpu = platform.processor()
    mem = ""
    try:
        mem = next(l.split(":", 1)[1].strip() for l in open("/proc/meminfo") if l.startswith("MemTotal"))
    except Exception:
        pass
    return {"os": platform.platform(), "cpu": cpu, "logical_cpus": os.cpu_count(), "mem_total": mem, "compiler": ver[0] if ver else cc,
            "load_avg_start": os.getloadavg() if hasattr(os, "getloadavg") else None, "pin_cpu_runtime": PIN_CPU if PIN else None}


def interleave(variants, reps, run):
    """variants: ordered dict name -> spec. run(name, spec, rep) -> sample dict. Round-robin, rep-major."""
    samples = {n: [] for n in variants}
    for rep in range(reps):
        for n, spec in variants.items():
            samples[n].append(run(n, spec, rep))
            print(f"  rep {rep + 1}/{reps} {n}: {samples[n][-1].get('wall_s', 0):.2f} s", file=sys.stderr, flush=True)
    return samples


def build_variants(args, images):
    v = {}
    for label, (path, plat) in images.items():
        base = ["--rom", str(path), "--runtime-dir", str(args.root / "platforms" / ("master-system" if plat == "master-system" else "genesis"))]
        if plat == "master-system":
            base += ["--platform", "master-system", "--mapper", "sega"]
        for aot in ("compatibility", "optimized"):
            for o in args.opt_levels:
                if label == "synthetic" and aot == "optimized" and o != "2":
                    continue
                v[f"{label}/{aot}/O{o}"] = {"args": base + ["--aot-policy", aot, "--optimize", o], "label": label}
    return {k: s for k, s in v.items() if not args.variant or k in args.variant}


def one_build(args, name, spec, rep, outroot):
    out = outroot / f"{name.replace('/', '_')}-{rep}"
    rep_json = out.with_suffix(".json")
    cmd = [str(args.cli), "build", "--output", str(out), "--cc", args.cc, "--jobs", str(args.jobs), "--performance-report", str(rep_json)] + spec["args"]
    t0 = time.perf_counter()
    r = subprocess.run(cmd, capture_output=True, text=True)
    wall = time.perf_counter() - t0
    if r.returncode != 0:
        raise SystemExit(f"build failed ({name}): " + r.stdout[-300:] + r.stderr[-300:])
    d = json.loads(rep_json.read_text())
    b = d["build"]
    c = b["compile"]
    ops = {o["name"]: o for o in b["operations"]}
    stages = {s["name"]: s for s in b["stages"]}
    res = {"wall_s": wall, "report_wall_ms": b["wall_ms"], "batch_wall_ms": c["batch_wall_ms"], "aggregate_job_ms": c["aggregate_job_ms"], "wasted_ms": c["wasted_ms"],
           "units": c["units_total"], "superseded": c["superseded"], "compiler_cpu_ms": (c.get("resources") or {}).get("compiler_cpu_ms"),
           "compiler_peak_rss_mib": ((c.get("resources") or {}).get("compiler_peak_rss_kib") or 0) / 1024.0,
           "executable_bytes": b["sizes"].get("executable_bytes"), "generated_c_bytes": b["sizes"].get("generated_c_bytes"),
           "ops_exclusive_ms": {k: o["exclusive_ms"] for k, o in ops.items()},
           "stages_inclusive_ms": {k: s["inclusive_ms"] for k, s in stages.items()},
           "categories_job_ms": {x["category"]: x["job_ms"] for x in c.get("categories", [])},
           "categories_wasted_ms": {x["category"]: x["wasted_ms"] for x in c.get("categories", [])}}
    exe = out / "game"
    res["exe"] = str(exe)
    return res


def collapse(samples):
    """Per-variant summary of the numeric fields; nested dicts are summarized per key."""
    out = {}
    for n, rows in samples.items():
        agg = {}
        for k in rows[0]:
            vals = [r.get(k) for r in rows]
            if isinstance(vals[0], dict):
                agg[k] = {kk: summarize([v.get(kk) if v else None for v in vals]) for kk in vals[0]}
            elif isinstance(vals[0], (int, float)):
                agg[k] = summarize(vals)
        out[n] = agg
    return out


def cmd_build(args):
    tmpdir = pathlib.Path(tempfile.mkdtemp(prefix="perf-bench-build-"))
    images = {}
    if args.synthetic:
        p = tmpdir / "synthetic.bin"
        p.write_bytes(spin_rom(args.root))
        images["synthetic"] = (p, "genesis")
    for spec in args.image:
        label, rest = spec.split("=", 1)
        path, _, plat = rest.partition(":")
        images[label] = (pathlib.Path(path), plat or "genesis")
    variants = build_variants(args, images)
    print(f"{len(variants)} build variants x {args.reps} reps, interleaved", file=sys.stderr)
    samples = interleave(variants, args.reps, lambda n, s, rep: one_build(args, n, s, rep, tmpdir))
    for rows in samples.values():
        for r in rows:
            r.pop("exe", None)
    out = {"kind": "build_benchmark", "cli_label": args.label, "reps": args.reps, "jobs": args.jobs, "env": environment(args.cc), "variants": collapse(samples)}
    pathlib.Path(args.output).write_text(json.dumps(out, indent=1))
    print(f"wrote {args.output}", file=sys.stderr)
    for n, a in out["variants"].items():
        print(f"{n:34s} wall median {a['wall_s']['median']:8.2f} s  cv {a['wall_s']['cv_pct']:4.1f}%  [{a['wall_s']['min']:.2f}..{a['wall_s']['max']:.2f}]")


def runtime_variants(args):
    v = {}
    sdl = args.sdl3 / "lib"
    base = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LD_LIBRARY_PATH": str(sdl), "SDL_AUDIODRIVER": "dummy", "SEGARECOMP_VIEWER_MUTE": "1"}
    for k in ("DISPLAY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR", "XDG_SESSION_TYPE"):   # hardware-backed renderers need the session's display
        if k in os.environ:
            base[k] = os.environ[k]
    for label in args.runtime_images:
        v[f"{label}/headless-unthrottled"] = {"exe": f"{label}/headless", "args": ["--instruction-budget", str(args.budget)], "env": {}, "kind": "headless"}
        for rend in args.renderers:
            drv = "dummy" if rend == "software" else None
            env = {"SDL_RENDER_DRIVER": rend}
            if drv:
                env["SDL_VIDEODRIVER"] = drv
            v[f"{label}/viewer-{rend}-unthrottled"] = {"exe": f"{label}/viewer", "args": ["--instruction-budget", str(args.budget)], "env": dict(env, SEGARECOMP_VIEWER_UNTHROTTLED="1"), "kind": "viewer"}
            v[f"{label}/viewer-{rend}-paced"] = {"exe": f"{label}/viewer", "args": ["--instruction-budget", str(args.paced_budget)], "env": dict(env, SEGARECOMP_VIEWER_UNTHROTTLED="0"), "kind": "viewer"}
    return {k: dict(s, base=base) for k, s in v.items() if not args.variant or k in args.variant}


def one_run(args, name, spec, rep, exes, tmp):
    rj = tmp / f"run-{name.replace('/', '_')}-{rep}.json"
    env = dict(spec["base"], **spec["env"], SEGARECOMP_PERF_REPORT=str(rj))
    cmd = PIN + [str(exes[spec["exe"]])] + spec["args"]
    t0 = time.perf_counter()
    r = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=900)
    wall = time.perf_counter() - t0
    d = json.loads(rj.read_text())
    rt = d["runtime"]
    spans = {s["name"]: s for s in rt["spans"]}
    frames = rt["frames"]["virtual"]
    res = {"wall_s": wall, "virtual_frames": frames, "session_s": rt["wall_ns"] / 1e9,
           "speed_pct_native": frames * d["config"]["target_frame_ns"] / rt["wall_ns"] * 100.0,
           "frame_avg_ms": rt["frame_interval"]["avg_ns"] / 1e6 if rt["frame_interval"].get("avg_ns") is not None else None,
           "frame_p99_ms": (rt["frame_interval"].get("p99_ns") or 0) / 1e6, "missed_deadlines": rt["pacing"]["missed_deadlines"], "presented": rt["frames"]["presented"],
           "span_ms_per_frame": {k: (s["total_ns"] / 1e6 / max(1, frames)) for k, s in spans.items() if s["total_ns"]}}
    res["renderer"] = d["config"].get("renderer")
    return res


def cmd_runtime(args):
    tmp = pathlib.Path(args.work_dir) if args.work_dir else pathlib.Path(tempfile.mkdtemp(prefix="perf-bench-run-"))
    tmp.mkdir(parents=True, exist_ok=True)
    args.runtime_images = []
    exes = {}
    for spec, opt in ((s, o) for s in args.image for o in args.opt_levels):   # opt "G" or "G/R": --optimize G [--runtime-optimize R]
        base_label, rest = spec.split("=", 1)
        label = f"{base_label}-O{opt.replace('/', '-R')}"
        rest, _, root_override = rest.partition("@")   # LABEL=PATH[:platform][@PRODUCT_ROOT]: build this variant from another checkout (A/B of a runtime change)
        path, _, plat = rest.partition(":")
        plat = plat or "genesis"
        args.runtime_images.append(label)
        rd = pathlib.Path(root_override or args.root) / "platforms" / ("master-system" if plat == "master-system" else "genesis")
        for kind in ("headless", "viewer"):
            out = tmp / f"{label}-{kind}"
            g, _, rt = opt.partition("/")
            cmd = [str(args.cli), "build", "--rom", path, "--output", str(out), "--cc", args.cc, "--jobs", str(args.jobs), "--runtime-dir", str(rd), "--aot-policy", args.aot, "--optimize", g]
            if rt:
                cmd += ["--runtime-optimize", rt]
            if plat == "master-system":
                cmd += ["--platform", "master-system", "--mapper", "sega"]
            if kind == "viewer":
                cmd += ["--sdl3-include", str(args.sdl3 / "include"), "--sdl3-lib", str(args.sdl3 / "lib")]
            if not (out / "game").exists():   # --work-dir keeps built programs between invocations
                r = subprocess.run(cmd, capture_output=True, text=True)
                if r.returncode != 0:
                    raise SystemExit("build failed: " + r.stdout[-300:] + r.stderr[-300:])
            exes[f"{label}/{kind}"] = out / "game"
    variants = runtime_variants(args)
    samples = interleave(variants, args.reps, lambda n, s, rep: one_run(args, n, s, rep, exes, tmp))
    out = {"kind": "runtime_benchmark", "cli_label": args.label, "reps": args.reps, "budget": args.budget, "paced_budget": args.paced_budget, "env": environment(args.cc),
           "variants": collapse(samples)}
    pathlib.Path(args.output).write_text(json.dumps(out, indent=1))
    for n, a in out["variants"].items():
        print(f"{n:42s} speed {a['speed_pct_native']['median']:7.1f}% cv {a['speed_pct_native']['cv_pct']:4.1f}%  frame avg {a['frame_avg_ms']['median']:.2f} ms  missed {a['missed_deadlines']['median']:.0f}")


def cmd_compare(args):
    a, b = (json.loads(pathlib.Path(p).read_text()) for p in (args.before, args.after))
    key = "wall_s" if a["kind"] == "build_benchmark" else "speed_pct_native"
    for n in sorted(set(a["variants"]) & set(b["variants"])):
        x, y = a["variants"][n][key], b["variants"][n][key]
        print(f"{n:42s} before {x['median']:9.2f} (cv {x['cv_pct']:4.1f}%)  after {y['median']:9.2f} (cv {y['cv_pct']:4.1f}%)  change {(y['median'] / x['median'] - 1) * 100:+6.1f}%")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("build", "runtime"):
        p = sub.add_parser(name)
        p.add_argument("--cli", type=pathlib.Path, required=True)
        p.add_argument("--root", type=pathlib.Path, required=True)
        p.add_argument("--cc", default="cc")
        p.add_argument("--image", action="append", default=[], help="LABEL=PATH[:master-system][@PRODUCT_ROOT]")
        p.add_argument("--reps", type=int, default=5)
        p.add_argument("--jobs", type=int, default=8)
        p.add_argument("--variant", action="append")
        p.add_argument("--label", default="")
        p.add_argument("-o", "--output", required=True)
    b = sub.choices["build"]
    b.add_argument("--synthetic", action="store_true")
    b.add_argument("--opt-levels", nargs="+", default=["2"])
    r = sub.choices["runtime"]
    r.add_argument("--sdl3", type=pathlib.Path, required=True)
    r.add_argument("--opt-levels", nargs="+", default=["2"], help="GEN or GEN/RUNTIME optimization pairs, e.g. 2 1 1/2")
    r.add_argument("--renderers", nargs="+", default=["software"])
    r.add_argument("--budget", type=int, default=30_000_000)
    r.add_argument("--paced-budget", type=int, default=10_000_000)
    r.add_argument("--aot", default="compatibility")
    r.add_argument("--work-dir", help="keep/reuse built programs here (a program already present is not rebuilt: use a fresh directory per CLI)")
    c = sub.add_parser("compare")
    c.add_argument("before")
    c.add_argument("after")
    args = ap.parse_args()
    {"build": cmd_build, "runtime": cmd_runtime, "compare": cmd_compare}[args.cmd](args)


if __name__ == "__main__":
    main()
