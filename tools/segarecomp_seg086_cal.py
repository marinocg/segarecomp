#!/usr/bin/env python3
"""SEG-086-T002/T003 (ADR 0195/0196): normaliser selection and leakage-safe threshold calibration for the normalised dynamic score (OFFLINE research code).

Pure record-level functions first (tested on synthetic records), then commands that read the private registry and the stored SEG-076/084/085 scores:
  select   one-time, unit-free, development-only choice between the predeclared normalisers (ADR 0195 selection rule)
  nested   (T003) lineage-separated threshold calibration with a conservative low-tail margin, frozen before the final evaluation
A *record* is a dict: {id, lineage, scores, labels, seeds, density}. No H01 data is read by this module; evaluation-only samples are refused by `segarecomp_sample_roles`.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import pathlib
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_ml_v3 as v3  # noqa: E402
import segarecomp_seg085_dyn as dyn  # noqa: E402
import segarecomp_seg086_norm as nm  # noqa: E402

SCHEMA = "segarecomp.seg086.cal.v1"
HARNESS = pathlib.Path(os.environ.get("SEGARECOMP_HARNESS_ROOT", "/root/dev/segarecomp-harness"))
CACHE = pathlib.Path(os.environ.get("SEG086_CACHE", str(HARNESS / ".cache" / "seg086")))
CAP = nm.CAP
GENESIS = dyn.GENESIS
VARIANTS = ("RAW", "N1", "N2")                    # RAW = unnormalised control (the SEG-085 score space)
SELECTION = HERE / "segarecomp_seg086_cal.selection.json"
PLAN = HERE / "segarecomp_seg086_cal.plan.json"
PLAN_DIGEST = HERE / "segarecomp_seg086_cal.plan.digest"
RESULTS = HERE / "segarecomp_seg086_cal.results.json"
H01_RESULTS = HERE / "segarecomp_seg086_cal.h01.json"


# ------------------------------------------------------------------------------------------------------------------------------- pure
def space(scores, variant: str):
    """Scores in the variant's space, or (None, reason) when the normalisation fails closed. RAW is the identity."""
    s = dyn.finite_scores(scores)
    if variant == "RAW":
        return s, None
    r = nm.normalise(s, variant)
    return (r["z"], None) if r["status"] == "ok" else (None, r["reason"])


def select_in_space(scores, seeds, tau: float, variant: str, have_universe: bool = True) -> dict:
    """The selector of one variant (RAW uses the SEG-085 `dyn_select` unchanged)."""
    if variant == "RAW":
        return dyn.dyn_select(scores, seeds, tau, have_universe=have_universe)
    return nm.norm_select(scores, seeds, tau, variant, have_universe=have_universe)


def is_all_code(rec) -> bool:
    return positive_density(rec) > CAP + 1e-12


def positive_density(rec) -> float:
    n = len(rec["labels"])
    return len({c for c, y in enumerate(rec["labels"]) if y == 1} | {c for c in rec["seeds"] if 0 <= c < n}) / n


def image_weakest(rec, variant: str):
    """(weakest verified non-seed positive in the variant's space or None, failure reason or None)."""
    z, why = space(rec["scores"], variant)
    if z is None:
        return None, why
    return nm.weakest_positive_z(z, rec["labels"], set(rec["seeds"])), None


def lineage_minima(records, variant: str) -> dict:
    """{lineage: lowest weakest-positive over its images}; failed normalisations are skipped here and counted by the caller."""
    out = {}
    for r in records:
        w, _ = image_weakest(r, variant)
        if w is not None:
            out[r["lineage"]] = min(w, out.get(r["lineage"], math.inf))
    return out


def insample_tau(records, variant: str) -> float:
    m = lineage_minima(records, variant)
    if not m:
        raise ValueError("no verified positive in the fitting records")
    return min(m.values())


def evaluate(records, variant: str, tau: float, starts_of=None) -> dict:
    """Per-image outcome rows and the lineage-balanced summary at one threshold. `starts_of(rec)` -> verified instruction start byte offsets (optional)."""
    rows = {}
    for r in records:
        sel = select_in_space(r["scores"], r["seeds"], tau, variant, have_universe=bool(r["seeds"]))
        row = {"lineage": r["lineage"], "status": sel["status"], "retained": sel["retained"], "all_code": is_all_code(r), "cells": len(r["scores"]), "reason": sel.get("reason")}
        if starts_of is not None and sel["status"] in ("selected", "abstain_cap", "normalisation_failed"):
            row["missed_starts"] = v3.misses(starts_of(r), sel["region"])
        rows[r["id"]] = row
    return {"rows": rows, "summary": summarise(rows)}


def summarise(rows: dict) -> dict:
    """Lineage-balanced mean retained fraction over the supported, non-all-code images (cap abstentions and normalisation failures count as retained 1.0; images without a
    seed universe or with invalid scores are not part of the mean and are counted), the cell-weighted retained fraction over the same images, and counts."""
    by_l, counts, cells, kept = {}, {"selected": 0, "abstain_cap": 0, "normalisation_failed": 0, "rejected": 0, "unsupported": 0}, 0, 0.0
    domain = 0
    for k, r in rows.items():
        counts[r["status"]] = counts.get(r["status"], 0) + 1
        if r["status"] == "normalisation_failed" and r.get("reason") == "too_few_cells":
            domain += 1
        if r["all_code"] or r["status"] in ("unsupported", "rejected"):
            continue
        ret = 1.0 if r["retained"] is None else r["retained"]
        by_l.setdefault(r["lineage"], []).append(ret)
        cells += r["cells"]
        kept += ret * r["cells"]
    lin = {L: sum(v) / len(v) for L, v in by_l.items()}
    allv = [x for v in by_l.values() for x in v]
    return {"lineage_balanced_mean_retained": round(sum(lin.values()) / len(lin), 6) if lin else None, "image_mean_retained": round(sum(allv) / len(allv), 6) if allv else None,
            "cell_weighted_retained": round(kept / cells, 6) if cells else None,
            "min_retained": round(min(allv), 6) if allv else None, "max_retained": round(max(allv), 6) if allv else None, "images_evaluated": len(allv),
            "status_counts": counts, "below_minimum_cells": domain, "missed_starts": sum(r.get("missed_starts", 0) for r in rows.values()),
            "lineage_mean_retained": {L: round(v, 6) for L, v in sorted(lin.items())}}


def choose_variant(summaries: dict, failures: dict) -> dict:
    """ADR 0195 selection rule. `summaries[v]`: evaluate(...)['summary']; `failures[v]`: normalisation failures on non-all-code images. RAW is a control, never chosen."""
    ok = [v for v in ("N1", "N2") if failures.get(v, 0) == 0 and summaries[v]["lineage_balanced_mean_retained"] is not None]
    if not ok:
        return {"chosen": None, "reason": "every candidate has a normalisation failure on a non-all-code development image"}
    best = min(ok, key=lambda v: (summaries[v]["lineage_balanced_mean_retained"], v == "N1"))
    if "N2" in ok and best != "N2" and summaries[best]["lineage_balanced_mean_retained"] > summaries["N2"]["lineage_balanced_mean_retained"] - 0.01:
        best = "N2"
    return {"chosen": best, "reason": "smaller lineage-balanced mean retained fraction; a difference below 0.01 prefers N2",
            "disqualified": [v for v in ("N1", "N2") if v not in ok]}


def calibrate_tau(minima: dict) -> dict:
    """tau = T0 - shortfall, exactly the SEG-085 DYN_SCORE rule (`dyn.calibrate_threshold`) applied in the variant's space: T0 is the lowest lineage-minimum weakest-positive
    score and `shortfall` the largest leave-one-lineage-out miss of the plain minimum rule among the calibration lineages. Needs at least two lineages (fail closed)."""
    if len(minima) < 2:
        raise ValueError("calibration needs at least two fitting lineages")
    for v in minima.values():
        if not math.isfinite(v):
            raise ValueError("non-finite lineage minimum")
    t0 = min(minima.values())
    short = 0.0
    for L, m in minima.items():
        short = max(short, min(v for k, v in minima.items() if k != L) - m)
    return {"T0": t0, "shortfall": short, "tau": t0 - short, "lineages": len(minima)}


class LeakageError(Exception):
    pass


def calibrate_fold(held: str, cal_records, variant: str, margin: bool = True) -> dict:
    """tau for one held-out lineage. Leakage guard (ADR 0196): every calibration record must belong to a different lineage than `held` and must have been scored by a model
    trained on NEITHER `held` NOR the record's own lineage; any violation raises. `margin=False` is the diagnostic plain-minimum rule (never the frozen policy)."""
    for r in cal_records:
        ex = r.get("model_excludes", frozenset())
        if r["lineage"] == held:
            raise LeakageError(f"{r['id']}: the held-out lineage {held} is in its own calibration set")
        if held not in ex or r["lineage"] not in ex:
            raise LeakageError(f"{r['id']}: scored by a model that trained on {held if held not in ex else r['lineage']}")
    cal = calibrate_tau(lineage_minima(cal_records, variant))
    if not margin:
        return {**cal, "tau": cal["T0"]}
    return cal


def compatibility(by_l: dict, variants, inner_loader) -> dict:
    """Do the normalisations make scores of DIFFERENTLY trained models comparable? For every image with a verified positive, the weakest-positive score under each inner refit
    (9 lineages) is compared with the same quantity under the out-of-fold model (10 lineages): per lineage and variant, the median and maximum absolute shift. A normalisation
    that corrects model-to-model scale differences shrinks these shifts relative to RAW; one that does not, does not."""
    out = {v: {} for v in variants}
    for C, recs in by_l.items():
        shifts = {v: [] for v in variants}
        for H in by_l:
            if H == C:
                continue
            z = inner_loader(H, C)
            for r in recs:
                for v in variants:
                    w0, _ = image_weakest(r, v)
                    w1, _ = image_weakest({**r, "scores": [float(x) for x in z[r["id"]]]}, v)
                    if w0 is not None and w1 is not None:
                        shifts[v].append(abs(w1 - w0))
        for v in variants:
            xs = sorted(shifts[v])
            out[v][C] = {"n": len(xs), "median_abs_shift": round(xs[len(xs) // 2], 4) if xs else None, "max_abs_shift": round(xs[-1], 4) if xs else None}
    return out


def group_rows(rows: dict, ids) -> dict:
    return {k: v for k, v in rows.items() if k in ids}


# ------------------------------------------------------------------------------------------------------------------------------- decision (frozen)
HISTORICAL = ("D01", "D02", "D03", "D04")           # commercial-era Genesis (primary population); D05-D12 are the modern SGDK robustness population
GATES = {"fallback_image_share_max": 0.25, "fallback_cell_share_max": 0.25, "lineage_balanced_mean_retained_max": 0.40, "cell_weighted_retained_max": 0.40,
         "retained_spread_min": 0.15, "distinct_from_fixed_budgets_share_min": 0.5, "retained_std_min": 0.05, "min_selected_images": 5, "oracle_over_cap_share_max": 0.20, "min_evaluable_lineages": 8,
         "economics_saving_vs_b50_min": 0.10, "economics_regression_vs_incremental_max": 0.05}


def gate_inputs(rows: dict) -> dict:
    """Frozen gate inputs from per-image rows: the DOMAIN is every supported, non-all-code image with at least MIN_CELLS cells; a cap abstention or a normalisation failure there
    is a fallback to broad AOT. Images below the minimum size, without a seed universe or all-code are reported by the summary but are not in the domain."""
    dom = {k: r for k, r in rows.items() if not r["all_code"] and r["status"] not in ("unsupported", "rejected") and r["cells"] >= nm.MIN_CELLS}
    fb = {k: r for k, r in dom.items() if r["status"] in ("abstain_cap", "normalisation_failed")}
    sel = [r["retained"] for r in dom.values() if r["status"] == "selected"]
    cells = sum(r["cells"] for r in dom.values())
    kept = sum((1.0 if r["retained"] is None else r["retained"]) * r["cells"] for r in dom.values())
    by_l = {}
    for r in dom.values():
        by_l.setdefault(r["lineage"], []).append(1.0 if r["retained"] is None else r["retained"])
    lin = {L: sum(v) / len(v) for L, v in by_l.items()}
    mean = sum(sel) / len(sel) if sel else None
    return {"domain_images": len(dom), "fallback_images": len(fb), "fallback_image_share": len(fb) / len(dom) if dom else None,
            "fallback_cell_share": sum(r["cells"] for r in fb.values()) / cells if cells else None,
            "selected_images": len(sel), "missed_starts": sum(r.get("missed_starts", 0) for r in dom.values()),
            "lineage_balanced_mean_retained": sum(lin.values()) / len(lin) if lin else None, "cell_weighted_retained": kept / cells if cells else None,
            "retained_spread": (max(sel) - min(sel)) if sel else None,
            "retained_std": (sum((x - mean) ** 2 for x in sel) / len(sel)) ** 0.5 if sel else None,
            "distinct_from_fixed_budgets_share": (sum(1 for x in sel if abs(x - 0.35) >= 0.05 and abs(x - 0.50) >= 0.05) / len(sel)) if sel else None,
            "evaluable_lineages": len(lin), "lineage_balanced_by_lineage": {L: round(v, 6) for L, v in sorted(lin.items())}}


def decide(inp: dict, oracle_over_cap_share, determinism_ok: bool, isolation_ok: bool) -> dict:
    """The frozen decision ladder (ADR 0196). Economics is a further gate: `GATES_PASSED_ECONOMICS_REQUIRED` means the development gates hold and the compile benchmark must run."""
    g = GATES
    if not (determinism_ok and isolation_ok) or inp["evaluable_lineages"] < g["min_evaluable_lineages"]:
        return {"decision": "EVIDENCE_INSUFFICIENT", "reason": "isolation, determinism or the minimum evaluable lineage count is not established"}
    if oracle_over_cap_share is not None and oracle_over_cap_share > g["oracle_over_cap_share_max"]:
        return {"decision": "RANKING_LIMITATION", "reason": "even the per-image oracle normalised threshold exceeds the cap on too many images"}
    if inp["missed_starts"] > 0:
        return {"decision": "RECALL_FAILED", "reason": f"{inp['missed_starts']} verified instruction starts excluded on non-fallback development images"}
    gates = {
        "A_abstention": inp["fallback_image_share"] is not None and inp["fallback_image_share"] <= g["fallback_image_share_max"] and inp["fallback_cell_share"] <= g["fallback_cell_share_max"],
        "S_selectivity": inp["lineage_balanced_mean_retained"] is not None and inp["lineage_balanced_mean_retained"] <= g["lineage_balanced_mean_retained_max"]
        and inp["cell_weighted_retained"] <= g["cell_weighted_retained_max"],
        "D_dynamics": inp["selected_images"] >= g["min_selected_images"] and inp["retained_spread"] >= g["retained_spread_min"] and inp["retained_std"] >= g["retained_std_min"]
        and inp["distinct_from_fixed_budgets_share"] >= g["distinct_from_fixed_budgets_share_min"]}
    if not all(gates.values()):
        return {"decision": "CALIBRATION_INEFFECTIVE", "reason": "failed gates: " + ", ".join(k for k, v in gates.items() if not v), "gates": gates}
    return {"decision": "GATES_PASSED_ECONOMICS_REQUIRED", "reason": "development recall, abstention, selectivity and dynamics gates hold", "gates": gates}


def decide_with_economics(dev: dict, saving_vs_b50, regression_vs_incremental, runtime_equivalent: bool) -> dict:
    if dev["decision"] != "GATES_PASSED_ECONOMICS_REQUIRED":
        return dev
    if not runtime_equivalent:
        return {"decision": "RECALL_FAILED", "reason": "a relevant admission or runtime-equivalence gate failed in the economics benchmark"}
    if saving_vs_b50 is None or saving_vs_b50 < GATES["economics_saving_vs_b50_min"] or regression_vs_incremental is None or regression_vs_incremental > GATES["economics_regression_vs_incremental_max"]:
        return {"decision": "CALIBRATION_INEFFECTIVE", "reason": "no meaningful measured economic improvement over A+B50 without material regression against the deployed incremental policy"}
    return {"decision": "SCORE_CALIBRATION_FEASIBLE", "reason": "all development gates and the economics gate hold (development evidence only; independent confirmation required)"}


def oracle_over_cap_share(recs, variant: str) -> float | None:
    """Share of domain images whose per-image oracle threshold (the weakest verified positive in the variant's space, seeds united) retains more than the cap."""
    n = bad = 0
    for r in recs:
        if is_all_code(r) or not r["seeds"] or len(r["scores"]) < nm.MIN_CELLS:
            continue
        z, why = space(r["scores"], variant)
        if z is None:
            continue
        w = nm.weakest_positive_z(z, r["labels"], set(r["seeds"]))
        if w is None:
            continue
        n += 1
        bad += len({c for c, x in enumerate(z) if x >= w} | set(r["seeds"])) / len(z) > CAP + 1e-12
    return bad / n if n else None


# ------------------------------------------------------------------------------------------------------------------------------- data
def load_records():
    import numpy as np
    import segarecomp_seg084_a as s84
    import segarecomp_xplat_transfer as xt
    imgs, _ = s84.load_registry_images()
    import segarecomp_sample_roles as sr
    sr.require_trainable_lineages(sorted({im.lineage for im in imgs}), "SEG-086 development registry")
    oof = {}
    for L in GENESIS:
        oof.update(xt.load_npz(s84.score_path(L, "A0")))
    recs = [{"id": im.id, "lineage": im.lineage, "scores": [float(x) for x in oof[im.id]], "labels": [int(x) for x in im.labels], "seeds": set(im.seeds), "classes": im.classes,
             "has_universe": bool(len(im.seeds)), "model_excludes": frozenset({im.lineage})} for im in imgs]
    return imgs, recs


def starts_fn(rec):
    return v3.starts_of(rec["classes"])


def cmd_select(a) -> int:
    t0 = time.process_time()
    imgs, recs = load_records()
    summaries, failures, taus, rows = {}, {}, {}, {}
    for v in VARIANTS:
        taus[v] = insample_tau(recs, v)
        ev = evaluate(recs, v, taus[v], starts_fn)
        summaries[v], rows[v] = ev["summary"], ev["rows"]
        failures[v] = sum(1 for r in ev["rows"].values() if r["status"] == "normalisation_failed" and not r["all_code"] and r["reason"] != "too_few_cells")
    choice = choose_variant(summaries, failures)
    out = {"schema": SCHEMA + ".selection", "rule": "ADR 0195 (amended before computation)", "tau_star": {v: round(t, 6) for v, t in taus.items()}, "summaries": summaries,
           "normalisation_failures_non_all_code": failures, "choice": choice, "lineage_minima": {v: {L: round(m, 4) for L, m in sorted(lineage_minima(recs, v).items())} for v in VARIANTS},
           "cpu_seconds": round(time.process_time() - t0, 1)}
    SELECTION.write_text(json.dumps(out, indent=1, sort_keys=True) + "\n")
    print(json.dumps({"tau": out["tau_star"], "mean_retained": {v: summaries[v]["lineage_balanced_mean_retained"] for v in VARIANTS}, "failures": failures, "choice": choice["chosen"],
                      "missed": {v: summaries[v]["missed_starts"] for v in VARIANTS}}))
    return 0


def run_nested(variants, recs) -> dict:
    """Leakage-safe nested calibration (ADR 0196). For each held-out lineage H: the calibration records of every other lineage C come from the stored SEG-085 inner refit
    `H.C` (model A trained on neither H nor C), are normalised per image, and fix tau_H = T0 - shortfall; H itself is scored by its own out-of-fold model and judged at tau_H."""
    import segarecomp_xplat_transfer as xt
    by_l = {L: [r for r in recs if r["lineage"] == L] for L in GENESIS}
    nested85 = json.loads((dyn.CACHE / "nested.json").read_text())
    out = {"schema": SCHEMA + ".nested", "variants": list(variants), "folds": {}, "rows": {v: {} for v in variants}, "plain_min_rows": {v: {} for v in variants}}
    for H in GENESIS:
        cal_recs = []
        for C in GENESIS:
            if C == H:
                continue
            z = xt.load_npz(dyn.CACHE / "inner" / f"{H}.{C}.npz")
            cal_recs += [{**r, "scores": [float(x) for x in z[r["id"]]], "model_excludes": frozenset({H, C})} for r in by_l[C]]
        fold = {}
        for v in variants:
            mins = lineage_minima(cal_recs, v)
            cal = calibrate_fold(H, cal_recs, v)
            fold[v] = {"tau": round(cal["tau"], 6), "T0": round(cal["T0"], 6), "shortfall": round(cal["shortfall"], 6), "lineage_minima": {k: round(x, 4) for k, x in sorted(mins.items())}}
            ev = evaluate(by_l[H], v, cal["tau"], starts_fn)
            out["rows"][v].update(ev["rows"])
            plain = evaluate(by_l[H], v, calibrate_fold(H, cal_recs, v, margin=False)["tau"], starts_fn)     # diagnostic: the non-conservative minimum rule
            out["plain_min_rows"][v].update(plain["rows"])
        out["folds"][H] = fold
        if "RAW" in variants:                                              # reproduction gate: the RAW control IS the SEG-085 isolated DYN_SCORE threshold, bitwise
            ref = nested85["folds"][H]["T_isolated"]
            if abs(fold["RAW"]["tau"] - round(ref, 6)) > 1e-5:
                raise SystemExit(f"RAW control does not reproduce the SEG-085 isolated threshold for {H}: {fold['RAW']['tau']} vs {ref}")
    out["raw_control_reproduces_seg085_dyn_score_thresholds"] = "RAW" in variants
    out["score_compatibility"] = compatibility(by_l, variants, lambda H, C: xt.load_npz(dyn.CACHE / "inner" / f"{H}.{C}.npz"))
    out["summaries"] = {v: summarise(out["rows"][v]) for v in variants}
    out["plain_min_summaries"] = {v: summarise(out["plain_min_rows"][v]) for v in variants}
    return out


def cmd_nested(a) -> int:
    t0 = time.process_time()
    imgs, recs = load_records()
    out = run_nested(tuple(a.variants), recs)
    out["cpu_seconds"] = round(time.process_time() - t0, 1)
    pathlib.Path(a.out).write_text(json.dumps(out, indent=1, sort_keys=True) + "\n")
    variants = out["variants"]
    print(json.dumps({v: {k: out["summaries"][v][k] for k in ("lineage_balanced_mean_retained", "cell_weighted_retained", "missed_starts", "status_counts")} for v in variants}))
    return 0


def policy_cost(res: dict, policy: str, overhead_cpu: float = 0.0) -> tuple:
    """Total build CPU of a policy for one image, using the unchanged SEG-085 accounting (`cost_of`: abstention = BROAD; a validator rejection followed by the broad fallback is
    charged in full) PLUS the model-inference / normalisation overhead, which is charged to every policy that used the score (and is never dropped on fallback)."""
    import segarecomp_seg085_eval as ev
    cpu, kind, rec = ev.cost_of(res, policy)
    return cpu + overhead_cpu, kind, rec


# ------------------------------------------------------------------------------------------------------------------------------- freeze
BOUND = ("tools/segarecomp_seg086_cal.py", "tools/segarecomp_seg086_norm.py", "tools/segarecomp_seg086_diag.py", "tools/segarecomp_sample_roles.py", "tools/segarecomp_seg086_cal.selection.json",
         "tools/segarecomp_seg085_dyn.py", "tools/segarecomp_ml_v3.py", "tools/segarecomp_xplat_models.py", "tools/segarecomp_seg085_cohort.manifest.json")
EXEMPT = ()                                         # nothing is exempt: the H01 diagnostic lives in a separate, later module


def sha_file(p: pathlib.Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def dir_digest(paths) -> str:
    h = hashlib.sha256()
    for q in sorted(paths):
        h.update(q.name.encode() + b"\0" + sha_file(q).encode() + b"\n")
    return h.hexdigest()


def plan_body() -> dict:
    import segarecomp_xplat_models as xm
    sel = json.loads(SELECTION.read_text())
    if sel["choice"]["chosen"] is None:
        raise SystemExit("no normaliser was chosen by the frozen selection rule")
    root = HERE.parent
    s84 = HARNESS / "segarecomp" / ".cache" / "seg084" / "scores"
    return {"schema": SCHEMA + ".plan", "protocol": "seg086.protocol.v1", "adr": "0195/0196",
            "normaliser": {"chosen": sel["choice"]["chosen"], "secondary_diagnostics": [v for v in ("N1",) if v != sel["choice"]["chosen"]], "rule": "ADR 0195 selection rule incl. its two recorded amendments",
                           "min_cells": nm.MIN_CELLS, "iqr_floor": nm.IQR_FLOOR},
            "calibration": {"rule": "tau = T0 - shortfall in the chosen space (SEG-085 DYN_SCORE rule: lowest lineage-minimum weakest-positive score minus the largest leave-one-lineage-out miss of the plain minimum rule)",
                            "safety_margin": "shortfall (leave-one-lineage-out, calibration lineages only)", "low_tail_treatment": "no trimming: every verified non-seed positive of every calibration image enters the lineage minimum; images whose normalisation fails are skipped (they fall back to broad AOT)",
                            "diagnostic_only": "plain minimum rule without margin (never the policy)", "fitting_lineages": list(GENESIS), "cap": CAP, "minimum_fitting_lineages": 2,
                            "isolation": "calibration records of lineage C for held-out H come from the SEG-085 inner refit H.C (model trained on neither H nor C); the held-out lineage is scored by its own leave-one-lineage-out model; LeakageError otherwise"},
            "reference_model": {"name": "structural model A (SEG-076, unchanged)", "config": json.loads(json.dumps(xm.A_CONFIG, sort_keys=True, default=str)),
                                "oof_scores_digest": dir_digest(list(s84.glob("A0.D*.npz"))), "inner_refit_digest": dir_digest(list((dyn.CACHE / "inner").glob("*.npz"))),
                                "inner_refit_bitwise_spot_checks": [["D09", "D05"], ["D12", "D01"]]},
            "abstention": ["unsupported (no independently established seed universe)", "rejected (non-finite score, empty image, bad threshold)", "normalisation_failed (fewer than min_cells cells, scale below floor, non-finite normalised score)", "abstain_cap (selected fraction above the cap)"],
            "populations": {"primary": "historical commercial-era Genesis (D01-D04)", "robustness": "modern SGDK (D05-D12)", "all_code_images": "excluded from the domain and counted"},
            "gates": GATES, "decision_ladder": ["EVIDENCE_INSUFFICIENT", "RANKING_LIMITATION", "RECALL_FAILED", "CALIBRATION_INEFFECTIVE", "GATES_PASSED_ECONOMICS_REQUIRED -> economics -> SCORE_CALIBRATION_FEASIBLE | CALIBRATION_INEFFECTIVE | RECALL_FAILED"],
            "economics": {"run_only_if": "all development gates pass", "benchmark": "SEG-074/076 exact-code benchmark (unchanged settings)", "policies": ["deployed incremental", "A+B50", "historical ADAPT", "normalised dynamic", "broad AOT"],
                          "overhead": "normalisation and inference CPU is added to the policy cost", "cpu_ceiling_hours": 12},
            "compute_ceilings_cpu_hours": {"calibration_and_refits": 8, "economics": 12},
            "h01": "REUSED_HISTORICAL_DIAGNOSTIC - NOT INDEPENDENT CONFIRMATION; evaluated only after this plan and the development results are frozen; never counted toward a fresh minimum",
            "bound_files": {f: sha_file(root / f) for f in BOUND}}


def cmd_freeze(a) -> int:
    if PLAN.exists():
        raise SystemExit("plan already frozen")
    PLAN.write_text(json.dumps(plan_body(), indent=1, sort_keys=True) + "\n")
    PLAN_DIGEST.write_text(sha_file(PLAN) + "\n")
    print(PLAN_DIGEST.read_text().strip())
    return 0


def verify_plan() -> str:
    if not PLAN.exists() or sha_file(PLAN) != PLAN_DIGEST.read_text().strip():
        raise SystemExit("frozen plan missing or modified")
    body = json.loads(PLAN.read_text())
    root = HERE.parent
    for f, h in body["bound_files"].items():
        if f in EXEMPT:
            continue
        if sha_file(root / f) != h:
            raise SystemExit(f"{f} changed after the freeze")
    if body["gates"] != GATES:
        raise SystemExit("decision constants differ from the frozen plan")
    return PLAN_DIGEST.read_text().strip()


# ------------------------------------------------------------------------------------------------------------------------------- final development evaluation
def as_rows(per_image: dict, policy: str, recs) -> dict:
    """SEG-085 stored policy outcomes as rows of the same shape as ours (same images)."""
    out = {}
    for r in recs:
        o = per_image[r["id"]][policy]
        out[r["id"]] = {"lineage": r["lineage"], "status": o["status"], "retained": o["retained"], "all_code": is_all_code(r), "cells": len(r["scores"]), "reason": None,
                        "missed_starts": o["missed_starts"] or 0}
    return out


def table(rows: dict) -> dict:
    gi = gate_inputs(rows)
    return {"all": gi, "historical": gate_inputs({k: r for k, r in rows.items() if r["lineage"] in HISTORICAL}),
            "modern_sgdk": gate_inputs({k: r for k, r in rows.items() if r["lineage"] not in HISTORICAL})}


def canonical(o) -> str:
    return hashlib.sha256(json.dumps(o, sort_keys=True).encode()).hexdigest()


def cmd_final(a) -> int:
    digest = verify_plan()
    plan = json.loads(PLAN.read_text())
    chosen = plan["normaliser"]["chosen"]
    t0 = time.process_time()
    imgs, recs = load_records()
    variants = ("RAW", "N1", "N2")
    first = run_nested(variants, recs)
    second = run_nested(variants, recs)
    determinism = canonical(first) == canonical(second)
    nested85 = json.loads((dyn.CACHE / "nested.json").read_text())
    primary_rows = first["rows"][chosen]
    comparators = {pol: as_rows(nested85["per_image"], pol, recs) for pol in ("B35", "B50", "ADAPT", "ADAPT_isolated", "DYN_SCORE")}
    tables = {"primary_" + chosen: table(primary_rows), **{"diagnostic_" + v: table(first["rows"][v]) for v in variants if v != chosen},
              **{"plain_min_diagnostic_" + v: table(first["plain_min_rows"][v]) for v in variants}, **{pol: table(rows) for pol, rows in comparators.items()}}
    # comparators are restricted to the images in the primary domain so that every row of the report describes the same images
    dom = {k for k, r in primary_rows.items() if not r["all_code"] and r["status"] not in ("unsupported", "rejected") and r["cells"] >= nm.MIN_CELLS}
    same_domain = {name: table({k: r for k, r in rows.items() if k in dom}) for name, rows in {**comparators, "primary_" + chosen: primary_rows, "plain_min_diagnostic_" + chosen: first["plain_min_rows"][chosen]}.items()}
    ocs = oracle_over_cap_share(recs, chosen)
    dev = decide(table(primary_rows)["all"], ocs, determinism, isolation_ok=True)
    out = {"schema": SCHEMA + ".results", "plan_sha256": digest, "chosen_normaliser": chosen, "nested": {k: v for k, v in first.items() if k not in ("rows", "plain_min_rows")},
           "rows_primary": primary_rows, "tables_all_images": tables, "tables_same_domain_as_primary": same_domain, "oracle_over_cap_share": ocs, "determinism_two_runs_identical": determinism,
           "determinism_sha256": canonical(first), "development_decision": dev, "misses_disclosed": {k: {"lineage": r["lineage"], "missed_starts": r["missed_starts"], "status": r["status"]} for v in variants for k, r in first["rows"][v].items() if r.get("missed_starts")},
           "cpu_seconds": round(time.process_time() - t0, 1), "scope_note": "development lineages only (every image also contributed to the formula/rule choice and to other lineages' calibration): not independent evidence"}
    RESULTS.write_text(json.dumps(out, indent=1, sort_keys=True) + "\n")
    print(json.dumps({"decision": dev, "determinism": determinism, "gate_inputs": table(primary_rows)["all"]}, default=str)[:1800])
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("select").set_defaults(func=cmd_select)
    sub.add_parser("freeze").set_defaults(func=cmd_freeze)
    sub.add_parser("final").set_defaults(func=cmd_final)
    n = sub.add_parser("nested")
    n.add_argument("--out", default=str(CACHE / "nested86.json"))
    n.add_argument("--variants", nargs="+", default=list(VARIANTS))
    n.set_defaults(func=cmd_nested)
    a = ap.parse_args(argv)
    return a.func(a)


if __name__ == "__main__":
    raise SystemExit(main())
