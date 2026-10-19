#!/usr/bin/env python3
"""SEG-075-T004 (ADR 0162): cross-platform transfer experiments under the frozen T003 design (OFFLINE, private research environment).

Every protocol definition (conditions, folds, models, metrics, decision rules) is read from the committed, digest-verified design plan
(tools/segarecomp_xplat_design.plan.json); this tool refuses to run when it differs. Training uses the UNCHANGED SEG-072 recipes
(`segarecomp_ml_v3_train`: train_m3r / fit_m4 / fit_m1 and their scorers); conditions differ only in the training image list.

  reproduce  bitwise check that the preserved SEG-072 Genesis-only fold scores reproduce in this environment (one M3-R fold, one M4-Dense
             seed-1 fold); the C0 reuse rule of the design depends on it
  fit        one fit: --model M3R|M4|M1 --condition C0|C1|C2 --fold <Genesis lineage> | xplat:<lineage> | all [--seed]; scores the held
             images; private scores + a JSON record (training ids, seconds, environment)
  jobs       print the pre-registered job list (for a parallel farm)
  table      per-image / per-lineage metrics for every model x condition from the stored scores, the transfer classification by the
             pre-registered rule, and the sanitized results document (aggregates only)

Sealed (reserved) lineages are refused by the design barrier; nothing here reads them.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import pathlib
import platform
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_ml_v3 as v3  # noqa: E402
import segarecomp_xplat_design as design  # noqa: E402

RESULTS = HERE / "segarecomp_xplat_transfer.results.json"
SEG072_SCORES = pathlib.Path(os.environ.get("SEG072_ROOT", "/root/dev/segarecomp-harness/.cache/seg072")) / "scores"
GENESIS_LINEAGES = ["D01", "D02", "D03", "D04", "D05", "D06", "D07", "D09", "D10", "D11", "D12"]
POLICIES = ("B35", "B50", "B60")


def tr():
    import segarecomp_ml_v3_train as t
    return t


def environment() -> dict:
    import numpy
    import sklearn
    import torch
    return {"python": platform.python_version(), "numpy": numpy.__version__, "sklearn": sklearn.__version__, "torch": torch.__version__,
            "device": "cpu", "deterministic": True, "machine": platform.machine()}


# ---------------------------------------------------------------------------------------------------------------- image lists
def genesis_dirs(lineages) -> list[pathlib.Path]:
    return tr().registry(set(lineages))


def xplat_dirs(snapshot: pathlib.Path, lineages, all_samples: bool = False) -> list[pathlib.Path]:
    return design.dev_sample_dirs(snapshot, list(lineages), representatives_only=not all_samples)


def fold_sets(plan: dict, condition: str, fold: str, snapshot: pathlib.Path) -> tuple[list[pathlib.Path], list[pathlib.Path], str]:
    """(training dirs, held dirs, held lineage) of one pre-registered fit."""
    xtrain = plan["conditions"][condition]["xplat_training"]
    if fold == "all":
        return genesis_dirs(GENESIS_LINEAGES) + xplat_dirs(snapshot, xtrain), [], ""
    if fold.startswith("xplat:"):
        x = fold.split(":", 1)[1]
        if x not in plan["conditions"]["C2"]["xplat_training"]:
            raise SystemExit(f"{x}: not a cross-platform development lineage")
        rest = [l for l in xtrain if l != x]
        return genesis_dirs(GENESIS_LINEAGES) + xplat_dirs(snapshot, rest), xplat_dirs(snapshot, [x], all_samples=True), x
    if fold not in GENESIS_LINEAGES:
        raise SystemExit(f"unknown fold {fold}")
    train = genesis_dirs([l for l in GENESIS_LINEAGES if l != fold]) + xplat_dirs(snapshot, xtrain)
    return train, genesis_dirs([fold]), fold


def load(dirs):
    t = tr()
    out = []
    for d in dirs:
        design.refuse_sealed([d])
        out.append(t.Image(d))
    return out


# ------------------------------------------------------------------------------------------------------------------------- fit
def score_path(out: pathlib.Path, model: str, condition: str, fold: str, seed: int | None) -> pathlib.Path:
    f = fold.replace(":", "-")
    return out / "scores" / (f"{model}.{condition}.{f}" + (f".s{seed}" if seed is not None else "") + ".npz")


def run_fit(model: str, condition: str, fold: str, seed: int | None, snapshot: pathlib.Path, out: pathlib.Path) -> dict:
    plan = design.verify_plan()
    v3.verify_plan()
    t = tr()
    train_dirs, held_dirs, held = fold_sets(plan, condition, fold, snapshot)
    if fold == "all":
        # the all-development model scores every cross-platform development sample (C0-all is the xplat_lolo baseline)
        held_dirs = xplat_dirs(snapshot, plan["conditions"]["C2"]["xplat_training"], all_samples=True)
    train, held_imgs = load(train_dirs), load(held_dirs)
    if held and any(im.lineage == held for im in train):
        raise SystemExit("held-out lineage in training data")
    t0 = time.time()
    log = (lambda m: print(f"[{model}/{condition}/{fold}] {m}", flush=True))
    if model == "M3R":
        import segarecomp_ml_region_gen2_train as g2
        if seed is not None and seed != g2.RECIPE["seed"]:
            g2.RECIPE = dict(g2.RECIPE, seed=seed)  # pre-registered noise replicate only (C0, seed 50)
        m = t.train_m3r(train, log)
        scores = {im.id: t.score_m3(m, im) for im in held_imgs}
        rec_seed = seed if seed is not None else 49
    elif model == "M4":
        import torch
        torch.set_num_threads(t.M4["threads"])
        torch.use_deterministic_algorithms(True)
        m = t.fit_m4(train, seed, torch.device("cpu"), log=log)
        scores = {im.id: t.infer_m4(m, im) for im in held_imgs}
        rec_seed = seed
    elif model == "M1":
        clf = t.fit_m1(train)
        scores = {im.id: t.score_m1(clf, im) for im in held_imgs}
        rec_seed = 49
    else:
        raise SystemExit("unknown model")
    path = score_path(out, model, condition, fold, seed if model == "M4" or (model == "M3R" and seed not in (None, 49)) else None)
    if fold == "all":
        path = path.with_name(path.stem + ".xplat.npz")
    t.save_scores(path, scores)
    if fold == "all":
        # final development models are preserved for T005/T006 (no retraining needed to reuse them)
        if model in ("M3R", "M4"):
            import torch
            torch.save(m.state_dict(), path.with_suffix(".pt"))
        else:
            import pickle
            path.with_suffix(".pkl").write_bytes(pickle.dumps(clf))
    rec = {"model": model, "condition": condition, "fold": fold, "seed": rec_seed, "seconds": round(time.time() - t0, 1),
           "train_images": sorted(im.id for im in train), "held_images": sorted(im.id for im in held_imgs),
           "train_lineages": sorted({im.lineage for im in train}), "plan_sha256": hashlib.sha256(design.PLAN.read_bytes()).hexdigest(),
           "environment": environment()}
    path.with_suffix(".json").write_text(json.dumps(rec, indent=1, sort_keys=True))
    print(json.dumps({k: rec[k] for k in ("model", "condition", "fold", "seed", "seconds")}), flush=True)
    return rec


def cmd_fit(a) -> int:
    run_fit(a.model, a.condition, a.fold, a.seed, pathlib.Path(a.snapshot).resolve(), pathlib.Path(a.out).resolve())
    return 0


def cmd_reproduce(a) -> int:
    """Retrain one preserved SEG-072 fold per model in condition C0 and compare bitwise."""
    import numpy as np
    out = pathlib.Path(a.out).resolve()
    snap = pathlib.Path(a.snapshot).resolve()
    res = {}
    for model, seed, ref in (("M3R", None, SEG072_SCORES / f"M3R.{a.fold}.npz"), ("M4", 1, SEG072_SCORES / f"M4.{a.fold}.s1.npz")):
        if a.only and model not in a.only:
            continue
        run_fit(model, "C0", a.fold, seed, snap, out / "reproduce")
        mine = np.load(score_path(out / "reproduce", model, "C0", a.fold, seed))
        theirs = np.load(ref)
        same = sorted(mine.files) == sorted(theirs.files) and all(np.array_equal(mine[k], theirs[k]) for k in mine.files)
        res[model] = {"fold": a.fold, "bitwise_identical": bool(same), "images": len(mine.files)}
    (out / "reproduce.json").write_text(json.dumps(res, indent=1, sort_keys=True))
    print(json.dumps(res))
    return 0 if all(r["bitwise_identical"] for r in res.values()) else 1


def jobs(plan: dict, c0_reuse: bool) -> list[str]:
    """The pre-registered fit list (one command line per fit)."""
    out = []

    def add(model, cond, fold, seed=None):
        out.append(f"{model} {cond} {fold}" + (f" {seed}" if seed is not None else ""))

    for cond in ("C0", "C1", "C2"):
        for fold in GENESIS_LINEAGES:
            if cond == "C0" and c0_reuse:
                continue
            add("M3R", cond, fold)
            add("M1", cond, fold)
            if cond in ("C0", "C2"):
                for s in (1, 2, 3):
                    add("M4", cond, fold, s)
    for fold in GENESIS_LINEAGES:
        add("M3R", "C0", fold, 50)
    for cond in ("C0", "C2"):
        add("M3R", cond, "all")
        add("M1", cond, "all")
        for s in (1, 2, 3):
            add("M4", cond, "all", s)
    for x in plan["conditions"]["C2"]["xplat_training"]:
        add("M3R", "C2", f"xplat:{x}")
        add("M1", "C2", f"xplat:{x}")
        for s in (1, 2, 3):
            add("M4", "C2", f"xplat:{x}", s)
    return out


def cmd_jobs(a) -> int:
    for j in jobs(design.verify_plan(), a.c0_reuse):
        print(j)
    return 0


# ----------------------------------------------------------------------------------------------------------------------- table
def load_npz(path: pathlib.Path) -> dict:
    import numpy as np
    with np.load(path) as z:
        return {k.replace("@@", "/"): z[k] for k in z.files}


def cell_scores(model_cfg: str, cond: str, fold: str, out: pathlib.Path, c0_reuse: bool, sizes: dict) -> dict | None:
    """{image id: cell scores} for one model configuration and fit (M4 = mean of the three seeds' word logits, then cell max)."""
    import numpy as np
    t = tr()
    f = fold.replace(":", "-")
    base = SEG072_SCORES if (cond == "C0" and c0_reuse and fold in GENESIS_LINEAGES) else out / "scores"
    if model_cfg in ("M3_R", "M1_HGB"):
        name = {"M3_R": "M3R", "M1_HGB": "M1"}[model_cfg]
        p = (base / f"{name}.{fold}.npz") if base == SEG072_SCORES else base / f"{name}.{cond}.{f}.npz"
        return {k: v.astype(np.float64) for k, v in load_npz(p).items()} if p.exists() else None
    if model_cfg == "M3_R@s50":
        p = out / "scores" / f"M3R.{cond}.{f}.s50.npz"
        return {k: v.astype(np.float64) for k, v in load_npz(p).items()} if p.exists() else None
    if model_cfg.startswith("M4_DENSE"):
        cell = int(model_cfg.split("@")[1])
        ps = [(base / f"M4.{fold}.s{s}.npz") if base == SEG072_SCORES else base / f"M4.{cond}.{f}.s{s}.npz" for s in (1, 2, 3)]
        if not all(p.exists() for p in ps):
            return None
        arrs = [load_npz(p) for p in ps]
        return {iid: t.cell_max(np.mean([a[iid] for a in arrs], axis=0).astype(np.float32), sizes[iid], cell) for iid in arrs[0]}
    raise SystemExit(model_cfg)


def image_metrics(classes: bytes, seeds: set, scores, cell: int = 128) -> dict:
    """Per-image metrics from cell scores (pure given numbers; unit-tested): FRF, residual FRF, misses / negative cells admitted at
    each fixed budget, short/mixed-stratum misses, AUROC over labelled cells."""
    labels = v3.cell_labels(classes, cell)
    starts = v3.starts_of(classes)
    s = [float(x) for x in scores]
    f = v3.frf(s, labels)
    out = {"cells": len(labels), "positive_cells": labels.count(1), "negative_cells": labels.count(0), "starts": len(starts),
           "frf": f["frf"], "residual_frf": v3.residual_frf(s, labels, seeds), "density": labels.count(1) / len(labels)}
    stratum = set()
    for c in range(len(labels)):
        if labels[c] == 1:
            chunk = classes[c * cell:(c + 1) * cell]
            if chunk.count(b"I") <= 8 or b"D" in chunk:
                stratum.add(c)
    for pol in POLICIES:
        reg = v3.policy_region(pol, s, None, seeds)["region"]
        out[f"misses_{pol}"] = v3.misses(starts, reg, cell)
        out[f"missed_cells_{pol}"] = sum(1 for c in range(len(labels)) if labels[c] == 1 and c not in reg)
        out[f"stratum_missed_cells_{pol}"] = sum(1 for c in stratum if c not in reg)
        out[f"negative_admitted_{pol}"] = sum(1 for c in reg if labels[c] == 0)
        out[f"retained_{pol}"] = len(reg) / len(labels)
    out["stratum_cells"] = len(stratum)
    pos = [x for x, y in zip(s, labels) if y == 1]
    neg = [x for x, y in zip(s, labels) if y == 0]
    out["auroc"] = auroc(pos, neg)
    return out


def auroc(pos, neg):
    if not pos or not neg:
        return None
    allv = sorted([(v, 1) for v in pos] + [(v, 0) for v in neg])
    rank_sum, i = 0.0, 0
    while i < len(allv):
        j = i
        while j < len(allv) and allv[j][0] == allv[i][0]:
            j += 1
        avg = (i + j + 1) / 2
        rank_sum += avg * sum(1 for k in range(i, j) if allv[k][1] == 1)
        i = j
    n1, n0 = len(pos), len(neg)
    return round((rank_sum - n1 * (n1 + 1) / 2) / (n1 * n0), 6)


def lineage_rows(per_image: dict, lineage_of: dict, reps: set | None = None) -> dict:
    """Per-lineage aggregation: max FRF over the lineage's images, summed misses / admissions, mean AUROC (representatives only for
    cross-platform lineages, so near-duplicates never weight a lineage)."""
    out = {}
    for iid, m in per_image.items():
        if reps is not None and iid.startswith("xp/") and iid not in reps:
            continue
        L = out.setdefault(lineage_of[iid], {"images": 0, "max_frf": 0.0, "max_residual_frf": 0.0, "aurocs": [], "positive_cells": 0,
                                             "stratum_cells": 0, **{f"{k}_{p}": 0 for p in POLICIES for k in
                                                                     ("misses", "missed_cells", "stratum_missed_cells", "negative_admitted")}})
        L["images"] += 1
        L["max_frf"] = max(L["max_frf"], m["frf"])
        L["max_residual_frf"] = max(L["max_residual_frf"], m["residual_frf"])
        L["positive_cells"] += m["positive_cells"]
        L["stratum_cells"] += m["stratum_cells"]
        if m["auroc"] is not None:
            L["aurocs"].append(m["auroc"])
        for p in POLICIES:
            for k in ("misses", "missed_cells", "stratum_missed_cells", "negative_admitted"):
                L[f"{k}_{p}"] += m[f"{k}_{p}"]
    for L in out.values():
        a = L.pop("aurocs")
        L["mean_auroc"] = round(sum(a) / len(a), 6) if a else None
        L["max_frf"] = round(L["max_frf"], 6)
        L["max_residual_frf"] = round(L["max_residual_frf"], 6)
    return dict(sorted(out.items()))


def classify_transfer(c0: dict, cx: dict, tau: float, insufficient: bool) -> dict:
    """Pre-registered transfer rule (pure; unit-tested). c0/cx: {lineage: {max_frf, misses_B50}}."""
    if insufficient:
        return {"class": "EVIDENCE_INSUFFICIENT"}
    common = sorted(set(c0) & set(cx))
    d = {l: cx[l]["max_frf"] - c0[l]["max_frf"] for l in common}
    d_mean = sum(d.values()) / len(d)
    miss0 = sum(c0[l]["misses_B50"] for l in common)
    missx = sum(cx[l]["misses_B50"] for l in common)
    worst = max(d.values())
    if d_mean <= -tau and worst <= tau and missx <= miss0:
        cls = "TRANSFER_HELPS"
    elif d_mean >= tau or worst > 2 * tau or missx > miss0:
        cls = "TRANSFER_HARMS"
    else:
        cls = "TRANSFER_NEUTRAL"
    return {"class": cls, "d_mean_frf": round(d_mean, 6), "worst_lineage_d_frf": round(worst, 6), "misses_B50": [miss0, missx],
            "per_lineage_d_frf": {l: round(v, 6) for l, v in d.items()}}


def cmd_table(a) -> int:
    plan = design.verify_plan()
    out = pathlib.Path(a.out).resolve()
    snap = pathlib.Path(a.snapshot).resolve()
    t = tr()
    gdirs = genesis_dirs(GENESIS_LINEAGES)
    xdirs = xplat_dirs(snap, plan["conditions"]["C2"]["xplat_training"], all_samples=True)
    reps = {json.loads((d / "meta.json").read_text())["id"] for d in xplat_dirs(snap, plan["conditions"]["C2"]["xplat_training"])}
    meta = {}
    for d in gdirs + xdirs:
        m = json.loads((d / "meta.json").read_text())
        meta[m["id"]] = {"dir": d, "lineage": m["lineage"], "size": m["size"]}
    sizes = {k: v["size"] for k, v in meta.items()}
    lineage_of = {k: v["lineage"] for k, v in meta.items()}
    cache = {}

    def per_image(iid, scores, cell):
        key = (iid, cell)
        if key not in cache:
            d = meta[iid]["dir"]
            cache[key] = ((d / "classes.bin").read_bytes(), set(json.loads((d / "seeds.json").read_text())))
        cls, seeds = cache[key]
        if cell != 128:
            seeds = {s * 128 // cell for s in seeds}
        return image_metrics(cls, seeds, scores, cell)

    configs = ["M3_R", "M1_HGB", "M4_DENSE@128", "M4_DENSE@64"]
    tables = {}
    for cfg in configs + ["M3_R@s50"]:
        for cond in ("C0", "C1", "C2"):
            if cfg == "M3_R@s50" and cond != "C0":
                continue
            rows = {}
            for fold in GENESIS_LINEAGES:
                sc = cell_scores(cfg, cond, fold, out, a.c0_reuse, sizes)
                if sc is None:
                    continue
                cell = int(cfg.split("@")[1]) if cfg.startswith("M4") else 128
                for iid, s in sc.items():
                    rows[iid] = per_image(iid, s, cell)
            xrows = {}
            for x in plan["conditions"]["C2"]["xplat_training"]:
                src_cond, fold = ("C2", f"xplat:{x}") if cond == "C2" else ("C0", "all") if cond == "C0" else (None, None)
                if src_cond is None:
                    continue
                if fold == "all":
                    sc = _all_model_scores(cfg, out, xdirs, x)
                else:
                    sc = cell_scores(cfg, src_cond, fold, out, False, sizes)
                if sc is None:
                    continue
                cell = int(cfg.split("@")[1]) if cfg.startswith("M4") else 128
                for iid, s in sc.items():
                    if lineage_of.get(iid) == x:
                        xrows[iid] = per_image(iid, s, cell)
            if rows or xrows:
                tables[f"{cfg}|{cond}"] = {"genesis": lineage_rows(rows, lineage_of) if rows else {},
                                           "xplat": lineage_rows(xrows, lineage_of, reps) if xrows else {},
                                           "genesis_images": len(rows), "xplat_images": len(xrows)}
    # noise floor and decisions
    g0, g50 = tables.get("M3_R|C0", {}).get("genesis", {}), tables.get("M3_R@s50|C0", {}).get("genesis", {})
    common = sorted(set(g0) & set(g50))
    diffs = [abs(g0[l]["max_frf"] - g50[l]["max_frf"]) for l in common]
    tau = max(0.01, 2 * (sum(diffs) / len(diffs))) if diffs else None
    insufficient = tau is None or tau > 0.05 or len(common) < len(GENESIS_LINEAGES)
    decisions = {}
    for cfg in ("M3_R", "M4_DENSE@128", "M4_DENSE@64", "M1_HGB"):
        for cond in ("C1", "C2"):
            k0, kx = f"{cfg}|C0", f"{cfg}|{cond}"
            if k0 in tables and kx in tables and tables[kx]["genesis"]:
                decisions[f"{cfg}|{cond}"] = classify_transfer(tables[k0]["genesis"], tables[kx]["genesis"], tau or 1.0, insufficient)
    overall = decisions.get("M3_R|C2", {"class": "EVIDENCE_INSUFFICIENT"})["class"]
    summary = {}
    for k, v in tables.items():
        g = v["genesis"]
        x = v["xplat"]
        summary[k] = {"genesis_mean_frf": round(sum(r["max_frf"] for r in g.values()) / len(g), 6) if g else None,
                      "genesis_worst_frf": max((r["max_frf"] for r in g.values()), default=None),
                      "genesis_mean_density": None,
                      "genesis_misses_B50": sum(r["misses_B50"] for r in g.values()) if g else None,
                      "genesis_negative_admitted_B50": sum(r["negative_admitted_B50"] for r in g.values()) if g else None,
                      "xplat_mean_frf": round(sum(r["max_frf"] for r in x.values()) / len(x), 6) if x else None,
                      "xplat_misses_B50": sum(r["misses_B50"] for r in x.values()) if x else None}
    doc = {"schema": "segarecomp.xplat_transfer.results.v1", "task": "SEG-075-T004", "adr": "0162",
           "plan_sha256": hashlib.sha256(design.PLAN.read_bytes()).hexdigest(), "c0_reuse": a.c0_reuse,
           "noise": {"tau": tau, "per_lineage_abs_diff_seed49_seed50": dict(zip(common, [round(x, 6) for x in diffs])),
                     "insufficient": insufficient},
           "summary": summary, "decisions": decisions, "transfer_class": overall, "tables": tables,
           "data_boundary": "aggregates only: per-lineage FRF, counts and AUROC; no per-cell score, address or image content"}
    target = pathlib.Path(a.results) if a.results else RESULTS
    target.write_text(json.dumps(doc, indent=1, sort_keys=True) + "\n")
    print(json.dumps({"transfer_class": overall, "tau": tau, "summary": summary, "decisions": {k: v["class"] for k, v in decisions.items()}},
                     indent=1))
    return 0


def _all_model_scores(cfg: str, out: pathlib.Path, xdirs, x: str):
    """C0-all scores of the cross-platform lineage x (the C0 'all' models scored every cross-platform development sample)."""
    import numpy as np
    t = tr()
    name = {"M3_R": "M3R.C0.all", "M1_HGB": "M1.C0.all"}.get(cfg)
    if cfg == "M3_R@s50":
        return None
    if name:
        p = out / "scores" / f"{name}.xplat.npz"
        return {k: v.astype(np.float64) for k, v in load_npz(p).items()} if p.exists() else None
    cell = int(cfg.split("@")[1])
    ps = [out / "scores" / f"M4.C0.all.s{s}.xplat.npz" for s in (1, 2, 3)]
    if not all(p.exists() for p in ps):
        return None
    arrs = [load_npz(p) for p in ps]
    sizes = {json.loads((d / "meta.json").read_text())["id"]: json.loads((d / "meta.json").read_text())["size"] for d in xdirs}
    return {iid: t.cell_max(np.mean([a[iid] for a in arrs], axis=0).astype(np.float32), sizes[iid], cell) for iid in arrs[0]}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name, fn in (("fit", cmd_fit), ("reproduce", cmd_reproduce), ("jobs", cmd_jobs), ("table", cmd_table)):
        p = sub.add_parser(name)
        p.set_defaults(func=fn)
        if name in ("fit", "reproduce", "table"):
            p.add_argument("--snapshot", required=True)
            p.add_argument("--out", required=True)
        if name == "fit":
            p.add_argument("--model", required=True, choices=("M3R", "M4", "M1"))
            p.add_argument("--condition", required=True, choices=("C0", "C1", "C2"))
            p.add_argument("--fold", required=True)
            p.add_argument("--seed", type=int)
        if name == "reproduce":
            p.add_argument("--fold", default="D05")
            p.add_argument("--only", action="append")
        if name in ("jobs", "table"):
            p.add_argument("--c0-reuse", action="store_true")
        if name == "table":
            p.add_argument("--results")
    a = ap.parse_args(argv)
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())
