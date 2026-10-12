#!/usr/bin/env python3
"""SEG-051 (ADR 0114): OFFLINE Z80 executable-region CNN pilot trainer / evaluator (private research tooling).

numpy and PyTorch are NOT product dependencies and are imported lazily; the release never needs them. All definitions
(cells, labels, ranking, FRF, budget policies, lineage split and leakage guard) are in the stdlib-only
`segarecomp_ml_region_z80.py`; this file only vectorises them and runs the plan frozen in
`segarecomp_ml_region_z80.plan.json` (its digest is checked before anything is fitted).

Private inputs (never committed): `--img-dir/<id>.sms` reference images, `--truth-dir/<id>.final.json` class strings and
`<id>.tok` Z80 decode-token reports. Output is aggregates only.

  dev     leave-one-lineage-out over the development lineages (candidates C1/C2 and the two controls)
  final   fit the selected candidate on all development lineages and score the sealed lineages EXACTLY ONCE
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_ml_region_z80 as z  # noqa: E402

PLAN = HERE / "segarecomp_ml_region_z80.plan.json"
DIGEST = HERE / "segarecomp_ml_region_z80.plan.digest"
SEEDS = (1, 2, 3)
EPOCHS = 30
CROP = 4096
BATCH = 16


def check_plan():
    plan = json.loads(PLAN.read_text())
    if z.canonical_digest(plan) != DIGEST.read_text().strip():
        raise SystemExit("plan digest mismatch: the frozen plan was modified")
    return plan


def load(np, name, img_dir, truth_dir):
    img = (pathlib.Path(img_dir) / f"{name}.sms").read_bytes()
    truth = json.loads((pathlib.Path(truth_dir) / f"{name}.final.json").read_text())
    if truth["rom_sha256"] != hashlib.sha256(img).hexdigest():
        raise SystemExit(f"{name}: image does not match its truth")
    kind, length, form = z.parse_token_report((pathlib.Path(truth_dir) / f"{name}.tok").read_bytes())
    n = len(img)
    padded = z.cell_count(n) * z.CELL_BYTES
    pad = padded - n
    byte = np.frombuffer(img + bytes(pad), dtype=np.uint8).astype(np.int64)
    tk = np.frombuffer(bytes(kind) + bytes(pad), dtype=np.uint8).astype(np.int64)
    tl = np.minimum(np.frombuffer(bytes(length) + bytes(pad), dtype=np.uint8).astype(np.int64), 7)
    tf = np.array([z.form_token(f) for f in form] + [0] * pad, dtype=np.int64)
    labels = np.array(z.cell_labels(truth["classes"]), dtype=np.int64)
    return {"name": name, "byte": byte, "kind": tk, "len": tl, "form": tf, "labels": labels, "n": n}


def build_model(torch, use_tokens):
    nn = torch.nn

    class Net(nn.Module):
        def __init__(self):
            super().__init__()
            self.eb = nn.Embedding(256, 16)
            dim = 16
            if use_tokens:
                self.ek = nn.Embedding(4, 4)
                self.el = nn.Embedding(8, 4)
                self.ef = nn.Embedding(z.FORM_CARDINALITY + 1, 8)
                dim += 16
            self.use_tokens = use_tokens
            chans = [dim, 32, 32, 32, 32]
            self.convs = nn.ModuleList(
                [nn.Conv1d(chans[i], chans[i + 1], 5, padding=2 * d, dilation=d) for i, d in enumerate((1, 2, 4, 8))]
            )
            self.head = nn.Linear(64, 1)

        def forward(self, b, k, l, f):
            parts = [self.eb(b)]
            if self.use_tokens:
                parts += [self.ek(k), self.el(l), self.ef(f)]
            x = torch.cat(parts, dim=-1).transpose(1, 2)
            for conv in self.convs:
                x = torch.relu(conv(x))
            bsz, ch, length = x.shape
            cells = x.reshape(bsz, ch, length // z.CELL_BYTES, z.CELL_BYTES)
            pooled = torch.cat([cells.mean(dim=3), cells.max(dim=3).values], dim=1).transpose(1, 2)
            return self.head(pooled).squeeze(-1)

    return Net()


def fit(np, torch, train, use_tokens, seed):
    torch.manual_seed(seed)
    rng = np.random.default_rng(seed)
    model = build_model(torch, use_tokens)
    opt = torch.optim.AdamW(model.parameters(), lr=0.002, weight_decay=0.0001)
    pos = sum(int((t["labels"] == z.POSITIVE).sum()) for t in train)
    neg = sum(int((t["labels"] == z.NEGATIVE).sum()) for t in train)
    pos_weight = torch.tensor(max(neg, 1) / max(pos, 1), dtype=torch.float32)
    total_bytes = sum(len(t["byte"]) for t in train)
    crops_per_epoch = max(BATCH, 2 * (total_bytes // CROP))
    sizes = np.array([len(t["byte"]) for t in train], dtype=np.float64)
    cells_per_crop = CROP // z.CELL_BYTES
    model.train()
    for _ in range(EPOCHS):
        for _ in range(crops_per_epoch // BATCH):
            bs, ks, ls, fs, lab = [], [], [], [], []
            for _ in range(BATCH):
                t = train[int(rng.choice(len(train), p=sizes / sizes.sum()))]
                ncell = len(t["labels"])
                c0 = int(rng.integers(0, max(1, ncell - cells_per_crop + 1)))
                s, e = c0 * z.CELL_BYTES, min(len(t["byte"]), (c0 + cells_per_crop) * z.CELL_BYTES)
                span = np.arange(s, s + CROP)
                valid = span < len(t["byte"])
                span = np.minimum(span, len(t["byte"]) - 1)
                bs.append(np.where(valid, t["byte"][span], 0))
                ks.append(np.where(valid, t["kind"][span], 0))
                ls.append(np.where(valid, t["len"][span], 0))
                fs.append(np.where(valid, t["form"][span], 0))
                lab_row = np.full(cells_per_crop, z.MASKED, dtype=np.int64)
                got = t["labels"][c0:c0 + cells_per_crop]
                lab_row[:len(got)] = got
                lab_row[0] = lab_row[-1] = z.MASKED  # receptive-field edge cells carry no loss
                lab.append(lab_row)
            tb = [torch.from_numpy(np.stack(a)) for a in (bs, ks, ls, fs)]
            y = torch.from_numpy(np.stack(lab))
            logits = model(*tb)
            mask = y != z.MASKED
            if not bool(mask.any()):
                continue
            loss = torch.nn.functional.binary_cross_entropy_with_logits(
                logits[mask], y[mask].float(), pos_weight=pos_weight)
            opt.zero_grad()
            loss.backward()
            opt.step()
    model.eval()
    return model


def score(np, torch, models, item):
    outs = []
    with torch.no_grad():
        args = [torch.from_numpy(item[k][None, :]) for k in ("byte", "kind", "len", "form")]
        for m in models:
            outs.append(m(*args)[0].numpy().astype(np.float64))
    return np.mean(outs, axis=0)  # mean logit across seeds


def lr_scores(np, train, item):
    def feats(t):
        b = t["byte"].reshape(-1, z.CELL_BYTES)
        h = np.zeros((b.shape[0], 256))
        for i in range(b.shape[0]):
            h[i] = np.bincount(b[i], minlength=256) / z.CELL_BYTES
        return h
    X = np.concatenate([feats(t) for t in train])
    y = np.concatenate([t["labels"] for t in train])
    keep = y != z.MASKED
    X, y = X[keep], y[keep].astype(np.float64)
    w, b = np.zeros(256), 0.0
    pw = max((y == 0).sum(), 1) / max((y == 1).sum(), 1)
    for _ in range(300):
        p = 1 / (1 + np.exp(-(X @ w + b)))
        g = (p - y) * np.where(y == 1, pw, 1.0)
        w -= 0.5 * (X.T @ g / len(y) + 1e-3 * w)
        b -= 0.5 * g.mean()
    return feats(item) @ w + b


def sigmoid(np, x):
    return 1 / (1 + np.exp(-x))


def evaluate(np, logits, labels, name, extra=None):
    probs = sigmoid(np, logits).tolist()
    lab = labels.tolist()
    # score ties are real for the control; ranking() is deterministic (lower cell first)
    out = {"lineage": name, "cells": len(lab), "positive_cells": sum(1 for v in lab if v == z.POSITIVE),
           "negative_cells": sum(1 for v in lab if v == z.NEGATIVE), "frf": round(z.frf(logits.tolist(), lab), 4)}
    out["_probs"] = probs
    if extra:
        out.update(extra)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["dev", "final"])
    ap.add_argument("--img-dir", required=True)
    ap.add_argument("--truth-dir", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--state", required=True, help="private dir for OOF state and the sealed-once marker")
    args = ap.parse_args()
    plan = check_plan()
    import numpy as np
    import torch
    torch.set_num_threads(4)
    torch.use_deterministic_algorithms(True)
    ids = [l["id"] for l in plan["lineages"]]
    role = {l["id"]: l["role"] for l in plan["lineages"]}
    dev_ids = sorted(i for i in ids if role[i] == "development")
    sealed_ids = sorted(i for i in ids if role[i] == "sealed")
    assert (dev_ids, sealed_ids) == z.split_lineages(ids)
    state = pathlib.Path(args.state)
    state.mkdir(parents=True, exist_ok=True)
    data = {i: load(np, i, args.img_dir, args.truth_dir) for i in ids}
    lineage_of = {i: i for i in ids}

    if args.mode == "dev":
        res = {"plan_digest": DIGEST.read_text().strip(), "mode": "dev", "folds": {}}
        oof = {}
        for held in dev_ids:
            train = [i for i in dev_ids if i != held]
            z.assert_split(train, [held], sealed_ids, lineage_of)
            row = {}
            for cand, use_tokens in (("C1", False), ("C2", True)):
                models = [fit(np, torch, [data[i] for i in train], use_tokens, s) for s in SEEDS]
                lg = score(np, torch, models, data[held])
                oof[(cand, held)] = lg
                row[cand] = evaluate(np, lg, data[held]["labels"], held)
            lg = lr_scores(np, [data[i] for i in train], data[held])
            oof[("LR", held)] = lg
            row["LR"] = evaluate(np, lg, data[held]["labels"], held)
            rs = np.random.default_rng(7).random(len(data[held]["labels"]))
            oof[("RANDOM", held)] = rs
            row["RANDOM"] = evaluate(np, rs, data[held]["labels"], held)
            res["folds"][held] = row
            print("fold", held, {k: v["frf"] for k, v in row.items()}, flush=True)
        means = {c: sum(res["folds"][h][c]["frf"] for h in dev_ids) / len(dev_ids) for c in ("C1", "C2", "LR", "RANDOM")}
        medians = {c: sorted(res["folds"][h][c]["frf"] for h in dev_ids)[len(dev_ids) // 2] for c in means}
        selected = "C2" if means["C1"] - means["C2"] >= 0.05 else "C1"
        frfs = [res["folds"][h][selected]["frf"] for h in dev_ids]
        fixed = z.fixed_budget(frfs)
        dev_oof = [(res["folds"][h][selected]["_probs"], res["folds"][h][selected]["frf"]) for h in dev_ids]
        mult = z.calibrate_adaptive(dev_oof)
        res.update({"mean_frf": means, "median_frf": medians, "selected": selected, "fixed_budget": fixed,
                    "adaptive_multiplier": mult})
        for h in dev_ids:
            for c in res["folds"][h]:
                res["folds"][h][c].pop("_probs", None)
        # dev recall at the frozen budgets (selected candidate)
        for h in dev_ids:
            lg = oof[(selected, h)].tolist()
            lab = data[h]["labels"].tolist()
            hit, tot = z.recall_at(lg, lab, fixed)
            res["folds"][h]["fixed_recall"] = [hit, tot]
        (state / "dev_state.json").write_text(json.dumps({"selected": selected, "fixed": fixed, "mult": mult}))
        pathlib.Path(args.out).write_text(json.dumps(res, indent=1, sort_keys=True) + "\n")
        print("selected", selected, "fixed", fixed, "adaptive_m", mult, "means", means, flush=True)
        return

    marker = state / "sealed_evaluated.marker"
    if marker.exists():
        raise SystemExit("sealed cohort already evaluated once; refusing a second evaluation")
    dev_state = json.loads((state / "dev_state.json").read_text())
    selected, fixed, mult = dev_state["selected"], dev_state["fixed"], dev_state["mult"]
    z.assert_split(dev_ids, [], sealed_ids, lineage_of)
    marker.write_text("started\n")
    models = [fit(np, torch, [data[i] for i in dev_ids], selected == "C2", s) for s in SEEDS]
    res = {"plan_digest": DIGEST.read_text().strip(), "mode": "final", "selected": selected, "fixed_budget": fixed,
           "adaptive_multiplier": mult, "sealed": {}}
    for sid in sealed_ids:
        if sid in dev_ids:
            raise SystemExit("sealed lineage in the development set")
        lg = score(np, torch, models, data[sid])
        lab = data[sid]["labels"].tolist()
        probs = sigmoid(np, lg).tolist()
        row = evaluate(np, lg, data[sid]["labels"], sid)
        row.pop("_probs")
        hit, tot = z.recall_at(lg.tolist(), lab, fixed)
        row["fixed"] = {"budget": fixed, "positives_covered": hit, "positives": tot, "c_outside_r": tot - hit,
                        "strict_recall": hit == tot}
        if mult is not None:
            ab = z.adaptive_budget(probs, mult)
            hit, tot = z.recall_at(lg.tolist(), lab, ab)
            row["adaptive"] = {"budget": round(ab, 4), "positives_covered": hit, "positives": tot,
                               "c_outside_r": tot - hit, "strict_recall": hit == tot,
                               "statistic": round(z.confident_fraction(probs), 4)}
        res["sealed"][sid] = row
        print("sealed", sid, {k: row[k] for k in ("frf", "fixed")}, flush=True)
    pathlib.Path(args.out).write_text(json.dumps(res, indent=1, sort_keys=True) + "\n")
    marker.write_text("done\n")


if __name__ == "__main__":
    main()
