#!/usr/bin/env python3
"""SEG-086-T004 (ADR 0197): REUSED_HISTORICAL_DIAGNOSTIC - NOT INDEPENDENT CONFIRMATION.

Phantasy Star IV (H01) was scored and its outcomes examined in SEG-085. After the SEG-086 plan and the development results are frozen, this module applies the FROZEN SEG-086
policy to its stored SEG-085 final-model scores. The sample is read only through `segarecomp_sample_roles.EvaluationContext` (frozen-manifest membership, reserved identities first);
the result never counts toward a fresh evaluation-lineage minimum and cannot change the decision. Nothing here fits anything.
"""
from __future__ import annotations

import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_seg086_cal as cal  # noqa: E402
import segarecomp_seg086_norm as nm  # noqa: E402
import segarecomp_sample_roles as sr  # noqa: E402
import segarecomp_sample_guard as sg  # noqa: E402
import segarecomp_ml_v3 as v3  # noqa: E402

LABEL = "REUSED_HISTORICAL_DIAGNOSTIC - NOT INDEPENDENT CONFIRMATION"
MANIFEST = HERE / "segarecomp_seg085_cohort.manifest.json"


def main() -> int:
    plan_digest = cal.verify_plan()
    if not cal.RESULTS.exists():
        raise SystemExit("the development results must exist (and be committed) before the exposed diagnostic image is scored")
    results = json.loads(cal.RESULTS.read_text())
    chosen = results["chosen_normaliser"]
    import segarecomp_ml_v3_train as tr
    import segarecomp_xplat_t005 as x5
    import numpy as np
    ctx = sr.EvaluationContext.from_file(MANIFEST, "SEG-086 " + LABEL)
    dirs = ctx.discover(cal.dyn.CACHE / "data")
    sg.install(tr)                                                                      # post-freeze protection layer (ADR 0198): every Image is role-checked
    ims = [x5.attach(tr.Image(d, evaluation=ctx), d) for d in dirs]
    imgs, recs = cal.load_records()
    tau_final = cal.calibrate_tau(cal.lineage_minima(recs, chosen))                    # deployment-style calibration: the eleven strict leave-one-lineage-out records
    out = {"label": LABEL, "plan_sha256": plan_digest, "development_results_sha256": cal.sha_file(cal.RESULTS), "manifest_sha256": ctx.digest, "normaliser": chosen,
           "tau_final": {k: round(v, 6) for k, v in tau_final.items() if k != "lineages"}, "images": {}}
    for im in ims:
        sc = np.load(cal.dyn.CACHE / "final" / (im.id.replace("/", "@@") + ".npy"))
        labels = [int(x) for x in im.labels]
        rec = {"id": im.id, "lineage": im.lineage, "scores": [float(x) for x in sc], "labels": labels, "seeds": set(im.seeds), "classes": im.classes}
        row = {}
        for name, tau in (("frozen_policy", tau_final["tau"]), ("plain_minimum_diagnostic_not_the_policy", tau_final["T0"])):
            sel = cal.select_in_space(rec["scores"], rec["seeds"], tau, chosen, have_universe=bool(rec["seeds"]))
            row[name] = {"status": sel["status"], "retained": cal.r4(sel["retained"]) if hasattr(cal, "r4") else sel["retained"],
                         "missed_starts": v3.misses(v3.starts_of(im.classes), sel["region"]) if sel["status"] in ("selected", "abstain_cap", "normalisation_failed") else None,
                         "cells": len(sc), "selected_fraction_before_cap": sel.get("selected_fraction")}
        out["images"][im.id] = row
    pathlib.Path(HERE / "segarecomp_seg086_cal.h01.json").write_text(json.dumps(out, indent=1, sort_keys=True, default=str) + "\n")
    print(json.dumps(out, default=str)[:1500])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
