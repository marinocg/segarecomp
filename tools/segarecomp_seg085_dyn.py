#!/usr/bin/env python3
"""SEG-085-T003 (ADR 0193): DYN_SCORE, unchanged ADAPT on structural model A, and leakage-safe calibration (OFFLINE research code; nothing here is a production route).

DYN_SCORE (frozen formula; no search, no per-game input)
  1. cell scores = the unchanged structural model A logits (128-byte cells);
  2. select every cell with score >= T (ties inclusive, so the result is order-independent) and union the independently established seed cells;
  3. abstain to broad AOT when the retained fraction exceeds CAP (0.60, the SEG-072 `MAX_RETENTION`) or when the inputs violate the applicability assumptions
     (non-finite score, no cells, no seed universe): abstention and rejection are counted separately and never truncate a selection;
  4. T is fitted only on development records:  m_L = lowest non-seed positive-cell score of lineage L;  T0 = min_L m_L;  shortfall = max_L (min_{L' != L} m_L' - m_L)
     (the leave-one-lineage-out miss of the plain minimum rule);  T = T0 - shortfall.  Records of lineage L come from a model that did NOT train on L and (nested
     fits) did not train on the held-out lineage either (bounded inner refits: one fit per ordered lineage pair).

ADAPT is the unchanged SEG-072 rule (`segarecomp_ml_v3.adaptive_budget`/`fit_adaptive_margin`); the only adapter is that model A's logit is the rank key and
`sigmoid(logit)` the probability. Commands: inner | nested | freeze.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_ml_v3 as v3  # noqa: E402

SCHEMA = "segarecomp.seg085.dyn.v1"
HARNESS = pathlib.Path(os.environ.get("SEGARECOMP_HARNESS_ROOT", "/root/dev/segarecomp-harness"))
CACHE = pathlib.Path(os.environ.get("SEG085_CACHE", str(HARNESS / ".cache" / "seg085")))
CAP = v3.MAX_RETENTION          # 0.60, frozen
GENESIS = ("D01", "D02", "D03", "D04", "D05", "D06", "D07", "D09", "D10", "D11", "D12")
PLAN = HERE / "segarecomp_seg085_dyn.plan.json"
PLAN_DIGEST = HERE / "segarecomp_seg085_dyn.plan.digest"
RESULTS = HERE / "segarecomp_seg085_dyn.results.json"


# ------------------------------------------------------------------------------------------------------------------------------- policy
def finite_scores(scores) -> list:
    out = []
    for x in scores:
        if isinstance(x, bool) or not isinstance(x, (int, float)) or not math.isfinite(x):
            raise ValueError("non-finite or non-numeric score")
        out.append(float(x))
    return out


def dyn_select(scores, seed_cells, threshold: float, cap: float = CAP, have_universe: bool = True) -> dict:
    """DYN_SCORE for one image. Returns {status, region (cell set), retained, cells_before_seeds, ...}. Pure and deterministic."""
    if not have_universe:
        return {"status": "unsupported", "region": set(), "retained": None, "reason": "no_seed_universe"}
    try:
        s = finite_scores(scores)
    except ValueError:
        return {"status": "rejected", "region": set(), "retained": None, "reason": "non_finite_score"}
    n = len(s)
    if n == 0 or not math.isfinite(threshold):
        return {"status": "rejected", "region": set(), "retained": None, "reason": "empty_or_bad_threshold"}
    scored = {c for c in range(n) if s[c] >= threshold}
    seeds = {c for c in seed_cells if 0 <= c < n}
    region = scored | seeds
    retained = len(region) / n
    if retained > cap + 1e-12:
        return {"status": "abstain_cap", "region": set(range(n)), "retained": 1.0, "cells_before_seeds": len(scored), "selected_fraction": retained}
    return {"status": "selected", "region": region, "retained": retained, "cells_before_seeds": len(scored), "seed_only_cells": len(seeds - scored)}


def lineage_minima(records) -> dict:
    """records: iterable of (lineage, min non-seed positive score or None). -> {lineage: min over its images}. Pure."""
    out = {}
    for L, m in records:
        if m is None:
            continue
        if not math.isfinite(m):
            raise ValueError("non-finite record")
        out[L] = min(m, out.get(L, math.inf))
    return out


def calibrate_threshold(records) -> dict:
    """T = T0 - shortfall (see module docstring). Needs at least two fitting lineages; otherwise the policy is not calibrated (fail closed)."""
    mins = lineage_minima(records)
    if len(mins) < 2:
        raise ValueError("calibration needs at least two fitting lineages")
    t0 = min(mins.values())
    short = 0.0
    for L, m in mins.items():
        others = min(v for k, v in mins.items() if k != L)
        short = max(short, others - m)
    return {"T0": t0, "shortfall": short, "T": t0 - short, "lineages": len(mins), "minima": mins}


def image_record(scores, labels, seed_cells):
    """Lowest non-seed positive-cell score of an image (None if it has none)."""
    seeds = set(seed_cells)
    vals = [float(x) for x, y, c in zip(scores, labels, range(len(labels))) if y == 1 and c not in seeds]
    return min(vals) if vals else None


def policy_outcome(policy: str, scores, probs, seed_cells, labels, classes: bytes, threshold=None, margin=None, have_universe=True) -> dict:
    """Common per-image outcome for B35, B50, ADAPT (unchanged) and DYN_SCORE: retained fraction, missed verified starts, abstention/rejection flags."""
    n = len(scores)
    starts = v3.starts_of(classes)
    if policy == "DYN_SCORE":
        r = dyn_select(scores, seed_cells, threshold, have_universe=have_universe)
        if r["status"] in ("rejected", "unsupported"):
            return {"status": r["status"], "retained": None, "missed_starts": None, "abstained": False}
        region, abst = r["region"], r["status"] == "abstain_cap"
        before = r.get("cells_before_seeds")
    else:
        r = v3.policy_region(policy, [float(x) for x in scores], [float(p) for p in probs], set(seed_cells), margin)
        region, abst = r["region"], r["abstained"]
        before = None
    return {"status": "abstain_cap" if abst else "selected", "retained": len(region) / n, "missed_starts": v3.misses(starts, region), "abstained": abst,
            "cells_before_seeds": before, "selected_fraction_before_cap": r.get("selected_fraction") if policy == "DYN_SCORE" else None,
            "positive_cells_missed": sum(1 for c in range(n) if labels[c] == 1 and c not in region)}


def sigmoid(x: float) -> float:
    return 1.0 / (1.0 + math.exp(-max(-700.0, min(700.0, x))))


# --------------------------------------------------------------------------------------------------------------------- model-A scoring
_STATE = {}


def load_images():
    import segarecomp_seg084_a as s84
    import segarecomp_xplat_models as xm
    imgs, _ = s84.load_registry_images()
    feats = {im.id: xm.a_features(im, xm.relocs_of(im)) for im in imgs}
    return s84, xm, imgs, feats


def train_matrix(np, imgs, feats, lineages):
    keep = [(feats[im.id][im.labels >= 0], (im.labels[im.labels >= 0] == 1).astype(np.int64)) for im in imgs if im.lineage in lineages]
    return np.concatenate([k[0] for k in keep]), np.concatenate([k[1] for k in keep])


def _inner_job(job):
    held, cal = job
    import numpy as np
    s84, xm, imgs, feats = _STATE["ctx"]
    train = [L for L in GENESIS if L not in (held, cal)]
    X, y = train_matrix(np, imgs, feats, set(train))
    clf = s84.fit_arrays(X, y)
    return held, cal, {im.id: s84.logit(clf, feats[im.id]) for im in imgs if im.lineage == cal}


def cmd_inner(a) -> int:
    """Bounded inner refits: for every ordered pair (held, calibration lineage) one model-A fit on the other nine lineages scoring the calibration lineage.
    Isolation: neither the held lineage nor the calibration lineage is in that model's training data."""
    import numpy as np
    from concurrent.futures import ProcessPoolExecutor
    os.environ.setdefault("OMP_NUM_THREADS", "2")
    _STATE["ctx"] = load_images()
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    jobs = [(h, c) for h in GENESIS for c in GENESIS if h != c and not (out / f"{h}.{c}.npz").exists()]
    with ProcessPoolExecutor(max_workers=a.workers) as ex:
        for held, cal, sc in ex.map(_inner_job, jobs):
            np.savez_compressed(out / f"{held}.{cal}.npz", **{k.replace("/", "@@"): v for k, v in sc.items()})
            print(json.dumps({"held": held, "cal": cal, "images": len(sc)}), flush=True)
    return 0


def cmd_nested(a) -> int:
    import numpy as np
    import segarecomp_xplat_transfer as xt
    import segarecomp_ml_v3_eval as ev
    s84, xm, imgs, feats = load_images()
    inner = pathlib.Path(a.inner)
    by_l = {L: [im for im in imgs if im.lineage == L] for L in GENESIS}

    def fold_scores(L):
        return xt.load_npz(s84.score_path(L, "A0"))

    oof = {L: fold_scores(L) for L in GENESIS}                           # model A trained on the other ten lineages (SEG-084 A0 = SEG-076 A)
    rec_oof = {L: [(L, image_record(oof[L][im.id], [int(x) for x in im.labels], im.seeds)) for im in by_l[L]] for L in GENESIS}
    results = {"schema": SCHEMA + ".nested", "cap": CAP, "per_image": {}, "folds": {}, "variants": {}}
    adapt_reg = {im.id: im for im in imgs}
    for held in GENESIS:
        # isolated calibration records: lineage C scored by a model trained on neither C nor the held lineage
        iso = []
        for C in GENESIS:
            if C == held:
                continue
            z = xt.load_npz(inner / f"{held}.{C}.npz")
            iso += [(C, image_record(z[im.id], [int(x) for x in im.labels], im.seeds)) for im in by_l[C]]
        cal_iso = calibrate_threshold(iso)
        # non-isolated records (the SEG-072 ADAPT protocol's limitation): lineage C scored by its own leave-one-lineage-out model, which trained on the held lineage
        leak = [r for C in GENESIS if C != held for r in rec_oof[C]]
        cal_leak = calibrate_threshold(leak)
        results["folds"][held] = {"T_isolated": cal_iso["T"], "T0_isolated": cal_iso["T0"], "shortfall_isolated": cal_iso["shortfall"],
                                  "T_nonisolated": cal_leak["T"], "minima_isolated": {k: round(v, 4) for k, v in cal_iso["minima"].items()}}
        # ADAPT margin fits: unchanged rule; records either from the OOF scores (SEG-072 protocol) or from the isolated inner scores (sensitivity)
        def adapt_fit(score_of):
            recs = []
            for C in GENESIS:
                if C == held:
                    continue
                for im in by_l[C]:
                    sc = score_of(C, im)
                    labels = [int(x) for x in im.labels]
                    recs.append((v3.residual_frf([float(x) for x in sc], labels, im.seeds), sum(1 for x in sc if sigmoid(float(x)) >= 0.5) / len(sc)))
            return v3.fit_adaptive_margin(recs)
        m_leak = adapt_fit(lambda C, im: oof[C][im.id])
        zi = {C: xt.load_npz(inner / f"{held}.{C}.npz") for C in GENESIS if C != held}
        m_iso = adapt_fit(lambda C, im: zi[C][im.id])
        results["folds"][held].update({"adapt_margin_nonisolated": m_leak, "adapt_margin_isolated": m_iso})
        for im in by_l[held]:
            sc = oof[held][im.id]
            labels = [int(x) for x in im.labels]
            probs = [sigmoid(float(x)) for x in sc]
            row = {"lineage": held, "cells": len(labels), "positive_cells": labels.count(1),
                   "oracle_residual_frf": v3.residual_frf([float(x) for x in sc], labels, im.seeds)}
            for pol, kw in (("B35", {}), ("B50", {}), ("ADAPT", {"margin": m_leak}), ("DYN_SCORE", {"threshold": cal_iso["T"]})):
                row[pol] = policy_outcome(pol, sc, probs, im.seeds, labels, im.classes, **kw)
            row["ADAPT_isolated"] = policy_outcome("ADAPT", sc, probs, im.seeds, labels, im.classes, margin=m_iso)
            row["DYN_SCORE_nonisolated"] = policy_outcome("DYN_SCORE", sc, probs, im.seeds, labels, im.classes, threshold=cal_leak["T"])
            row["has_universe"] = bool(len(im.seeds))
            results["per_image"][im.id] = row
    # final calibration (deployment): all eleven lineages' leave-one-lineage-out records; used for cohort images and cross-platform transfer scores
    final = calibrate_threshold([r for L in GENESIS for r in rec_oof[L]])
    recs_final = []
    for C in GENESIS:
        for im in by_l[C]:
            sc = oof[C][im.id]
            recs_final.append((v3.residual_frf([float(x) for x in sc], [int(x) for x in im.labels], im.seeds), sum(1 for x in sc if sigmoid(float(x)) >= 0.5) / len(sc)))
    results["final"] = {"adapt_margin_unchanged_protocol": v3.fit_adaptive_margin(recs_final), "T": final["T"], "T0": final["T0"], "shortfall": final["shortfall"], "minima": {k: round(v, 4) for k, v in final["minima"].items()}}
    pathlib.Path(a.out).write_text(json.dumps(results, indent=1, sort_keys=True))
    print(json.dumps({"final": results["final"]}))
    return 0


# ----------------------------------------------------------------------------------------------------------------------------- freeze
BOUND = ("tools/segarecomp_seg085_dyn.py", "tools/segarecomp_seg085_eval.py", "tools/segarecomp_seg085_cohort.py", "tools/segarecomp_seg085_diag.py", "tools/segarecomp_ml_v3.py",
         "tools/segarecomp_xplat_models.py", "tools/segarecomp_oracle_bench.py", "tests/segarecomp_seg085_test.py")


def sha_file(p: pathlib.Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def plan_body(nested: dict) -> dict:
    root = HERE.parent
    return {"schema": SCHEMA + ".plan", "protocol": "seg085.protocol.v1", "adr": "0190/0193",
            "policies": {"B35": "ml_v3 fixed budget 35 % over model-A ranking + seeds", "B50": "ml_v3 fixed budget 50 %", "ADAPT": "unchanged SEG-072 rule; margin fitted on the eleven leave-one-lineage-out records",
                         "DYN_SCORE": "score >= T cells union seeds; abstain above CAP; T = T0 - shortfall (module docstring)"},
            "cap": CAP, "cell_bytes": v3.CELL_BYTES,
            "calibration": {"fitting_lineages": list(GENESIS), "isolation": "inner refits (one fit per ordered lineage pair) for nested folds; leave-one-lineage-out records for the final calibration",
                            "T_final": nested["final"]["T"], "T0_final": nested["final"]["T0"], "shortfall_final": nested["final"]["shortfall"],
                            "adapt_margin_final": nested["final"]["adapt_margin_unchanged_protocol"]},
            "nested_results_sha256": hashlib.sha256(json.dumps(nested, sort_keys=True).encode()).hexdigest(),
            "bound_files": {f: sha_file(root / f) for f in BOUND if (root / f).exists()}}


def cmd_freeze(a) -> int:
    nested = json.loads(pathlib.Path(a.nested).read_text())
    body = plan_body(nested)
    PLAN.write_text(json.dumps(body, indent=1, sort_keys=True) + "\n")
    PLAN_DIGEST.write_text(sha_file(PLAN) + "\n")
    print(PLAN_DIGEST.read_text().strip())
    return 0


def verify_plan() -> str:
    """The frozen plan digest and the digests of the files it binds (except this module's own later edits are refused too)."""
    if not PLAN.exists() or sha_file(PLAN) != PLAN_DIGEST.read_text().strip():
        raise SystemExit("frozen plan missing or modified")
    body = json.loads(PLAN.read_text())
    root = HERE.parent
    for f, h in body["bound_files"].items():
        if f in ("tools/segarecomp_seg085_eval.py", "tests/segarecomp_seg085_test.py"):
            continue                                            # the evaluation driver and tests may be completed after the freeze; policy + calibration code may not
        if sha_file(root / f) != h:
            raise SystemExit(f"{f} changed after the freeze")
    return PLAN_DIGEST.read_text().strip()


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    i = sub.add_parser("inner")
    i.add_argument("--out", default=str(CACHE / "inner"))
    i.add_argument("--workers", type=int, default=8)
    i.set_defaults(func=cmd_inner)
    n = sub.add_parser("nested")
    n.add_argument("--inner", default=str(CACHE / "inner"))
    n.add_argument("--out", default=str(CACHE / "nested.json"))
    n.set_defaults(func=cmd_nested)
    fz = sub.add_parser("freeze")
    fz.add_argument("--nested", default=str(CACHE / "nested.json"))
    fz.set_defaults(func=cmd_freeze)
    a = ap.parse_args(argv)
    return a.func(a)


if __name__ == "__main__":
    raise SystemExit(main())
