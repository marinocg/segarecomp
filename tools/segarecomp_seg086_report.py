#!/usr/bin/env python3
"""SEG-086-T004 (ADR 0197): aggregate report of the frozen development evaluation (reads `segarecomp_seg086_cal.results.json`; recomputes regions only to classify misses). OFFLINE research code."""
from __future__ import annotations

import json
import pathlib
import statistics
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_seg086_cal as cal  # noqa: E402
import segarecomp_seg086_norm as nm  # noqa: E402
import segarecomp_ml_v3 as v3  # noqa: E402

OUT = HERE / "segarecomp_seg086_report.json"


def r4(x):
    return None if x is None else round(float(x), 4)


def retention_stats(rows: dict) -> dict:
    """Per-image retention over the gate domain (fallbacks count 1.0): min / mean / median / max, worst lineage, counts by status and reason."""
    dom = {k: r for k, r in rows.items() if not r["all_code"] and r["status"] not in ("unsupported", "rejected") and r["cells"] >= nm.MIN_CELLS}
    vals = [1.0 if r["retained"] is None else r["retained"] for r in dom.values()]
    by_l = {}
    for r, v in zip(dom.values(), vals):
        by_l.setdefault(r["lineage"], []).append(v)
    lin = {L: sum(v) / len(v) for L, v in by_l.items()}
    reasons = {}
    for r in dom.values():
        k = r["status"] if not r.get("reason") else f"{r['status']}:{r['reason']}"
        reasons[k] = reasons.get(k, 0) + 1
    worst = max(lin, key=lambda L: (lin[L], L)) if lin else None
    return {"domain_images": len(dom), "min": r4(min(vals)) if vals else None, "mean": r4(sum(vals) / len(vals)) if vals else None, "median": r4(statistics.median(vals)) if vals else None,
            "max": r4(max(vals)) if vals else None, "worst_lineage": worst, "worst_lineage_mean": r4(lin[worst]) if worst else None, "non_fallback_images": sum(1 for r in dom.values() if r["status"] == "selected"),
            "fallback_reasons": reasons, "lineage_mean": {L: r4(v) for L, v in sorted(lin.items())},
            "all_code_images": sum(1 for r in rows.values() if r["all_code"]), "unsupported_images": sum(1 for r in rows.values() if r["status"] == "unsupported"),
            "below_minimum_cells_images": sum(1 for r in rows.values() if r["status"] == "normalisation_failed" and r.get("reason") == "too_few_cells")}


def classify_misses(variant: str, nested: dict, recs, rows: dict) -> list:
    """For every row with missed starts: how many missed starts lie in a mixed code/data cell versus a pure-code cell, and whether the image has a seed effect."""
    out = []
    by_id = {r["id"]: r for r in recs}
    for k, row in rows.items():
        if not row.get("missed_starts"):
            continue
        r = by_id[k]
        tau = nested["folds"][row["lineage"]][variant]["tau"]
        sel = cal.select_in_space(r["scores"], r["seeds"], tau, variant)
        mixed = pure = 0
        for a in v3.starts_of(r["classes"]):
            c = a // v3.CELL_BYTES
            if c in sel["region"]:
                continue
            chunk = r["classes"][c * v3.CELL_BYTES:(c + 1) * v3.CELL_BYTES]
            if b"D" in chunk:
                mixed += 1
            else:
                pure += 1
        out.append({"image": k, "lineage": row["lineage"], "variant": variant, "missed_starts": row["missed_starts"], "in_mixed_code_data_cells": mixed, "in_pure_code_cells": pure,
                    "cells_before_seeds": sel.get("cells_before_seeds"), "seed_only_cells": sel.get("seed_only_cells")})
    return out


def threshold_vs_plateau(nested: dict, results: dict, recs, variant: str) -> dict:
    """Why the frozen threshold abstains: for each held-out image the fraction of its cells with normalised score >= tau_H (the plateau sits at about 0 in N2)."""
    fr = []
    for r in recs:
        row = results["rows_primary"].get(r["id"])
        if row is None or row["status"] in ("unsupported", "rejected") or row["all_code"] or len(r["scores"]) < nm.MIN_CELLS:
            continue
        z, why = cal.space(r["scores"], variant)
        if z is None:
            continue
        tau = nested["folds"][r["lineage"]][variant]["tau"]
        fr.append(sum(1 for x in z if x >= tau) / len(z))
    taus = [f["N2"]["tau"] for f in nested["folds"].values()]
    return {"tau_range": [r4(min(taus)), r4(max(taus))], "fraction_of_cells_at_or_above_tau": {"min": r4(min(fr)), "median": r4(statistics.median(fr)), "max": r4(max(fr))} if fr else None, "images": len(fr)}


def low_tail_character(recs) -> dict:
    """Descriptive (after the decision rule was applied): what kind of cell are the verified non-seed positives that score below 0 (inside the data plateau) compared with the rest?"""
    import segarecomp_seg085_diag as sd
    low, rest = {"cells": 0, "mixed_with_data": 0, "mostly_data": 0, "single_start": 0}, {"cells": 0, "mixed_with_data": 0, "mostly_data": 0, "single_start": 0}
    for r in recs:
        lab = r["labels"]
        st, dt, _ = sd.cell_stats(r["classes"], lab)
        for c, y in enumerate(lab):
            if y == 1 and c not in r["seeds"]:
                d = low if r["scores"][c] < 0.0 else rest
                d["cells"] += 1
                d["mixed_with_data"] += dt[c] > 0
                d["mostly_data"] += dt[c] * 2 > v3.CELL_BYTES
                d["single_start"] += st[c] == 1
    return {"below_zero": low, "at_or_above_zero": rest, "note": "counts only; the low-tail positives are mostly mixed or data-dominated 128-byte cells containing few instruction starts"}


def main() -> int:
    import segarecomp_sample_guard as sg
    sg.install()                                                                        # post-freeze protection layer (ADR 0198)
    res = json.loads(cal.RESULTS.read_text())
    imgs, recs = cal.load_records()
    nested = res["nested"]
    chosen = res["chosen_normaliser"]
    first = cal.run_nested(("RAW", "N1", "N2"), recs)
    if cal.canonical(first) != res["determinism_sha256"]:
        raise SystemExit("the stored results are not reproduced by a fresh run")
    out = {"schema": "segarecomp.seg086.report.v1", "chosen_normaliser": chosen, "plan_sha256": res["plan_sha256"], "reproduces_stored_results": True,
           "retention": {"primary_" + chosen: retention_stats(first["rows"][chosen]), "plain_min_diagnostic_" + chosen: retention_stats(first["plain_min_rows"][chosen]),
                         **{"diagnostic_" + v: retention_stats(first["rows"][v]) for v in ("RAW", "N1") if v != chosen}},
           "disclosed_misses": [m for v in ("RAW", "N1", "N2") for m in classify_misses(v, first, recs, first["rows"][v])] + [dict(m, rule="plain_minimum_diagnostic") for v in ("RAW", "N1", "N2") for m in classify_misses(v, {"folds": {L: {v: {"tau": f[v]["T0"]}} for L, f in first["folds"].items()}}, recs, first["plain_min_rows"][v])],
           "low_tail_positive_character": low_tail_character(recs), "threshold_vs_plateau": threshold_vs_plateau(first, res, recs, chosen), "validator_rejections": "none: no proposal was passed to the closure and validator (the frozen policy abstains on every domain image, and the economics gate was not reached)",
           "seed_and_closure_effects": "offline only: seeds are united with every selection (seed-only cells are reported per selected row); structural closure was not evaluated because no region was proposed",
           "normalisation_failures": {"primary_domain": sum(1 for r in first["rows"][chosen].values() if r["status"] == "normalisation_failed" and r.get("reason") != "too_few_cells" and not r["all_code"]),
                                      "below_minimum_cells": sum(1 for r in first["rows"][chosen].values() if r.get("reason") == "too_few_cells")}}
    OUT.write_text(json.dumps(out, indent=1, sort_keys=True) + "\n")
    print(json.dumps({"retention": {k: {a: v[a] for a in ("domain_images", "min", "mean", "median", "worst_lineage", "non_fallback_images")} for k, v in out["retention"].items()},
                      "misses": out["disclosed_misses"], "thr": out["threshold_vs_plateau"], "fail": out["normalisation_failures"]}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
