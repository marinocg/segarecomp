#!/usr/bin/env python3
"""SEG-083-T001: bounded model-A diagnostic and readiness measurements for the five-game batch (OFFLINE; needs numpy and scikit-learn).

  diagnose   frozen model A (SEG-072 M1 HistGradientBoosting, HGB_CONFIG unchanged), leave-one-lineage-out over every qualified SEG-072 lineage,
             arms B0 / C / T / TW / T_matched (five new games) and C81 / T81 / T_all / C_all (SEG-081 games alone, all eight). Beside pooled AP/AUC (never
             a readiness criterion) it reports, from the same cell scores, the SEG-075 excess-retention metrics (lineage-max ER, Genesis mean and worst ER),
             retained-region recall and missed instruction starts at B35/B50, boundary-stratum misses, hard-negative errors and paired fold deltas.
  measure    combine per-game results, code-shingle containment, signature sets and the diagnostic into the readiness measurements and the decision.

Training data are the new games' labels only; every evaluation fold is a source-backed SEG-072 lineage. No sealed cohort is read.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_genesis_bizhawk as bz  # noqa: E402
import segarecomp_genesis_evidence as ge  # noqa: E402
import segarecomp_genesis_tasbatch5 as t5  # noqa: E402
import segarecomp_genesis_tasbatch_ml as ml81  # noqa: E402
import segarecomp_genesis_tasbatch as tb  # noqa: E402

ARMS = ("B0", "C", "T", "TW", "T_matched", "C81", "T81", "T_all", "C_all", "B0_sub")
POLS = ("B35", "B50")
STRATA = ("d_byte", "adjacent_d_only", "short_run", "boundary")


def seed_addresses(meta_dir: pathlib.Path, kind: str = "genesis_rom") -> list:
    """Exactly `segarecomp_ml_v3_eval.seed_addresses`: `d.txt` for a Genesis ROM, else `seed_starts.txt`; an absent file means no seeds (historical definition)."""
    p = meta_dir / ("d.txt" if kind == "genesis_rom" else "seed_starts.txt")
    return [int(t, 16) for t in p.read_text().split()] if p.exists() else []


def image_region_metrics(classes: bytes, seeds: list, scores) -> dict:
    """Per-image SEG-075 metrics plus B35/B50 strata (pure given numbers)."""
    import segarecomp_xplat_explore_eval as xe
    m = xe.image_metrics_ext(classes, seeds, scores)
    return {"informative": m["informative"], "er": m["er"], "positive_cells": m["positive_cells"], "starts": m["starts"],
            **{k: m[k] for k in m if k.startswith(("misses_B", "missed_cells_B", "negative_admitted_B")) and k.split("_")[-1] in POLS},
            "strata": {s: {k: v for k, v in m["strata"][s].items() if k != "ranks"} for s in STRATA}}


def fold_summary(images: list) -> dict:
    """Lineage-level aggregation of image metrics (lineage max ER over informative images; recall and misses summed over images)."""
    inf = [i for i in images if i["informative"]]
    out = {"er_max": max((i["er"] for i in inf), default=None), "er_mean_image": (sum(i["er"] for i in inf) / len(inf)) if inf else None,
           "images": len(images), "starts": sum(i["starts"] for i in images)}
    pos = sum(i["positive_cells"] for i in images)
    for pol in POLS:
        out[f"missed_starts_{pol}"] = sum(i[f"misses_{pol}"] for i in images)
        out[f"recall_cells_{pol}"] = round(1 - sum(i[f"missed_cells_{pol}"] for i in images) / pos, 5) if pos else None
        out[f"negative_admitted_{pol}"] = sum(i[f"negative_admitted_{pol}"] for i in images)
        for s in STRATA:
            out[f"stratum_{s}_missed_{pol}"] = sum(i["strata"][s][f"missed_{pol}"] for i in images)
    return out


def headroom(b0_er_by_fold: dict) -> dict:
    vals = {f: v for f, v in b0_er_by_fold.items() if v is not None}
    return {"folds_ge_005": sum(1 for v in vals.values() if v >= 0.05),
            "folds_ge_010": sum(1 for v in vals.values() if v >= 0.10), "b0_er_by_fold": vals}


def cmd_diagnose(a) -> int:
    tb.verify_plan()
    import numpy as np
    from sklearn.ensemble import HistGradientBoostingClassifier
    from sklearn.metrics import average_precision_score, roc_auc_score
    import segarecomp_ml_v3_train as tr
    import segarecomp_ml_region_gen2_train as g2
    tr.DATA = pathlib.Path(a.registry)
    priv5, priv81 = pathlib.Path(a.private), pathlib.Path(a.private81)
    g5, g81 = sorted(a.games), sorted(a.games81)
    plan_sha = ge.sha256(tb.PLAN.read_bytes())
    if len(g5) < 3:
        raise ge.EvidenceError("the model-A diagnostic needs >= 3 qualified new games")
    for g in g5:
        r = json.loads((pathlib.Path(a.results_dir) / f"{g}.json").read_text())
        if not r["classification"]["class"].startswith("QUALIFIED") or r.get("plan_sha256") != plan_sha:
            raise ge.EvidenceError(f"{g}: not QUALIFIED under the current plan")
    for g in g81:
        r = json.loads((pathlib.Path(a.results81_dir) / f"{g}.json").read_text())
        if not r["classification"]["class"].startswith("QUALIFIED"):
            raise ge.EvidenceError(f"{g}: SEG-081 game not QUALIFIED")
    qualified = tr.qualified_lineages()
    dirs: dict = {}
    for d in tr.registry(set(qualified)):
        dirs.setdefault(json.loads((d / "meta.json").read_text())["lineage"], []).append(d)
    reg_images = [(json.loads((d / "meta.json").read_text())["id"], json.loads((d / "meta.json").read_text())["sha256"],
                   json.loads((d / "meta.json").read_text())["lineage"]) for ds in dirs.values() for d in ds]
    iso = {g: tb.isolation_check(g, reg_images) for g in g5}
    if not all(v["ok"] for v in iso.values()):
        raise ge.EvidenceError(f"corpus isolation collision: {iso}")
    feat_cache: dict = {}

    def feats(im):
        if im.id not in feat_cache:
            feat_cache[im.id] = g2.m1_features(im)
        return feat_cache[im.id]

    new = {}
    for g in g5 + g81:
        root = priv5 if g in g5 else priv81
        new[g] = {"C": tr.Image(root / g / "policy" / "ctl" / "strong"), "T": tr.Image(root / g / "policy" / "tas" / "strong"),
                  "TW": tr.Image(root / g / "policy" / "tas" / "weak")}
        new[g]["TW_w"] = np.array(json.loads((root / g / "policy" / "tas" / "weak" / "weights.json").read_text()))
        new[g]["shingles"] = ml81.code_shingles(new[g]["T"].classes, new[g]["T"].rom) | ml81.code_shingles(new[g]["C"].classes, new[g]["C"].rom)
    cross = {}
    names = g5 + g81
    for i, x in enumerate(names):
        for y in names[i + 1:]:
            cross[f"{x}~{y}"] = round(max(ml81.containment(new[x]["shingles"], new[y]["shingles"]), ml81.containment(new[y]["shingles"], new[x]["shingles"])), 5)
    rng = np.random.RandomState(83)
    matched = {}
    for g in g5:
        t, c = new[g]["T"], new[g]["C"]
        keep = np.zeros(t.n_cells, dtype=bool)
        for lab in (1, 0):
            idx = np.nonzero(t.labels == lab)[0]
            want = int((c.labels == lab).sum())
            if len(idx) > want:
                idx = rng.choice(idx, want, replace=False)
            keep[idx] = True
        matched[g] = keep
    held_sh = {}
    for lin, ds in dirs.items():
        s = set()
        for d in ds:
            im = tr.Image(d)
            s |= ml81.code_shingles(im.classes, im.rom)
        held_sh[lin] = s
    reg_cont = {g: round(max(max(ml81.containment(new[g]["shingles"], held_sh[l]), ml81.containment(held_sh[l], new[g]["shingles"])) for l in held_sh), 5) for g in names}
    res = {"folds": {}, "pooled": {}, "isolation": iso, "pairwise_containment": cross, "registry_containment_max": reg_cont, "withheld": {}, "games": g5, "games81": g81}
    sub_rng = np.random.RandomState(83)
    arm_games = {"B0_sub": [], "B0": [], "C": g5, "T": g5, "TW": g5, "T_matched": g5, "C81": g81, "T81": g81, "T_all": g5 + g81, "C_all": g5 + g81}
    arm_src = {"C": "C", "T": "T", "TW": "TW", "T_matched": "T", "C81": "C", "T81": "T", "T_all": "T", "C_all": "C"}
    pooled = {arm: ([], []) for arm in ARMS}
    region = {arm: [] for arm in ARMS}
    for held in sorted(dirs):
        train = [im for lin in sorted(dirs) if lin != held for im in (tr.Image(d) for d in dirs[lin])]
        test = [tr.Image(d) for d in dirs[held]]
        usable = []
        for g in names:
            cx, cy = ml81.containment(new[g]["shingles"], held_sh[held]), ml81.containment(held_sh[held], new[g]["shingles"])
            if max(cx, cy) > ml81.CONTAINMENT_MAX:
                res["withheld"].setdefault(held, []).append(g)
            else:
                usable.append(g)
        xs = [feats(im)[im.labels >= 0] for im in train]
        ys = [(im.labels[im.labels >= 0] == 1).astype(np.int64) for im in train]
        ws = [np.ones(len(y)) for y in ys]
        hh = []
        for t in test:
            neg = {int(c) for c in np.nonzero(t.labels == 0)[0]}
            hard = bz.hard_cells(t.classes, t.rom, t.tokens.tobytes(), neg)
            hh.append(np.array([(int(c) in hard) for c in np.nonzero(t.labels >= 0)[0]]))
        hh = np.concatenate(hh)
        seeds = [seed_addresses(d, json.loads((d / 'meta.json').read_text())['kind']) for d in dirs[held]]
        fold, thr = {}, None
        for arm in ARMS:
            X, Y, Wt = list(xs), list(ys), list(ws)
            if arm == "B0_sub":
                sel = [sub_rng.rand(len(y)) < 0.9 for y in ys]
                X, Y, Wt = [x[k] for x, k in zip(xs, sel)], [y[k] for y, k in zip(ys, sel)], [w[k] for w, k in zip(ws, sel)]
            for g in arm_games[arm]:
                if g not in usable:
                    continue
                im = new[g][arm_src[arm]]
                keep = im.labels >= 0
                if arm == "T_matched":
                    keep = keep & matched[g]
                wt = new[g]["TW_w"][keep] if arm == "TW" else np.ones(int(keep.sum()))
                X.append(feats(im)[keep])
                Y.append((im.labels[keep] == 1).astype(np.int64))
                Wt.append(wt)
            clf = HistGradientBoostingClassifier(**g2.HGB_CONFIG).fit(np.concatenate(X), np.concatenate(Y), sample_weight=np.concatenate(Wt))
            sc, yy, imgs = [], [], []
            for t, sd in zip(test, seeds):
                logit = clf.decision_function(feats(t))
                keep = t.labels >= 0
                sc.append(logit[keep])
                yy.append(t.labels[keep])
                imgs.append(image_region_metrics(t.classes, sd, logit))
            sc, yy = np.concatenate(sc), np.concatenate(yy)
            if arm == "B0":
                thr = float(np.percentile(sc[yy == 1], 10))
            fold[arm] = {"ap": round(float(average_precision_score(yy, sc)), 5), "auc": round(float(roc_auc_score(yy, sc)), 5), "cells": int(len(yy)),
                         "positives": int((yy == 1).sum()), "train_cells": int(sum(len(y) for y in Y)), "new_games_used": [g for g in arm_games[arm] if g in usable],
                         "fn_rate_positive_cells": round(float((sc[yy == 1] < thr).mean()), 5), "fp_rate_negative_cells": round(float((sc[yy == 0] >= thr).mean()), 5),
                         "fp_rate_hard_negative_cells": round(float((sc[hh] >= thr).mean()), 5) if hh.any() else None, **fold_summary(imgs)}
            pooled[arm][0].append(sc)
            pooled[arm][1].append(yy)
        res["folds"][held] = fold
        print(json.dumps({"fold": held, "ap": {k: v["ap"] for k, v in fold.items()}, "er": {k: v["er_max"] for k, v in fold.items()}}), flush=True)
    for arm, (s, y) in pooled.items():
        s, y = np.concatenate(s), np.concatenate(y)
        res["pooled"][arm] = {"ap": round(float(average_precision_score(y, s)), 5), "auc": round(float(roc_auc_score(y, s)), 5)}
    folds = res["folds"]

    def mean(arm, key):
        v = [folds[f][arm][key] for f in folds if folds[f][arm][key] is not None]
        return round(sum(v) / len(v), 5) if v else None
    res["genesis"] = {arm: {"mean_er": mean(arm, "er_max"), "worst_er": max(folds[f][arm]["er_max"] for f in folds if folds[f][arm]["er_max"] is not None),
                            "missed_starts_B35": sum(folds[f][arm]["missed_starts_B35"] for f in folds), "missed_starts_B50": sum(folds[f][arm]["missed_starts_B50"] for f in folds),
                            "mean_recall_cells_B35": mean(arm, "recall_cells_B35"), "mean_recall_cells_B50": mean(arm, "recall_cells_B50"),
                            "boundary_missed_B35": sum(folds[f][arm]["stratum_boundary_missed_B35"] for f in folds),
                            "boundary_missed_B50": sum(folds[f][arm]["stratum_boundary_missed_B50"] for f in folds),
                            "fn_rate": mean(arm, "fn_rate_positive_cells"), "fp_rate": mean(arm, "fp_rate_negative_cells"),
                            "fp_rate_hard": mean(arm, "fp_rate_hard_negative_cells")} for arm in ARMS}

    noise = round(sum(abs(folds[f]["B0_sub"]["er_max"] - folds[f]["B0"]["er_max"]) for f in folds if folds[f]["B0"]["er_max"] is not None) /
                  max(1, sum(1 for f in folds if folds[f]["B0"]["er_max"] is not None)), 5)
    res["noise_floor_er"] = noise

    def paired(x, y, key="er_max", sign=1):
        d = [sign * (folds[f][x][key] - folds[f][y][key]) for f in folds if folds[f][x][key] is not None and folds[f][y][key] is not None]
        tol = noise if key == "er_max" else 0
        return {"mean_delta": round(sum(d) / len(d), 5), "exceeds_noise": abs(sum(d) / len(d)) > noise if key == "er_max" else None,
                "folds_better": sum(1 for v in d if v < -tol - 1e-9), "folds_worse": sum(1 for v in d if v > tol + 1e-9), "folds": len(d)}
    pairs = (("T", "B0"), ("T", "C"), ("T", "T_matched"), ("C", "B0"), ("T_all", "B0"), ("T_all", "T81"), ("T81", "B0"), ("TW", "T"))
    res["paired_er"] = {f"{x}-{y}": paired(x, y) for x, y in pairs}
    res["paired_missed_starts"] = {f"{x}-{y}@{p}": paired(x, y, f"missed_starts_{p}") for x, y in pairs for p in POLS}
    res["paired_recall_cells"] = {f"{x}-{y}@{p}": paired(x, y, f"recall_cells_{p}", -1) for x, y in pairs for p in POLS}
    res["residual"] = {f: {"b0": folds[f]["B0"]["er_max"], "t_all": folds[f]["T_all"]["er_max"],
                           "residual": bool(folds[f]["T_all"]["er_max"] is not None and folds[f]["T_all"]["er_max"] >= t5.READY["floor_residual_er"] and
                                            (folds[f]["B0"]["er_max"] - folds[f]["T_all"]["er_max"]) <= noise)} for f in folds}
    res["residual_folds"] = sum(1 for v in res["residual"].values() if v["residual"])
    res["difficult_folds"] = {f: {arm: {"er_max": folds[f][arm]["er_max"], "ap": folds[f][arm]["ap"]} for arm in ("B0", "C", "T", "T_matched", "T_all")} for f in ("D01", "D03", "D07") if f in folds}
    res["headroom_historical_not_a_criterion"] = headroom({f: folds[f]["B0"]["er_max"] for f in folds})
    rates = {arm: {k: mean(arm, k) for k in ("fn_rate_positive_cells", "fp_rate_negative_cells", "fp_rate_hard_negative_cells")} for arm in ARMS}
    res["macro_error_rates"] = rates
    res["verdict"] = ml81.ml_verdict(res["pooled"], folds, rates)
    res["plan_sha256"] = plan_sha
    pathlib.Path(a.out).write_text(json.dumps(res, indent=1, sort_keys=True))
    print(json.dumps({"pooled": res["pooled"], "genesis": res["genesis"], "residual_folds": res["residual_folds"], "noise": res["noise_floor_er"], "verdict": res["verdict"]["verdict"]}, sort_keys=True))
    return 0


def cmd_measure(a) -> int:
    """Readiness measurements from per-game results (five new + SEG-081), containment and the diagnostic; prints the frozen decision."""
    tb.verify_plan()
    diag = json.loads(pathlib.Path(a.diagnostic).read_text())
    res5 = {g: json.loads((pathlib.Path(a.results_dir) / f"{g}.json").read_text()) for g in a.games}
    res81 = {g: json.loads((pathlib.Path(a.results81_dir) / f"{g}.json").read_text()) for g in a.games81}
    cls = {g: r["classification"]["class"] for g, r in {**res5, **res81}.items()}
    pair = {tuple(k.split("~")): v for k, v in diag["pairwise_containment"].items()}
    use_new = sorted(g for g in a.games if cls[g] == "QUALIFIED_USEFUL")
    use_all = sorted(g for g, c in cls.items() if c == "QUALIFIED_USEFUL")
    indep_new = t5.independent_lineages(use_new, pair, diag["registry_containment_max"])
    indep_all = t5.independent_lineages(use_all, pair, diag["registry_containment_max"])
    reg = tb.sigs_from_json(json.loads(pathlib.Path(a.registry_sigs).read_text()))
    roots = {**{g: pathlib.Path(a.private) for g in a.games}, **{g: pathlib.Path(a.private81) for g in a.games81}}

    def sigset(games):
        f, c = set(), set()
        for g in games:
            s = tb.sigs_from_json(json.loads((roots[g] / g / "sigs_tas_strong.json").read_text()))
            f |= s["forms"]
            c |= s["cfg"]
        return f, c
    nf, nc = sigset(sorted(g for c in indep_new["clusters"] for g in c))
    of, oc = sigset(a.games81)
    novel = len((nf - reg["forms"] - of) | (nc - reg["cfg"] - oc))
    hard = sum(res5[g]["data"]["strong"]["tas"]["hard_negative_cells"] for c in indep_new["clusters"] for g in c)
    prov = (not any(c == "LABEL_POLICY_CONFLICT" for c in cls.values())) and bool(a.snapshot_verified)
    m = {"new_useful_independent_lineages": indep_new["effective_independent"], "total_useful_independent_lineages": indep_all["effective_independent"],
         "new_novel_signatures": novel, "new_hard_negative_cells": hard, "residual_folds": diag["residual_folds"], "provenance_ok": prov}
    out = {"measurements": m, "decision": t5.readiness_decision(m), "independence_new": indep_new, "independence_all": indep_all, "classes": cls,
           "acquisition": t5.acquisition_class({g: cls[g] for g in a.games}, a.open_review_blockers), "thresholds": t5.READY,
           "note": "SEG-081 classes were judged against the player-1-only control; only the five new games were judged against the all-controller control"}
    pathlib.Path(a.out).write_text(json.dumps(out, indent=1, sort_keys=True))
    print(json.dumps(out, sort_keys=True))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("diagnose")
    p.add_argument("--registry", required=True), p.add_argument("--private", required=True), p.add_argument("--private81", required=True)
    p.add_argument("--games", nargs="+", required=True), p.add_argument("--games81", nargs="+", default=["vm2", "bnr", "ewj2"])
    p.add_argument("--results-dir", required=True), p.add_argument("--results81-dir", required=True), p.add_argument("--out", required=True)
    p.set_defaults(fn=cmd_diagnose)
    p = sub.add_parser("measure")
    p.add_argument("--diagnostic", required=True), p.add_argument("--private", required=True), p.add_argument("--private81", required=True)
    p.add_argument("--games", nargs="+", required=True), p.add_argument("--games81", nargs="+", default=["vm2", "bnr", "ewj2"])
    p.add_argument("--results-dir", required=True), p.add_argument("--results81-dir", required=True), p.add_argument("--registry-sigs", required=True)
    p.add_argument("--snapshot-verified", action="store_true"), p.add_argument("--open-review-blockers", type=int, default=0), p.add_argument("--out", required=True)
    p.set_defaults(fn=cmd_measure)
    a = ap.parse_args(argv)
    try:
        return a.fn(a)
    except ge.EvidenceError as e:
        print(json.dumps({"error": str(e)}), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
