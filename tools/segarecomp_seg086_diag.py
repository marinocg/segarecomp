#!/usr/bin/env python3
"""SEG-086-T001 (ADR 0195, checkpoint C1): why are the raw structural-model-A logits not transferable across lineages? (OFFLINE research diagnostics; no fitting except the
single bounded final-model refit that checks the final-versus-leave-one-lineage-out score scale.)

Four error classes are kept apart because they need different remedies (per image, mutually exclusive, evaluated in this order):
  BUDGET_INFEASIBLE   verified code density (positive cells plus independently established seed cells) alone exceeds the retention cap;
  RANKING_ERROR       density fits the cap but even the ORACLE per-image threshold (the weakest verified positive cell, ties inclusive) retains more than the cap;
  CALIBRATION_ERROR   the oracle threshold retains <= cap (the ranking is adequate) but the lineage-separated absolute threshold abstains (retains > cap) or misses a start;
  OK                  none of the above.
EVALUATION_UNCERTAINTY is a flag, not a class: masked (unknown) cell share and the label-boundary audit bound how much of any of the above could be a label artefact.

Pure functions first (tested on synthetic data), then the `run` command that reads the private registry and the stored SEG-076/084/085 score artifacts.
Product evidence is aggregate only (per-lineage quantiles, counts); per-cell arrays stay in the ignored cache.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import pathlib
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_ml_v3 as v3  # noqa: E402

SCHEMA = "segarecomp.seg086.diag.v1"
HARNESS = pathlib.Path(os.environ.get("SEGARECOMP_HARNESS_ROOT", "/root/dev/segarecomp-harness"))
CACHE = pathlib.Path(os.environ.get("SEG086_CACHE", str(HARNESS / ".cache" / "seg086")))
S85 = pathlib.Path(os.environ.get("SEG085_CACHE", str(HARNESS / ".cache" / "seg085")))
RESULTS = HERE / "segarecomp_seg086_diag.results.json"
CAP = v3.MAX_RETENTION
QS = (0.01, 0.05, 0.10, 0.25, 0.50, 0.75, 0.90, 0.99)
GENESIS = ("D01", "D02", "D03", "D04", "D05", "D06", "D07", "D09", "D10", "D11", "D12")
CLASSES = ("OK", "CALIBRATION_ERROR", "RANKING_ERROR", "BUDGET_INFEASIBLE")


def r4(x):
    return None if x is None else round(float(x), 4)


# ------------------------------------------------------------------------------------------------------------------------------- pure
def quantile_summary(values, qs=QS) -> dict:
    """Linear-interpolated quantiles of a finite numeric sequence (None for an empty one). Deterministic, no numpy."""
    v = sorted(float(x) for x in values)
    if any(not math.isfinite(x) for x in v):
        raise ValueError("non-finite value")
    out = {}
    for q in qs:
        key = f"q{int(round(q * 100)):02d}"
        if not v:
            out[key] = None
            continue
        pos = q * (len(v) - 1)
        lo = int(math.floor(pos))
        hi = min(lo + 1, len(v) - 1)
        out[key] = r4(v[lo] + (v[hi] - v[lo]) * (pos - lo))
    return out


def weakest_positive(scores, labels, seeds):
    """Lowest score among verified positive cells that are not independently established seeds (None if there is none)."""
    vals = [float(s) for c, (s, y) in enumerate(zip(scores, labels)) if y == 1 and c not in seeds]
    return min(vals) if vals else None


def oracle_fraction(scores, seeds, threshold) -> float | None:
    """Retained fraction of the oracle threshold: cells with score >= threshold (ties inclusive) united with the seed cells."""
    if threshold is None:
        return None
    n = len(scores)
    return len({c for c, s in enumerate(scores) if s >= threshold} | {c for c in seeds if 0 <= c < n}) / n


def positive_density(labels, seeds) -> float:
    n = len(labels)
    return len({c for c, y in enumerate(labels) if y == 1} | {c for c in seeds if 0 <= c < n}) / n


def classify_error(density: float, oracle_frac: float | None, abstained: bool, missed: int, cap: float = CAP) -> str:
    """The four-way split above. `abstained`/`missed` describe the lineage-separated absolute-threshold policy on the same image."""
    if density > cap + 1e-12:
        return "BUDGET_INFEASIBLE"
    if oracle_frac is not None and oracle_frac > cap + 1e-12:
        return "RANKING_ERROR"
    if abstained or missed > 0:
        return "CALIBRATION_ERROR"
    return "OK"


def weak_positive_character(scores, labels, seeds, starts_per_cell, data_bytes_per_cell, cell_bytes: int = v3.CELL_BYTES) -> dict:
    """Aggregate character of the verified positive cells that score below the image's own negative-cell 90th percentile (i.e. inside the data plateau): how many carry
    a single start, how many are mixed code/data. Compared with all non-seed positive cells of the image. Counts only."""
    neg = [float(s) for s, y in zip(scores, labels) if y == 0]
    if not neg:
        return {"weak": 0, "positive": 0}
    cut = quantile_summary(neg, (0.90,))["q90"]
    pos = [c for c, y in enumerate(labels) if y == 1 and c not in seeds]
    weak = [c for c in pos if float(scores[c]) < cut]

    def prof(cells):
        return {"cells": len(cells), "single_start": sum(1 for c in cells if starts_per_cell[c] == 1), "mixed_with_data": sum(1 for c in cells if data_bytes_per_cell[c] > 0),
                "mostly_data": sum(1 for c in cells if data_bytes_per_cell[c] * 2 > cell_bytes)}
    return {"cutoff_is_image_negative_q90": True, "weak": prof(weak), "all_positive": prof(pos)}


def ranks(a):
    import numpy as np
    order = np.argsort(a, kind="stable")
    r = np.empty(len(a), dtype=np.float64)
    r[order] = np.arange(len(a), dtype=np.float64)
    return r


def spearman(a, b) -> float | None:
    """Spearman correlation via stable ranks (scores are continuous; ties are broken by position identically in both)."""
    import numpy as np
    if len(a) != len(b) or len(a) < 2:
        return None
    ra, rb = ranks(np.asarray(a)), ranks(np.asarray(b))
    if ra.std() == 0 or rb.std() == 0:
        return None
    return float(np.corrcoef(ra, rb)[0, 1])


def location_scale_candidates(scores) -> dict:
    """Label-free location / scale statistics of ONE image's scores (the quantities T002 may normalise with); descriptive only."""
    q = quantile_summary(scores, (0.10, 0.25, 0.50, 0.75, 0.90))
    return {"median": q["q50"], "iqr": r4(q["q75"] - q["q25"]), "lower_half_spread": r4(q["q50"] - q["q10"]), "q25": q["q25"], "q10": q["q10"]}


def lineage_instability(minima: dict) -> dict:
    """Spread of per-lineage weakest-positive scores: the quantity that decides whether one absolute threshold can serve every lineage."""
    vals = [v for v in minima.values() if v is not None]
    if not vals:
        return {"n": 0}
    return {"n": len(vals), "min": r4(min(vals)), "max": r4(max(vals)), "range": r4(max(vals) - min(vals)), "median": quantile_summary(vals, (0.5,))["q50"]}


# ------------------------------------------------------------------------------------------------------------------------------- run
def _load():
    import numpy as np
    import segarecomp_seg084_a as s84
    import segarecomp_xplat_transfer as xt
    imgs, _ = s84.load_registry_images()
    oof = {}
    for L in GENESIS:
        oof.update(xt.load_npz(s84.score_path(L, "A0")))
    return np, s84, xt, imgs, oof


def cmd_run(a) -> int:
    np, s84, xt, imgs, oof = _load()
    import segarecomp_seg085_dyn as dyn
    import segarecomp_xplat_models as xm
    t0 = time.process_time()
    nested = json.loads((S85 / "nested.json").read_text())
    by_l = {L: [im for im in imgs if im.lineage == L] for L in GENESIS}
    private = {}
    out = {"schema": SCHEMA, "cap": CAP, "per_lineage": {}, "per_image_classes": {}, "fit_subset": {}, "final_vs_lolo": {}, "data_boundary": "aggregates only; per-cell arrays stay in the ignored cache"}
    minima, floors = {}, {}
    for L in GENESIS:
        pos, neg, allc, mp, dens, masked, meds, iqrs, lows = [], [], [], [], [], 0, [], [], []
        cells = 0
        for im in by_l[L]:
            sc = oof[im.id]
            lab = [int(x) for x in im.labels]
            seeds = set(im.seeds)
            pos += [float(s) for c, (s, y) in enumerate(zip(sc, lab)) if y == 1 and c not in seeds]
            neg += [float(s) for s, y in zip(sc, lab) if y == 0]
            masked += lab.count(-1)
            cells += len(lab)
            w = weakest_positive(sc, lab, seeds)
            mp.append(w)
            dens.append(positive_density(lab, seeds))
            ls = location_scale_candidates(sc)
            meds.append(ls["median"]), iqrs.append(ls["iqr"]), lows.append(ls["lower_half_spread"])
            floors[im.id] = float(np.min(sc))
        vals = [m for m in mp if m is not None]
        minima[L] = min(vals) if vals else None
        out["per_lineage"][L] = {"images": len(by_l[L]), "cells": cells, "masked_cell_share": r4(masked / cells), "mean_positive_density": r4(sum(dens) / len(dens)),
                                 "positive_scores": quantile_summary(pos), "negative_scores": quantile_summary(neg), "weakest_positive_min": r4(minima[L]),
                                 "weakest_positive_images_median": quantile_summary(vals, (0.5,))["q50"], "image_median_range": [r4(min(meds)), r4(max(meds))],
                                 "image_iqr_range": [r4(min(iqrs)), r4(max(iqrs))], "image_lower_half_spread_range": [r4(min(lows)), r4(max(lows))],
                                 "n_nonseed_positive_cells": len(pos), "n_negative_cells": len(neg)}
    out["weakest_positive_instability"] = lineage_instability(minima)
    import segarecomp_seg085_diag as sd
    chars = {}
    for L in GENESIS:
        agg = {"weak": {"cells": 0, "single_start": 0, "mixed_with_data": 0, "mostly_data": 0}, "all_positive": {"cells": 0, "single_start": 0, "mixed_with_data": 0, "mostly_data": 0}}
        for im in by_l[L]:
            lab = [int(x) for x in im.labels]
            st, dt, _ = sd.cell_stats(im.classes, lab)
            w = weak_positive_character(oof[im.id], lab, set(im.seeds), st, dt)
            if w.get("weak") == 0:
                continue
            for part in agg:
                for k in agg[part]:
                    agg[part][k] += w[part][k]
        chars[L] = agg
    out["weak_positive_character"] = chars
    # global floor versus the frozen SEG-085 threshold
    t_final = nested["final"]["T"]
    out["score_floor"] = {"global_min_over_images": r4(min(floors.values())), "dyn_score_T_final": r4(t_final), "images_whose_minimum_score_is_above_T": sum(1 for v in floors.values() if v >= t_final)}
    # (3) fit subsets and (7) magnitude versus ranking: lineage C scored by the OOF model (trained on 10 lineages) and by every inner model (9 lineages, one more held out)
    fit = {}
    for C in GENESIS:
        rows = []
        for H in GENESIS:
            if H == C:
                continue
            z = xt.load_npz(dyn.CACHE / "inner" / f"{H}.{C}.npz")
            for im in by_l[C]:
                x, y = oof[im.id], z[im.id]
                lab = [int(t) for t in im.labels]
                wx, wy = weakest_positive(x, lab, set(im.seeds)), weakest_positive(y, lab, set(im.seeds))
                rows.append({"rho": spearman(x, y), "median_shift": float(np.median(y) - np.median(x)), "q10_shift": float(np.percentile(y, 10) - np.percentile(x, 10)),
                             "weakest_shift": None if wx is None or wy is None else wy - wx, "weakest_inner": wy})
        rho = [r["rho"] for r in rows if r["rho"] is not None]
        ws = [r["weakest_shift"] for r in rows if r["weakest_shift"] is not None]
        wi = [r["weakest_inner"] for r in rows if r["weakest_inner"] is not None]
        fit[C] = {"fits": len(rows), "spearman_min": r4(min(rho)) if rho else None, "spearman_median": quantile_summary(rho, (0.5,))["q50"],
                  "median_shift_abs_max": r4(max(abs(r["median_shift"]) for r in rows)), "q10_shift_abs_max": r4(max(abs(r["q10_shift"]) for r in rows)),
                  "weakest_shift_range": [r4(min(ws)), r4(max(ws))] if ws else None, "weakest_over_fits_range": [r4(min(wi)), r4(max(wi))] if wi else None}
    out["fit_subset"] = fit
    # (2) error classes per image with the SEG-085 isolated, lineage-separated absolute threshold of the held-out lineage
    tally = {c: 0 for c in CLASSES}
    for L in GENESIS:
        T = nested["folds"][L]["T_isolated"]
        for im in by_l[L]:
            sc, lab, seeds = oof[im.id], [int(x) for x in im.labels], set(im.seeds)
            w = weakest_positive(sc, lab, seeds)
            of = oracle_fraction(sc, seeds, w)
            sel = dyn.dyn_select(sc, seeds, T)
            missed = v3.misses(v3.starts_of(im.classes), sel["region"]) if sel["status"] == "selected" else 0
            cls = classify_error(positive_density(lab, seeds), of, sel["status"] == "abstain_cap", missed)
            tally[cls] += 1
            out["per_image_classes"][im.id] = {"lineage": L, "class": cls, "density": r4(positive_density(lab, seeds)), "oracle_fraction": r4(of), "T_isolated": r4(T),
                                                "masked_share": r4(lab.count(-1) / len(lab)), "status": sel["status"],
                                                "fraction_logit_above_zero": r4(float(np.mean(np.asarray(sc) > 0.0))),
                                                "masked_above_zero_share": r4(sum(1 for s_, y_ in zip(sc, lab) if y_ == -1 and s_ > 0.0) / max(1, lab.count(-1)))}
    out["error_class_tally"] = tally
    fa = [v["fraction_logit_above_zero"] for v in out["per_image_classes"].values()]
    tgt = [min(1.0, v["density"] + v["masked_share"]) for v in out["per_image_classes"].values()]
    out["label_free_code_fraction"] = {"pearson_fraction_above_zero_vs_density_plus_masked": r4(float(np.corrcoef(fa, tgt)[0, 1])),
                                       "pearson_fraction_above_zero_vs_density": r4(float(np.corrcoef(fa, [v["density"] for v in out["per_image_classes"].values()])[0, 1]))}
    # (8) final all-development model versus the leave-one-lineage-out scores
    import segarecomp_seg084_a as s84m
    feats = {im.id: xm.a_features(im, xm.relocs_of(im)) for im in imgs}
    X, y = dyn.train_matrix(np, imgs, feats, set(GENESIS))
    clf = s84m.fit_arrays(X, y)
    fv = {}
    for L in GENESIS:
        rs, ms, qs, lo = [], [], [], []
        for im in by_l[L]:
            f = s84m.logit(clf, feats[im.id])
            rs.append(spearman(oof[im.id], f)), ms.append(float(np.median(f) - np.median(oof[im.id]))), qs.append(float(np.percentile(f, 10) - np.percentile(oof[im.id], 10)))
            lab = [int(t) for t in im.labels]
            wf, wo = weakest_positive(f, lab, set(im.seeds)), weakest_positive(oof[im.id], lab, set(im.seeds))
            lo.append(None if wf is None or wo is None else wf - wo)
        rr = [x for x in rs if x is not None]
        fv[L] = {"spearman_min": r4(min(rr)) if rr else None, "median_shift_abs_max": r4(max(abs(m) for m in ms)), "q10_shift_abs_max": r4(max(abs(m) for m in qs)),
                 "weakest_positive_shift_range": [r4(min(x for x in lo if x is not None)), r4(max(x for x in lo if x is not None))] if any(x is not None for x in lo) else None}
    out["final_vs_lolo"] = {"caveat": "the final model has TRAINED on every development image: positive scores and weakest-positive shifts are in-sample and optimistic; only the negative-plateau location (median, q10) is a clean scale comparison",
                           "per_lineage": fv, "training_matrix_rows": int(len(y))}
    out["cpu_seconds"] = round(time.process_time() - t0, 1)
    out["cpu_note"] = "one final-model refit with the unchanged SEG-076 configuration; all other inputs are stored SEG-076/084/085 scores"
    RESULTS.write_text(json.dumps(out, indent=1, sort_keys=True) + "\n")
    print(json.dumps({"tally": tally, "instability": out["weakest_positive_instability"], "floor": out["score_floor"], "cpu": out["cpu_seconds"]}))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("run").set_defaults(func=cmd_run)
    a = ap.parse_args(argv)
    return a.func(a)


if __name__ == "__main__":
    raise SystemExit(main())
