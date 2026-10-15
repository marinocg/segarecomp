#!/usr/bin/env python3
"""SEG-060 Checkpoint A: oracle-bound screen of the adaptive SMS retention-budget premise (stdlib only).

Reads only the committed SEG-056 development results (no ROM, truth, logits or numpy). Every quantity used here
(`positive_fraction`, `retained_for_recall_*`, per-seed worst ranks) is derived from source-backed LABELS, so it is an
ORACLE input that an inference-time estimator could not observe. The screen asks: even with these oracle inputs, does a
simple leave-one-lineage-out (LOLO) rule bound each held-out lineage's full-recall retention below the fixed 0.65 budget?
If it cannot, density-type and recall-point-type estimators are insufficient; this says nothing about estimators built from
other score-derived features (tail shape, gaps, knee, seed agreement), which this screen does not evaluate.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
RESULTS = HERE / "segarecomp_ml_region_z80_gen2.dev_results.json"
CANDIDATE = "D512HP/cell32/top2"
CAP = 0.65
MATERIAL = 0.10  # "materially smaller" = at least 10 points under the cap


def load(path=RESULTS):
    d = json.loads(pathlib.Path(path).read_text())
    folds = d["candidates"][CANDIDATE]["folds"]
    seed_std = d["candidates"][CANDIDATE]["seed_pstdev"]
    rows = {}
    for lid, f in sorted(folds.items()):
        rows[lid] = {
            "cells": f["cells"],
            "pos": f["positive_fraction"],
            "r90": f["retained_for_recall_0.9"],
            "r99": f["retained_for_recall_0.99"],
            "full": f["retained_full_recall"],
            "seed_std": seed_std[lid],
        }
    return rows


def _ratio_rule(num, margin):
    def rule(held, train):
        return margin * held[num] * max(t["full"] / t[num] for t in train)
    return rule


RULES = {
    # name: (rule, label-derived input)
    "density_x_max_ratio": (_ratio_rule("pos", 1.0), "positive_fraction"),
    "density_x_max_ratio_x1.2": (_ratio_rule("pos", 1.2), "positive_fraction"),
    "r99_x_max_ratio_x1.1": (_ratio_rule("r99", 1.1), "retained_for_recall_0.99"),
    "constant_max_of_others": (lambda h, tr: max(t["full"] for t in tr), "none"),
}


def lolo(rows, rule):
    out = {}
    for held in rows:
        train = [r for k, r in rows.items() if k != held]
        raw = rule(rows[held], train)
        budget = min(CAP, raw)
        out[held] = {"budget": budget, "need": rows[held]["full"], "miss": budget < rows[held]["full"], "abstain": raw >= CAP}
    return out


def summarize(res):
    n = len(res)
    mean = sum(v["budget"] for v in res.values()) / n
    return {
        "mean_budget": mean,
        "gain_vs_fixed": CAP - mean,
        "misses": sorted(k for k, v in res.items() if v["miss"]),
        "materially_smaller": sum(1 for v in res.values() if v["budget"] <= CAP - MATERIAL),
    }


def run(path=RESULTS):
    rows = load(path)
    report = {"candidate": CANDIDATE, "rows": rows, "rules": {}}
    for name, (rule, inp) in RULES.items():
        res = lolo(rows, rule)
        report["rules"][name] = {"label_derived_input": inp, "per_lineage": res, "summary": summarize(res)}
    report["max_headroom_lineage"] = min(rows, key=lambda k: CAP - rows[k]["full"])
    report["min_headroom"] = CAP - rows[report["max_headroom_lineage"]]["full"]
    return report


def svg(rows):
    w, h, m = 360, 300, 40
    sx = lambda x: m + x * (w - 2 * m) / 0.5
    sy = lambda y: h - m - y * (h - 2 * m) / 0.7
    parts = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" font-family="sans-serif" font-size="10">',
             f'<rect width="{w}" height="{h}" fill="white"/>',
             f'<line x1="{m}" y1="{sy(CAP):.1f}" x2="{w-m}" y2="{sy(CAP):.1f}" stroke="#c00" stroke-dasharray="4"/>',
             f'<text x="{m+2}" y="{sy(CAP)-3:.1f}" fill="#c00">fixed 0.65</text>',
             f'<line x1="{m}" y1="{h-m}" x2="{w-m}" y2="{h-m}" stroke="black"/><line x1="{m}" y1="{m}" x2="{m}" y2="{h-m}" stroke="black"/>',
             f'<text x="{w/2-60:.0f}" y="{h-8}">true code-cell fraction (label oracle)</text>',
             f'<text x="4" y="{m-10}">full-recall retention</text>']
    for k, r in rows.items():
        parts.append(f'<circle cx="{sx(r["pos"]):.1f}" cy="{sy(r["full"]):.1f}" r="3" fill="#06c"/>')
        parts.append(f'<text x="{sx(r["pos"])+5:.1f}" y="{sy(r["full"])+3:.1f}">{k}</text>')
    parts.append("</svg>")
    return "\n".join(parts) + "\n"


def table(report):
    rows = report["rows"]
    lines = ["| lineage | cells | code fraction (oracle) | r90 | r99 | full recall | full/code | seed pstdev |", "|---|---|---|---|---|---|---|---|"]
    for k, r in rows.items():
        lines.append(f"| {k} | {r['cells']} | {r['pos']:.3f} | {r['r90']:.3f} | {r['r99']:.3f} | {r['full']:.3f} | {r['full']/r['pos']:.2f} | {r['seed_std']:.3f} |")
    lines += ["", "| LOLO rule (oracle input) | mean budget | gain vs 0.65 | smaller by >=10 pts | misses |", "|---|---|---|---|---|"]
    for name, v in report["rules"].items():
        s = v["summary"]
        lines.append(f"| {name} ({v['label_derived_input']}) | {s['mean_budget']:.3f} | {s['gain_vs_fixed']:+.3f} | {s['materially_smaller']} | {', '.join(s['misses']) or 'none'} |")
    return "\n".join(lines)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default=str(RESULTS))
    ap.add_argument("--svg")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args(argv)
    rep = run(a.results)
    if a.svg:
        pathlib.Path(a.svg).write_text(svg(rep["rows"]))
    print(json.dumps(rep, indent=1, sort_keys=True) if a.json else table(rep))
    return 0


if __name__ == "__main__":
    sys.exit(main())
