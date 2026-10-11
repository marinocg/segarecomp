#!/usr/bin/env python3
"""SEG-049 (ADR 0107): bounded broad vs production v1 vs native gen2 differential of one Genesis image (measurement only).

Runs the unchanged bridge three times for a fixed frame count and an explicit instruction budget: broad, `--ml-admission` (v1) and
`--ml-gen2-admission` (the native frozen gen2 M3 + B35 route, admission through the unchanged prune + validator). Required: equal outcome, frame count,
final-state, coverage digests and the SAME executed ROM-PC set as broad. A no-render frame-stream digest is recorded but is NOT evidence of visual
equivalence. Output: one sanitized JSON object (counts, bytes, seconds, booleans; no address, digest value or per-PC datum). Intermediate artifacts stay in
the ignored `--work` directory (inside the product root).

usage: genesis_ml_gen2_differential.py --segarecomp <cli> --rom <image> --work <ignored-dir> --frames N [--cc clang] [--timeout S]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
KEYS = ("outcome", "frames", "final_state", "coverage", "frame_stream")


def timed(cmd, timeout):
    with tempfile.NamedTemporaryFile("r") as stat:
        done = subprocess.run(["/usr/bin/time", "-f", "%e %U %S %M", "-o", stat.name] + cmd, capture_output=True, text=True, timeout=timeout)
        wall, user, system, rss = open(stat.name).read().split()[-4:]
    return done, float(wall), float(user) + float(system), int(rss)


def executed_pcs(out: pathlib.Path, rom_size: int) -> set:
    bitmap = (out / "coverage-private" / "coverage.bitmap").read_bytes()
    pcs = set()
    for index in range(min(len(bitmap), (rom_size + 15) // 16)):
        if bitmap[index]:
            for bit in range(8):
                if bitmap[index] >> bit & 1 and (index * 8 + bit) * 2 < rom_size:
                    pcs.add((index * 8 + bit) * 2)
    return pcs


def variant(cli, rom, out, frames, extra, cc, timeout):
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    cmd = [sys.executable, str(ROOT / "tools/genesis_startup_bridge.py"), "--segarecomp", cli, "--rom", str(rom), "--mode", "commercial", "--immutable-rom-aot",
           "--execution-coverage", str(frames), "--coverage-epoch-frames", str(frames), "--coverage-no-render",
           "--instruction-budget", str(max(10_000_000, frames * 100_000)), "--out-dir", str(out), "--cc", cc] + extra
    done, wall, cpu, rss = timed(cmd, timeout)
    line = next((l for l in done.stderr.splitlines() if l.startswith("COVERAGE_SUMMARY ")), None)
    files = [p for p in (out / "generated").rglob("*") if p.is_file()] if (out / "generated").exists() else []
    files += [p for p in out.glob("*.generated.c") if p.is_file()]
    rec = {"ok": line is not None, "wall_seconds": wall, "cpu_seconds": round(cpu, 1), "peak_rss_kib": rss, "generated_bytes": sum(p.stat().st_size for p in files)}
    if line:
        d = json.loads(line.split(" ", 1)[1])
        rec.update({"outcome": d["outcome"], "frames": d["frames_published"], "final_state": d["final_state_digest"], "coverage": d["coverage_digest"],
                    "frame_stream": d["frame_stream_digest"]})
    return rec


def admission_line(cli, rom, flag):
    digest = hashlib.sha256(rom.read_bytes()).hexdigest()
    with tempfile.TemporaryDirectory() as tmp:
        probe = subprocess.run([cli, "emit-general-startup-bridge-c", "--rom", str(rom), "--reset-entry", "--rom-sha256", digest, "--immutable-rom-aot", flag,
                                "--generated-c-output", str(pathlib.Path(tmp) / "p.c")], capture_output=True, text=True)
    line = next((l for l in probe.stderr.splitlines() if "m68k admission: requested=optimized" in l), "")
    return dict(tok.split("=", 1) for tok in line.split()[3:] if "=" in tok)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--segarecomp", required=True)
    parser.add_argument("--rom", required=True, type=pathlib.Path)
    parser.add_argument("--work", required=True, type=pathlib.Path)
    parser.add_argument("--frames", required=True, type=int)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--timeout", type=int, default=7200)
    args = parser.parse_args()
    work = args.work.resolve()
    if subprocess.run(["git", "check-ignore", "-q", "--", str(work)], cwd=ROOT).returncode != 0:
        raise SystemExit("--work must be an ignored directory inside the product root")
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    size = args.rom.stat().st_size
    variants = {"broad": [], "v1": ["--ml-admission"], "gen2": ["--ml-gen2-admission"]}
    rec, pcs = {}, {}
    for name, extra in variants.items():
        rec[name] = variant(args.segarecomp, args.rom, work / name, args.frames, extra, args.cc, args.timeout)
        pcs[name] = executed_pcs(work / name, size) if rec[name]["ok"] else None
    report = {"schema": "segarecomp.ml_gen2_differential.v1", "frames_requested": args.frames, "rom_bytes": size}
    for name in variants:
        r = rec[name]
        report[name] = {k: r[k] for k in ("ok", "wall_seconds", "cpu_seconds", "peak_rss_kib", "generated_bytes", "outcome", "frames") if k in r}
    for name in ("v1", "gen2"):
        if rec["broad"]["ok"] and rec[name]["ok"]:
            report[name]["equal_to_broad"] = {k: rec[name][k] == rec["broad"][k] for k in KEYS}
            report[name]["executed_pcs"] = len(pcs[name])
            report[name]["executed_pcs_outside_broad"] = len(pcs[name] - pcs["broad"])
            report[name]["broad_pcs_not_executed"] = len(pcs["broad"] - pcs[name])
            report[name]["generated_bytes_vs_broad"] = round(rec[name]["generated_bytes"] / max(1, rec["broad"]["generated_bytes"]), 4)
            report[name]["cpu_vs_broad"] = round(rec[name]["cpu_seconds"] / max(0.1, rec["broad"]["cpu_seconds"]), 4)
            report[name]["wall_vs_broad"] = round(rec[name]["wall_seconds"] / max(0.1, rec["broad"]["wall_seconds"]), 4)
    if rec["broad"]["ok"]:
        report["broad_executed_pcs"] = len(pcs["broad"])
    for name, flag in (("v1", "--immutable-rom-aot-ml-admission"), ("gen2", "--immutable-rom-aot-ml-gen2-admission")):
        a = admission_line(args.segarecomp, args.rom, flag)
        report[name]["admission"] = {k: a[k] for k in ("producer", "fallback", "reason", "fallback_class", "validator", "universe", "k", "cells", "windows", "ml_selected") if k in a}
    print(json.dumps(report, indent=1, sort_keys=True))
    ok = all(report[n].get("equal_to_broad") and all(v for k, v in report[n]["equal_to_broad"].items() if k != "frame_stream") and
             report[n].get("executed_pcs_outside_broad") == 0 for n in ("v1", "gen2") if n in report and "equal_to_broad" in report[n])
    return 0 if ok and rec["broad"]["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
