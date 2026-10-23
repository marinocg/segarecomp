#!/usr/bin/env python3
"""SEG-080-T003: versioned training-label confidence policy for evidence-tiered Genesis labels + a minimal model-A diagnostic (OFFLINE).

Input: label directories already produced by `segarecomp_genesis_bizhawk(.full).py label` (classes.bin, tokens.bin, spans.json, meta.json) for the
private Sonic 2 TAS and control traces. Nothing is re-traced. Source truth is opened ONLY by `measure` and `diagnose` (evaluators); `apply` never
sees it. Authoritative generated-native code admission is not touched: this layer only decides how research training labels are formed.

Policy `seg080.label_policy.v1` (per-byte tiers from the label directory):
  E   executed instruction (I/c)         strong code
  R   direct static reach from E (I/c)   weak code
  H   hardware header (D)                strong data   (independently supported)
  M   DMA-source extent (D)              strong data   (independently supported: the hardware read these bytes as a DMA source)
  W   ordinary observed data read (D)    weak data, NONEXCLUSIVE: it never vetoes a code label and never removes a byte from a code cell
  U   unknown/masked                     permitted; never a negative
Variants: `strong` = E + H + M; `weak` = strong + R + W (code always wins a byte). Cell labels use the unchanged SEG-072 cell rule. Cell sample
weights: 1.0 when the label rests only on strong tiers (a positive cell holding an E start; a negative cell of only H/M bytes), else WEAK_WEIGHT.

  apply     label dir -> two training-label sample dirs (classes.bin, tokens.bin, seeds.json, meta.json, weights.json) under a private root
  measure   evaluator: coverage, per-tier errors, recovered cells, mixed cells, hard negatives (TAS vs control), per policy variant
  diagnose  model A (SEG-072 M1, HistGradientBoosting) leave-one-lineage-out diagnostic over non-Sonic lineages (needs numpy/scikit-learn)
  freeze / verify-plan   the pre-registration (policy + design + bound code digests)
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_genesis_evidence as ge  # noqa: E402

POLICY_ID = "seg080.label_policy.v1"
WEAK_WEIGHT = 0.25
CELL = ge.CELL_BYTES
PLAN = HERE / "segarecomp_genesis_label_policy.t003.plan.json"
PLAN_DIGEST = HERE / "segarecomp_genesis_label_policy.t003.plan.digest"
BOUND = ["tools/segarecomp_genesis_label_policy.py", "tools/segarecomp_genesis_evidence.py", "tools/segarecomp_genesis_bizhawk.py",
         "tools/segarecomp_ml_v3_train.py"]

PROTOCOL = {
    "schema": "segarecomp.genesis_label_policy.plan",
    "task": "SEG-080-T003",
    "policy": POLICY_ID,
    "preserves": "ADR 0181 (REPLAY_INVALID) and ADR 0182 (LABELS_UNSAFE) are unchanged; this is a training-label layer, not a revised Gate B; "
                 "generated-native code admission is untouched and fail-closed",
    "tiers": {"E": "executed I/c: strong code", "R": "direct static reach I/c: weak code", "H": "hardware header: strong data",
              "M": "DMA-source extent: strong data (independently supported)", "W": "ordinary observed data read: weak, nonexclusive data",
              "U": "unknown / masked: permitted"},
    "variants": {"strong": "E + H + M", "weak": "strong + R + W; code always wins a byte; W never vetoes a code cell"},
    "cell_rule": "unchanged SEG-072: positive when any I, negative when every byte is D, else masked",
    "cell_weights": {"strong": 1.0, "weak_only_label": WEAK_WEIGHT,
                     "definition": "weight 1.0 iff the cell label rests on strong tiers only (positive with an E start; negative of only H/M bytes)"},
    "truth_use": "evaluator only (`measure`, `diagnose` scoring); never used to form, repair or filter training labels",
    "inputs": "existing private Sonic 2 label directories from ADR 0182's runs (TAS, seed-1 control, seed-2 control); no retracing",
    "measure": ["coverage lost when weak tiers are excluded (bytes and cells)", "per-tier errors split into code-as-data / data-as-code / wrong I-c",
                "weak-negative cells containing truth code (training-label noise)", "extra correct code-positive, mixed and hard-negative cells "
                "(TAS minus control, cells with a wrong label excluded), per variant", "code and data errors are always reported separately"],
    "diagnostic": {
        "model": "SEG-072 model A = M1 HistGradientBoostingClassifier over the multiscale aggregates (frozen HGB_CONFIG); masked cells excluded",
        "folds": "leave-one-lineage-out over the qualified non-Sonic lineages (every qualified lineage except D01); train = the other non-Sonic "
                 "qualified lineages (truth labels) + the arm's Sonic 2 labels; the Sonic lineage (s1, sk, s2 truth) never enters the base pool",
        "arms": {"B0": "base only", "S_tas": "base + TAS strong labels", "W_tas": "base + TAS strong+weak labels (weighted)",
                 "S_ctl": "base + seed-1 control strong labels", "W_ctl": "base + seed-1 control strong+weak labels (weighted)",
                 "T": "base + SEG-072 complete Sonic 2 truth (reference only)"},
        "weights": "sample_weight = cell weight; B0 and T use 1.0",
        "metrics": "per held-out lineage and pooled over held-out cells: average precision and ROC AUC of the cell score against truth cell labels",
        "in_image_note": "an additional non-blind check scores the Sonic 2 cells that the arm left unlabelled against truth; not generalisation evidence",
        "not_claimed": "no blind generalisation (Sonic 2 is already in SEG-072); no A/B/C retraining; one game; one seed set"},
    "recommendation_rule": {
        "A_clean_strong_tier": "strong tiers E/H/M have zero code-as-data and data-as-code bytes and the E I/c wrong rate <= 0.001",
        "B_coverage": "the strong variant keeps >= 50% of the weak variant's TAS-over-control extra correct code-positive cells",
        "C_no_harm": "pooled AP of S_tas >= pooled AP of B0 - 0.005 and >= pooled AP of S_ctl - 0.005",
        "verdict": "single different-engine TAS pilot recommended iff A and B and C; otherwise not recommended until the failing part is fixed"},
    "limits": {"cpu_hours": 4, "tracing": "none", "retraining": "none beyond the minimal model-A arms"},
}
PLAN_EXTRA = ("bound_code",)


# ---------------------------------------------------------------------------------------------------------------------------- tiers
U, E, R, H, M, W = 0, 1, 2, 3, 4, 5


def tiers_of(label_dir: pathlib.Path) -> tuple[bytearray, bytes]:
    """Per-byte tier map of a label directory (never touches source truth). Returns (tiers, classes)."""
    cls = (label_dir / "classes.bin").read_bytes()
    spans = json.loads((label_dir / "spans.json").read_text())
    estart = set(spans["tiers"]["E"])
    rstart = set(spans["tiers"]["R"])
    t = bytearray(len(cls))
    cur = U
    for i, c in enumerate(cls):
        if c == ord("I"):
            cur = E if i in estart else R if i in rstart else None
            if cur is None:
                raise ge.EvidenceError("accepted I label with no code tier")
            t[i] = cur
        elif c == ord("c"):
            t[i] = cur
        else:
            cur = U
    for e in spans["data_extents"]:
        if e["masked"]:
            continue
        code = {"HW_HEADER": H, "DMA": M, "DR": W}[e["kind"]]
        for k in range(e["start"], min(e["end"], len(cls))):
            if cls[k] == ord("D") and t[k] == U:
                t[k] = code
    if any(c == ord("D") and not t[i] for i, c in enumerate(cls)):
        raise ge.EvidenceError("a data label without a tier")
    return t, cls


def variant_classes(tiers: bytes, which: str) -> bytes:
    allowed_code = {E} if which == "strong" else {E, R}
    allowed_data = {H, M} if which == "strong" else {H, M, W}
    out = bytearray(b"U" * len(tiers))
    # code first (code always wins a byte); `c` bytes follow their start's tier, so keep whole instructions or nothing
    return out, allowed_code, allowed_data


def build_variant(tiers: bytes, cls: bytes, which: str) -> bytes:
    out, allowed_code, allowed_data = variant_classes(tiers, which)
    for i, c in enumerate(cls):
        if c in (ord("I"), ord("c")) and tiers[i] in allowed_code:
            out[i] = c
        elif c == ord("D") and tiers[i] in allowed_data:
            out[i] = c
    return bytes(out)


def cell_weights(tiers: bytes, vcls: bytes, labels: list) -> list:
    """Sample weight per cell (0.0 for masked)."""
    w = []
    for c, y in enumerate(labels):
        s, e = c * CELL, min(len(vcls), (c + 1) * CELL)
        if y == 1:
            strong = any(vcls[k] == ord("I") and tiers[k] == E for k in range(s, e))
        elif y == 0:
            strong = all(tiers[k] in (H, M) for k in range(s, e))
        else:
            w.append(0.0)
            continue
        w.append(1.0 if strong else WEAK_WEIGHT)
    return w


def cell_labels(classes: bytes) -> list:
    sys.path.insert(0, str(HERE))
    import segarecomp_ml_v3 as v3
    return v3.cell_labels(classes)


def cmd_apply(a) -> int:
    verify_plan()
    src, out = pathlib.Path(a.label_dir), pathlib.Path(a.out)
    ge.require_private(out.parent if not out.exists() else out)
    tiers, cls = tiers_of(src)
    meta = json.loads((src / "meta.json").read_text())
    summary = {}
    for which in ("strong", "weak"):
        d = out / which
        d.mkdir(parents=True, exist_ok=False)
        v = build_variant(tiers, cls, which)
        labels = cell_labels(v)
        (d / "classes.bin").write_bytes(v)
        (d / "tokens.bin").write_bytes((src / "tokens.bin").read_bytes())
        (d / "seeds.json").write_text((src / "seeds.json").read_text())
        (d / "weights.json").write_text(json.dumps(cell_weights(tiers, v, labels)))
        m = dict(meta, id=f"{a.name}-{which}", lineage=a.lineage, policy=POLICY_ID, variant=which)
        m["bytes"] = {c: v.count(c.encode()) for c in "IcDU"}
        (d / "meta.json").write_text(json.dumps(m, indent=1, sort_keys=True))
        summary[which] = {"cells_pos": labels.count(1), "cells_neg": labels.count(0), "cells_masked": labels.count(-1), "bytes": m["bytes"]}
    print(json.dumps(summary, sort_keys=True))
    return 0


# ------------------------------------------------------------------------------------------------------------------------- measure
def tier_errors(tiers: bytes, cls: bytes, truth: bytes) -> dict:
    names = {E: "E", R: "R", H: "H", M: "M", W: "W"}
    out = {}
    for code, name in names.items():
        n = wrong = cad = dac = 0
        wi = wc = 0
        for i, c in enumerate(cls):
            if tiers[i] != code:
                continue
            n += 1
            t = truth[i]
            if c == ord("D"):
                if t != ord("D"):
                    wrong += 1
                    cad += 1
            else:
                if t != c:
                    wrong += 1
                    if t == ord("D"):
                        dac += 1
                    elif c == ord("I"):
                        wi += 1
                    else:
                        wc += 1
        out[name] = {"labelled_bytes": n, "wrong": wrong, "code_as_data": cad, "data_as_code": dac, "wrong_I": wi, "wrong_c": wc}
    return out


def noisy_negative_cells(vcls: bytes, labels: list, truth: bytes, weights: list) -> dict:
    n = {"strong": 0, "weak": 0}
    bad = {"strong": 0, "weak": 0}
    for c, y in enumerate(labels):
        if y != 0:
            continue
        k = "strong" if weights[c] == 1.0 else "weak"
        n[k] += 1
        s, e = c * CELL, min(len(vcls), (c + 1) * CELL)
        if any(truth[i] in (ord("I"), ord("c")) for i in range(s, e)):
            bad[k] += 1
    return {"negative_cells": n, "negative_cells_containing_truth_code": bad}


def cmd_measure(a) -> int:
    verify_plan()
    import segarecomp_genesis_bizhawk as bz
    img = pathlib.Path(a.rom).read_bytes()
    truth = pathlib.Path(a.truth).read_bytes()               # evaluator only
    if ge.sha256(truth) != json.loads(pathlib.Path(a.bizhawk_plan).read_text())["truth_sha256"]:
        raise ge.EvidenceError("truth differs from the experiment's frozen plan")
    root = pathlib.Path(a.private)
    res = {"schema": "segarecomp.genesis_label_policy.results", "policy": POLICY_ID, "plan_sha256": ge.sha256(PLAN.read_bytes())}
    data = {}
    for name in ("tas", "ctl", "ctl2"):
        tiers, cls = tiers_of(root / "labels" / name)
        data[name] = {"tiers": tiers, "cls": cls}
        for which in ("strong", "weak"):
            v = pathlib.Path(a.policy_root) / name / which
            data[name][which] = (v / "classes.bin").read_bytes()
            lab = cell_labels(data[name][which])
            wts = json.loads((v / "weights.json").read_text())
            data[name][which + "_cells"] = bz.cell_sets(data[name][which], truth, img)
            data[name][which + "_tokens"] = (v / "tokens.bin").read_bytes()
            data[name][which + "_labels"] = lab
            data[name][which + "_weights"] = wts
    res["per_tier_errors"] = {n: tier_errors(data[n]["tiers"], data[n]["cls"], truth) for n in data}
    res["tier_bytes"] = {n: {c: data[n]["tiers"].count(code) for c, code in (("E", E), ("R", R), ("H", H), ("M", M), ("W", W))} for n in data}
    cov = {}
    for n in data:
        cov[n] = {}
        for which in ("strong", "weak"):
            lab = data[n][which + "_labels"]
            v = data[n][which]
            cov[n][which] = {"labelled_bytes": sum(1 for c in v if c != ord("U")), "I": v.count(b"I"), "c": v.count(b"c"), "D": v.count(b"D"),
                             "cells_pos": lab.count(1), "cells_neg": lab.count(0), "cells_masked": lab.count(-1),
                             **noisy_negative_cells(v, lab, truth, data[n][which + "_weights"])}
        s, w = cov[n]["strong"], cov[n]["weak"]
        cov[n]["loss_when_weak_excluded"] = {"labelled_bytes": round(1 - s["labelled_bytes"] / w["labelled_bytes"], 4),
                                             "positive_cells": round(1 - s["cells_pos"] / w["cells_pos"], 4),
                                             "negative_cells": round(1 - s["cells_neg"] / w["cells_neg"], 4)}
    res["coverage"] = cov
    rec = {}
    for which in ("strong", "weak"):
        ts, cs, c2 = (data[n][which + "_cells"] for n in ("tas", "ctl", "ctl2"))
        mg, m2 = bz.marginal(ts, cs), bz.marginal(ts, c2)
        th = bz.hard_cells(data["tas"][which], img, data["tas"][which + "_tokens"], ts["neg"])
        ch = bz.hard_cells(data["ctl"][which], img, data["ctl"][which + "_tokens"], cs["neg"])
        rec[which] = {"extra_correct_pos_cells": len(mg["pos"]), "extra_correct_mixed_cells": len(mg["mixed"]), "extra_correct_neg_cells": len(mg["neg"]),
                      "extra_hard_negative_cells": len(th - ch), "extra_mixed_that_are_also_extra_pos": len(set(mg["pos"]) & set(mg["mixed"])),
                      "vs_seed2_control": {"pos": len(m2["pos"]), "mixed": len(m2["mixed"]), "neg": len(m2["neg"])},
                      "cells_with_wrong_label": {"tas": len(ts["bad"]), "control": len(cs["bad"])}}
    res["recovered"] = rec
    A = all(res["per_tier_errors"]["tas"][t]["code_as_data"] == 0 and res["per_tier_errors"]["tas"][t]["data_as_code"] == 0 for t in "EHM")
    ew = res["per_tier_errors"]["tas"]["E"]
    A = A and (ew["wrong"] / max(1, ew["labelled_bytes"])) <= 0.001
    B = rec["strong"]["extra_correct_pos_cells"] >= 0.5 * rec["weak"]["extra_correct_pos_cells"]
    res["recommendation_inputs"] = {"A_clean_strong_tier": bool(A), "B_coverage": bool(B)}
    pathlib.Path(a.out).write_text(json.dumps(res, indent=1, sort_keys=True))
    print(json.dumps({"A": bool(A), "B": bool(B), "recovered": rec, "loss": {n: cov[n]["loss_when_weak_excluded"] for n in cov}}, sort_keys=True))
    return 0


# ----------------------------------------------------------------------------------------------------------------------- diagnostic
def cmd_diagnose(a) -> int:
    verify_plan()
    import numpy as np
    from sklearn.ensemble import HistGradientBoostingClassifier
    from sklearn.metrics import average_precision_score, roc_auc_score
    import segarecomp_ml_v3_train as tr
    import segarecomp_ml_region_gen2_train as g2
    import segarecomp_ml_v3 as v3
    data = pathlib.Path(a.registry)
    tr.DATA = data
    truth_dir = data / "s2"
    pol = pathlib.Path(a.policy_root)
    qualified = [l for l in tr.qualified_lineages() if l != "D01"]
    dirs = {m["lineage"]: [] for m in map(lambda p: json.loads((p / "meta.json").read_text()), tr.registry(set(qualified)))}
    for d in tr.registry(set(qualified)):
        dirs[json.loads((d / "meta.json").read_text())["lineage"]].append(d)
    feat_cache = {}

    def feats(im):
        if im.id not in feat_cache:
            feat_cache[im.id] = g2.m1_features(im)
        return feat_cache[im.id]

    sonic = {"truth": tr.Image(truth_dir)}
    for n, k in (("tas", "tas"), ("ctl", "ctl")):
        for which in ("strong", "weak"):
            im = tr.Image(pol / n / which)
            sonic[f"{n}_{which}"] = im
    full = {k: json.loads((pol / k.split("_")[0] / k.split("_")[1] / "weights.json").read_text()) for k in sonic if k != "truth"}
    sonic_feat = feats(sonic["truth"])
    arms = {"B0": None, "S_tas": "tas_strong", "W_tas": "tas_weak", "S_ctl": "ctl_strong", "W_ctl": "ctl_weak", "T": "truth"}
    results = {"folds": {}, "pooled": {}, "in_image": {}}
    pooled = {arm: ([], []) for arm in arms}
    for held in sorted(dirs):
        train = [im for l in sorted(dirs) if l != held for im in (tr.Image(d) for d in dirs[l])]
        test = [tr.Image(d) for d in dirs[held]]
        xs = [feats(im)[im.labels >= 0] for im in train]
        ys = [(im.labels[im.labels >= 0] == 1).astype(np.int64) for im in train]
        ws = [np.ones(len(y)) for y in ys]
        res = {}
        for arm, key in arms.items():
            X, Y, Wt = list(xs), list(ys), list(ws)
            if key is not None:
                im = sonic[key]
                keep = im.labels >= 0
                X.append(sonic_feat[keep])
                Y.append((im.labels[keep] == 1).astype(np.int64))
                Wt.append(np.array(full[key])[keep] if key != "truth" else np.ones(int(keep.sum())))
            clf = HistGradientBoostingClassifier(**g2.HGB_CONFIG).fit(np.concatenate(X), np.concatenate(Y), sample_weight=np.concatenate(Wt))
            sc, yy = [], []
            for t in test:
                p = clf.predict_proba(feats(t))[:, 1]
                keep = t.labels >= 0
                sc.append(p[keep])
                yy.append(t.labels[keep])
            sc, yy = np.concatenate(sc), np.concatenate(yy)
            res[arm] = {"ap": round(float(average_precision_score(yy, sc)), 5), "auc": round(float(roc_auc_score(yy, sc)), 5), "cells": int(len(yy))}
            pooled[arm][0].append(sc)
            pooled[arm][1].append(yy)
            if held == sorted(dirs)[0] and key is not None and key != "truth":
                im = sonic[key]
                un = im.labels < 0
                tt = sonic["truth"].labels
                p = clf.predict_proba(sonic_feat)[:, 1]
                results["in_image"][arm] = {"unlabelled_cells": int(un.sum()), "ap_on_unlabelled": round(float(average_precision_score(tt[un], p[un])), 5)
                                            if len(set(tt[un])) > 1 else None, "note": "non-blind: same image as the arm's labels"}
        results["folds"][held] = res
    for arm, (s, y) in pooled.items():
        s, y = np.concatenate(s), np.concatenate(y)
        results["pooled"][arm] = {"ap": round(float(average_precision_score(y, s)), 5), "auc": round(float(roc_auc_score(y, s)), 5)}
    P = results["pooled"]
    C = P["S_tas"]["ap"] >= P["B0"]["ap"] - 0.005 and P["S_tas"]["ap"] >= P["S_ctl"]["ap"] - 0.005
    results["recommendation_inputs"] = {"C_no_harm": bool(C), "deltas_ap": {arm: round(P[arm]["ap"] - P["B0"]["ap"], 5) for arm in P}}
    pathlib.Path(a.out).write_text(json.dumps(results, indent=1, sort_keys=True))
    print(json.dumps({"pooled": P, "C": bool(C)}, sort_keys=True))
    return 0


# ----------------------------------------------------------------------------------------------------------------------------- plan
def bound_digests() -> dict:
    root = HERE.parent
    return {f: ge.sha256((root / f).read_bytes()) for f in BOUND}


def cmd_freeze(a) -> int:
    if PLAN.exists():
        raise ge.EvidenceError("plan already frozen")
    doc = dict(PROTOCOL)
    doc["bound_code"] = bound_digests()
    data = ge.canonical(doc)
    PLAN.write_bytes(data)
    PLAN_DIGEST.write_text(ge.sha256(data) + "\n")
    print(json.dumps({"plan_sha256": ge.sha256(data)}))
    return 0


def verify_plan() -> dict:
    data = PLAN.read_bytes()
    if ge.sha256(data) != PLAN_DIGEST.read_text().strip():
        raise ge.EvidenceError("plan digest mismatch")
    doc = json.loads(data)
    if {k: v for k, v in doc.items() if k not in PLAN_EXTRA} != json.loads(json.dumps(PROTOCOL)):
        raise ge.EvidenceError("committed plan differs from the protocol in code")
    if doc["bound_code"] != bound_digests():
        raise ge.EvidenceError("bound code changed after the freeze")
    return doc


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("apply")
    p.add_argument("--label-dir", required=True), p.add_argument("--out", required=True), p.add_argument("--name", required=True)
    p.add_argument("--lineage", default="SONIC2_TAS")
    p.set_defaults(fn=cmd_apply)
    p = sub.add_parser("measure")
    p.add_argument("--private", required=True), p.add_argument("--policy-root", required=True), p.add_argument("--rom", required=True)
    p.add_argument("--truth", required=True), p.add_argument("--bizhawk-plan", required=True), p.add_argument("--out", required=True)
    p.set_defaults(fn=cmd_measure)
    p = sub.add_parser("diagnose")
    p.add_argument("--registry", required=True), p.add_argument("--policy-root", required=True), p.add_argument("--out", required=True)
    p.set_defaults(fn=cmd_diagnose)
    p = sub.add_parser("freeze")
    p.set_defaults(fn=cmd_freeze)
    p = sub.add_parser("verify-plan")
    p.set_defaults(fn=lambda a: print(json.dumps({"plan_sha256": ge.sha256(PLAN.read_bytes()), "ok": bool(verify_plan())})) or 0)
    a = ap.parse_args(argv)
    try:
        return a.fn(a)
    except ge.EvidenceError as e:
        print(json.dumps({"error": str(e)}), file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
