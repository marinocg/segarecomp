#!/usr/bin/env python3
"""SEG-077-T009 step 5: does partial, evidence-tiered labelling carry training signal? (OFFLINE research code; model A only, CPU)

Simulation on source-backed SEG-072 Genesis cartridge images (complete truth): partial labels are produced by the static-first labeller
(segarecomp_genesis_static_v2, tiers V,J,PT,FILL) WITHOUT reading truth, and model A (segarecomp_xplat_models.fit_a, unchanged) is trained
with and without them; every evaluation uses the held-out lineage's complete truth.

Folds: leave-one-lineage-out over the Genesis cartridge lineages (SCD modules excluded). Conditions per held-out lineage F:
  TRUTH_ALL           every other lineage with complete truth
  PARTIAL_ALL         every other lineage with partial labels only
  MINUS_X             lineage X omitted (X in the commercial lineages D01-D04, X != F)
  MINUS_X+PARTIAL_X   lineage X present with partial labels only (the realistic case: adding a partially labelled new game)
Metric: held-out lineage ER (SEG-075 definition: lineage max image ER), from segarecomp_xplat_transfer.image_metrics.

  plan    freeze tools/segarecomp_seg077_partial.plan.json (+ .digest) before any fit
  run     run the frozen matrix (write-once results under the private research root)
  table   aggregate-only results document with the frozen rule's class
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
OUT = pathlib.Path("/root/dev/segarecomp-harness/.cache/m68k-corpus/t009/partial")
PLAN = HERE / "segarecomp_seg077_partial.plan.json"
PLAN_DIGEST = HERE / "segarecomp_seg077_partial.plan.digest"
RESULTS = HERE / "segarecomp_seg077_partial.results.json"
PARTIAL_TIERS = ("V", "J", "PT", "FILL")
COMMERCIAL = ("D01", "D02", "D03", "D04")
BOUND = ("segarecomp_seg077_partial.py", "segarecomp_genesis_static_v2.py", "segarecomp_genesis_evidence.py", "segarecomp_xplat_models.py",
         "segarecomp_xplat_transfer.py", "segarecomp_ml_v3.py", "segarecomp_ml_v3_train.py", "segarecomp_xplat_stability.py",
         "segarecomp_xplat_t005.py", "segarecomp_ml_v3_data.py", "segarecomp_xplat_design.py", "segarecomp_xplat_decide.py",
         "segarecomp_xplat_explore.py", "segarecomp_xplat_explore_eval.py", "segarecomp_ml_region_gen2.py", "segarecomp_ml_region_gen2_train.py",
         "segarecomp_xplat_design.plan.json", "segarecomp_xplat_design.plan.digest")
RULE = {"question": "does adding a lineage with partial static-first labels (U masked) improve held-out ER over omitting it?",
        "statistic": "d(F, X) = ER(MINUS_X+PARTIAL_X) - ER(MINUS_X); cluster by held-out fold: D(F) = mean over X of d(F, X) (11 values); "
                     "exact two-sided sign-flip test over the 11 D(F); d is also reported per X",
        "useful": "mean D <= -0.002 and sign-flip p <= 0.10", "harmful": "mean D >= +0.002 and sign-flip p <= 0.10",
        "otherwise": "NO_MEASURABLE_EFFECT",
        "descriptive": "signal retention = (ER(PARTIAL_ALL) vs ER(TRUTH_ALL)) per fold; partial-label coverage and wrong-label rate vs truth",
        "caveat": "the six commercial images influenced the v1 rule selection (exposed); flicky (lineage D02) is in the development set the "
                  "labeller rules were tuned on (its partial labels are optimistic); PARTIAL_TIERS were chosen after the v2 audit; JT data "
                  "bytes are excluded from the partial labels (JT fails on the exposed set); development use only, not an independent test"}


def sha(p: pathlib.Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def cart_dirs():
    import segarecomp_genesis_static_v2 as v2
    return v2.seg072_images(v2.DEV_SET + v2.HELD_OUT)


def lineage_of(d) -> str:
    return json.loads((d / "meta.json").read_text())["lineage"]


def partial_classes(d: pathlib.Path) -> bytes:
    """Static-first labels for one image (cached; truth is never read here)."""
    import segarecomp_genesis_static_v2 as v2
    m = json.loads((d / "meta.json").read_text())
    cache = OUT / "labels" / hashlib.sha256(PLAN.read_bytes()).hexdigest()[:16] / f"{m['id']}.cls"
    if cache.is_file():
        return cache.read_bytes()
    work = OUT / "work"
    work.mkdir(parents=True, exist_ok=True)
    work.chmod(0o755)
    img_path = pathlib.Path(m["image"])
    res = v2.label_static(img_path.read_bytes(), (d / "tokens.bin").read_bytes(), img_path, work, PARTIAL_TIERS)
    res["data"] = [x for x in res["data"] if x[2] != "JT"]
    cls, _ = v2.classes_of(len(img_path.read_bytes()), res)
    cache.parent.mkdir(parents=True, exist_ok=True)
    cache.write_bytes(bytes(cls))
    return bytes(cls)


def jobs(lineages: list[str]) -> list[dict]:
    out = []
    for F in lineages:
        out.append({"fold": F, "cond": "TRUTH_ALL", "x": None})
        out.append({"fold": F, "cond": "PARTIAL_ALL", "x": None})
        for X in COMMERCIAL:
            if X != F and X in lineages:
                out.append({"fold": F, "cond": "MINUS_X", "x": X})
                out.append({"fold": F, "cond": "MINUS_X+PARTIAL_X", "x": X})
    return out


def cmd_plan(a) -> int:
    lins = sorted({lineage_of(d) for d in cart_dirs()})
    doc = {"schema": "segarecomp.seg077.partial.plan.v1", "task": "SEG-077-T009 step 5", "rule": RULE, "partial_tiers": list(PARTIAL_TIERS),
           "lineages": lins, "images": {json.loads((d / 'meta.json').read_text())["id"]: lineage_of(d) for d in cart_dirs()},
           "jobs": jobs(lins), "bound_code": {f: sha(HERE / f) for f in BOUND}, "model": "A (fit_a, fixed configuration), CPU research venv"}
    data = (json.dumps(doc, indent=1, sort_keys=True) + "\n").encode()
    PLAN.write_bytes(data)
    PLAN_DIGEST.write_text(hashlib.sha256(data).hexdigest() + "\n")
    print(json.dumps({"plan_sha256": hashlib.sha256(data).hexdigest(), "jobs": len(doc["jobs"])}))
    return 0


def verify_plan() -> dict:
    data = PLAN.read_bytes()
    if hashlib.sha256(data).hexdigest() != PLAN_DIGEST.read_text().strip():
        raise SystemExit("partial plan digest mismatch")
    doc = json.loads(data)
    if doc["bound_code"] != {f: sha(HERE / f) for f in BOUND} or doc["rule"] != RULE:
        raise SystemExit("partial plan code or rule changed")
    return doc


def load(d: pathlib.Path, partial: bool):
    import segarecomp_ml_v3 as v3
    import segarecomp_ml_v3_train as t
    import segarecomp_xplat_t005 as x5
    import numpy as np
    im = x5.attach(t.Image(d), d)
    if partial:
        cls = partial_classes(d)
        im.classes = cls
        im.labels = np.array(v3.cell_labels(cls), dtype=np.int8)
        im.wlabels = np.array(v3.word_labels(cls), dtype=np.int8)
        im.starts = v3.starts_of(cls)
    return im


def cmd_run(a) -> int:
    import segarecomp_xplat_models as xm
    import segarecomp_xplat_transfer as xt
    import segarecomp_ml_v3_train as t
    doc = verify_plan()
    dirs = cart_dirs()
    by_lin = {}
    for d in dirs:
        by_lin.setdefault(lineage_of(d), []).append(d)
    res_dir = OUT / "results" / hashlib.sha256(PLAN.read_bytes()).hexdigest()[:16]
    res_dir.mkdir(parents=True, exist_ok=True)
    for j in doc["jobs"]:
        key = f"{j['fold']}.{j['cond']}.{j['x'] or '-'}"
        f = res_dir / f"{key}.json"
        if f.is_file():
            continue
        train = []
        for L, ds in sorted(by_lin.items()):
            if L == j["fold"]:
                continue
            if j["cond"] == "TRUTH_ALL":
                train += [load(d, False) for d in ds]
            elif j["cond"] == "PARTIAL_ALL":
                train += [load(d, True) for d in ds]
            elif j["cond"] == "MINUS_X" and L != j["x"]:
                train += [load(d, False) for d in ds]
            elif j["cond"] == "MINUS_X+PARTIAL_X":
                train += [load(d, L == j["x"]) for d in ds]
        clf = xm.fit_a(train)
        per = {}
        for d in by_lin[j["fold"]]:
            im = load(d, False)
            m = xt.image_metrics((d / "classes.bin").read_bytes(), im.seed_addresses, xm.score_a(clf, im), 128)
            per[im.id] = {"er": m["er"], "frf": m["frf"], "misses_B50": m["misses_B50"]}
        ers = [v["er"] for v in per.values() if v["er"] is not None]
        f.write_text(json.dumps({"job": j, "lineage_max_er": max(ers) if ers else None, "images": per,
                                 "plan_sha256": hashlib.sha256(PLAN.read_bytes()).hexdigest()}))
        print(json.dumps({"job": key, "er": max(ers) if ers else None}), flush=True)
    return 0


def cmd_table(a) -> int:
    import segarecomp_xplat_stability as st
    doc = verify_plan()
    R = {}
    digest = hashlib.sha256(PLAN.read_bytes()).hexdigest()
    for p in (OUT / "results" / digest[:16]).glob("*.json"):
        r = json.loads(p.read_text())
        if r.get("plan_sha256") != digest:
            raise SystemExit(f"{p.name}: result belongs to another plan")
        j = r["job"]
        R[(j["fold"], j["cond"], j["x"])] = r["lineage_max_er"]
    pairs, per_x = {}, {}
    for (F, c, X), er in R.items():
        if c == "MINUS_X+PARTIAL_X" and er is not None and R.get((F, "MINUS_X", X)) is not None:
            v = er - R[(F, "MINUS_X", X)]
            pairs.setdefault(F, []).append(v)
            per_x.setdefault(X, []).append(v)
    if len(R) != len(doc["jobs"]) or len(pairs) != len(doc["lineages"]):
        raise SystemExit(f"incomplete matrix: {len(R)}/{len(doc['jobs'])} jobs, {len(pairs)} fold means (fail closed)")
    d = [sum(v) / len(v) for F, v in sorted(pairs.items())]
    mean = sum(d) / len(d) if d else None
    p = st.sign_flip_p(d) if d else None
    cls = "NO_MEASURABLE_EFFECT"
    if d and mean <= -0.002 and p <= 0.10:
        cls = "USEFUL"
    elif d and mean >= 0.002 and p <= 0.10:
        cls = "HARMFUL"
    retention = {F: {"truth_all": R.get((F, "TRUTH_ALL", None)), "partial_all": R.get((F, "PARTIAL_ALL", None))} for F in doc["lineages"]}
    import segarecomp_ml_v3 as v3
    cov = {}
    for dd in cart_dirs():
        mm = json.loads((dd / "meta.json").read_text())
        pc, tr = partial_classes(dd), (dd / "classes.bin").read_bytes()
        known = [(a, b) for a, b in zip(pc, tr) if a != ord("U") and b != ord("U")]
        wrong = sum(1 for a, b in known if (chr(a), chr(b)) in {("I", "c"), ("I", "D"), ("c", "I"), ("c", "D"), ("D", "I"), ("D", "c")})
        lp, lt = v3.cell_labels(pc), v3.cell_labels(tr)
        cov[mm["id"]] = {"labelled_fraction": round(sum(1 for a in pc if a != ord("U")) / len(pc), 4), "wrong_rate_known": round(wrong / max(1, len(known)), 6),
                         "positive_cells_recalled": round(sum(1 for a, b in zip(lp, lt) if a == b == 1) / max(1, lt.count(1)), 4),
                         "negative_cells_recalled": round(sum(1 for a, b in zip(lp, lt) if a == b == 0) / max(1, lt.count(0)), 4)}
    out = {"schema": "segarecomp.seg077.partial.results.v1", "plan_sha256": hashlib.sha256(PLAN.read_bytes()).hexdigest(), "rule": RULE,
           "per_x_mean_d": {X: round(sum(v) / len(v), 6) for X, v in sorted(per_x.items())}, "fold_means": [round(x, 6) for x in d],
           "partial_label_coverage": cov,
           "folds": len(d), "mean_d": round(mean, 6) if mean is not None else None, "sign_flip_p": round(p, 6) if p is not None else None,
           "class": cls, "retention": retention, "jobs_done": len(R), "jobs_planned": len(doc["jobs"])}
    RESULTS.write_text(json.dumps(out, indent=1, sort_keys=True) + "\n")
    print(json.dumps({k: out[k] for k in ("folds", "mean_d", "sign_flip_p", "class", "jobs_done", "jobs_planned")}))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("plan").set_defaults(fn=cmd_plan)
    sub.add_parser("run").set_defaults(fn=cmd_run)
    sub.add_parser("table").set_defaults(fn=cmd_table)
    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
