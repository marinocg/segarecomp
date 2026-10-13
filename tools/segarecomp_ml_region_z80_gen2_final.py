#!/usr/bin/env python3
"""SEG-056 sealed stage (private research tooling): fit the SELECTED frozen candidate on all development lineages and score the
three sealed lineages EXACTLY ONCE (marker file). Candidate, cell size, reduction and fixed budget are read from the committed
development results; nothing here chooses anything. Sealed truth is opened only by this script."""
import argparse
import hashlib
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_ml_region_z80_gen2 as g  # noqa: E402
import segarecomp_ml_region_z80_gen2_train as T  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img-dirs", nargs="+", required=True)
    ap.add_argument("--truth-dirs", nargs="+", required=True)
    ap.add_argument("--state", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--device", choices=["auto", "cpu", "cuda"], default="cpu")
    a = ap.parse_args()
    plan = T.check_plan()
    dev_res = json.loads((HERE / "segarecomp_ml_region_z80_gen2.dev_results.json").read_text())
    if dev_res["plan_digest"] != T.DIGEST.read_text().strip() or not dev_res.get("dev_gate_pass"):
        raise SystemExit("development gate not passed for this frozen plan: the sealed cohort stays closed")
    name, cs, agg = dev_res["selected"].split("/")
    cell, budget = int(cs[4:]), float(dev_res["fixed_budget"])
    state = pathlib.Path(a.state)
    marker = state / "sealed_evaluated.marker"
    if marker.exists():
        raise SystemExit("sealed cohort already evaluated once; refusing a second evaluation")
    np, torch = T.setup(a.threads)
    device = T.pick_device(torch, a.device)
    dev, sealed = T.dev_ids(plan), T.sealed_ids(plan)

    def find(lid):
        for idir, tdir in zip(a.img_dirs, a.truth_dirs):
            if (pathlib.Path(idir) / f"{lid}.sms").exists():
                return T.load(np, lid, idir, tdir)
        raise SystemExit(f"missing private inputs for {lid}")

    train = [find(i) for i in dev]
    assert not ({t["name"] for t in train} & set(sealed))
    marker.write_text("started\n")
    models = [T.fit(np, torch, train, name, s, device) for s in T.SEEDS]
    ident = hashlib.sha256()
    for m in models:
        for k, v in sorted(m.state_dict().items()):
            ident.update(k.encode())
            ident.update(v.detach().cpu().numpy().tobytes())
    res = {"plan_digest": T.DIGEST.read_text().strip(), "candidate": name, "cell_bytes": cell, "reduction": agg,
           "fixed_budget": budget, "weights_sha256": ident.hexdigest(), "device": str(device), "sealed": {}}
    for sid in sealed:
        item = find(sid)
        logits = np.mean([T.infer(np, torch, m, item) for m in models], axis=0)
        labels = g.cell_labels(item["classes"], cell)
        roots = g.root_cells(item["n"], cell)
        scores = g.aggregate(list(logits), cell, agg)
        row = g.metrics(scores, labels, roots)
        miss, pos, ret = g.missed_at_budget(scores, labels, roots, budget)
        row["fixed"] = {"budget": budget, "positive_cells": pos, "c_outside_r": miss, "retained_fraction": round(ret, 4),
                        "strict_recall": miss == 0}
        res["sealed"][sid] = row
        print("sealed", sid, row["fixed"], flush=True)
    mean_ret = sum(r["fixed"]["retained_fraction"] for r in res["sealed"].values()) / len(res["sealed"])
    res["mean_retained_fraction"] = round(mean_ret, 4)
    res["sealed_success"] = bool(all(r["fixed"]["strict_recall"] for r in res["sealed"].values()) and mean_ret <= 0.65)
    pathlib.Path(a.out).write_text(json.dumps(res, indent=1, sort_keys=True) + "\n")
    marker.write_text("done\n")
    print("sealed_success", res["sealed_success"])


if __name__ == "__main__":
    main()
