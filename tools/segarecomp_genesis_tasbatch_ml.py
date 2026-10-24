#!/usr/bin/env python3
"""SEG-081-T001: bounded cross-game model-A diagnostic (OFFLINE; needs numpy and scikit-learn).

Frozen model A (SEG-072 M1 HistGradientBoosting, HGB_CONFIG unchanged), leave-one-lineage-out over every qualified SEG-072 lineage. Arms add the
qualified new games' labels to each fold's training pool:  B0 base | C seed-1 control strong | T TAS strong | TW TAS strong+weak (cell weights) |
T_matched TAS strong down-sampled to the control's positive/negative cell counts (sample-volume sensitivity).

Isolation: new-game image SHA-256 and lineage ids must be absent from the registry; a game is withheld from a fold when 12-byte-shingle containment of
labelled code runs between it and the held-out lineage exceeds 0.30 in either direction. The verdict is the pure function `ml_verdict` (frozen rules).
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
import segarecomp_genesis_tasbatch as tb  # noqa: E402

SHINGLE = 12
CONTAINMENT_MAX = 0.30
ARMS = ("B0", "C", "T", "TW", "T_matched")
R = {"gain_ap": 0.005, "gain_vs_control": 0.003, "worst_lineage": -0.02, "fn_gain": 0.01, "fp_gain": 0.002, "fn_harm": 0.02, "fp_harm": 0.005, "sensitivity": 0.005}


def ml_verdict(P: dict, folds: dict, rates: dict) -> dict:
    """Frozen three-state verdict (pure). P: pooled {arm: {ap, auc}}; folds: {lineage: {arm: {ap}}}; rates: pooled mean error rates per arm."""
    d = {arm: round(P[arm]["ap"] - P["B0"]["ap"], 5) for arm in P}
    sens = max(abs(d["T"]), abs(P["T"]["ap"] - P["C"]["ap"]))
    worst = min(folds[f]["T"]["ap"] - folds[f]["B0"]["ap"] for f in folds)
    dt_c = P["T"]["ap"] - P["C"]["ap"]
    fn = rates["T"]["fn_rate_positive_cells"] - rates["B0"]["fn_rate_positive_cells"]
    fp = rates["T"]["fp_rate_negative_cells"] - rates["B0"]["fp_rate_negative_cells"]
    out = {"delta_ap_vs_B0": d, "delta_T_minus_C": round(dt_c, 5), "worst_lineage_delta_T": round(worst, 5), "fn_delta_T": round(fn, 5), "fp_delta_T": round(fp, 5),
           "sensitivity_max_abs_delta": round(sens, 5)}
    if sens < R["sensitivity"]:
        out.update(verdict="ML_INCONCLUSIVE", reason="the diagnostic cannot distinguish the inputs (max |pooled AP delta| below 0.005; saturated baseline)")
    elif d["T"] <= -R["gain_ap"] or (worst < R["worst_lineage"] and d["T"] < 0) or fn > R["fn_harm"] or fp > R["fp_harm"]:
        out.update(verdict="ML_HARM_SUPPORTED", reason="TAS strong labels lowered pooled AP, a lineage, or the fixed-operating-point error rates beyond the frozen margins")
    elif d["T"] >= R["gain_ap"] and dt_c >= R["gain_vs_control"] and worst >= R["worst_lineage"] and fn <= R["fn_gain"] and fp <= R["fp_gain"] \
            and d.get("T_matched", 0) >= 0.5 * d["T"]:
        out.update(verdict="ML_GAIN_SUPPORTED", reason="TAS strong labels beat B0 and the control with no lineage or error-rate regression")
    else:
        out.update(verdict="ML_INCONCLUSIVE", reason="volume-confounded or within the frozen margins" if d["T"] >= R["gain_ap"] and d.get("T_matched", 0) < 0.5 * d["T"]
                   else "effects within the frozen margins")
    return out


def code_shingles(classes: bytes, rom: bytes) -> set:
    """Hashes of 12-byte windows inside contiguous labelled code runs (I/c)."""
    out, i, n = set(), 0, len(classes)
    while i < n:
        if classes[i] in (73, 99):
            j = i
            while j < n and classes[j] in (73, 99):
                j += 1
            for k in range(i, j - SHINGLE + 1):
                out.add(int.from_bytes(hashlib.blake2b(rom[k:k + SHINGLE], digest_size=8).digest(), "big"))
            i = j
        else:
            i += 1
    return out


def containment(x: set, y: set) -> float:
    return len(x & y) / len(x) if x else 0.0


def cmd_diagnose(a) -> int:
    tb.verify_plan()
    import numpy as np
    from sklearn.ensemble import HistGradientBoostingClassifier
    from sklearn.metrics import average_precision_score, roc_auc_score
    import segarecomp_ml_v3_train as tr
    import segarecomp_ml_region_gen2_train as g2
    tr.DATA = pathlib.Path(a.registry)
    priv = pathlib.Path(a.private)
    games = sorted(a.games)
    plan_sha = ge.sha256(tb.PLAN.read_bytes())
    if len(games) < 2:
        raise ge.EvidenceError("the model-A diagnostic needs >= 2 qualified games")
    for g in games:
        r = json.loads((pathlib.Path(a.results_dir) / f"{g}.json").read_text())
        if not r["classification"]["class"].startswith("QUALIFIED") or r.get("plan_sha256") != plan_sha:
            raise ge.EvidenceError(f"{g}: not QUALIFIED under the current plan")
    qualified = tr.qualified_lineages()
    dirs: dict = {}
    for d in tr.registry(set(qualified)):
        dirs.setdefault(json.loads((d / "meta.json").read_text())["lineage"], []).append(d)
    reg_images = [(json.loads((d / "meta.json").read_text())["id"], json.loads((d / "meta.json").read_text())["sha256"],
                   json.loads((d / "meta.json").read_text())["lineage"]) for ds in dirs.values() for d in ds]
    iso = {g: tb.isolation_check(g, reg_images) for g in games}
    if not all(v["ok"] for v in iso.values()):
        raise ge.EvidenceError(f"corpus isolation collision: {iso}")
    feat_cache: dict = {}

    def feats(im):
        if im.id not in feat_cache:
            feat_cache[im.id] = g2.m1_features(im)
        return feat_cache[im.id]

    new = {}
    for g in games:
        new[g] = {"C": tr.Image(priv / g / "policy" / "ctl" / "strong"), "T": tr.Image(priv / g / "policy" / "tas" / "strong"),
                  "TW": tr.Image(priv / g / "policy" / "tas" / "weak")}
        new[g]["TW_w"] = np.array(json.loads((priv / g / "policy" / "tas" / "weak" / "weights.json").read_text()))
        new[g]["shingles"] = code_shingles(new[g]["T"].classes, new[g]["T"].rom)
    results_cross = {}
    for i, g1 in enumerate(games):
        for g2_ in games[i + 1:]:
            results_cross[f"{g1}~{g2_}"] = [round(containment(new[g1]["shingles"], new[g2_]["shingles"]), 5), round(containment(new[g2_]["shingles"], new[g1]["shingles"]), 5)]
    rng = np.random.RandomState(81)
    matched = {}
    for g in games:
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
            s |= code_shingles(im.classes, im.rom)
        held_sh[lin] = s
    results = {"folds": {}, "pooled": {}, "isolation": iso, "cross_game_containment": results_cross, "containment": {}, "withheld": {}}
    pooled = {arm: ([], []) for arm in ARMS}
    for held in sorted(dirs):
        train = [im for lin in sorted(dirs) if lin != held for im in (tr.Image(d) for d in dirs[lin])]
        test = [tr.Image(d) for d in dirs[held]]
        usable = []
        for g in games:
            cx = containment(new[g]["shingles"], held_sh[held])
            cy = containment(held_sh[held], new[g]["shingles"])
            results["containment"].setdefault(held, {})[g] = [round(cx, 5), round(cy, 5)]
            if max(cx, cy) > CONTAINMENT_MAX:
                results["withheld"].setdefault(held, []).append(g)
            else:
                usable.append(g)
        xs = [feats(im)[im.labels >= 0] for im in train]
        ys = [(im.labels[im.labels >= 0] == 1).astype(np.int64) for im in train]
        ws = [np.ones(len(y)) for y in ys]
        hh = []
        for t in test:
            neg = {int(c) for c in np.nonzero(t.labels == 0)[0]}
            hard = bz.hard_cells(t.classes, t.rom, t.tokens.tobytes(), neg)
            keep = t.labels >= 0
            hh.append(np.array([(int(c) in hard) for c in np.nonzero(keep)[0]]))
        hh = np.concatenate(hh)
        res, thr = {}, None
        for arm in ARMS:
            X, Y, Wt = list(xs), list(ys), list(ws)
            for g in usable:
                if arm == "B0":
                    continue
                im = {"C": new[g]["C"], "T": new[g]["T"], "TW": new[g]["TW"], "T_matched": new[g]["T"]}[arm]
                keep = im.labels >= 0
                if arm == "T_matched":
                    keep = keep & matched[g]
                wt = new[g]["TW_w"][keep] if arm == "TW" else np.ones(int(keep.sum()))
                X.append(feats(im)[keep])
                Y.append((im.labels[keep] == 1).astype(np.int64))
                Wt.append(wt)
            clf = HistGradientBoostingClassifier(**g2.HGB_CONFIG).fit(np.concatenate(X), np.concatenate(Y), sample_weight=np.concatenate(Wt))
            sc, yy = [], []
            for t in test:
                p = clf.predict_proba(feats(t))[:, 1]
                keep = t.labels >= 0
                sc.append(p[keep])
                yy.append(t.labels[keep])
            sc, yy = np.concatenate(sc), np.concatenate(yy)
            if arm == "B0":
                thr = float(np.percentile(sc[yy == 1], 10))
            res[arm] = {"ap": round(float(average_precision_score(yy, sc)), 5), "auc": round(float(roc_auc_score(yy, sc)), 5), "cells": int(len(yy)),
                        "positives": int((yy == 1).sum()), "train_cells": int(sum(len(y) for y in Y)), "new_games_used": usable if arm != "B0" else [],
                        "fn_rate_positive_cells": round(float((sc[yy == 1] < thr).mean()), 5), "fp_rate_negative_cells": round(float((sc[yy == 0] >= thr).mean()), 5),
                        "fp_rate_hard_negative_cells": round(float((sc[hh] >= thr).mean()), 5) if hh.any() else None}
            pooled[arm][0].append(sc)
            pooled[arm][1].append(yy)
        results["folds"][held] = res
        print(json.dumps({"fold": held, "ap": {k: v["ap"] for k, v in res.items()}}), flush=True)
    for arm, (s, y) in pooled.items():
        s, y = np.concatenate(s), np.concatenate(y)
        results["pooled"][arm] = {"ap": round(float(average_precision_score(y, s)), 5), "auc": round(float(roc_auc_score(y, s)), 5)}
    rates = {arm: {k: round(float(np.mean([results["folds"][f][arm][k] for f in results["folds"] if results["folds"][f][arm][k] is not None])), 5)
                   for k in ("fn_rate_positive_cells", "fp_rate_negative_cells", "fp_rate_hard_negative_cells")} for arm in ARMS}
    results["macro_error_rates"] = rates
    results["verdict"] = ml_verdict(results["pooled"], results["folds"], rates)
    results["per_lineage_delta_ap"] = {f: {arm: round(results["folds"][f][arm]["ap"] - results["folds"][f]["B0"]["ap"], 5) for arm in ARMS} for f in results["folds"]}
    results["games"] = games
    results["plan_sha256"] = ge.sha256(tb.PLAN.read_bytes())
    pathlib.Path(a.out).write_text(json.dumps(results, indent=1, sort_keys=True))
    print(json.dumps({"pooled": results["pooled"], "verdict": results["verdict"]}, sort_keys=True))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("diagnose")
    p.add_argument("--registry", required=True), p.add_argument("--private", required=True), p.add_argument("--games", nargs="+", required=True)
    p.add_argument("--results-dir", required=True), p.add_argument("--out", required=True)
    p.set_defaults(fn=cmd_diagnose)
    a = ap.parse_args(argv)
    try:
        return a.fn(a)
    except ge.EvidenceError as e:
        print(json.dumps({"error": str(e)}), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
