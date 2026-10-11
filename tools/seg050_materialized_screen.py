#!/usr/bin/env python3
"""SEG-050-T001 (ADR 0110): sanitized screening / baseline of materialized executable images through the shipped `segarecomp build` route.

Measurement only. For one authorized local Genesis image this runs the unchanged consumer build (Compatibility, or `--aot-policy optimized
[--aot-model gen2]`) and reports AGGREGATES: the ADR 0049 / RAM-thunk preparation counts and termination, the broad-universe sizes of the
alias-less and final emissions (so the materialized-image share is attributable), the ML admission report class, compile CPU / wall and the
generated C size. With `--screen-only` the build is stopped right after the `m68k-alias-prepare` stage (cheap classification of A/B/C/D/E).

No address, byte, hash of code, per-window datum or listing is retained: the JSON holds counts, byte sizes, seconds and stable classes only.
Intermediate artifacts stay under the ignored `--work` directory.

usage: seg050_materialized_screen.py --segarecomp <cli> --rom <image> --work <ignored-dir> --runtime-dir <platforms/genesis>
           [--policy compatibility|optimized] [--model v1|gen2] [--optimize 0|1] [--jobs N] [--screen-only] [--timeout S] [--cc gcc] [--cxx g++]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import signal
import subprocess
import sys
import threading
import time


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_log(text: str) -> dict:
    """Sanitized aggregates of build.log (counts and stable tokens only)."""
    accepted = [int(m) for m in re.findall(r"immutable-rom AOT enumeration: aligned_start_count=\d+ accepted_count=(\d+)", text)]
    rounds_lines = re.findall(r"m68k alias preparation round: aliases=(\d+)(?: ram_thunks=(\d+))?", text)
    prep = re.search(r"m68k alias preparation: rounds=(\d+) aliases=(\d+)(?: ram_thunks=(\d+))? alias_bytes=(\d+) termination=(\w+)", text)
    admission = re.findall(r"m68k admission: requested=optimized (.*)", text)
    reasons = []
    for line in admission:
        fields = dict(token.split("=", 1) for token in line.split() if "=" in token)
        reasons.append({k: fields[k] for k in ("producer", "fallback", "reason", "universe", "k0", "k", "pruned", "validator") if k in fields})
    out = {
        "emissions": len(accepted),
        "universe_first_emission": accepted[0] if accepted else None,
        "universe_final_emission": accepted[-1] if accepted else None,
        "preparation_rounds_logged": len(rounds_lines),
    }
    if prep:
        out["preparation"] = {"rounds": int(prep[1]), "aliases": int(prep[2]), "ram_thunks": int(prep[3] or 0), "alias_bytes": int(prep[4]),
                              "termination": prep[5]}
    if accepted and len(accepted) > 1:
        out["materialized_starts"] = accepted[-1] - accepted[0]
        out["materialized_share_ppm"] = (accepted[-1] - accepted[0]) * 1_000_000 // accepted[-1]
    if reasons:
        out["admission_emissions"] = len(reasons)
        out["admission_first"] = reasons[0]
        out["admission_last"] = reasons[-1]
        out["admission_fallback_reasons"] = sorted({r.get("reason", "?") for r in reasons if r.get("fallback") == "1"})
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--segarecomp", required=True)
    ap.add_argument("--rom", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--runtime-dir", required=True)
    ap.add_argument("--policy", default="compatibility", choices=["compatibility", "optimized"])
    ap.add_argument("--model", default="v1", choices=["v1", "gen2"])
    ap.add_argument("--optimize", default="1")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--screen-only", action="store_true")
    ap.add_argument("--timeout", type=int, default=3600)
    ap.add_argument("--cc", default="gcc")
    ap.add_argument("--cxx", default="g++")
    args = ap.parse_args()
    rom = pathlib.Path(args.rom).resolve()
    work = pathlib.Path(args.work).resolve()
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    staged = work / "image.md"
    shutil.copyfile(rom, staged)
    out = work / "out"
    cmd = [args.segarecomp, "build", "--rom", str(staged), "--output", str(out), "--cc", args.cc, "--cxx", args.cxx, "--runtime-dir", args.runtime_dir,
           "--optimize", args.optimize, "--runtime-optimize", "1", "--jobs", str(args.jobs), "--aot-policy", args.policy]
    if args.policy == "optimized" and args.model == "gen2":
        cmd += ["--aot-model", "gen2"]
    start = time.time()
    stages: dict[str, float] = {}
    proc = subprocess.Popen(["/usr/bin/time", "-f", "%e %U %S %M", "-o", str(work / "time.txt")] + cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, start_new_session=True)
    stopped_early = False
    timer = threading.Timer(args.timeout, lambda: os.killpg(proc.pid, signal.SIGKILL))
    timer.start()
    result_line = ""
    for line in proc.stdout:  # type: ignore[union-attr]
        match = re.match(r"@stage (\S+) (begin|done)", line)
        if match:
            stages[match[1] + "." + match[2]] = round(time.time() - start, 1)
            if args.screen_only and match[1] == "m68k-alias-prepare" and match[2] == "done":
                stopped_early = True
                os.killpg(proc.pid, signal.SIGKILL)
                break
        if line.startswith("@result"):
            result_line = line.split()[1] if len(line.split()) > 1 else ""
    proc.wait()
    timer.cancel()
    record: dict = {"rom_bytes": rom.stat().st_size, "rom_sha256": sha256(rom), "policy": args.policy, "model": args.model if args.policy == "optimized" else "none",
                    "optimize": args.optimize, "screen_only": args.screen_only, "result": "screened" if stopped_early else (result_line or "no_result"),
                    "stage_seconds": stages}
    log = out / "build.log"
    if log.exists():
        record.update(parse_log(log.read_text(errors="replace")))
    status_path = out / "status.json"
    if status_path.exists():
        status = json.loads(status_path.read_text())
        record["status"] = status.get("status")
        for key in ("m68k_alias_preparation",):
            if key in status:
                record[key] = status[key]
        policy = status.get("aot_policy", {})
        if policy:
            record["aot_policy"] = {k: policy[k] for k in ("effective", "fallback", "reason", "fallback_class", "selected_ppm", "admitted_ppm", "validator",
                                                          "universe", "k", "pruned") if k in policy}
        if "z80" in status:
            record["z80"] = {k: status["z80"][k] for k in ("outcome", "images", "generated_bytes", "compile_ms", "materialize_ms") if k in status["z80"]}
        images = status.get("executable_images", {}).get("m68k")
        if images:
            record["m68k_images"] = images
    time_path = work / "time.txt"
    if time_path.exists() and not stopped_early:
        wall, user, system, rss = time_path.read_text().split()[-4:]
        record["wall_seconds"] = float(wall)
        record["cpu_seconds"] = round(float(user) + float(system), 1)
        record["peak_rss_kib"] = int(rss)
    generated = out / "generated"
    if generated.exists():
        record["generated_c_bytes"] = sum(p.stat().st_size for p in generated.rglob("*") if p.is_file())
    print(json.dumps(record, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
