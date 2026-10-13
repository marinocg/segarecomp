#!/usr/bin/env python3
"""SEG-056 checkpoints 2-4 (private research driver; numpy only): corpus identity, development-failure diagnosis, label
audit, and historical C1 (+ structural roots) evaluation from the retained SEG-051 out-of-fold scores.

Reads private inputs (--img-dir, --truth-dir, --oof-dir); emits aggregates only. Sealed lineages are never opened: the
role list is taken from the frozen SEG-051 plan and any sealed id is refused by `open_lineage`.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_ml_region_z80 as z1  # noqa: E402
import segarecomp_ml_region_z80_gen2 as g  # noqa: E402

PLAN1 = HERE / "segarecomp_ml_region_z80.plan.json"


def lineages():
    plan = json.loads(PLAN1.read_text())
    return {l["id"]: l for l in plan["lineages"]}


def verify_identity(img_dir):
    """Image identity only (hashes): does not open any truth, so it is safe for sealed lineages."""
    out = {}
    for lid, l in lineages().items():
        h = hashlib.sha256((pathlib.Path(img_dir) / f"{lid}.sms").read_bytes()).hexdigest()
        out[lid] = {"role": l["role"], "image_sha256_matches_frozen_plan": h == l["image_sha256"]}
    return out


def open_lineage(lid, img_dir, truth_dir):
    if lineages()[lid]["role"] != "development":
        raise SystemExit(f"{lid}: sealed lineage truth must not be opened during development")
    img = (pathlib.Path(img_dir) / f"{lid}.sms").read_bytes()
    truth = json.loads((pathlib.Path(truth_dir) / f"{lid}.final.json").read_text())
    assert truth["rom_sha256"] == hashlib.sha256(img).hexdigest()
    kind, length, form = z1.parse_token_report((pathlib.Path(truth_dir) / f"{lid}.tok").read_bytes())
    return img, truth["classes"], length


def label_audit(classes, length):
    """Cross-check the 'c' definition against the product decoder: every I must be followed by L-1 continuation bytes."""
    bad_len = 0
    starts = 0
    q_edge = 0
    for p, ch in enumerate(classes):
        if ch != "I":
            continue
        starts += 1
        ln = length[p]
        tail = classes[p + 1:p + ln]
        if len(tail) < ln - 1 or any(t != "c" for t in tail):
            if any(t == "Q" for t in tail):
                q_edge += 1
            else:
                bad_len += 1
    orphan_c = sum(1 for p, ch in enumerate(classes) if ch == "c" and (p == 0 or classes[p - 1] not in "Ic"))
    return {"instruction_starts": starts, "listing_vs_decoder_length_mismatch": bad_len,
            "quarantine_edge": q_edge, "orphan_continuation_bytes": orphan_c}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img-dir", required=True)
    ap.add_argument("--truth-dir", required=True)
    ap.add_argument("--oof-dir", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    import numpy as np
    L = lineages()
    dev = sorted(i for i, l in L.items() if l["role"] == "development")
    res = {"identity": verify_identity(a.img_dir), "lineages": {}}
    for lid in dev:
        img, classes, length = open_lineage(lid, a.img_dir, a.truth_dir)
        oof = np.load(pathlib.Path(a.oof_dir) / f"oof_c1_{lid}.npy").tolist()
        lab32 = g.cell_labels(classes, 32)
        n = len(lab32)
        roots = g.root_cells(len(img), 32)
        m_plain = g.metrics(oof, lab32, [])
        m_roots = g.metrics(oof, lab32, roots)
        # worst residual positives
        order = z1.ranking(oof)
        rank = {c: r + 1 for r, c in enumerate(order)}
        pos = [c for c, l in enumerate(lab32) if l == z1.POSITIVE and c not in roots]
        worst = sorted(pos, key=lambda c: -rank[c])
        def comp(c):
            seg = classes[c * 32:(c + 1) * 32]
            return {k: seg.count(k) for k in "IcDAUQ"}
        top = []
        for c in worst[:6]:
            cc = comp(c)
            top.append({"rank_fraction": round(rank[c] / n, 3), "instruction_starts": cc["I"], "data_bytes": cc["D"],
                        "ambiguous_bytes": cc["A"], "image_edge": c in (0, n - 1),
                        "trainable_in_c1": c not in (0, n - 1)})
        # tail structure: how many positives sit below the 0.5 / 0.75 rank fraction, and how fragment-like they are
        tail = [c for c in pos if rank[c] / n > 0.5]
        tail75 = [c for c in pos if rank[c] / n > 0.75]
        frag = [c for c in tail if comp(c)["I"] <= 2]
        tail_adj = [c for c in tail if (c - 1 in pos or c + 1 in pos) or (lab32[max(c - 1, 0)] == 1 or lab32[min(c + 1, n - 1)] == 1)]
        negs = sum(1 for l in lab32 if l == z1.NEGATIVE)
        bl = g.byte_labels(classes)
        res["lineages"][lid] = {
            "image_bytes": len(img), "cells32": n, "root_cells32": roots,
            "label_coverage": {"positive_cells": sum(1 for l in lab32 if l == 1), "negative_cells": negs,
                               "masked_cells": sum(1 for l in lab32 if l == -1),
                               "positive_bytes": sum(1 for b in bl if b == 1), "negative_bytes": sum(1 for b in bl if b == 0),
                               "masked_bytes": sum(1 for b in bl if b == -1)},
            "label_audit": label_audit(classes, length),
            "c1_without_roots": m_plain, "c1_with_roots": m_roots,
            "worst_residual_positive_cells": top,
            "positives_ranked_below_half": len(tail), "positives_ranked_below_three_quarters": len(tail75),
            "tail_cells_with_at_most_two_starts": len(frag),
            "tail_cells_adjacent_to_other_positive": len(tail_adj),
        }
        print(lid, m_plain["frf"], m_roots["residual_frf"], flush=True)
    pathlib.Path(a.out).write_text(json.dumps(res, indent=1, sort_keys=True) + "\n")


if __name__ == "__main__":
    main()
