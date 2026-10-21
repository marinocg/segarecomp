#!/usr/bin/env python3
"""SEG-076-T004 C5 (exploratory, development-only): generated-native compilation economics of the SEG-076 candidates through SEG-074's
UNCHANGED exact-code benchmark (tools/segarecomp_oracle_bench.py, ADR 0156-0158; its frozen results are not touched).

Regions (128-byte cells, all on the 11 SEG-074 gate images, out-of-fold development scores only):
  BROAD            same-session broad immutable-ROM AOT (runtime reference; SEG-074 BROAD / INC / ORACLE values are reused as context)
  M3RB50           SEG-072's saved M3-R + B50 region (the bench's own strategy, unchanged)
  A_B50            the same B50 construction (segarecomp_ml_v3.policy_region with the image's seed cells; proven to reproduce all 50 saved
                   M3-R regions bit for bit from M3-R's own scores) applied to A's deterministic T001 fold scores
  CV2E_B50         ... applied to the deterministic C-v2 3-seed logit ensemble (T005; word-logit mean then 128-byte cell max)
  M3RE4_B50        ... applied to the M3-R C0 4-seed (49-52) cell-logit ensemble (T003; SEG-072 s49, SEG-075 s50, SEG-076 s51/s52)
  *_FULL_RECALL    DIAGNOSTIC, truth-informed: every cell ranked at or above the model's weakest positive, plus seed cells - the region the
                   model's ER describes (cells_needed); for M3-R s49 and A. Not deployable (it reads labels), like SEG-074's oracles
Every region goes through the bench's measure_image path unchanged (same prune, admission validator, broad fallback, emitter, compile
flags, runtime check). Recorded per strategy: accepted / fallback, selected cells, admitted identities, generated C bytes, compile CPU,
total build CPU, runtime equality with BROAD. Model analysis cost (fit and inference CPU from the T001 records) is reported beside it.

  plan     freeze the C5 plan (tools/segarecomp_xplat_economics.plan.json + .digest): bound code, CLI binary and compiler identity,
           images, strategies, settings - before any measurement
  measure  run the measurements (one image at a time, the bench's 4 compile jobs, cold cache)
  table    aggregate-only results (tools/segarecomp_xplat_economics.results.json)
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_xplat_explore as rx  # noqa: E402

PLAN = HERE / "segarecomp_xplat_economics.plan.json"
DIGEST = HERE / "segarecomp_xplat_economics.plan.digest"
RESULTS = HERE / "segarecomp_xplat_economics.results.json"
BUNDLE = rx.HARNESS / ".cache" / "seg072-handoff" / "bundle-v1"
T001 = rx.HARNESS / ".cache" / "seg076" / "t001"
T001_ROCM = rx.HARNESS / ".cache" / "seg076" / "t001-rocm"
SETTINGS = {"cc": "clang", "opt": "O2", "jobs": 4, "frames": 120, "timeout": 3600, "cache": False}
STRATEGIES = ("BROAD", "M3RB50", "A_B50", "CV2E_B50", "M3RE4_B50", "M3R_FULL_RECALL", "A_FULL_RECALL")
T003 = rx.HARNESS / ".cache" / "seg076" / "t003"
T005 = rx.HARNESS / ".cache" / "seg076" / "t005"
BOUND = ("segarecomp_xplat_economics.py", "segarecomp_oracle_bench.py", "segarecomp_xplat_explore.py", "segarecomp_xplat_explore_eval.py",
         "segarecomp_ml_v3.py", "segarecomp_ml_v3_train.py", "segarecomp_xplat_transfer.py", "segarecomp_xplat_explore.t001.plan.digest",
         "segarecomp_xplat_explore.t001.rocm.plan.digest", "segarecomp_xplat_stability.plan.digest", "segarecomp_xplat_cv2.plan.digest")


def sha(p: pathlib.Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def bench():
    import segarecomp_oracle_bench as ob
    return ob


def score_files() -> dict[str, pathlib.Path]:
    out = {}
    for f in rx.GENESIS:
        out[f"seg072/M3R.{f}.npz"] = rx.xt().SEG072_SCORES / f"M3R.{f}.npz"
        out[f"seg076/A.C0.{f}.npz"] = T001 / "scores" / f"A.C0.{f}.npz"
        out[f"seg075/M3R.C0.{f}.s50.npz"] = rx.SEG075 / "t004" / "scores" / f"M3R.C0.{f}.s50.npz"
        for sd in (51, 52):
            out[f"seg076-t003/M3R.C0.{f}.s{sd}.npz"] = T003 / "scores" / f"M3R.C0.{f}.s{sd}.npz"
        for sd in (1, 2, 3):
            out[f"seg076-t005/CV2.C0.{f}.s{sd}.npz"] = T005 / "scores" / f"CV2.C0.{f}.s{sd}.npz"
    return out


def build_plan(cli: pathlib.Path) -> dict:
    rx.verify_plan()
    cc = subprocess.run([SETTINGS["cc"], "--version"], capture_output=True, text=True).stdout.splitlines()[0]
    return {"schema": "segarecomp.xplat_economics.plan.v1", "task": "SEG-076-T004", "checkpoint": "C5",
            "bound_code": {n: sha(HERE / n) for n in BOUND}, "cli_sha256": sha(cli), "compiler": cc, "settings": SETTINGS,
            "images": dict(bench().GATE_IMAGES), "strategies": list(STRATEGIES), "scores": {k: sha(v) for k, v in score_files().items()},
            "trigger": "development candidates that lower Genesis mean ER at verified full recall with 0 B50 misses against the saved "
                       "M3-R s49 (0.146): A 0.105 (T001), the M3-R 4-seed ensemble 0.117 (T003), the C-v2 3-seed ensemble 0.127 (T005); "
                       "C5 measures what that does to real generated-native compilation",
            "region_construction": "B50 = segarecomp_ml_v3.policy_region('B50', cell scores, None, seed cells); verified to reproduce all 50 "
                                   "saved SEG-072 M3-R B50 regions bit for bit from M3-R's own fold scores. FULL_RECALL = {cells with score "
                                   ">= the weakest positive's score} | seed cells (diagnostic, truth-informed)",
            "candidates": "reproducible ones only: the original C seed-1 draw (ADR 0165) is replaced by the deterministic C-v2 ensemble "
                          "because T005 (ADR 0169) showed it was a likely favourable, non-reproducible draw",
            "reporting": "per image and per lineage (lineage mean of the image ratio vs same-session BROAD): selected cells, admission "
                         "accepted / fallback, generated C bytes, compile CPU, total build CPU, runtime equality; model analysis CPU "
                         "beside it; no decision threshold (C5 is measurement; the synthesis ADR interprets it)",
            "untouched": "segarecomp_oracle_bench.py code and its committed results (results.json, t002.json) are not modified"}


def write_plan(cli: pathlib.Path) -> dict:
    doc = build_plan(cli)
    PLAN.write_text(json.dumps(doc, indent=1, sort_keys=True) + "\n")
    DIGEST.write_text(hashlib.sha256(rx.canonical(doc)).hexdigest() + "\n")
    return doc


def verify_plan(cli: pathlib.Path | None = None) -> dict:
    if not (PLAN.is_file() and DIGEST.is_file()):
        raise SystemExit("C5 plan missing: no measurement before the committed plan")
    doc = json.loads(PLAN.read_text())
    if hashlib.sha256(rx.canonical(doc)).hexdigest() != DIGEST.read_text().strip():
        raise SystemExit("C5 plan digest mismatch")
    for n, want in doc["bound_code"].items():
        if sha(HERE / n) != want:
            raise SystemExit(f"{n} differs from the code bound by the C5 plan")
    for k, p in score_files().items():
        if sha(p) != doc["scores"][k]:
            raise SystemExit(f"score {k} differs from its bound digest")
    if cli is not None and sha(cli) != doc["cli_sha256"]:
        raise SystemExit("CLI binary differs from the C5 plan")
    return doc


def plan_sha() -> str:
    return DIGEST.read_text().strip()


# ------------------------------------------------------------------------------------------------------------------------ regions
def model_cells(iid: str, lineage: str, size: int) -> dict:
    """128-byte cell scores per model for one development image (out-of-fold)."""
    import numpy as np
    import segarecomp_ml_v3_train as t
    X = rx.xt()
    f = score_files()
    out = {"M3R": np.asarray(X.load_npz(f[f"seg072/M3R.{lineage}.npz"])[iid], dtype=np.float64),
           "A": np.asarray(X.load_npz(f[f"seg076/A.C0.{lineage}.npz"])[iid], dtype=np.float64)}
    m3 = [X.load_npz(f[f"seg072/M3R.{lineage}.npz"])[iid], X.load_npz(f[f"seg075/M3R.C0.{lineage}.s50.npz"])[iid]]
    m3 += [X.load_npz(f[f"seg076-t003/M3R.C0.{lineage}.s{sd}.npz"])[iid] for sd in (51, 52)]
    out["M3RE4"] = np.mean([np.asarray(x, dtype=np.float64) for x in m3], axis=0)
    cw = np.mean([X.load_npz(f[f"seg076-t005/CV2.C0.{lineage}.s{sd}.npz"])[iid] for sd in (1, 2, 3)], axis=0)
    out["CV2E"] = np.asarray(t.cell_max(cw.astype(np.float32), size, 128), dtype=np.float64)
    return out


def regions_for(iid: str, lineage: str, classes: bytes, seed_addresses) -> dict:
    import segarecomp_ml_v3 as v3
    labels = v3.cell_labels(classes, 128)
    seeds = {a // 128 for a in seed_addresses if 0 <= a < len(classes)}
    out = {}
    for model, sc in model_cells(iid, lineage, len(classes)).items():
        s = [float(x) for x in sc]
        if len(s) != len(labels):
            raise SystemExit(f"{iid}/{model}: score length {len(s)} != cells {len(labels)}")
        out[f"{model}_B50"] = v3.policy_region("B50", s, None, seeds)["region"]
        if model in ("M3R", "A"):
            pos = [c for c, y in enumerate(labels) if y == 1]
            worst = min(s[c] for c in pos) if pos else float("inf")
            out[f"{model}_FULL_RECALL"] = {c for c, x in enumerate(s) if x >= worst} | seeds
    return out


def cmd_measure(a) -> int:
    ob = bench()
    cli = pathlib.Path(a.cli).resolve()
    verify_plan(cli)
    work = pathlib.Path(a.work).resolve()
    rx.guard([work])
    dg, bundle = ob._bundle(argparse.Namespace(bundle=str(BUNDLE), rebase=None))
    dg.require_private_dir(work)
    reg = dg.registry(bundle)
    m3r = json.loads(bundle.file("seg072/results/table.M3_R.json").read_text())
    saved = {r["id"]: r["policies"]["B50"]["_region"] for r in m3r["records"] if "B50" in r.get("policies", {})}
    for iid in a.images or list(ob.GATE_IMAGES):
        out = work / iid / "c5.json"
        if out.exists():
            print(f"{iid}: done", file=sys.stderr)
            continue
        m = reg[iid]
        rom = bundle.resolve_image(m)
        d = pathlib.Path(m["dir"])
        classes = bundle.file(str((d / "classes.bin").relative_to(bundle.payload))).read_bytes()
        seeds = json.loads(bundle.file(str((d / "seeds.json").relative_to(bundle.payload))).read_text())
        meta_dir = next(x for x in rx.xt().genesis_dirs(rx.GENESIS) if json.loads((x / "meta.json").read_text())["id"] == iid)
        regs = regions_for(iid, m["lineage"], classes, rx.xt().genesis_seed_addresses(meta_dir))
        if regs["M3R_B50"] != set(saved[iid]):
            raise SystemExit(f"{iid}: the B50 construction does not reproduce the saved M3-R region")
        res = {"lineage": m["lineage"], "rom_sha256": m["sha256"], "cells": len(classes) // 128 + (1 if len(classes) % 128 else 0),
               "strategies": {}}
        runs = [("BROAD", ("BROAD",), None), ("M3RB50", ("M3RB50",), sorted(saved[iid]))]
        runs += [(n, ("M3RB50",), sorted(regs[n])) for n in ("A_B50", "CV2E_B50", "M3RE4_B50", "M3R_FULL_RECALL", "A_FULL_RECALL")]
        for name, strat, cells in runs:
            t0 = time.time()
            r = ob.measure_image(str(cli), rom, m["sha256"], classes, seeds, cells, work / iid / ("w_" + name), SETTINGS["cc"],
                                 SETTINGS["opt"], SETTINGS["jobs"], SETTINGS["frames"], SETTINGS["timeout"], strategies=strat,
                                 cache=SETTINGS["cache"])
            rec = ob.public(r["strategies"].get(strat[0], {}))
            rec["selected_cells"] = None if cells is None else len(cells)
            rec["wall_seconds_total"] = round(time.time() - t0, 1)
            res["strategies"][name] = rec
            print(f"{iid} {name}: cpu={rec.get('total_cpu')} accepted={rec.get('accepted')}", file=sys.stderr, flush=True)
        import shutil
        for name, _s, _c in runs:
            shutil.rmtree(work / iid / ("w_" + name), ignore_errors=True)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(res, indent=1, sort_keys=True))
    return 0


# -------------------------------------------------------------------------------------------------------------------------- table
RUNTIME_COMPARE = ("ok", "outcome", "frames")


def cmd_table(a) -> int:
    """Aggregate-only C5 results: per strategy, totals and the lineage mean of the per-image ratio to same-session BROAD."""
    ob = bench()
    verify_plan()
    work = pathlib.Path(a.work).resolve()
    rx.guard([work])
    images = list(ob.GATE_IMAGES)
    per = {}
    for iid in images:
        p = work / iid / "c5.json"
        if not p.is_file():
            raise SystemExit(f"{iid}: not measured")
        per[iid] = json.loads(p.read_text())
    keys = ("total_cpu", "compile_cpu", "generated_bytes", "executable_bytes")
    rows = {}
    for name in STRATEGIES:
        acc = {"images": 0, "accepted": 0, "fallback_or_rejected": 0, "runtime_equal_to_broad": 0, "runtime_checked": 0,
               "selected_cells": 0, **{k: 0.0 for k in keys}}
        ratios = {k: {} for k in keys}
        for iid, r in per.items():
            rec, broad = r["strategies"].get(name, {}), r["strategies"].get("BROAD", {})
            acc["images"] += 1
            ok = bool(rec.get("accepted")) if name != "BROAD" else True
            acc["accepted"] += int(ok)
            acc["fallback_or_rejected"] += int(not ok)
            acc["selected_cells"] += rec.get("selected_cells") or 0
            if name != "BROAD" and rec.get("runtime") and broad.get("runtime"):
                acc["runtime_checked"] += 1
                acc["runtime_equal_to_broad"] += int(all(rec["runtime"].get(k) == broad["runtime"].get(k) for k in RUNTIME_COMPARE))
            for k in keys:
                v = rec.get(k) if ok else broad.get(k)       # a rejected admission falls back to broad (the bench's deployed semantics)
                if v is None:
                    continue
                acc[k] += v
                if broad.get(k):
                    ratios[k].setdefault(r["lineage"], []).append(v / broad[k])
        acc["lineage_mean_ratio_vs_broad"] = {k: round(sum(sum(x) / len(x) for x in d.values()) / len(d), 4) if d else None
                                              for k, d in ratios.items()}
        for k in keys:
            acc[k] = round(acc[k], 2)
        rows[name] = acc
    frozen = json.loads((HERE / "segarecomp_oracle_bench.results.json").read_text())
    context = {}
    for strat in ("BROAD", "INC", "M3RB50", "ORACLE_REGION", "ORACLE_INSTR"):
        tot = [im["strategies"][strat]["total_cpu"] for im in frozen["images"].values() if strat in im.get("strategies", {})
               and "total_cpu" in im["strategies"][strat]]
        context[strat] = {"images": len(tot), "total_cpu_sum": round(sum(tot), 1)}
    analysis = {}
    for model, task_dir, pat in (("A", T001, "A.C0.D*.json"), ("CV2", T005, "CV2.C0.D*.json")):
        recs = [json.loads(p.read_text()) for p in sorted((task_dir / "scores").glob(pat))]
        if recs:
            analysis[model] = {"fit_cpu_seconds_per_fit": round(sum(r["fit_cpu_seconds"] for r in recs) / len(recs), 1),
                               "inference_seconds_per_image": round(sum(r["inference_seconds"]["total"] for r in recs) /
                                                                    max(1, sum(r["inference_seconds"]["images"] for r in recs)), 3)}
    doc = {"schema": "segarecomp.xplat_economics.results.v1", "task": "SEG-076-T004", "checkpoint": "C5", "plan_sha256": plan_sha(),
           "settings": SETTINGS, "images": len(per), "strategies": rows, "seg074_frozen_context": context, "model_analysis_cost": analysis,
           "notes": ["ratios are per image against the same-session BROAD build, averaged within lineage then across lineages",
                     "a rejected admission is counted at its broad-fallback cost (deployed semantics)",
                     "*_FULL_RECALL regions are truth-informed diagnostics, not deployable policies",
                     "SEG-074 frozen totals are context from a different session and settings (cache), not a paired baseline"],
           "data_boundary": "aggregates only"}
    (pathlib.Path(a.results) if a.results else RESULTS).write_text(json.dumps(doc, indent=1, sort_keys=True) + "\n")
    print(json.dumps({k: {kk: v[kk] for kk in ("accepted", "total_cpu", "generated_bytes", "lineage_mean_ratio_vs_broad")} for k, v in rows.items()},
                     indent=1))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    pl = sub.add_parser("plan")
    pl.add_argument("--cli", required=True)
    pl.set_defaults(func=lambda a: (print(json.dumps({"plan_sha256": hashlib.sha256(rx.canonical(write_plan(pathlib.Path(a.cli).resolve()))).hexdigest()})), 0)[1])
    ck = sub.add_parser("check")
    ck.add_argument("--cli")
    ck.set_defaults(func=lambda a: (verify_plan(pathlib.Path(a.cli).resolve() if a.cli else None), print(json.dumps({"plan_sha256": plan_sha(), "check": "PASS"})), 0)[2])
    m = sub.add_parser("measure")
    m.add_argument("--cli", required=True)
    m.add_argument("--work", required=True)
    m.add_argument("--images", nargs="*")
    m.set_defaults(func=cmd_measure)
    t = sub.add_parser("table")
    t.add_argument("--work", required=True)
    t.add_argument("--results")
    t.set_defaults(func=cmd_table)
    a = ap.parse_args(argv)
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())
