#!/usr/bin/env python3
"""SEG-056: frozen metric table from private score arrays (numpy only; no fitting, no sealed access).

  dev     read logit_<cand>_<held>_s<seed>.npy (dense candidates) and c1cell_<held>.npy (historical C1 refit, 32-byte cell
          logits), aggregate to cells (frozen cell sizes / reductions), add structural roots, and write aggregates.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import statistics
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_ml_region_z80 as z1  # noqa: E402
import segarecomp_ml_region_z80_gen2 as g  # noqa: E402
import segarecomp_ml_region_z80_gen2_train as T  # noqa: E402

CELL_SIZES = (32, 16)
AGGREGATORS = ("max", "top2")


def fold_metrics(byte_logits, classes, n_img, cell, agg):
    labels = g.cell_labels(classes, cell)
    scores = g.aggregate(list(byte_logits), cell, agg)
    return g.metrics(scores, labels, g.root_cells(n_img, cell))


def summarize(rows):
    keys = ("residual_frf", "retained_full_recall", "frf")
    out = {}
    for k in keys:
        vals = [r[k] for r in rows.values()]
        out[f"mean_{k}"] = round(sum(vals) / len(vals), 4)
        out[f"max_{k}"] = round(max(vals), 4)
        out[f"median_{k}"] = round(statistics.median(vals), 4)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img-dirs", nargs="+", required=True)
    ap.add_argument("--truth-dirs", nargs="+", required=True)
    ap.add_argument("--state", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    plan = T.check_plan()
    import numpy as np
    state = pathlib.Path(a.state)
    dev = T.dev_ids(plan)
    data = {}
    for lid in dev:
        for idir, tdir in zip(a.img_dirs, a.truth_dirs):
            if (pathlib.Path(idir) / f"{lid}.sms").exists():
                data[lid] = T.load(np, lid, idir, tdir)
    res = {"plan_digest": T.DIGEST.read_text().strip(), "candidates": {}, "c1_refit": {}}
    # historical C1 refit on the same folds (cell logits, 32-byte cells), without and with roots
    for tag in ("no_roots", "roots"):
        rows = {}
        for lid in dev:
            p = state / f"c1cell_{lid}.npy"
            if not p.exists():
                continue
            sc = np.load(p).tolist()
            labels = g.cell_labels(data[lid]["classes"], 32)
            rows[lid] = g.metrics(sc, labels, g.root_cells(data[lid]["n"], 32) if tag == "roots" else [])
        if rows:
            res["c1_refit"][tag] = {"folds": rows, **summarize(rows)}
    for name in T.CANDIDATES:
        for cell in CELL_SIZES:
            for agg in AGGREGATORS:
                folds, per_seed = {}, {}
                for lid in dev:
                    files = [state / f"logit_{name}_{lid}_s{s}.npy" for s in T.SEEDS]
                    if not all(f.exists() for f in files):
                        folds = None
                        break
                    arrs = [np.load(f) for f in files]
                    folds[lid] = fold_metrics(np.mean(arrs, axis=0), data[lid]["classes"], data[lid]["n"], cell, agg)
                    per_seed[lid] = [fold_metrics(x, data[lid]["classes"], data[lid]["n"], cell, agg)["residual_frf"]
                                     for x in arrs]
                if folds is None:
                    continue
                key = f"{name}/cell{cell}/{agg}"
                seed_sd = {lid: round(statistics.pstdev(v), 4) for lid, v in per_seed.items()}
                res["candidates"][key] = {"folds": folds, "per_seed_residual_frf": per_seed, "seed_pstdev": seed_sd,
                                          **summarize(folds)}
    if res["candidates"]:
        order = sorted(res["candidates"], key=lambda k: (res["candidates"][k]["mean_retained_full_recall"], k))
        sel = order[0]
        mx = res["candidates"][sel]["max_residual_frf"]
        fixed = z1.ceil_to(mx + z1.FIXED_MARGIN, z1.FIXED_STEP)
        res["selected"] = sel
        res["fixed_budget"] = round(fixed, 2)
        res["fixed_budget_misses"] = {}
        # strict recall at the fixed budget (roots included)
        name, cs, agg = sel.split("/")
        cell = int(cs[4:])
        for lid in dev:
            arrs = [np.load(state / f"logit_{name}_{lid}_s{s}.npy") for s in T.SEEDS]
            sc = g.aggregate(list(np.mean(arrs, axis=0)), cell, agg)
            lab = g.cell_labels(data[lid]["classes"], cell)
            miss, pos, ret = g.missed_at_budget(sc, lab, g.root_cells(data[lid]["n"], cell), min(1.0, fixed))
            res["fixed_budget_misses"][lid] = {"missed": miss, "positives": pos, "retained": round(ret, 4)}
        res["dev_gate_pass"] = bool(fixed <= 0.65 and all(v["missed"] == 0 for v in res["fixed_budget_misses"].values()))
    pathlib.Path(a.out).write_text(json.dumps(res, indent=1, sort_keys=True) + "\n")
    print("selected", res.get("selected"), "fixed", res.get("fixed_budget"), "gate", res.get("dev_gate_pass"))


if __name__ == "__main__":
    main()
