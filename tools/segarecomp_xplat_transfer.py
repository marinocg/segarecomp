#!/usr/bin/env python3
"""SEG-075-T004 (ADR 0161/0162): cross-platform transfer experiments under the frozen T003 design (OFFLINE, private research environment).

The protocol (conditions, folds, models, metrics, decision rules) is the committed, digest-verified design plan; this file and the SEG-072
code it calls are bound into that plan by digest (`segarecomp_xplat_design.verify_plan` refuses any drift). Training uses the UNCHANGED
SEG-072 recipes (`segarecomp_ml_v3_train`: train_m3r / fit_m4 / fit_m1 and their scorers); conditions differ only in the training list.

  reproduce  retrain one preserved SEG-072 Genesis-only fold of EACH of M3_R, M4_DENSE seed 1 and M1_HGB and compare bit for bit; writes
             reproduce.json bound to the plan digest. C0 fold scores are reused only when all three are identical
  fit        one fit: --model M3R|M4|M1 --condition C0|C1|C2 --fold <Genesis lineage> | xplat:<lineage> | all [--seed]
  jobs       the pre-registered fit list (C0 Genesis folds included unless reproduce.json passes)
  table      per-image / per-lineage metrics for every model x condition, completeness, noise (tau, miss allowances), transfer classes,
             the ordering records and complete budgets; sanitized results document (aggregates only)

Sealed (reserved) lineages are refused by the design barrier; nothing here reads them.
"""
from __future__ import annotations

import argparse
import hashlib
import json
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
CONFIGS = ("M1_HGB", "M3_R", "M4_DENSE@128", "M4_DENSE@64")
FITTED_CONDITIONS = {"M1_HGB": ("C0", "C1", "C2"), "M3_R": ("C0", "C1", "C2"), "M4_DENSE@128": ("C0", "C2"), "M4_DENSE@64": ("C0", "C2")}


def tr():
    import segarecomp_ml_v3_train as t
    return t


def plan_sha() -> str:
    return hashlib.sha256(design.PLAN.read_bytes()).hexdigest()


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
    """(training dirs, scored dirs, held lineage) of one pre-registered fit."""
    xtrain = plan["conditions"][condition]["xplat_training"]
    dev_x = plan["conditions"]["C2"]["xplat_training"]
    if fold == "all":
        # the all-development model scores every cross-platform development representative (C0-all is the xplat_lolo baseline)
        return genesis_dirs(GENESIS_LINEAGES) + xplat_dirs(snapshot, xtrain), xplat_dirs(snapshot, dev_x), ""
    if fold.startswith("xplat:"):
        x = fold.split(":", 1)[1]
        if x not in dev_x or condition != "C2":
            raise SystemExit(f"{fold}: xplat_lolo folds exist for C2 and the cross-platform development lineages only")
        rest = [l for l in xtrain if l != x]
        return genesis_dirs(GENESIS_LINEAGES) + xplat_dirs(snapshot, rest), xplat_dirs(snapshot, [x]), x
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
    stem = f"{model}.{condition}.{f}" + (f".s{seed}" if seed is not None else "")
    return out / "scores" / (stem + (".xplat" if fold == "all" else "") + ".npz")


def run_fit(model: str, condition: str, fold: str, seed: int | None, snapshot: pathlib.Path, out: pathlib.Path) -> dict:
    plan = design.verify_plan()
    v3.verify_plan()
    t = tr()
    if model == "M4" and seed not in (1, 2, 3):
        raise SystemExit("M4 requires --seed 1|2|3")
    if model == "M3R" and seed not in (None, 49, 50):
        raise SystemExit("M3R seeds: 49 (recipe) or 50 (C0 noise replicate)")
    if model == "M3R" and seed == 50 and condition != "C0":
        raise SystemExit("the seed-50 replicate is pre-registered for C0 only")
    train_dirs, held_dirs, held = fold_sets(plan, condition, fold, snapshot)
    train, held_imgs = load(train_dirs), load(held_dirs)
    if held and any(im.lineage == held for im in train):
        raise SystemExit("held-out lineage in training data")
    t0 = time.time()
    log = (lambda m: print(f"[{model}/{condition}/{fold}] {m}", flush=True))
    weights = None
    if model == "M3R":
        import segarecomp_ml_region_gen2_train as g2
        saved = g2.RECIPE
        try:
            if seed == 50:
                g2.RECIPE = dict(saved, seed=50)
            m = t.train_m3r(train, log)
        finally:
            g2.RECIPE = saved
        scores = {im.id: t.score_m3(m, im) for im in held_imgs}
        weights = m
    elif model == "M4":
        import torch
        torch.set_num_threads(t.M4["threads"])
        torch.use_deterministic_algorithms(True)
        m = t.fit_m4(train, seed, torch.device("cpu"), log=log)
        scores = {im.id: t.infer_m4(m, im) for im in held_imgs}
        weights = m
    elif model == "M1":
        clf = t.fit_m1(train)
        scores = {im.id: t.score_m1(clf, im) for im in held_imgs}
    else:
        raise SystemExit("unknown model")
    path = score_path(out, model, condition, fold, seed if (model == "M4" or seed == 50) else None)
    t.save_scores(path, scores)
    if fold == "all":
        # final development models are preserved for T005/T006 (reusable without retraining)
        if weights is not None:
            import torch
            torch.save(weights.state_dict(), path.with_suffix(".pt"))
        else:
            import pickle
            path.with_suffix(".pkl").write_bytes(pickle.dumps(clf))
    rec = {"model": model, "condition": condition, "fold": fold, "seed": seed if seed is not None else 49, "seconds": round(time.time() - t0, 1),
           "train_images": sorted(im.id for im in train), "held_images": sorted(im.id for im in held_imgs),
           "train_lineages": sorted({im.lineage for im in train}), "plan_sha256": plan_sha(), "environment": environment()}
    path.with_suffix(".json").write_text(json.dumps(rec, indent=1, sort_keys=True))
    print(json.dumps({k: rec[k] for k in ("model", "condition", "fold", "seed", "seconds")}), flush=True)
    return rec


def cmd_fit(a) -> int:
    run_fit(a.model, a.condition, a.fold, a.seed, pathlib.Path(a.snapshot).resolve(), pathlib.Path(a.out).resolve())
    return 0


def cmd_reproduce(a) -> int:
    """Retrain one preserved SEG-072 Genesis-only fold per model (C0) and compare bit for bit; reproduce.json gates C0 reuse."""
    import numpy as np
    out = pathlib.Path(a.out).resolve()
    snap = pathlib.Path(a.snapshot).resolve()
    res = {"plan_sha256": plan_sha(), "fold": a.fold, "models": {}}
    for model, seed, ref in (("M3R", None, SEG072_SCORES / f"M3R.{a.fold}.npz"), ("M4", 1, SEG072_SCORES / f"M4.{a.fold}.s1.npz"),
                             ("M1", None, SEG072_SCORES / f"M1.{a.fold}.npz")):
        run_fit(model, "C0", a.fold, seed, snap, out / "reproduce")
        mine = np.load(score_path(out / "reproduce", model, "C0", a.fold, seed))
        theirs = np.load(ref)
        same = sorted(mine.files) == sorted(theirs.files) and all(np.array_equal(mine[k], theirs[k]) for k in mine.files)
        res["models"][model] = {"bitwise_identical": bool(same), "images": len(mine.files)}
    res["c0_reuse_admitted"] = all(r["bitwise_identical"] for r in res["models"].values())
    (out / "reproduce.json").write_text(json.dumps(res, indent=1, sort_keys=True))
    print(json.dumps(res))
    return 0 if res["c0_reuse_admitted"] else 1


def c0_reuse(out: pathlib.Path) -> bool:
    """C0 fold scores are SEG-072's only when reproduce.json passed for all three models under the CURRENT plan digest."""
    p = out / "reproduce.json"
    if not p.is_file():
        return False
    r = json.loads(p.read_text())
    return (r.get("plan_sha256") == plan_sha() and r.get("c0_reuse_admitted") is True
            and set(r.get("models", {})) == {"M3R", "M4", "M1"} and all(m["bitwise_identical"] for m in r["models"].values()))


def jobs(plan: dict, reuse: bool) -> list[str]:
    """The pre-registered fit list (one line per fit: MODEL COND FOLD [SEED])."""
    out = []

    def add(model, cond, fold, seed=None):
        out.append(f"{model} {cond} {fold}" + (f" {seed}" if seed is not None else ""))

    for cond in ("C0", "C1", "C2"):
        for fold in GENESIS_LINEAGES:
            if cond == "C0" and reuse:
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
    for j in jobs(design.verify_plan(), c0_reuse(pathlib.Path(a.out).resolve())):
        print(j)
    return 0


# ----------------------------------------------------------------------------------------------------------------------- metrics
def load_npz(path: pathlib.Path) -> dict:
    import numpy as np
    with np.load(path) as z:
        return {k.replace("@@", "/"): z[k] for k in z.files}


def image_metrics(classes: bytes, seed_addresses, scores, cell: int = 128) -> dict:
    """Per-image metrics from cell scores (pure given numbers; unit-tested): ER (excess retention) on informative images, FRF, residual
    ER, misses / negative cells admitted at each fixed budget, short/mixed-stratum misses, AUROC over labelled cells."""
    labels = v3.cell_labels(classes, cell)
    s = [float(x) for x in scores]
    if len(s) != len(labels):
        raise SystemExit(f"score length {len(s)} != cells {len(labels)}")
    seeds = {a // cell for a in seed_addresses if 0 <= a < len(classes)}
    starts = v3.starts_of(classes)
    pos, cells = labels.count(1), len(labels)
    f = v3.frf(s, labels)
    informative = 0 < pos < cells
    out = {"cells": cells, "positive_cells": pos, "negative_cells": labels.count(0), "starts": len(starts), "frf": f["frf"],
           "informative": informative, "er": (f["cells_needed"] - pos) / (cells - pos) if informative else None,
           "residual_frf": v3.residual_frf(s, labels, seeds)}
    if informative:
        # residual ER: cells needed in rank order beyond the seeds, excess over the non-seed positives, over the non-positive cells
        rest_pos = sum(1 for c, y in enumerate(labels) if y == 1 and c not in seeds)
        needed = round(out["residual_frf"] * cells)
        out["residual_er"] = max(0, needed - rest_pos) / (cells - pos)
    stratum = set()
    for c in range(cells):
        if labels[c] == 1:
            chunk = classes[c * cell:(c + 1) * cell]
            if chunk.count(b"I") <= 8 or b"D" in chunk:
                stratum.add(c)
    for pol in POLICIES:
        reg = v3.policy_region(pol, s, None, seeds)["region"]
        out[f"misses_{pol}"] = v3.misses(starts, reg, cell)
        out[f"missed_cells_{pol}"] = sum(1 for c in range(cells) if labels[c] == 1 and c not in reg)
        out[f"stratum_missed_cells_{pol}"] = sum(1 for c in stratum if c not in reg)
        out[f"negative_admitted_{pol}"] = sum(1 for c in reg if labels[c] == 0)
        out[f"retained_{pol}"] = len(reg) / cells
    out["stratum_cells"] = len(stratum)
    out["auroc"] = auroc([x for x, y in zip(s, labels) if y == 1], [x for x, y in zip(s, labels) if y == 0])
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
        rank_sum += (i + j + 1) / 2 * sum(1 for k in range(i, j) if allv[k][1] == 1)
        i = j
    n1, n0 = len(pos), len(neg)
    return round((rank_sum - n1 * (n1 + 1) / 2) / (n1 * n0), 6)


def lineage_rows(per_image: dict, lineage_of: dict) -> dict:
    """Per lineage: max ER over informative images (None if none), trivial-image count, max FRF, summed misses / admissions, mean AUROC."""
    out = {}
    for iid, m in sorted(per_image.items()):
        L = out.setdefault(lineage_of[iid], {"images": 0, "trivial_images": 0, "ers": [], "max_frf": 0.0, "aurocs": [], "positive_cells": 0,
                                             "stratum_cells": 0, **{f"{k}_{p}": 0 for p in POLICIES for k in
                                                                     ("misses", "missed_cells", "stratum_missed_cells", "negative_admitted")}})
        L["images"] += 1
        if m["informative"]:
            L["ers"].append(m["er"])
        else:
            L["trivial_images"] += 1
        L["max_frf"] = max(L["max_frf"], m["frf"])
        L["positive_cells"] += m["positive_cells"]
        L["stratum_cells"] += m["stratum_cells"]
        if m["auroc"] is not None:
            L["aurocs"].append(m["auroc"])
        for p in POLICIES:
            for k in ("misses", "missed_cells", "stratum_missed_cells", "negative_admitted"):
                L[f"{k}_{p}"] += m[f"{k}_{p}"]
    for L in out.values():
        ers, a = L.pop("ers"), L.pop("aurocs")
        L["max_er"] = round(max(ers), 6) if ers else None
        L["mean_auroc"] = round(sum(a) / len(a), 6) if a else None
        L["max_frf"] = round(L["max_frf"], 6)
    return out


def mean_er(rows: dict):
    v = [r["max_er"] for r in rows.values() if r["max_er"] is not None]
    return round(sum(v) / len(v), 6) if v else None


def classify_transfer(c0: dict, cx: dict, tau: float, allowance: dict, insufficient: bool) -> dict:
    """Pre-registered transfer rule (pure; unit-tested). c0/cx: {lineage: {max_er, misses_B50}}; allowance: {lineage: a_l}."""
    if insufficient:
        return {"class": "EVIDENCE_INSUFFICIENT"}
    ls = [l for l in GENESIS_LINEAGES if l in c0 and l in cx and c0[l]["max_er"] is not None and cx[l]["max_er"] is not None]
    d = {l: cx[l]["max_er"] - c0[l]["max_er"] for l in ls}
    e = {l: max(0, cx[l]["misses_B50"] - c0[l]["misses_B50"] - allowance.get(l, 0)) for l in GENESIS_LINEAGES if l in c0 and l in cx}
    d_mean = sum(d.values()) / len(d) if d else 0.0
    worst = max(d.values()) if d else 0.0
    if d_mean <= -tau and worst <= tau and not any(e.values()):
        cls = "TRANSFER_HELPS"
    elif d_mean >= tau or worst > 2 * tau or any(e.values()):
        cls = "TRANSFER_HARMS"
    else:
        cls = "TRANSFER_NEUTRAL"
    return {"class": cls, "d_mean_er": round(d_mean, 6), "worst_lineage_d_er": round(worst, 6),
            "extra_b50_misses_beyond_allowance": {l: v for l, v in e.items() if v}, "per_lineage_d_er": {l: round(v, 6) for l, v in d.items()}}


def complete_budget(rows: dict, allowance_sum: int):
    """Smallest fixed budget whose Genesis B-misses (summed over lineages) are within the seed-replicate allowance (pre-registered)."""
    for pol in POLICIES:
        if sum(r[f"misses_{pol}"] for r in rows.values()) <= allowance_sum:
            return pol
    return None


# ------------------------------------------------------------------------------------------------------------------------- table
def genesis_seed_addresses(meta_dir: pathlib.Path) -> list[int]:
    import segarecomp_ml_v3_eval as ev
    m = json.loads((meta_dir / "meta.json").read_text())
    m["dir"] = str(meta_dir)
    return ev.seed_addresses(m)


def m4_cells(arrs: list[dict], iid: str, size: int, cell: int):
    import numpy as np
    return tr().cell_max(np.mean([a[iid] for a in arrs], axis=0).astype(np.float32), size, cell)


def fit_scores(cfg: str, cond: str, fold: str, out: pathlib.Path, reuse: bool, sizes: dict, seed50: bool = False) -> dict | None:
    """{image id: cell scores} of one model configuration / fit, or None when any planned score file is missing."""
    import numpy as np
    f = fold.replace(":", "-")
    from_seg072 = cond == "C0" and reuse and fold in GENESIS_LINEAGES and not seed50
    suffix = ".xplat" if fold == "all" else ""
    if cfg in ("M3_R", "M1_HGB"):
        name = {"M3_R": "M3R", "M1_HGB": "M1"}[cfg]
        p = (SEG072_SCORES / f"{name}.{fold}.npz") if from_seg072 else out / "scores" / f"{name}.{cond}.{f}{'.s50' if seed50 else ''}{suffix}.npz"
        return {k: v.astype(np.float64) for k, v in load_npz(p).items()} if p.exists() else None
    cell = int(cfg.split("@")[1])
    ps = [(SEG072_SCORES / f"M4.{fold}.s{s}.npz") if from_seg072 else out / "scores" / f"M4.{cond}.{f}.s{s}{suffix}.npz" for s in (1, 2, 3)]
    if not all(p.exists() for p in ps):
        return None
    arrs = [load_npz(p) for p in ps]
    return {iid: m4_cells(arrs, iid, sizes[iid], cell) for iid in arrs[0]}


def cmd_table(a) -> int:
    import numpy as np
    plan = design.verify_plan()
    out = pathlib.Path(a.out).resolve()
    snap = pathlib.Path(a.snapshot).resolve()
    reuse = c0_reuse(out)
    t = tr()
    dev_x = plan["conditions"]["C2"]["xplat_training"]
    meta = {}
    for d in genesis_dirs(GENESIS_LINEAGES):
        m = json.loads((d / "meta.json").read_text())
        meta[m["id"]] = {"dir": d, "lineage": m["lineage"], "size": m["size"], "seeds": genesis_seed_addresses(d)}
    for d in xplat_dirs(snap, dev_x):
        m = json.loads((d / "meta.json").read_text())
        meta[m["id"]] = {"dir": d, "lineage": m["lineage"], "size": m["size"], "seeds": m.get("seed_addresses", [])}
    sizes = {k: v["size"] for k, v in meta.items()}
    lineage_of = {k: v["lineage"] for k, v in meta.items()}
    classes = {}

    def metrics(iid, scores, cell):
        if iid not in classes:
            classes[iid] = (meta[iid]["dir"] / "classes.bin").read_bytes()
        return image_metrics(classes[iid], meta[iid]["seeds"], scores, cell)

    tables, missing = {}, []
    for cfg in CONFIGS + ("M3_R@s50",):
        base_cfg = cfg.split("@s")[0] if cfg.startswith("M3_R@") else cfg
        cell = int(cfg.split("@")[1]) if cfg.startswith("M4") else 128
        for cond in FITTED_CONDITIONS[base_cfg] if cfg != "M3_R@s50" else ("C0",):
            g = {}
            for fold in GENESIS_LINEAGES:
                sc = fit_scores(base_cfg, cond, fold, out, reuse, sizes, seed50=cfg == "M3_R@s50")
                if sc is None:
                    missing.append(f"{cfg}|{cond}|{fold}")
                    continue
                for iid, s in sc.items():
                    g[iid] = metrics(iid, s, cell)
            x = {}
            if cfg != "M3_R@s50" and cond in ("C0", "C2"):
                for lin in dev_x:
                    sc = fit_scores(base_cfg, "C0", "all", out, False, sizes) if cond == "C0" else fit_scores(base_cfg, "C2", f"xplat:{lin}", out, False, sizes)
                    if sc is None:
                        missing.append(f"{cfg}|{cond}|xplat:{lin}")
                        continue
                    for iid, s in sc.items():
                        if lineage_of.get(iid) == lin:
                            x[iid] = metrics(iid, s, cell)
            tables[f"{cfg}|{cond}"] = {"genesis": lineage_rows(g, lineage_of), "xplat": lineage_rows(x, lineage_of)}
    # M3_FROZEN zero-shot reference (SEG-072's stored Genesis scores; cross-platform by inference)
    import segarecomp_ml_v3_eval as ev
    reg = {iid: {"size": v["size"]} for iid, v in meta.items() if not iid.startswith("xp/")}
    fz = ev.config_scores("M3_FROZEN", reg, [])
    g = {iid: metrics(iid, sc[0], 128) for iid, sc in fz.items()}
    fm = t.frozen_m3_model()
    x = {iid: metrics(iid, t.score_m3(fm, t.Image(meta[iid]["dir"])), 128) for iid in meta if iid.startswith("xp/")}
    tables["M3_FROZEN|-"] = {"genesis": lineage_rows(g, lineage_of), "xplat": lineage_rows(x, lineage_of)}
    # noise (tau, miss allowances) and completeness
    g49, g50 = tables["M3_R|C0"]["genesis"], tables["M3_R@s50|C0"]["genesis"]
    complete = not missing and all(set(tables[k]["genesis"]) == set(GENESIS_LINEAGES) for k in tables)
    diffs = {l: abs(g49[l]["max_er"] - g50[l]["max_er"]) for l in GENESIS_LINEAGES
             if l in g49 and l in g50 and g49[l]["max_er"] is not None and g50[l]["max_er"] is not None}
    tau = max(0.01, 2 * sum(diffs.values()) / len(diffs)) if diffs else None
    allowance = {l: abs(g49[l]["misses_B50"] - g50[l]["misses_B50"]) for l in GENESIS_LINEAGES if l in g49 and l in g50}
    insufficient = (not complete) or tau is None or tau > 0.05
    decisions = {}
    for cfg in CONFIGS:
        for cond in ("C1", "C2"):
            if cond in FITTED_CONDITIONS[cfg]:
                decisions[f"{cfg}|{cond}"] = classify_transfer(tables[f"{cfg}|C0"]["genesis"], tables[f"{cfg}|{cond}"]["genesis"], tau or 1.0,
                                                               allowance, insufficient)
    overall = decisions["M3_R|C2"]["class"]
    asum = sum(allowance.values())
    records = {}
    for k, v in tables.items():
        g, x = v["genesis"], v["xplat"]
        records[k] = {"genesis_b50_misses": sum(r["misses_B50"] for r in g.values()), "genesis_mean_er": mean_er(g),
                      "genesis_worst_er": max((r["max_er"] for r in g.values() if r["max_er"] is not None), default=None),
                      "genesis_negative_admitted_B50": sum(r["negative_admitted_B50"] for r in g.values()),
                      "genesis_trivial_images": sum(r["trivial_images"] for r in g.values()),
                      "xplat_mean_er": mean_er(x), "xplat_b50_misses": sum(r["misses_B50"] for r in x.values()) if x else None,
                      "complete_budget": complete_budget(g, asum) if g else None}
    doc = {"schema": "segarecomp.xplat_transfer.results.v2", "task": "SEG-075-T004", "adr": "0162", "plan_sha256": plan_sha(),
           "c0_reuse": reuse, "complete": complete, "missing_fits": missing,
           "noise": {"tau": tau, "per_lineage_abs_d_er_seed49_seed50": {l: round(v, 6) for l, v in diffs.items()},
                     "b50_miss_allowance": allowance, "insufficient": insufficient},
           "records": records, "decisions": decisions, "transfer_class": overall, "tables": tables,
           "data_boundary": "aggregates only: per-lineage ER/FRF, counts and AUROC; no per-cell score, address or image content"}
    target = pathlib.Path(a.results) if a.results else RESULTS
    target.write_text(json.dumps(doc, indent=1, sort_keys=True) + "\n")
    print(json.dumps({"transfer_class": overall, "tau": tau, "complete": complete, "missing": len(missing), "records": records,
                      "decisions": {k: v["class"] for k, v in decisions.items()}}, indent=1))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name, fn in (("fit", cmd_fit), ("reproduce", cmd_reproduce), ("jobs", cmd_jobs), ("table", cmd_table)):
        p = sub.add_parser(name)
        p.set_defaults(func=fn)
        p.add_argument("--out", required=True)
        if name != "jobs":
            p.add_argument("--snapshot", required=True)
        if name == "fit":
            p.add_argument("--model", required=True, choices=("M3R", "M4", "M1"))
            p.add_argument("--condition", required=True, choices=("C0", "C1", "C2"))
            p.add_argument("--fold", required=True)
            p.add_argument("--seed", type=int)
        if name == "reproduce":
            p.add_argument("--fold", default="D05")
        if name == "table":
            p.add_argument("--results")
    a = ap.parse_args(argv)
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())
