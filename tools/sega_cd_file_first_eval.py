#!/usr/bin/env python3
"""SEG-100-T005 (ADR 0225): aggregate, privacy-safe evaluation of file-first broad AOT on an authorized local disc.

Builds the same disc twice through the supported `segarecomp sega-cd build` - once as the Compatibility reference (route-derived extents, no file-first)
and once with `--file-first` - runs both final programs on the same bounded route, and compares them. Output: counts, classes, times and sizes only.
No address, offset, LBA, name, hash or byte of the disc or the BIOS is printed or written; the local work directory holds the build outputs and is the
operator's to delete. A run that differs from the reference in any machine-state field is reported, never hidden.

usage: sega_cd_file_first_eval.py --segarecomp CLI --root PRODUCT_ROOT --cc CC --cue DISC.cue --bios BIOS.bin --work DIR
           [--backup-ram FILE] [--budget N] [--policy structural|all] [--jobs N] [--skip-reference] [--max-offsets N]
"""
import argparse
import json
import pathlib
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "tests"))
import sega_cd_common as common  # noqa: E402


def region_class(cpu, address):
    if cpu == 0:
        return "main_work_ram" if address >= 0xE00000 else "main_word_ram_view" if 0x200000 <= address < 0x240000 else "main_prg_window" if 0x20000 <= address < 0x40000 else "main_other"
    return "sub_prg_ram" if address < 0x80000 else "sub_word_ram_view" if address < 0xE0000 else "sub_other"


def build(args, out, *extra):
    cmd = [args.segarecomp, "sega-cd", "build", "--cue", args.cue, "--bios", args.bios, "--output", str(out), "--runtime-dir", args.root, "--cc", args.cc,
           "--jobs", str(args.jobs), "--cache-dir", str(args.work / "objects"), *extra]
    if args.backup_ram:
        cmd += ["--backup-ram", args.backup_ram]
    if args.budget:
        cmd += ["--budget", str(args.budget)]
    rc, _, cpu, rss, wall, err = common.measured(cmd)
    status = json.loads((out / "status.json").read_text()) if (out / "status.json").exists() else {}
    return {"exit": rc, "wall_s": round(wall, 1), "cpu_s": round(cpu, 1), "peak_child_rss_mib": rss // 1024, "status": status.get("status"), "rounds": status.get("rounds"),
            "result": status.get("result"), "stop": status.get("stop"), "module_extents": len([e for e in status.get("extents", []) if int(e.split(":")[1]) >= 32768]),
            "boot_extents": len([e for e in status.get("extents", []) if int(e.split(":")[1]) < 32768]), "static_extents": status.get("static_extents"),
            "inventory": status.get("inventory", {}).get("status"), "unresolved": status.get("inventory", {}).get("unresolved"), "file_first": status.get("file_first")}, status


def run(args, program, report, coverage):
    cmd = [str(program), "--disc-dir", str(pathlib.Path(args.cue).parent), "--bios", args.bios, "--instruction-budget", str(args.run_budget), "--report", str(report), "--coverage", str(coverage)]
    if args.backup_ram:
        cmd += ["--backup-ram", args.backup_ram]
    rc, _, cpu, rss, wall, err = common.measured(cmd, timeout=3600)
    return {"exit": rc, "wall_s": round(wall, 1), "cpu_s": round(cpu, 1), "peak_rss_mib": rss // 1024}


def machine_state(report):
    r = dict(report)
    for k in ("program_identity", "dynamic"):
        r.pop(k, None)
    return r


def executed(coverage):
    rows = {0: set(), 1: set()}
    for line in open(coverage):
        cpu, pc, _, _ = line.split()
        if cpu in ("0", "1"):
            rows[int(cpu)].add(int(pc, 16))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--segarecomp", required=True)
    ap.add_argument("--root", required=True)
    ap.add_argument("--cc", default="gcc")
    ap.add_argument("--cue", required=True)
    ap.add_argument("--bios", required=True)
    ap.add_argument("--work", required=True, type=pathlib.Path)
    ap.add_argument("--backup-ram")
    ap.add_argument("--budget", type=int, default=0)
    ap.add_argument("--run-budget", type=int, default=300000000)
    ap.add_argument("--policy", default="structural")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--max-offsets", type=int, default=0)
    ap.add_argument("--skip-reference", action="store_true")
    args = ap.parse_args()
    args.work.mkdir(parents=True, exist_ok=True)
    out = {"policy": args.policy}
    ref_report = ref_cov = None
    if not args.skip_reference:
        out["reference"], ref_status = build(args, args.work / "reference")
        out["reference"]["run"] = run(args, args.work / "reference" / "program", args.work / "reference_report.json", args.work / "reference_cov.txt")
        ref_report = json.load(open(args.work / "reference_report.json"))
        ref_cov = executed(args.work / "reference_cov.txt")
    extra = ["--file-first", args.policy] + (["--file-first-max-offsets", str(args.max_offsets)] if args.max_offsets else [])
    out["file_first"], ff_status = build(args, args.work / "file_first", *extra)
    out["file_first"]["run"] = run(args, args.work / "file_first" / "program", args.work / "file_first_report.json", args.work / "file_first_cov.txt")
    ff_report = json.load(open(args.work / "file_first_report.json"))
    ff_cov = executed(args.work / "file_first_cov.txt")
    dyn = ff_report.get("dynamic", {})
    out["dynamic_run"] = {k: dyn.get(k) for k in ("images", "candidate_bytes", "compiled_offsets", "unique_bodies", "binds", "invalidations", "replacements", "rechecks", "recheck_failures",
                                                  "lookups", "lookup_hits", "scans", "scan_bytes", "retired", "last_refusal", "refusals", "event_count")}
    bound = dyn.get("bindings", [])
    out["dynamic_run"]["live_bindings"] = len(bound)
    out["dynamic_run"]["distinct_images_bound_at_end"] = len({b["image"] for b in bound})
    out["dynamic_run"]["live_bindings_by_region"] = {}
    for b in bound:
        c = region_class(b["cpu"], b["base"])
        out["dynamic_run"]["live_bindings_by_region"][c] = out["dynamic_run"]["live_bindings_by_region"].get(c, 0) + 1
    out["dynamic_run"]["images_bound_during_run"] = len({e["image"] for e in dyn.get("events", []) if e["kind"] == "bound"}) if dyn.get("event_count", 0) <= 64 else None
    out["dynamic_run"]["event_log_complete"] = dyn.get("event_count", 0) <= 64
    out["file_first_run_stop"] = ff_report.get("stop", {}).get("class")
    out["file_first_executed_instruction_starts"] = {"main": len(ff_cov[0]), "sub": len(ff_cov[1])}
    if ref_report is not None:
        out["reference_run_stop"] = ref_report.get("stop", {}).get("class")
        out["reference_executed_instruction_starts"] = {"main": len(ref_cov[0]), "sub": len(ref_cov[1])}
        same = machine_state(ref_report) == machine_state(ff_report)
        differing = sorted(k for k in set(machine_state(ref_report)) | set(machine_state(ff_report)) if machine_state(ref_report).get(k) != machine_state(ff_report).get(k))
        out["comparison"] = {"identical_machine_state": same, "differing_fields": differing, "same_executed_instruction_starts": ref_cov == ff_cov,
                             "executed_only_by_file_first": {"main": len(ff_cov[0] - ref_cov[0]), "sub": len(ff_cov[1] - ref_cov[1])},
                             "executed_only_by_reference": {"main": len(ref_cov[0] - ff_cov[0]), "sub": len(ref_cov[1] - ff_cov[1])},
                             "retired_instructions": {"reference": sum(c["retired"] for c in ref_report["cpus"]), "file_first": sum(c["retired"] for c in ff_report["cpus"])}}
    print(json.dumps(out, indent=1, sort_keys=True))


if __name__ == "__main__":
    sys.exit(main())
