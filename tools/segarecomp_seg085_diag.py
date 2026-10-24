#!/usr/bin/env python3
"""SEG-085-T001 (ADR 0191): error taxonomy and evaluation-distribution assessment of the structural model A, plus the SEG-072 ADAPT reproduction.

OFFLINE, PRIVATE research code. Nothing here fits, tunes or retrains a model: every score is a stored SEG-084 (A0 fold) or SEG-076 (A.C0.all) score, and the ADAPT
reproduction re-evaluates the stored SEG-072 scores with the unchanged `segarecomp_ml_v3` policy code. Durable output is aggregate counts and ratios only.

Commands:
  taxonomy     per image / lineage / category error taxonomy of model A (Genesis out-of-fold + the cross-platform transfer scores)
  adapt-repro  reproduce the SEG-072 M3-R / frozen-M3 ADAPT development numbers from the stored scores
Each taxonomy row also carries the label-reliability audit (direct-control-target contradictions of the labelled starts; no model involved).
"""
from __future__ import annotations

import argparse
import json
import math
import os
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_ml_v3 as v3  # noqa: E402

SCHEMA = "segarecomp.seg085.diag.v1"
RESULTS = HERE / "segarecomp_seg085_diag.results.json"
HARNESS = pathlib.Path(os.environ.get("SEGARECOMP_HARNESS_ROOT", "/root/dev/segarecomp-harness"))
CELL = v3.CELL_BYTES

# Category of every lineage and the evidence it rests on (ADR 0152 / 0159 / 0161; not inferred from a title or platform).
CATEGORIES = {
    "historical_genesis": "commercial-era Genesis, community reconstruction (build-identical rebuild; boundaries by the reconstruction authors)",
    "modern_sgdk": "modern SGDK/GCC homebrew (author builds; GCC-listing / re-assembly truth)",
    "xplat_period": "cross-platform M68K, period toolchain or period source",
    "xplat_modern": "cross-platform M68K, modern toolchain or modern authored source",
    "xplat_diagnostic": "tiny diagnostic probes",
}
LINEAGES = {
    "D01": ("historical_genesis", "Sonic 1/2/S&K (1991-94) + 36 Sonic CD (1993) modules; Sega hand assembly; AS / asl rebuilds, listing semantics", "one engine family"),
    "D02": ("historical_genesis", "Flicky (Genesis 1991); Sega hand assembly; AS asl rebuild", ""),
    "D03": ("historical_genesis", "Shining Force I/II (1992/93); SN 68k 2.53 rebuild; data split of the authorized image by the reconstruction authors", "boundaries from a community data split"),
    "D04": ("historical_genesis", "Phantasy Star II (1989); AS Bld 89 rebuild", ""),
    "D05": ("modern_sgdk", "Genesis 2048, SGDK 1.62 / GCC", ""),
    "D06": ("modern_sgdk", "TetrisMD, SGDK 1.41 / GCC", ""),
    "D07": ("modern_sgdk", "240p Test Suite (SGDK 1.11 fork, GCC 3.4.6, non-LTO)", ""),
    "D09": ("modern_sgdk", "MegaPong, SGDK 1.34 / GCC", ""),
    "D10": ("modern_sgdk", "Genesis Megatetris, SGDK 1.90 / GCC", ""),
    "D11": ("modern_sgdk", "Penguin World, SGDK 1.70 / GCC", ""),
    "D12": ("modern_sgdk", "MegaDriving, SGDK 2.11 / GCC 13.2", ""),
    "BattleIsleEditor": ("xplat_period", "Amiga; the author's 1991 source, A68K 2.71 + vlink, executables byte-identical", "authentic corroboration"),
    "EmuTOS": ("xplat_modern", "Atari ST/Amiga firmware reimplementation; GCC 4.6.4 + gas; reproducibility observed, not pinned", "authored 2001+"),
    "amiga-realtime3d": ("xplat_modern", "Amiga programs, vasm, CC BY-SA 4.0 repository", "modern authored"),
    "toslibc": ("xplat_modern", "Atari ST C library and examples, m68k-elf GCC 13.3", "modern toolchain"),
    "HelloAmi": ("xplat_diagnostic", "Amiga vasm units, tiny", ""),
}
GENESIS = ("D01", "D02", "D03", "D04", "D05", "D06", "D07", "D09", "D10", "D11", "D12")
XPLAT = ("BattleIsleEditor", "EmuTOS", "amiga-realtime3d", "toslibc", "HelloAmi")
TOKEN_NF = 8
DYNAMIC = (4, 5)          # token control: dynamic jump, dynamic call
# Direct-flow target words that are not an instruction-start contradiction: unknown bytes are never evidence.


def np_():
    import numpy
    return numpy


def r6(x):
    return None if x is None else round(float(x), 6)


# ------------------------------------------------------------------------------------------------------------------------ pure metrics
def quantiles(values, qs):
    """Nearest-rank quantiles of a numeric list (None when empty). Pure."""
    v = sorted(float(x) for x in values)
    if not v:
        return {f"p{int(q * 100):02d}": None for q in qs}
    return {f"p{int(q * 100):02d}": r6(v[min(len(v) - 1, max(0, int(round(q * (len(v) - 1)))))]) for q in qs}


def cell_stats(classes: bytes, labels, cell: int = CELL):
    """Per-cell (starts, data bytes, code bytes) from the byte-class string. Pure."""
    n = len(labels)
    starts, data, code = [0] * n, [0] * n, [0] * n
    for c in range(n):
        chunk = classes[c * cell:(c + 1) * cell]
        starts[c] = chunk.count(b"I")
        data[c] = chunk.count(b"D")
        code[c] = starts[c] + chunk.count(b"c")
    return starts, data, code


def image_taxonomy(classes: bytes, seed_addresses, scores, struct, tokens, cell: int = CELL) -> dict:
    """Aggregate error taxonomy of one image under one score vector (pure given numbers; unit-tested on synthetic arrays)."""
    labels = v3.cell_labels(classes, cell)
    n = len(labels)
    seeds = {a // cell for a in seed_addresses if 0 <= a < len(classes)}
    s = [float(x) for x in scores]
    if len(s) != n:
        raise SystemExit(f"score length {len(s)} != cells {n}")
    starts_per, data_per, code_per = cell_stats(classes, labels, cell)
    pos = [c for c in range(n) if labels[c] == 1]
    neg = [c for c in range(n) if labels[c] == 0]
    masked = n - len(pos) - len(neg)
    start_addrs = v3.starts_of(classes)
    reach = struct["reach"]
    unreached = sum(1 for a in start_addrs if not reach[a // 2])
    order = v3.rank_order(s)
    rank = [0] * n
    for k, c in enumerate(order):
        rank[c] = k
    regions = {pol: v3.policy_region(pol, s, None, seeds)["region"] for pol in ("B35", "B50")}
    pos_set = set(pos)
    mixed = [c for c in pos if data_per[c] > 0]
    dyn_cells = {c for c in range(n) if _has_dynamic(tokens, c, cell)}
    near_dyn = {c for c in range(n) if dyn_cells & {c - 2, c - 1, c, c + 1, c + 2}}
    out = {
        "cells": n, "positive_cells": len(pos), "negative_cells": len(neg), "masked_cells": masked, "starts": len(start_addrs),
        "density_labelled": r6(len(pos) / max(1, len(pos) + len(neg))), "density_all_cells": r6(len(pos) / n),
        "oracle_fraction": r6((len(pos_set | seeds)) / n),               # an ideal ranker still retains this fraction at full recall
        "oracle_fraction_if_masked_is_code": r6((len(pos_set | seeds | {c for c in range(n) if labels[c] < 0})) / n),   # upper bound: every masked cell is real code
        "masked_fraction": r6(masked / n),
        "seed_cells": len(seeds), "seed_positive_share": r6(len(seeds & pos_set) / max(1, len(pos))),
        "starts_unreached_by_direct_flow": r6(unreached / max(1, len(start_addrs))),
        "positive_cells_mixed_with_data": len(mixed), "mixed_share": r6(len(mixed) / max(1, len(pos))),
        "dynamic_control_cells": len(dyn_cells), "positive_cells_near_dynamic_control": len(pos_set & near_dyn),
        "short_positive_cells": sum(1 for c in pos if starts_per[c] <= 8),
    }
    for pol, region in regions.items():
        missed = [c for c in pos if c not in region]
        miss_starts = sum(starts_per[c] for c in missed)
        mset = set(missed)
        out[pol] = {
            "retained": r6(len(region) / n), "oracle_feasible": bool(out["oracle_fraction"] <= len(region) / n + 1e-12),
            "missed_cells": len(missed), "missed_starts": miss_starts,
            "mechanism": {
                "mixed_with_data": sum(1 for c in mset if data_per[c] > 0),
                "executable_boundary_adjacent": sum(1 for c in mset if (c > 0 and (c - 1) in region and labels[c - 1] == 1) or (c + 1 < n and (c + 1) in region and labels[c + 1] == 1)),
                "short_run": sum(1 for c in mset if starts_per[c] <= 8),
                "unreached_by_direct_flow": sum(1 for c in mset if any(not reach[a // 2] for a in range(c * cell, min((c + 1) * cell, len(classes)), 2) if classes[a:a + 1] == b"I")),
                "near_dynamic_control": sum(1 for c in mset if c in near_dyn),
            },
            "negative_admitted": sum(1 for c in region if labels[c] == 0), "masked_admitted": sum(1 for c in region if labels[c] < 0),
        }
    # budget-independent hard tail: non-seed positive cells ranked beyond the first quarter of the image's cells
    tail = [c for c in pos if c not in seeds and rank[c] >= n // 4]
    out["tail25"] = {"cells": len(tail), "mixed_with_data": sum(1 for c in tail if data_per[c] > 0),
                     "executable_boundary_adjacent": sum(1 for c in tail if (c > 0 and labels[c - 1] == 1) or (c + 1 < n and labels[c + 1] == 1)),
                     "short_run": sum(1 for c in tail if starts_per[c] <= 8), "near_dynamic_control": sum(1 for c in tail if c in near_dyn),
                     "unreached_by_direct_flow": sum(1 for c in tail if any(not reach[a // 2] for a in range(c * cell, min((c + 1) * cell, len(classes)), 2) if classes[a:a + 1] == b"I"))}
    pos_scores = [s[c] for c in pos if c not in seeds]
    neg_scores = [s[c] for c in neg]
    out["score"] = {"positive_non_seed": quantiles(pos_scores, (0.0, 0.01, 0.05, 0.5)), "negative": quantiles(neg_scores, (0.5, 0.95, 0.99, 1.0)),
                    "worst_positive_rank_fraction": r6(max((rank[c] for c in pos if c not in seeds), default=0) / n)}
    # density estimate used by SEG-072 ADAPT: fraction of cells with p >= 0.5 (balanced classifier -> over-estimates density)
    out["adapt_density_estimate"] = r6(sum(1 for x in s if 1.0 / (1.0 + math.exp(-max(-700.0, min(700.0, x)))) >= 0.5) / n)
    out["frf"] = r6(v3.frf(s, labels)["frf"])
    out["residual_frf"] = r6(v3.residual_frf(s, labels, seeds))
    return out


def _has_dynamic(tokens, c: int, cell: int) -> bool:
    w0, w1 = c * (cell // 2), (c + 1) * (cell // 2)
    for w in range(w0, min(w1, len(tokens))):
        if tokens[w][0] == 2 and tokens[w][3] in DYNAMIC:
            return True
    return False


def target_audit(classes: bytes, struct, nwords: int) -> dict:
    """Label contradictions: the direct control-flow target of each labelled start against the labels (no model involved). A target landing on a
    labelled instruction start is consistent; on unknown (U) or outside the image it is not evidence; on labelled data (D) or inside an instruction
    (continuation c) it contradicts the labels. Pure given arrays."""
    target = struct["target"]
    ok = unknown = on_data = inside = n = 0
    for a in v3.starts_of(classes):
        t = int(target[a // 2])
        if t < 0 or t >= nwords:
            continue
        n += 1
        b = classes[t * 2:t * 2 + 1]
        if b == b"I":
            ok += 1
        elif b == b"U":
            unknown += 1
        elif b == b"D":
            on_data += 1
        elif b == b"c":
            inside += 1
    return {"direct_targets": n, "on_start": ok, "on_unknown": unknown, "on_labelled_data": on_data, "inside_instruction": inside,
            "contradiction_rate": r6((on_data + inside) / n) if n else None}


def tokens_array(im):
    return im.tokens.reshape(-1, TOKEN_NF)


def aggregate(rows):
    """Per-group aggregation of taxonomy rows: sums and per-lineage means (a lineage is one unit, never an image count)."""
    by = {}
    for r in rows:
        by.setdefault(r["lineage"], []).append(r)
    out = {}
    for L, rs in by.items():
        t = lambda k: sum(r[k] for r in rs)           # noqa: E731
        pos = t("positive_cells")
        out[L] = {"images": len(rs), "cells": t("cells"), "positive_cells": pos, "starts": t("starts"),
                  "density_labelled": r6(pos / max(1, pos + t("negative_cells"))),
                  "mixed_share": r6(t("positive_cells_mixed_with_data") / max(1, pos)),
                  "B35_missed_starts": sum(r["B35"]["missed_starts"] for r in rs), "B50_missed_starts": sum(r["B50"]["missed_starts"] for r in rs),
                  "B35_images_missing": sum(1 for r in rs if r["B35"]["missed_starts"]), "B50_images_missing": sum(1 for r in rs if r["B50"]["missed_starts"]),
                  "B35_oracle_infeasible_images": sum(1 for r in rs if not r["B35"]["oracle_feasible"]),
                  "mean_er": r6(sum(r["er"] for r in rs if r["er"] is not None) / max(1, sum(1 for r in rs if r["er"] is not None))),
                  "max_er": r6(max((r["er"] for r in rs if r["er"] is not None), default=None))}
    return out


# ------------------------------------------------------------------------------------------------------------------------ loading
def genesis_images():
    import segarecomp_seg084_a as s84
    imgs, _ = s84.load_registry_images()
    return s84, imgs


def cmd_taxonomy(a) -> int:
    import numpy as np
    import segarecomp_xplat_models as xm
    import segarecomp_xplat_explore_eval as ev6
    import segarecomp_xplat_transfer as xt
    s84, imgs = genesis_images()
    rows = []
    for L in GENESIS:
        sc = xt.load_npz(s84.score_path(L, "A0"))
        for im in [i for i in imgs if i.lineage == L]:
            m = ev6.image_metrics_ext(im.classes, im.seed_addresses, sc[im.id])
            t = image_taxonomy(im.classes, im.seed_addresses, sc[im.id], xm.structure(im), tokens_array(im))
            t.update({"id": im.id, "lineage": L, "category": LINEAGES[L][0], "role": "genesis_oof", "er": r6(m["er"]), "auroc": r6(m["auroc"]),
                      "size": im.size, "audit": target_audit(im.classes, xm.structure(im), im.n_words)})
            rows.append(t)
    # cross-platform transfer scores (Genesis-only model A fitted on all eleven lineages: SEG-076 condition C0 'all'); transfer, not out-of-fold
    snap = HARNESS / ".cache" / "seg075" / "t003" / "snapshot"
    xrows = []
    xs = HARNESS / ".cache" / "seg076" / "t001" / "scores" / "A.C0.all.xplat.npz"
    if snap.is_dir() and xs.is_file():
        import segarecomp_xplat_t005 as x5
        dirs = xt.xplat_dirs(snap, XPLAT)
        xsc = xt.load_npz(xs)
        for im, d in zip(xt.load(dirs), dirs):
            im = x5.attach(im, d)
            if im.id not in xsc:
                continue
            m = ev6.image_metrics_ext(im.classes, im.seed_addresses, xsc[im.id])
            t = image_taxonomy(im.classes, im.seed_addresses, xsc[im.id], xm.structure(im), tokens_array(im))
            t.update({"id": im.id, "lineage": im.lineage, "category": LINEAGES[im.lineage][0], "role": "xplat_transfer", "er": r6(m["er"]) if m["er"] is not None else None,
                      "auroc": r6(m["auroc"]), "size": im.size, "audit": target_audit(im.classes, xm.structure(im), im.n_words)})
            xrows.append(t)
    rows_all = rows + xrows
    cat = {}
    audit_tot = {}
    for r in rows_all:
        t = audit_tot.setdefault(r["category"], {"direct_targets": 0, "on_start": 0, "on_unknown": 0, "on_labelled_data": 0, "inside_instruction": 0})
        for k in t:
            t[k] += r["audit"][k]
    for c in CATEGORIES:
        sel = [r for r in rows_all if r["category"] == c]
        if sel:
            lin = aggregate(sel)
            cat[c] = {"definition": CATEGORIES[c], "lineages": lin, "n_lineages": len(lin), "n_images": len(sel),
                      "lineage_mean_er": r6(sum(v["mean_er"] for v in lin.values() if v["mean_er"] is not None) / max(1, len(lin))),
                      "lineage_mean_max_er": r6(sum(v["max_er"] for v in lin.values() if v["max_er"] is not None) / max(1, sum(1 for v in lin.values() if v["max_er"] is not None))),
                      "positive_cell_share_of_corpus": r6(sum(v["positive_cells"] for v in lin.values()) / max(1, sum(r["positive_cells"] for r in rows_all))),
                      "B35_missed_starts": sum(v["B35_missed_starts"] for v in lin.values()), "B50_missed_starts": sum(v["B50_missed_starts"] for v in lin.values()),
                      "label_audit": {**audit_tot[c], "contradiction_rate": r6((audit_tot[c]["on_labelled_data"] + audit_tot[c]["inside_instruction"]) / max(1, audit_tot[c]["direct_targets"]))},
                      "lineages_with_B35_miss": sorted(L for L, v in lin.items() if v["B35_missed_starts"]),
                      "lineages_with_B50_miss": sorted(L for L, v in lin.items() if v["B50_missed_starts"])}
    total_b35 = sum(r["B35"]["missed_starts"] for r in rows_all)
    d07 = sum(r["B35"]["missed_starts"] for r in rows_all if r["lineage"] == "D07")
    out = {"schema": SCHEMA + ".taxonomy", "lineage_table": {L: {"category": v[0], "evidence": v[1], "note": v[2]} for L, v in LINEAGES.items()},
           "categories": cat, "images": rows_all,
           "d07": {"share_of_all_B35_missed_starts": r6(d07 / total_b35) if total_b35 else None, "total_B35_missed_starts": total_b35, "d07_B35_missed_starts": d07},
           "scope_note": "Genesis rows are out-of-fold (strict leave-one-lineage-out, SEG-084 A0 = SEG-076 model A, bitwise); cross-platform rows are transfer scores of the "
                         "all-Genesis model A and are never out-of-fold; no row is a held-out independent evaluation (those are SEG-085-T002/T004)"}
    pathlib.Path(a.out).write_text(json.dumps(out, indent=1, sort_keys=True))
    print(json.dumps({"images": len(rows_all), "categories": {c: (v["n_lineages"], v["n_images"], v["B35_missed_starts"], v["B50_missed_starts"]) for c, v in cat.items()},
                      "d07_share": out["d07"]}))
    return 0


def cmd_adapt_repro(a) -> int:
    """Reproduce the SEG-072 ADAPT development results from the stored scores with the unchanged policy code and compare them with the committed record."""
    import segarecomp_ml_v3_eval as ev
    reg = ev.registry()
    import segarecomp_ml_v3_train as tr
    folds = v3.lolo_folds(tr.qualified_lineages())
    committed = json.loads((HERE / "segarecomp_ml_v3.dev_results.json").read_text())
    out = {"schema": SCHEMA + ".adapt_repro", "configs": {}}
    for cfg in a.configs:
        res = ev.evaluate_config(cfg, reg, folds)
        pol = {p: v for p, v in res["pairs"].items()}
        com = committed["selection"]["table"].get(f"{cfg}+ADAPT") or {}
        ok = bool(com) and abs(pol["ADAPT"]["mean_retained"] - com["mean_retained"]) < 1e-9 and pol["ADAPT"]["misses"] == com["misses"]
        out["configs"][cfg] = {"pairs": {p: {k: (r6(x) if isinstance(x, float) else x) for k, x in v.items()} for p, v in pol.items()}, "margins": {k: r6(x) for k, x in res["margins"].items()},
                               "committed_ADAPT": com, "reproduces_committed": ok,
                               "per_lineage_ADAPT": {L: v["policies"]["ADAPT"] for L, v in res["lineage"].items()},
                               "abstentions": sum(1 for r in res["records"] if r["policies"]["ADAPT"]["abstained"]),
                               "density_estimate_vs_labelled_density": {r["id"]: [r6(r["density_e"]), r6(r["positive_cells"] / r["cells"])] for r in res["records"] if r["cells"] >= 1000}}
        print(cfg, json.dumps({"ADAPT": out["configs"][cfg]["pairs"]["ADAPT"], "reproduces": ok}))
    pathlib.Path(a.out).write_text(json.dumps(out, indent=1, sort_keys=True))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("taxonomy")
    t.add_argument("--out", default=str(HERE / "segarecomp_seg085_diag.taxonomy.json"))
    t.set_defaults(func=cmd_taxonomy)
    r = sub.add_parser("adapt-repro")
    r.add_argument("--configs", nargs="+", default=["M3_R", "M3_FROZEN", "M1_HGB"])
    r.add_argument("--out", default=str(HERE / "segarecomp_seg085_diag.adapt.json"))
    r.set_defaults(func=cmd_adapt_repro)
    a = ap.parse_args(argv)
    return a.func(a)


if __name__ == "__main__":
    raise SystemExit(main())
