#!/usr/bin/env python3
"""SEG-086-T002 (ADR 0195/0196, checkpoint C2): label-free per-image score normalisation and the normalised dynamic-score selector (OFFLINE research code; no production route).

Normalisers (predeclared in ADR 0195; they see ONE image's model-A scores and nothing else):
  N1  z = (s - median(S)) / IQR(S)          robust location and scale; fails closed when IQR(S) < IQR_FLOOR (flat or plateau-only distributions)
  N2  z = s - Q10(S)                        plateau-anchored location shift, unit (shared logit) scale
Both are deterministic, monotone non-decreasing in s (within-image ranking is preserved), use linear-interpolated quantiles of the sorted scores, need no labels, no ROM
identity, no era and no per-game parameter, and are NOT probabilities.

Selector `norm_select` (same contract as SEG-085 `dyn_select`, with normalisation as an extra, separately counted outcome):
  1. reject non-finite / non-numeric scores or an empty image (status `rejected`) and an image without an independently established seed universe (`unsupported`);
  2. normalise; failure (too few cells, scale below the floor, non-finite z) -> `normalisation_failed` (broad AOT; never a truncated selection);
  3. select cells with z >= tau (ties inclusive: order independent) and union the seed cells;
  4. retained fraction above CAP -> `abstain_cap` (broad AOT, never a truncated region).
Validator rejection is a downstream event and is never produced here.
"""
from __future__ import annotations

import math
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_ml_v3 as v3  # noqa: E402
import segarecomp_seg085_dyn as dyn  # noqa: E402

SCHEMA = "segarecomp.seg086.norm.v1"
CAP = v3.MAX_RETENTION                      # 0.60, frozen
MIN_CELLS = 64
IQR_FLOOR = 0.5                             # logits; below this the N1 scale is not identifiable
METHODS = ("N1", "N2")


def quantile(sorted_vals, q: float) -> float:
    """Linear-interpolated quantile of an ascending list. Pure and deterministic."""
    n = len(sorted_vals)
    if n == 0:
        raise ValueError("empty")
    pos = q * (n - 1)
    lo = int(math.floor(pos))
    hi = min(lo + 1, n - 1)
    return sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * (pos - lo)


def normalise(scores, method: str) -> dict:
    """{status: ok, z, location, scale} or {status: failed, reason}. Non-finite input raises ValueError (the caller maps it to `rejected`)."""
    if method not in METHODS:
        raise ValueError(f"unknown normaliser {method}")
    s = dyn.finite_scores(scores)
    n = len(s)
    if n < MIN_CELLS:
        return {"status": "failed", "reason": "too_few_cells"}
    srt = sorted(s)
    if method == "N1":
        loc = quantile(srt, 0.5)
        scale = quantile(srt, 0.75) - quantile(srt, 0.25)
        if not (scale >= IQR_FLOOR):
            return {"status": "failed", "reason": "scale_below_floor", "location": loc, "scale": scale}
    else:
        loc, scale = quantile(srt, 0.10), 1.0
    z = [(x - loc) / scale for x in s]
    if not all(math.isfinite(x) for x in z):
        return {"status": "failed", "reason": "non_finite_normalised_score"}
    return {"status": "ok", "z": z, "location": loc, "scale": scale}


def norm_select(scores, seed_cells, tau: float, method: str, cap: float = CAP, have_universe: bool = True) -> dict:
    """Normalised dynamic-score selection for one image. Returns {status, region (cell set), retained, ...}; pure and deterministic."""
    if not have_universe:
        return {"status": "unsupported", "region": set(), "retained": None, "reason": "no_seed_universe"}
    try:
        s = dyn.finite_scores(scores)
    except ValueError:
        return {"status": "rejected", "region": set(), "retained": None, "reason": "non_finite_score"}
    n = len(s)
    if n == 0 or not math.isfinite(tau):
        return {"status": "rejected", "region": set(), "retained": None, "reason": "empty_or_bad_threshold"}
    nz = normalise(s, method)
    if nz["status"] != "ok":
        return {"status": "normalisation_failed", "region": set(range(n)), "retained": 1.0, "reason": nz["reason"]}
    z = nz["z"]
    scored = {c for c in range(n) if z[c] >= tau}
    seeds = {c for c in seed_cells if 0 <= c < n}
    region = scored | seeds
    retained = len(region) / n
    if retained > cap + 1e-12:
        return {"status": "abstain_cap", "region": set(range(n)), "retained": 1.0, "cells_before_seeds": len(scored), "selected_fraction": retained,
                "location": nz["location"], "scale": nz["scale"]}
    return {"status": "selected", "region": region, "retained": retained, "cells_before_seeds": len(scored), "seed_only_cells": len(seeds - scored),
            "location": nz["location"], "scale": nz["scale"]}


def weakest_positive_z(z, labels, seeds):
    """Lowest normalised score among verified positive non-seed cells (None if there is none)."""
    vals = [float(v) for c, (v, y) in enumerate(zip(z, labels)) if y == 1 and c not in seeds]
    return min(vals) if vals else None
