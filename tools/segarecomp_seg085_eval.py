#!/usr/bin/env python3
"""SEG-085-T004 (ADR 0194): evaluation of B35 / B50 / ADAPT / DYN_SCORE and generated-native economics (OFFLINE, PRIVATE research code).

Policies and the calibration are the ones frozen by `segarecomp_seg085_dyn.plan.json` (verified before any cohort image is scored). Economics reuse SEG-074's
unchanged exact-code benchmark `segarecomp_oracle_bench.measure_image` (prune + admission validator + broad fallback + emitter + compile + runtime equality;
rejected work followed by a broad fallback is charged in full by that path). Nothing here fits a model except the one documented all-Genesis model-A fit that
reproduces the SEG-076 `A.C0.all` recipe bitwise (checked against the stored cross-platform scores).

Commands: final-scores | outcomes | econ | econ-table
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_ml_v3 as v3  # noqa: E402
import segarecomp_seg085_dyn as dyn  # noqa: E402

SCHEMA = "segarecomp.seg085.eval.v1"
HARNESS = dyn.HARNESS
CACHE = dyn.CACHE
COHORT_ROOT = CACHE / "data"
SETTINGS = {"cc": "clang", "opt": "O2", "jobs": 4, "frames": 120, "timeout": 3600, "cache": False}     # SEG-076 C5 settings, unchanged
POLICIES = ("B35", "B50", "ADAPT", "DYN_SCORE")


def cohort_dirs():
    return sorted(m.parent for m in COHORT_ROOT.rglob("meta.json") if "classes_sha256" in json.loads(m.read_text()))


def load_cohort():
    import segarecomp_ml_v3_train as tr
    import segarecomp_xplat_t005 as x5
    import segarecomp_seg085_cohort as co
    out = []
    for d in cohort_dirs():
        m = json.loads((d / "meta.json").read_text())
        co.refuse_reserved(m["sha256"])
        im = x5.attach(tr.Image(d), d)
        out.append(im)
    return out


def cmd_final_scores(a) -> int:
    """Fit model A on the eleven development lineages with the SEG-076 recipe, gate on bitwise reproduction of the stored cross-platform scores, score the cohort."""
    import numpy as np
    import segarecomp_xplat_transfer as xt
    import segarecomp_xplat_t005 as x5
    s84, xm, imgs, feats = dyn.load_images()
    X, y = dyn.train_matrix(np, imgs, feats, set(dyn.GENESIS))
    clf = s84.fit_arrays(X, y)
    # gate: the stored all-Genesis cross-platform scores of SEG-076 must be reproduced exactly (same data, same recipe)
    snap = HARNESS / ".cache" / "seg075" / "t003" / "snapshot"
    ref = HARNESS / ".cache" / "seg076" / "t001" / "scores" / "A.C0.all.xplat.npz"
    gate = {"checked": False}
    if snap.is_dir() and ref.is_file():
        stored = xt.load_npz(ref)
        ok = True
        n = 0
        for d in xt.xplat_dirs(snap, ("BattleIsleEditor", "EmuTOS", "amiga-realtime3d", "toslibc", "HelloAmi")):
            im = x5.attach(xt.load([d])[0], d)
            if im.id in stored:
                sc = s84.logit(clf, xm.a_features(im, xm.relocs_of(im)))
                ok &= bool(np.array_equal(sc, stored[im.id]))
                n += 1
        gate = {"checked": True, "images": n, "bitwise_equal": bool(ok)}
        if not ok:
            raise SystemExit("final model A does not reproduce the stored SEG-076 all-Genesis scores")
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    for im in load_cohort():
        t0 = time.process_time()
        sc = s84.logit(clf, xm.a_features(im, xm.relocs_of(im)))
        cpu = time.process_time() - t0
        np.save(out / (im.id.replace("/", "@@") + ".npy"), sc)
        (out / (im.id.replace("/", "@@") + ".json")).write_text(json.dumps({"id": im.id, "inference_cpu_seconds": round(cpu, 3), "gate": gate}))
        print(json.dumps({"id": im.id, "cells": len(sc), "inference_cpu": round(cpu, 2), "gate": gate}))
    return 0


def policy_params(nested: dict) -> dict:
    return {"T_final": nested["final"]["T"], "adapt_margin_final": nested.get("adapt_margin_final")}


def cmd_outcomes(a) -> int:
    import numpy as np
    nested = json.loads(pathlib.Path(a.nested).read_text())
    plan = dyn.verify_plan()
    T = nested["final"]["T"]
    margin = nested["final"]["adapt_margin_unchanged_protocol"]
    rows = {}
    for im in load_cohort():
        sc = np.load(pathlib.Path(a.scores) / (im.id.replace("/", "@@") + ".npy"))
        labels = [int(x) for x in im.labels]
        probs = [dyn.sigmoid(float(x)) for x in sc]
        have_u = bool(len(im.seeds))
        row = {"lineage": im.lineage, "cells": len(labels), "positive_cells": labels.count(1), "starts": len(im.starts), "masked_cells": labels.count(-1)}
        for pol in POLICIES:
            kw = {"threshold": T, "have_universe": have_u} if pol == "DYN_SCORE" else ({"margin": margin} if pol == "ADAPT" else {})
            row[pol] = dyn.policy_outcome(pol, sc, probs, im.seeds, labels, im.classes, **kw)
            if pol == "DYN_SCORE":
                r = dyn.dyn_select(sc, im.seeds, T, have_universe=have_u)
                row[pol]["cells_after_seeds"] = len(r["region"]) if r["status"] == "selected" else None
        rows[im.id] = row
        regions = {pol: sorted(dyn.dyn_select(sc, im.seeds, T)["region"]) if pol == "DYN_SCORE" else sorted(
            v3.policy_region(pol, [float(x) for x in sc], probs, set(im.seeds), margin if pol == "ADAPT" else None)["region"]) for pol in POLICIES}
        (pathlib.Path(a.scores) / (im.id.replace("/", "@@") + ".regions.json")).write_text(json.dumps(regions))
    out = {"schema": SCHEMA + ".cohort_outcomes", "plan_sha256": plan, "T": T, "adapt_margin": margin, "images": rows}
    pathlib.Path(a.out).write_text(json.dumps(out, indent=1, sort_keys=True))
    print(json.dumps({k: {p: (v[p]["status"], v[p]["retained"], v[p]["missed_starts"]) for p in POLICIES} for k, v in rows.items()}))
    return 0


def ob():
    import segarecomp_oracle_bench as o
    return o


def cmd_econ(a) -> int:
    """Measure BROAD plus each distinct non-broad region of the selected policies for the listed images with SEG-074's benchmark (cold, same session)."""
    o = ob()
    cli = pathlib.Path(a.cli).resolve()
    work = pathlib.Path(a.work).resolve()
    sets = json.loads(pathlib.Path(a.regions).read_text())         # {image id: {"dir": registry dir, "policies": {policy: cells list | null (abstain/unsupported)}}}
    for iid, spec in sets.items():
        out = work / iid.replace("/", "@@") / "econ.json"
        if out.exists():
            print(f"{iid}: done", file=sys.stderr)
            continue
        d = pathlib.Path(spec["dir"])
        m = json.loads((d / "meta.json").read_text())
        rom = pathlib.Path(m["image"])
        classes = (d / "classes.bin").read_bytes()
        seeds = json.loads((d / "seeds.json").read_text())
        distinct = {}
        for pol, cells in spec["policies"].items():
            if cells is not None:
                distinct.setdefault(tuple(cells), []).append(pol)
        res = {"sha256": m["sha256"], "strategies": {}, "policy_to_region": {}}
        t0 = time.time()
        try:
            r = o.measure_image(str(cli), rom, m["sha256"], classes, seeds, None, work / iid.replace("/", "@@") / "w_BROAD", SETTINGS["cc"], SETTINGS["opt"],
                            SETTINGS["jobs"], SETTINGS["frames"], SETTINGS["timeout"], strategies=("BROAD",), cache=SETTINGS["cache"])
        except Exception as e:                                   # the broad reference itself cannot be built: an economics coverage gap, recorded, never substituted
            res["coverage_gap"] = {"stage": "BROAD", "error": str(e)[:300]}
            (work / iid.replace("/", "@@")).mkdir(parents=True, exist_ok=True)
            out.write_text(json.dumps(res, indent=1, sort_keys=True))
            print(f"{iid}: COVERAGE GAP {res['coverage_gap']['error'][:80]}", file=sys.stderr, flush=True)
            continue
        res["strategies"]["BROAD"] = o.public(r["strategies"]["BROAD"])
        res["universe"], res["labelled_starts_in_universe"] = r["universe"], r["labelled_starts_in_universe"]
        for k, (cells, pols) in enumerate(sorted(distinct.items(), key=lambda kv: kv[1])):
            name = "+".join(pols)
            rr = o.measure_image(str(cli), rom, m["sha256"], classes, seeds, list(cells), work / iid.replace("/", "@@") / f"w_{k}", SETTINGS["cc"], SETTINGS["opt"],
                                 SETTINGS["jobs"], SETTINGS["frames"], SETTINGS["timeout"], strategies=("M3RB50",), cache=SETTINGS["cache"])
            rec = o.public(rr["strategies"].get("M3RB50", {}))
            rec["selected_cells"] = len(cells)
            res["strategies"][name] = rec
            for p in pols:
                res["policy_to_region"][p] = name
        res["wall_seconds_total"] = round(time.time() - t0, 1)
        import shutil
        for p in (work / iid.replace("/", "@@")).glob("w_*"):
            shutil.rmtree(p, ignore_errors=True)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(res, indent=1, sort_keys=True))
        print(f"{iid}: measured {list(res['strategies'])}", file=sys.stderr, flush=True)
    return 0


def cmd_econ_inc(a) -> int:
    """Same-benchmark measurement of the deployed incremental route (INC: native gen2 M3 + B35 admission) for the economics images."""
    o = ob()
    cli = pathlib.Path(a.cli).resolve()
    work = pathlib.Path(a.work).resolve()
    sets = json.loads(pathlib.Path(a.regions).read_text())
    for iid, spec in sets.items():
        out = work / iid.replace("/", "@@") / "inc.json"
        if out.exists():
            continue
        d = pathlib.Path(spec["dir"])
        m = json.loads((d / "meta.json").read_text())
        r = o.measure_image(str(cli), pathlib.Path(m["image"]), m["sha256"], (d / "classes.bin").read_bytes(), json.loads((d / "seeds.json").read_text()), None,
                            work / iid.replace("/", "@@") / "w_INC", SETTINGS["cc"], SETTINGS["opt"], SETTINGS["jobs"], SETTINGS["frames"], SETTINGS["timeout"],
                            strategies=("INC",), cache=False)
        import shutil
        shutil.rmtree(work / iid.replace("/", "@@") / "w_INC", ignore_errors=True)
        out.write_text(json.dumps(o.public(r["strategies"].get("INC", {})), indent=1, sort_keys=True))
        print(f"{iid}: INC measured", file=sys.stderr, flush=True)
    return 0


def cost_of(res: dict, policy: str, inc: dict | None = None):
    """Total build CPU of a policy for one image from the measured strategies. Abstention/unsupported = the same-session BROAD cost; a validator
    rejection followed by the broad fallback is charged in full (rejected plan CPU + BROAD). Returns (cpu, kind, record)."""
    broad = res["strategies"]["BROAD"]
    name = res["policy_to_region"].get(policy)
    if name is None:
        return broad["total_cpu"], "broad_abstain", broad
    rec = res["strategies"][name]
    if not rec.get("accepted", False) or "total_cpu" not in rec:
        return rec.get("plan_cpu", 0.0) + broad["total_cpu"], "rejected_then_broad", rec
    return rec["total_cpu"], "selective", rec


def cmd_econ_table(a) -> int:
    work = pathlib.Path(a.work).resolve()
    groups = {"independent_historical": ["h01@@ps4"], "development_historical": ["s1", "s2", "sk", "flicky", "sf1", "sf2", "ps2"], "development_sgdk": ["d05", "d06", "d07", "d09"]}
    out = {"schema": SCHEMA + ".economics", "settings": SETTINGS, "groups": {}, "images": {}}
    for g, ids in groups.items():
        agg = {p: 0.0 for p in ("BROAD",) + POLICIES[1:]}
        agg["INC"] = 0.0
        gen = {p: 0 for p in agg}
        n = 0
        for iid in ids:
            f = work / iid / "econ.json"
            if not f.is_file():
                continue
            res = json.loads(f.read_text())
            inc = json.loads((work / iid / "inc.json").read_text()) if (work / iid / "inc.json").is_file() else None
            row = {"universe": res.get("universe"), "labelled_starts_in_universe": res.get("labelled_starts_in_universe"), "policies": {}}
            row["BROAD"] = {"total_cpu": res["strategies"]["BROAD"]["total_cpu"], "compile_cpu": res["strategies"]["BROAD"]["compile_cpu"],
                            "generated_bytes": res["strategies"]["BROAD"]["generated_bytes"]}
            agg["BROAD"] += row["BROAD"]["total_cpu"]
            for p in ("B50", "ADAPT", "DYN_SCORE"):
                if p not in res["policy_to_region"] and not (p in ("DYN_SCORE", "ADAPT")):
                    continue
                cpu, kind, rec = cost_of(res, p)
                rt, rb = rec.get("runtime") or {}, res["strategies"]["BROAD"].get("runtime") or {}
                eq = None if (kind != "selective" or not rt.get("ok") or not rb.get("ok")) else all(rt.get(k) == rb.get(k) for k in ("outcome", "frames", "final_state", "coverage", "frame_stream"))
                row["policies"][p] = {"kind": kind, "total_cpu": round(cpu, 1), "c_outside_k": rec.get("c_outside_k"), "runtime_equal_to_broad": eq,
                                      "accepted": rec.get("accepted"), "selected_cells": rec.get("selected_cells"), "generated_bytes": rec.get("generated_bytes"),
                                      "compile_cpu": rec.get("compile_cpu"), "reason": rec.get("reason")}
                agg[p] += cpu
            if inc is not None and inc.get("total_cpu") is not None:
                ri, rb2 = inc.get("runtime") or {}, res["strategies"]["BROAD"].get("runtime") or {}
                row["INC"] = {"total_cpu": round(inc["total_cpu"], 1), "accepted": inc.get("accepted"),
                              "runtime_equal_to_broad": (all(ri.get(k) == rb2.get(k) for k in ("outcome", "frames", "final_state", "coverage", "frame_stream")) if ri.get("ok") and rb2.get("ok") else None)}
                agg["INC"] += inc["total_cpu"]
            out["images"][iid] = row
            n += 1
        if n:
            excl = [i for i in ids if i in out["images"] and any((v.get("c_outside_k") or 0) > 0 for v in out["images"][i]["policies"].values())]
            ex = {}
            if excl:
                ex = {k: round(sum(out["images"][i]["BROAD"]["total_cpu"] if k == "BROAD" else (out["images"][i]["INC"]["total_cpu"] if k == "INC" else out["images"][i]["policies"][k]["total_cpu"])
                                   for i in ids if i in out["images"] and i not in excl), 1) for k in ("ADAPT", "B50", "BROAD", "INC")}
            out["groups"][g] = {"images_with_missed_starts_after_closure": excl, "pooled_excluding_those_images": ex, "images_in_group": len(ids),"images": n, "pooled_total_cpu": {k: round(v, 1) for k, v in agg.items() if v},
                                "ratio_to_B50": {k: round(v / agg["B50"], 4) for k, v in agg.items() if v and agg["B50"]}}
    pathlib.Path(a.out).write_text(json.dumps(out, indent=1, sort_keys=True))
    print(json.dumps(out["groups"], sort_keys=True))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("final-scores")
    f.add_argument("--out", default=str(CACHE / "final"))
    f.set_defaults(func=cmd_final_scores)
    o_ = sub.add_parser("outcomes")
    o_.add_argument("--nested", default=str(CACHE / "nested.json"))
    o_.add_argument("--scores", default=str(CACHE / "final"))
    o_.add_argument("--out", default=str(HERE / "segarecomp_seg085_eval.cohort.json"))
    o_.set_defaults(func=cmd_outcomes)
    e = sub.add_parser("econ")
    e.add_argument("--cli", required=True)
    e.add_argument("--regions", required=True)
    e.add_argument("--work", required=True)
    e.set_defaults(func=cmd_econ)
    ei = sub.add_parser("econ-inc")
    ei.add_argument("--cli", required=True)
    ei.add_argument("--regions", required=True)
    ei.add_argument("--work", required=True)
    ei.set_defaults(func=cmd_econ_inc)
    et = sub.add_parser("econ-table")
    et.add_argument("--work", required=True)
    et.add_argument("--out", default=str(HERE / "segarecomp_seg085_eval.economics.json"))
    et.set_defaults(func=cmd_econ_table)
    a = ap.parse_args(argv)
    return a.func(a)


if __name__ == "__main__":
    raise SystemExit(main())
