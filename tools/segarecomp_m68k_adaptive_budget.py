#!/usr/bin/env python3
"""SEG-061-T001: PRIVATE research driver for the M68K adaptive-budget feasibility experiment (ADR 0124). Never a production route.

Checkpoint A reduced fold: the frozen SEG-049 five-title out-of-fold M3 scores are NOT recoverable on every machine
(Shining Force / Phantasy Star II truth needs wine-hosted ASM68K/asw builds). `reduced-fold` reuses the frozen M3 recipe
(`segarecomp_ml_region_gen2_train.fit_and_score`, seed 49) with a REDUCED training set (the other available titles). Its scores are NOT the
frozen out-of-fold predictions and are never labelled as such; they are exploratory only. numpy/torch imported lazily (private env).
"""
from __future__ import annotations

import argparse
import json
import pathlib
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import segarecomp_ml_region_gen2 as g  # noqa: E402
import segarecomp_ml_region_gen2_train as tr  # noqa: E402

AVAILABLE = ("s1", "flicky", "sk")


def reduced_path(held: str) -> pathlib.Path:
    return tr.ROOT / "scores" / "reduced" / f"M3.{held}.npy"


def cmd_reduced_fold(args) -> int:
    import numpy as np
    held = args.held_out
    train_ids = [t for t in AVAILABLE if t != held]
    g.assert_split(train_ids, held)
    titles = {t: tr.Title(t) for t in AVAILABLE}
    started = time.time()
    out = tr.fit_and_score("M3", {t: titles[t] for t in train_ids} | {held: titles[held]}, train_ids, [held],
                           log=lambda m: print(f"[M3-reduced/{held}] {m}", flush=True))
    scores = out[held]
    path = reduced_path(held)
    path.parent.mkdir(parents=True, exist_ok=True)
    np.save(path, scores)
    labels = titles[held].labels
    rec = {"held_out": held, "train": train_ids, "cells": int(len(scores)), "frf": g.frf(scores, labels.tolist()),
           "seconds": time.time() - started, "score_digest": g.prediction_digest({held: scores.tolist()}), "reduced_not_frozen": True}
    (path.parent / f"M3.{held}.json").write_text(json.dumps(rec, indent=1, sort_keys=True))
    print(json.dumps({"held_out": held, "frf": round(rec["frf"]["frf"], 4), "secs": round(rec["seconds"])}))
    return 0


# --------------------------------------------------------------------------------------------- Checkpoint B (FROZEN before truth is joined)
FEATURE_SPEC = {
    "A_shape": "logit(score) at descending rank fractions 10/20/30 % (L10, L20, L30) and L10-L30; within-ROM rank based",
    "B_boundary": "tie_frac25 = fraction of cells whose score equals the score at rank 25 %; max_gap = largest adjacent logit gap among ranks 10..40 %",
    "C_structural": "NOT EVALUATED (optional family; no decoder-token disagreement feature defined before data)",
}


def features(scores) -> dict:
    """Label-free, within-ROM, rank-based features (stdlib only, deterministic)."""
    import math
    s = sorted((float(x) for x in scores), reverse=True)
    n = len(s)
    eps = 1e-12
    lg = [math.log(min(max(x, eps), 1 - eps) / min(max(1 - x, eps), 1)) for x in s]

    def at(f):
        return lg[min(n - 1, int(f * n))]
    lo, hi = int(0.10 * n), int(0.40 * n)
    gaps = [lg[i] - lg[i + 1] for i in range(lo, min(hi, n - 1))]
    pivot = s[min(n - 1, int(0.25 * n))]
    return {"L10": at(0.10), "L20": at(0.20), "L30": at(0.30), "shape_L10_L30": at(0.10) - at(0.30),
            "tie_frac25": sum(1 for x in s if x == pivot) / n, "max_gap_10_40": max(gaps) if gaps else 0.0}


def cmd_features(args) -> int:
    import numpy as np
    out = {}
    for h in AVAILABLE:
        sc = np.load(reduced_path(h))
        out[h] = features(sc)
        if args.with_truth:
            t = tr.Title(h)
            fr = g.frf(sc, t.labels.tolist())["frf"]
            out[h]["required_fraction_frf"] = fr
            out[h]["b35_headroom"] = 0.35 / fr
    print(json.dumps(out, indent=1, sort_keys=True))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("reduced-fold")
    p.add_argument("--held-out", choices=AVAILABLE, required=True)
    p.set_defaults(func=cmd_reduced_fold)
    q = sub.add_parser("features")
    q.add_argument("--with-truth", action="store_true")
    q.set_defaults(func=cmd_features)
    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
