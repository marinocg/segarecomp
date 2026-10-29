#!/usr/bin/env python3
"""SEG-101-T003 (ADR 0227, 0229): run ONE frozen Sega CD program on several pre-registered routes and report what it executed - aggregate and privacy-safe.

The frozen program is never rebuilt: before and after every route its hash and the file inventory of its directory are verified, the route runs with an
empty PATH (no compiler or linker can be started), and the run's own report states the typed stop. Per route it collects the program's report and
`--coverage`, optional frame/RAM captures at pre-registered reference frames, compares them with the independent reference's captures of the SAME route, and
derives from the report's per-image account (ADR 0228) which compiled images were bound / executed and which of those the build did not observe.
Nothing committed may contain an image id, address, hash or byte: `--private` receives the full JSON, stdout only counts and classes.

usage: sega_cd_m5_routes.py --program P --disc-dir D --bios B [--backup-ram F] --work W --observed-from REPORT.json
         --route NAME:SCRIPT:UNTIL_TICK[:REFDIR:FRAMES[:EXTRA_ARGS]] ... --expect NAME=STOP ... [--base-onset 1202] [--budget N] [--private FILE]
         (FRAMES: comma-separated reference frames relative to the entry; REFDIR: the reference's output directory for that route; EXTRA_ARGS: space-separated program arguments, used by the
          project-authored fixtures' machine-input choice)
"""
import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import time


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def inventory(directory):
    """Every file under the program's directory with size and content hash: nothing may appear, change or vanish across a route."""
    return {str(p.relative_to(directory)): [p.stat().st_size, sha256(p)] for p in sorted(directory.rglob("*")) if p.is_file()}


def swap(b):
    return bytes(b[i ^ 1] for i in range(len(b)))


def compare_capture(gen, ref, base, n):
    """Equality classes of one reference frame: canonical frame, VRAM (either byte order), and the three RAM images (differing byte counts)."""
    out = {"frame": n}
    g, r = gen / ("canon_%d.bin" % (base + n)), ref / ("canon_%d.bin" % n)
    if not g.exists() or not r.exists():
        out["captured"] = False
        return out
    G, R = g.read_bytes(), r.read_bytes()
    out["captured"] = True
    out["frame_equal"] = G == R
    out["frame_differing_pixels"] = None if len(G) != len(R) else sum(1 for i in range(4, len(G), 2) if G[i:i + 2] != R[i:i + 2])
    vg, vr = (gen / ("vdp_%d.bin" % (base + n))).read_bytes(), (ref / ("vdp_%d.bin" % n)).read_bytes()
    out["vram_equal"] = vg[-0x10000:] == vr[-0x10000:] or vg[-0x10000:] == swap(vr[-0x10000:])
    for name, rn in (("word_ram", "wd_e"), ("work_ram", "wr_e"), ("prg_ram", "pr_e")):
        a, b = (gen / ("%s_%d.bin" % (name, base + n))).read_bytes(), (ref / ("%s%d.bin" % (rn, n))).read_bytes()
        out[name + "_differing_bytes"] = sum(x != y for x, y in zip(a, b)) + abs(len(a) - len(b))
    return out


def executed_starts(coverage):
    rows = {0: set(), 1: set()}
    for line in open(coverage):
        cpu, pc, _, _ = line.split()
        if cpu in ("0", "1"):
            rows[int(cpu)].add(int(pc, 16))
    return rows


def images(report, key):
    return {e["image"] for e in report["dynamic"][key]} if key == "exercised" else set(report["dynamic"][key])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--program", required=True, type=pathlib.Path)
    ap.add_argument("--disc-dir", required=True)
    ap.add_argument("--bios", required=True)
    ap.add_argument("--backup-ram")
    ap.add_argument("--work", required=True, type=pathlib.Path)
    ap.add_argument("--observed-from", required=True, help="the build route's (R0) run report: its executed images are the build-observed set")
    ap.add_argument("--route", action="append", required=True)
    ap.add_argument("--base-onset", type=int, default=1202)
    ap.add_argument("--budget", type=int, default=300000000)
    ap.add_argument("--private", type=pathlib.Path)
    ap.add_argument("--expect", action="append", default=[], help="NAME=STOP_CLASS: the pre-registered typed stop of a route ('none' = it must reach its deadline/idle); every route needs one")
    args = ap.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    pdir = args.program.parent
    program_hash = sha256(args.program)
    base_inventory = inventory(pdir)
    observed_report = json.load(open(args.observed_from))
    observed = images(observed_report, "exercised")
    result = {"program_sha256": program_hash, "program_bytes": args.program.stat().st_size, "build_observed_images": len(observed), "routes": {}}
    private = {"build_observed_ids": sorted(observed)}
    for spec in args.route:
        parts = spec.split(":")
        name, script, until = parts[0], parts[1], parts[2]
        refdir = pathlib.Path(parts[3]) if len(parts) > 3 and parts[3] else None
        frames = [int(x) for x in parts[4].split(",")] if len(parts) > 4 and parts[4] else []
        out = args.work / name
        out.mkdir(parents=True, exist_ok=True)
        cmd = [str(args.program.resolve()), "--disc-dir", args.disc_dir, "--bios", args.bios, "--instruction-budget", str(args.budget), "--report", str(out / "report.json"),
               "--coverage", str(out / "coverage.txt")]
        if args.backup_ram:
            cmd += ["--backup-ram", args.backup_ram]
        if script:
            cmd += ["--input-script", script]
        if len(parts) > 5 and ":".join(parts[5:]):
            cmd += ":".join(parts[5:]).split()
        if until and int(until) > 0:
            cmd += ["--until-tick", until]
        if frames:
            cmd += ["--capture-dir", str(out)] + sum((["--capture-onset", str(args.base_onset + n)] for n in frames), [])
        start = time.time()
        done = subprocess.run(cmd, env={"PATH": "/nonexistent"}, cwd=str(out), capture_output=True, text=True, timeout=7200)
        wall = time.time() - start
        report = json.load(open(out / "report.json"))
        if report.get("program_identity") != observed_report.get("program_identity"):
            print("error: --observed-from is not a report of this program", file=sys.stderr)
            return 4
        after = inventory(pdir)
        starts = executed_starts(out / "coverage.txt")
        dyn = report["dynamic"]
        exercised = images(report, "exercised")
        bound = images(report, "bound_images")
        retired_total = sum(c["retired"] for c in report["cpus"])
        unvisited = exercised - observed
        per = {e["image"]: e for e in dyn["exercised"]}
        route = {"exit": done.returncode, "wall_s": round(wall, 1), "stop": report.get("stop", {}).get("class"), "result": report.get("result"), "retired_total": retired_total,
                 "retired_dynamic": dyn["retired"], "retired_static": retired_total - dyn["retired"], "executed_starts": {"main": len(starts[0]), "sub": len(starts[1])},
                 "images_compiled": dyn["images"], "images_bound": len(bound), "images_executed": len(exercised), "images_build_observed_executed": len(exercised & observed),
                 "images_previously_unvisited_executed": len(unvisited), "unvisited_retired": sum(per[i]["retired"] for i in unvisited), "unvisited_distinct_offsets": sum(per[i]["offsets"] for i in unvisited),
                 "executed_offsets_total": sum(e["offsets"] for e in dyn["exercised"]), "binds": dyn["binds"], "binds_main": dyn["binds_main"], "binds_sub": dyn["binds_sub"], "replacements": dyn["replacements"],
                 "invalidations": dyn["invalidations"], "refusals": dyn["refusals"], "last_refusal": dyn["last_refusal"], "recheck_failures": dyn["recheck_failures"],
                 "scan_bytes": dyn["scan_bytes"], "program_unchanged": sha256(args.program) == program_hash and after == base_inventory, "empty_path": True, "env_path": "/nonexistent"}
        if refdir is not None and frames:
            route["frames"] = [compare_capture(out, refdir, args.base_onset, n) for n in frames]
            route["frames_equal"] = sum(1 for f in route["frames"] if f.get("frame_equal"))
            route["frames_compared"] = len(frames)
        result["routes"][name] = route
        private[name] = {"unvisited_ids": sorted(unvisited), "executed": dyn["exercised"], "bound": sorted(bound), "events": dyn["events"], "stop": report.get("stop"), "starts_main": sorted(starts[0]), "starts_sub": sorted(starts[1])}
    for name, route in result["routes"].items():
        route["executed_starts_new_vs_R0"] = None
    r0 = private.get("R0")
    if r0 is not None:
        for name in result["routes"]:
            if name != "R0":
                result["routes"][name]["executed_starts_new_vs_R0"] = {"main": len(set(private[name]["starts_main"]) - set(r0["starts_main"])), "sub": len(set(private[name]["starts_sub"]) - set(r0["starts_sub"]))}
    expected = dict(e.split("=", 1) for e in args.expect)
    failures = []
    for name, route in result["routes"].items():
        if name not in expected:
            failures.append("%s: no pre-registered expected stop" % name)
        elif route["stop"] != expected[name]:
            failures.append("%s: stop %s, expected %s" % (name, route["stop"], expected[name]))
        for f in route.get("frames", []):
            if not f.get("captured") or not f.get("frame_equal") or not f.get("vram_equal"):
                failures.append("%s: reference frame %d missing or different" % (name, f["frame"]))
    result["failures"] = failures
    result["program_unchanged_after_all_routes"] = sha256(args.program) == program_hash and inventory(pdir) == base_inventory
    if args.private:
        args.private.write_text(json.dumps({"aggregate": result, "private": private}, indent=1))
    print(json.dumps(result, indent=1, sort_keys=True))
    if not result["program_unchanged_after_all_routes"]:
        return 3
    return 5 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
