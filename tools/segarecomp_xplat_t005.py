#!/usr/bin/env python3
"""SEG-075-T005 (ADR 0161 decisions.t005_scope, ADR 0163): runner for the pre-registered models A, B and C.

  addendum  write the T005 pre-fit addendum (tools/segarecomp_xplat_design.t005.json): plan digest, digests of the model / runner /
            decision code, the fixed configurations, the gates. Committed and independently reviewed BEFORE any A/B/C fit
  fit       one fit of A, B or C (same folds as T004: Genesis lineage | xplat:<lineage> | all), refused unless the addendum matches the
            current code and plan; the training condition is NOT a parameter: it is C2 unless the T004 overall class is TRANSFER_HARMS
            (then C0), read from the committed T004 results; every record carries the addendum digest
  gate      evaluate the pre-registered B / C gates from the T004 + T005 records (decide.select / b_gate / better_than)
  table     A/B/C per-lineage metrics with the T004 metric code, candidates for decide.select over {M1, A, M3_R, M4@128, M4@64, B, C}

The T004 runner (segarecomp_xplat_transfer.py, bound by the frozen plan) supplies folds, image loading, metrics and the barrier.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_xplat_decide as decide  # noqa: E402
import segarecomp_xplat_design as design  # noqa: E402
import segarecomp_xplat_transfer as xt  # noqa: E402

ADDENDUM = HERE / "segarecomp_xplat_design.t005.json"
T004_RESULTS = HERE / "segarecomp_xplat_transfer.results.json"
T005_RESULTS = HERE / "segarecomp_xplat_t005.results.json"
BOUND = ("segarecomp_xplat_models.py", "segarecomp_xplat_t005.py", "segarecomp_xplat_decide.py", "segarecomp_xplat_transfer.py",
         "segarecomp_xplat_design.py")


def sha(p: pathlib.Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def addendum_doc() -> dict:
    import segarecomp_xplat_models as xm
    plan = design.verify_plan()
    return {"schema": "segarecomp.xplat_design.t005_addendum.v1", "task": "SEG-075-T005", "adr": "0161/0163",
            "plan_sha256": hashlib.sha256(design.PLAN.read_bytes()).hexdigest(),
            "bound_code": {n: sha(HERE / n) for n in BOUND},
            "a_config": xm.A_CONFIG, "bc_architecture": xm.B_ARCH, "bc_training": "segarecomp_ml_v3_train.M4 recipe unchanged, seeds 1-3",
            "a_features": "segarecomp_xplat_models.a_features (210 columns: 59 token-value counts, chain max/mean/long, targets out/in, "
                          "zero/ASCII fraction, entropy, relocation fields, seed flag, PROVEN-reach count; then the +/-1 and +/-4 "
                          "neighbour means)",
            "condition_rule": plan["decisions"]["t005_scope"]["condition"],
            "gates": {"B": plan["decisions"]["t005_scope"]["model_B"], "C": plan["decisions"]["t005_scope"]["model_C"]},
            "enforcement": "fit refuses unless this addendum's digests equal the current files and the plan; each fit record carries "
                           "the addendum digest; no A/B/C fit was run before this addendum was committed"}


def verify_addendum() -> str:
    if not ADDENDUM.is_file():
        raise SystemExit("T005 addendum missing: no A/B/C fit before the reviewed pre-fit addendum")
    doc = json.loads(ADDENDUM.read_text())
    plan_sha = hashlib.sha256(design.PLAN.read_bytes()).hexdigest()
    design.verify_plan()
    if doc["plan_sha256"] != plan_sha:
        raise SystemExit("T005 addendum is bound to a different plan")
    for n, want in doc["bound_code"].items():
        if sha(HERE / n) != want:
            raise SystemExit(f"{n} differs from the code bound by the T005 addendum")
    return sha(ADDENDUM)


def condition() -> str:
    """C2 unless the committed T004 overall class is TRANSFER_HARMS (then C0); EVIDENCE_INSUFFICIENT stops T005."""
    r = json.loads(T004_RESULTS.read_text())
    if r.get("plan_sha256") != hashlib.sha256(design.PLAN.read_bytes()).hexdigest():
        raise SystemExit("T004 results are not bound to the current plan")
    cls = r["transfer_class"]
    if cls == "EVIDENCE_INSUFFICIENT":
        raise SystemExit("T004 is EVIDENCE_INSUFFICIENT: T005 is not admitted")
    return "C0" if cls == "TRANSFER_HARMS" else "C2"


def attach(im, d: pathlib.Path):
    """Give a loaded image its seed addresses (Genesis: registry reports; cross-platform: meta) and its directory (relocations)."""
    im._dir = d
    m = json.loads((d / "meta.json").read_text())
    im.seed_addresses = m["seed_addresses"] if "seed_addresses" in m else xt.genesis_seed_addresses(d)
    return im


def run_fit(model: str, fold: str, seed: int | None, snapshot: pathlib.Path, out: pathlib.Path) -> dict:
    import segarecomp_xplat_models as xm
    import segarecomp_ml_v3_train as t
    add = verify_addendum()
    plan = design.verify_plan()
    cond = condition()
    if model in ("B", "C") and seed not in (1, 2, 3):
        raise SystemExit("B/C require --seed 1|2|3")
    if model == "A" and seed is not None:
        raise SystemExit("A is deterministic (random_state 49): no --seed")
    train_dirs, held_dirs, held = xt.fold_sets(plan, cond, fold, snapshot) if not fold.startswith("xplat:") else \
        xt.fold_sets(plan, "C2", fold, snapshot) if cond == "C2" else (None, None, None)
    if train_dirs is None:
        raise SystemExit("xplat_lolo folds are defined for C2 only")
    train = [attach(im, d) for im, d in zip(xt.load(train_dirs), train_dirs)]
    held_imgs = [attach(im, d) for im, d in zip(xt.load(held_dirs), held_dirs)]
    if held and any(im.lineage == held for im in train):
        raise SystemExit("held-out lineage in training data")
    t0 = time.time()
    log = (lambda m: print(f"[{model}/{cond}/{fold}] {m}", flush=True))
    if model == "A":
        clf = xm.fit_a(train)
        scores = {im.id: xm.score_a(clf, im) for im in held_imgs}
    else:
        net = xm.fit_bc(train, seed, model == "C", log)
        scores = {im.id: xm.infer_bc(net, im, model == "C") for im in held_imgs}
    path = xt.score_path(out, model, cond, fold, seed)
    t.save_scores(path, scores)
    if fold == "all":
        if model == "A":
            import pickle
            path.with_suffix(".pkl").write_bytes(pickle.dumps(clf))
        else:
            import torch
            torch.save(net.state_dict(), path.with_suffix(".pt"))
    rec = {"model": model, "condition": cond, "fold": fold, "seed": seed, "seconds": round(time.time() - t0, 1), "addendum_sha256": add,
           "train_images": sorted(im.id for im in train), "held_images": sorted(im.id for im in held_imgs), "environment": xt.environment()}
    path.with_suffix(".json").write_text(json.dumps(rec, indent=1, sort_keys=True))
    print(json.dumps({k: rec[k] for k in ("model", "condition", "fold", "seed", "seconds")}), flush=True)
    return rec


def fit_scores(model: str, cond: str, fold: str, out: pathlib.Path, sizes: dict):
    import numpy as np
    import segarecomp_ml_v3_train as t
    f = fold.replace(":", "-")
    suffix = ".xplat" if fold == "all" else ""
    if model == "A":
        p = out / "scores" / f"A.{cond}.{f}{suffix}.npz"
        return {k: v.astype(np.float64) for k, v in xt.load_npz(p).items()} if p.exists() else None
    ps = [out / "scores" / f"{model}.{cond}.{f}.s{s}{suffix}.npz" for s in (1, 2, 3)]
    if not all(p.exists() for p in ps):
        return None
    arrs = [xt.load_npz(p) for p in ps]
    return {iid: t.cell_max(np.mean([a[iid] for a in arrs], axis=0).astype(np.float32), sizes[iid], 128) for iid in arrs[0]}


def records_for(model: str, cond: str, out: pathlib.Path, snap: pathlib.Path) -> tuple[dict, list]:
    plan = design.verify_plan()
    dev_x = plan["conditions"]["C2"]["xplat_training"]
    meta = {}
    for d in xt.genesis_dirs(xt.GENESIS_LINEAGES):
        m = json.loads((d / "meta.json").read_text())
        meta[m["id"]] = {"dir": d, "lineage": m["lineage"], "size": m["size"], "seeds": xt.genesis_seed_addresses(d)}
    for d in xt.xplat_dirs(snap, dev_x):
        m = json.loads((d / "meta.json").read_text())
        meta[m["id"]] = {"dir": d, "lineage": m["lineage"], "size": m["size"], "seeds": m.get("seed_addresses", [])}
    sizes = {k: v["size"] for k, v in meta.items()}
    lineage_of = {k: v["lineage"] for k, v in meta.items()}
    missing, g, x = [], {}, {}
    for fold in xt.GENESIS_LINEAGES:
        sc = fit_scores(model, cond, fold, out, sizes)
        if sc is None:
            missing.append(fold)
            continue
        for iid, s in sc.items():
            g[iid] = xt.image_metrics((meta[iid]["dir"] / "classes.bin").read_bytes(), meta[iid]["seeds"], s, 128)
    for lin in dev_x:
        sc = fit_scores(model, cond, f"xplat:{lin}", out, sizes) if cond == "C2" else fit_scores(model, cond, "all", out, sizes)
        if sc is None:
            missing.append(f"xplat:{lin}")
            continue
        for iid, s in sc.items():
            if lineage_of.get(iid) == lin:
                x[iid] = xt.image_metrics((meta[iid]["dir"] / "classes.bin").read_bytes(), meta[iid]["seeds"], s, 128)
    gr, xr = xt.lineage_rows(g, lineage_of), xt.lineage_rows(x, lineage_of)
    return {"genesis": gr, "xplat": xr}, missing


def candidates_and_gates(t004: dict, t005_tables: dict) -> dict:
    """Pre-registered gates (pure over the result documents; unit-tested)."""
    tau, asum = t004["noise"]["tau"], sum(t004["noise"]["b50_miss_allowance"].values())
    cands = list(t004["candidates"])
    for model, tab in t005_tables.items():
        if tab is None:
            continue
        g, x = tab["genesis"], tab["xplat"]
        cands.append({"config": model, "condition": tab["condition"], "genesis_b50_misses": sum(r["misses_B50"] for r in g.values()),
                      "genesis_mean_er": xt.mean_er(g), "xplat_mean_er": xt.mean_er(x), "complete_budget": xt.complete_budget(g, asum)})
    base = [c for c in cands if c["config"] in ("M1_HGB", "A", "M3_R", "M4_DENSE@128", "M4_DENSE@64")]
    best_base = decide.select(base, tau, asum)
    gate_b = decide.b_gate(best_base) and t004["transfer_class"] != "EVIDENCE_INSUFFICIENT"
    by = {c["config"]: c for c in cands}
    gate_c = ("A" in by and "B" in by and decide.better_than(by["B"], by["A"], tau, asum)
              and by["A"]["genesis_mean_er"] - by["B"]["genesis_mean_er"] >= tau)
    selected = decide.select(cands, tau, asum)
    return {"candidates": cands, "best_without_B_C": best_base, "gate_B": gate_b, "gate_C": gate_c, "selected": selected, "tau": tau,
            "allowance_sum": asum}


def cmd_addendum(a) -> int:
    doc = addendum_doc()
    ADDENDUM.write_text(json.dumps(doc, indent=1, sort_keys=True) + "\n")
    print(json.dumps({"addendum_sha256": sha(ADDENDUM), "bound": list(doc["bound_code"])}))
    return 0


def cmd_fit(a) -> int:
    run_fit(a.model, a.fold, a.seed, pathlib.Path(a.snapshot).resolve(), pathlib.Path(a.out).resolve())
    return 0


def cmd_table(a) -> int:
    verify_addendum()
    out, snap = pathlib.Path(a.out).resolve(), pathlib.Path(a.snapshot).resolve()
    t004 = json.loads(T004_RESULTS.read_text())
    cond = condition()
    tables, missing = {}, {}
    for model in a.models:
        tab, miss = records_for(model, cond, out, snap)
        missing[model] = miss
        tables[model] = None if miss else dict(tab, condition=cond)
    res = candidates_and_gates(t004, tables)
    doc = {"schema": "segarecomp.xplat_t005.results.v1", "task": "SEG-075-T005", "addendum_sha256": sha(ADDENDUM), "condition": cond,
           "missing": missing, "tables": tables, **res,
           "data_boundary": "aggregates only: per-lineage ER/FRF, counts and AUROC; no per-cell score, address or image content"}
    (pathlib.Path(a.results) if a.results else T005_RESULTS).write_text(json.dumps(doc, indent=1, sort_keys=True) + "\n")
    print(json.dumps({k: doc[k] for k in ("condition", "missing", "gate_B", "gate_C", "selected", "best_without_B_C")}, indent=1))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("addendum").set_defaults(func=cmd_addendum)
    f = sub.add_parser("fit")
    f.add_argument("--model", required=True, choices=("A", "B", "C"))
    f.add_argument("--fold", required=True)
    f.add_argument("--seed", type=int)
    f.add_argument("--snapshot", required=True)
    f.add_argument("--out", required=True)
    f.set_defaults(func=cmd_fit)
    t = sub.add_parser("table")
    t.add_argument("--snapshot", required=True)
    t.add_argument("--out", required=True)
    t.add_argument("--models", nargs="+", default=["A"])
    t.add_argument("--results")
    t.set_defaults(func=cmd_table)
    a = ap.parse_args(argv)
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())
