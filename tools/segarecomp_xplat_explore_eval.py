#!/usr/bin/env python3
"""SEG-076 (exploratory, development-only): pure evaluation, decision and barrier helpers for the SEG-076 research runners.

  sealed_target       fail-closed sealed-subtree resolver: both slash conventions, lexical '..' resolution, case-folded names, and the
                      resolved target when it exists (the SEG-075 frozen guard only parsed POSIX separators and resolved existing paths)
  image_metrics_ext   SEG-075's unchanged per-image metrics (segarecomp_xplat_transfer.image_metrics) plus SEG-076's boundary strata:
                      positive cells holding any D byte, positive cells adjacent to a D-only cell, positive cells with <= 8 starts; for
                      each, missed cells at B35/B50 and the normalized rank distribution
  lineage_summary     per-lineage rows (SEG-075 max ER definition kept) plus mean image ER, mean FRF and stratum rank quantiles
  expansion_class     the T001 pre-registered expansion rule (EXPAND_B / EXPAND_C / EXPAND_B_AND_C / NEGATIVE_STOP)

Everything here is pure over numbers / bytes (unit-tested on synthetic data). No address, image content or per-cell value leaves it:
results are counts, fractions and quantiles.
"""
from __future__ import annotations

import ntpath
import pathlib
import posixpath
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

POLICIES = ("B35", "B50", "B60")
STRATA = ("d_byte", "adjacent_d_only", "short_run", "boundary")
SHORT_RUN_STARTS = 8


# ------------------------------------------------------------------------------------------------------------------------ barrier
def _components(text: str) -> list[str]:
    """Lexical path components under BOTH conventions: drive / UNC prefixes dropped, '.' removed, '..' applied, names case-folded."""
    s = text.replace("\\", "/")
    _drive, rest = ntpath.splitdrive(s)
    norm = posixpath.normpath(rest) if rest else ""
    return [c.casefold() for c in norm.split("/") if c not in ("", ".", "..")]


def sealed_target(path, reserved: set[str], sealed_dirname: str = "sealed") -> bool:
    """True when `path` names (or resolves into) the sealed subtree or a reserved lineage/sample, whether or not it exists.
    Fails closed: any component equal to the sealed directory name or a reserved id (before '#'/'@' sample suffixes) counts, also for
    nonexistent Windows-style paths. A path whose existence cannot be determined is judged on its lexical components only."""
    names = set()
    text = str(path)
    for comp in _components(text):
        names.add(comp)
        names.add(comp.split("#", 1)[0].split("@", 1)[0])
    try:
        p = pathlib.Path(text)
        if p.exists() or p.is_symlink():
            for comp in p.resolve().parts:
                c = comp.casefold()
                names.add(c)
                names.add(c.split("#", 1)[0].split("@", 1)[0])
    except (OSError, ValueError):
        pass
    bad = {sealed_dirname.casefold()} | {r.casefold() for r in reserved}
    return bool(names & bad)


def refuse(paths, reserved: set[str], sealed_dirname: str = "sealed") -> None:
    for p in paths:
        if sealed_target(p, reserved, sealed_dirname):
            raise SystemExit(f"evaluation barrier: sealed/reserved material refused: {p}")


# ------------------------------------------------------------------------------------------------------------------------ metrics
def boundary_strata(classes: bytes, labels: list[int], cell: int = 128) -> dict[str, set[int]]:
    """Positive-cell strata near instruction/data boundaries (cell indices; runtime only, never persisted)."""
    d_byte, adj, short = set(), set(), set()
    n = len(labels)
    for c in range(n):
        if labels[c] != 1:
            continue
        chunk = classes[c * cell:(c + 1) * cell]
        if b"D" in chunk:
            d_byte.add(c)
        if (c > 0 and labels[c - 1] == 0) or (c + 1 < n and labels[c + 1] == 0):
            adj.add(c)
        if chunk.count(b"I") <= SHORT_RUN_STARTS:
            short.add(c)
    return {"d_byte": d_byte, "adjacent_d_only": adj, "short_run": short, "boundary": d_byte | adj | short}


def image_metrics_ext(classes: bytes, seed_addresses, scores, cell: int = 128) -> dict:
    import segarecomp_ml_v3 as v3
    import segarecomp_xplat_transfer as xt
    out = xt.image_metrics(classes, seed_addresses, scores, cell)
    labels = v3.cell_labels(classes, cell)
    s = [float(x) for x in scores]
    seeds = {a // cell for a in seed_addresses if 0 <= a < len(classes)}
    order = v3.rank_order(s)
    rank = [0] * len(s)
    for k, c in enumerate(order):
        rank[c] = k
    regions = {pol: v3.policy_region(pol, s, None, seeds)["region"] for pol in ("B35", "B50")}
    strata = boundary_strata(classes, labels, cell)
    n = max(1, len(s))
    out["strata"] = {}
    for name, cells in strata.items():
        out["strata"][name] = {"cells": len(cells), **{f"missed_{pol}": sum(1 for c in cells if c not in regions[pol]) for pol in regions},
                               "ranks": sorted(rank[c] / n for c in cells)}
    return out


def quantiles(values: list[float], qs=(0.5, 0.9, 1.0)) -> dict:
    if not values:
        return {f"q{int(q * 100)}": None for q in qs}
    v = sorted(values)
    return {f"q{int(q * 100)}": round(v[min(len(v) - 1, max(0, int(round(q * (len(v) - 1)))))], 6) for q in qs}


def lineage_summary(per_image: dict, lineage_of: dict) -> dict:
    """SEG-075 lineage rows (max ER over informative images etc.) extended with mean image ER, mean FRF and stratum aggregates."""
    import segarecomp_xplat_transfer as xt
    rows = xt.lineage_rows({k: {kk: vv for kk, vv in m.items() if kk != "strata"} for k, m in per_image.items()}, lineage_of)
    ers, frfs, strata = {}, {}, {}
    for iid, m in per_image.items():
        L = lineage_of[iid]
        if m["informative"]:
            ers.setdefault(L, []).append(m["er"])
        frfs.setdefault(L, []).append(m["frf"])
        st = strata.setdefault(L, {s: {"cells": 0, "missed_B35": 0, "missed_B50": 0, "ranks": []} for s in STRATA})
        for s in STRATA:
            x = m["strata"][s]
            st[s]["cells"] += x["cells"]
            st[s]["missed_B35"] += x["missed_B35"]
            st[s]["missed_B50"] += x["missed_B50"]
            st[s]["ranks"].extend(x["ranks"])
    for L, r in rows.items():
        e = ers.get(L, [])
        r["mean_image_er"] = round(sum(e) / len(e), 6) if e else None
        r["informative_images"] = len(e)
        r["mean_frf"] = round(sum(frfs[L]) / len(frfs[L]), 6)
        r["strata"] = {s: {"cells": v["cells"], "missed_B35": v["missed_B35"], "missed_B50": v["missed_B50"], "rank": quantiles(v["ranks"])}
                       for s, v in strata[L].items()}
    return rows


def summary(rows: dict) -> dict:
    """Genesis-level aggregates of one configuration (mean ER = mean over lineages of the lineage max ER, SEG-075's definition)."""
    ers = [r["max_er"] for r in rows.values() if r["max_er"] is not None]
    mers = [r["mean_image_er"] for r in rows.values() if r.get("mean_image_er") is not None]
    out = {"lineages": len(rows), "mean_er": round(sum(ers) / len(ers), 6) if ers else None, "worst_er": round(max(ers), 6) if ers else None,
           "mean_of_mean_image_er": round(sum(mers) / len(mers), 6) if mers else None,
           "worst_frf": round(max(r["max_frf"] for r in rows.values()), 6) if rows else None}
    for pol in POLICIES:
        out[f"misses_{pol}"] = sum(r[f"misses_{pol}"] for r in rows.values())
        out[f"missed_cells_{pol}"] = sum(r[f"missed_cells_{pol}"] for r in rows.values())
        out[f"negative_admitted_{pol}"] = sum(r[f"negative_admitted_{pol}"] for r in rows.values())
    for s in STRATA:
        for pol in ("B35", "B50"):
            out[f"{s}_missed_{pol}"] = sum(r["strata"][s][f"missed_{pol}"] for r in rows.values()) if rows and "strata" in next(iter(rows.values())) else None
    au = [r["mean_auroc"] for r in rows.values() if r["mean_auroc"] is not None]
    out["mean_auroc"] = round(sum(au) / len(au), 6) if au else None
    return out


# ------------------------------------------------------------------------------------------------------------------------ decision
def expansion_class(configs: dict, rule: dict) -> dict:
    """T001's pre-registered expansion rule (pure; unit-tested).

    configs: {name: {"rows": {lineage: {max_er, ...}}, "summary": summary(rows)} or None when the fit set failed / is incomplete}.
    rule: {"candidates": ["B", "C"], "comparators": ["M4_DENSE_s1@128", "M3_R_s49", "A"], "mean_er_margin": 0.02,
           "lineage_tolerance": 0.05}.
    The best one-seed comparator is the available comparator with the lowest Genesis mean ER (ties: fewer B50 misses, then the listed
    order). A candidate qualifies iff its mean ER is lower by >= margin, no lineage is worse by more than the tolerance, and it has no
    more boundary-stratum missed cells at B50 and no more total B50 misses than that comparator. A missing / failed candidate never
    qualifies."""
    comps = [(c, configs.get(c)) for c in rule["comparators"]]
    comps = [(c, v) for c, v in comps if v is not None and v["summary"]["mean_er"] is not None]
    if not comps:
        return {"class": "NEGATIVE_STOP", "reason": "no comparator available", "best_comparator": None, "candidates": {}}
    order = {c: i for i, c in enumerate(rule["comparators"])}
    best_name, best = min(comps, key=lambda cv: (cv[1]["summary"]["mean_er"], cv[1]["summary"]["misses_B50"], order[cv[0]]))
    bs = best["summary"]
    res = {}
    for cand in rule["candidates"]:
        v = configs.get(cand)
        if v is None or v["summary"]["mean_er"] is None:
            res[cand] = {"qualifies": False, "reason": "missing or failed fits"}
            continue
        s = v["summary"]
        d_mean = bs["mean_er"] - s["mean_er"]
        worse = {L: round(r["max_er"] - best["rows"][L]["max_er"], 6) for L, r in v["rows"].items()
                 if L in best["rows"] and r["max_er"] is not None and best["rows"][L]["max_er"] is not None}
        checks = {"mean_er_advantage": round(d_mean, 6), "mean_er_ok": d_mean >= rule["mean_er_margin"] - 1e-12,
                  "worst_lineage_regression": max(worse.values()) if worse else 0.0,
                  "lineage_ok": all(x <= rule["lineage_tolerance"] + 1e-12 for x in worse.values()),
                  "boundary_missed_B50": s["boundary_missed_B50"], "comparator_boundary_missed_B50": bs["boundary_missed_B50"],
                  "boundary_ok": s["boundary_missed_B50"] <= bs["boundary_missed_B50"],
                  "misses_B50": s["misses_B50"], "comparator_misses_B50": bs["misses_B50"], "misses_ok": s["misses_B50"] <= bs["misses_B50"]}
        checks["qualifies"] = checks["mean_er_ok"] and checks["lineage_ok"] and checks["boundary_ok"] and checks["misses_ok"]
        res[cand] = checks
    q = [c for c in rule["candidates"] if res[c]["qualifies"]]
    cls = {(): "NEGATIVE_STOP", ("B",): "EXPAND_B", ("C",): "EXPAND_C", ("B", "C"): "EXPAND_B_AND_C"}[tuple(q)]
    return {"class": cls, "best_comparator": best_name, "best_comparator_mean_er": bs["mean_er"], "candidates": res}
