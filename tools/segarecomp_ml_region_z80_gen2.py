#!/usr/bin/env python3
"""SEG-056 (ADR 0117): stdlib-only definitions for the second-generation SMS Z80 executable-region experiment.

Offline research definitions (labels, structural roots, byte->cell aggregation, ranking, full-recall metrics with roots).
Shared with `segarecomp_ml_region_z80.py` (SEG-051, frozen history). numpy/PyTorch live only in the private drivers.
"""
from __future__ import annotations

import hashlib
import json
import math
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_ml_region_z80 as z1  # noqa: E402

POSITIVE, NEGATIVE, MASKED = z1.POSITIVE, z1.NEGATIVE, z1.MASKED
BYTE_POS, BYTE_NEG, BYTE_MASK = 1, 0, -1

# Architecture-defined Z80 entry points of a cartridge image. Offsets are image offsets: on the Sega mapper the first
# 1 KiB of ROM is always mapped at 0x0000 (it cannot be paged out), so image offset == CPU address for these three.
#   0x0000 reset; 0x0038 maskable interrupt (IM 1, and IM 0 on SMS because the open bus reads 0xFF = RST 38h);
#   0x0066 non-maskable interrupt (Pause button). No other vector (RST 08..30, image start data) is an entry.
STRUCTURAL_ROOT_ADDRESSES = (0x0000, 0x0038, 0x0066)


def root_cells(image_len: int, cell_bytes: int):
    """Decision cells that contain a structural entry address (independent of title, labels and observed PCs)."""
    return sorted({a // cell_bytes for a in STRUCTURAL_ROOT_ADDRESSES if a < image_len})


def byte_labels(classes: str):
    """Per-byte occupancy label: I and c (verified instruction byte) -> 1; D (confident data) -> 0; A, U, Q -> masked."""
    table = {"I": BYTE_POS, "c": BYTE_POS, "D": BYTE_NEG}
    return [table.get(ch, BYTE_MASK) for ch in classes]


def cell_labels(classes: str, cell_bytes: int):
    """Cell label: positive = holds an instruction start (I); negative = every byte D; else masked (== SEG-051 at 32)."""
    out = []
    n = (len(classes) + cell_bytes - 1) // cell_bytes
    for c in range(n):
        seg = classes[c * cell_bytes:(c + 1) * cell_bytes]
        out.append(POSITIVE if "I" in seg else (NEGATIVE if seg and set(seg) == {"D"} else MASKED))
    return out


def aggregate(byte_scores, cell_bytes: int, mode: str):
    """Deterministic byte-logit -> cell score reduction. mode: 'max' | 'mean' | 'top2' (mean of the two largest)."""
    n = (len(byte_scores) + cell_bytes - 1) // cell_bytes
    out = []
    for c in range(n):
        seg = byte_scores[c * cell_bytes:(c + 1) * cell_bytes]
        if mode == "max":
            out.append(max(seg))
        elif mode == "mean":
            out.append(sum(seg) / len(seg))
        elif mode == "top2":
            s = sorted(seg, reverse=True)[:2]
            out.append(sum(s) / len(s))
        else:
            raise ValueError("unknown aggregation")
    return out


def _cover_count(order, labels, roots, k):
    chosen = set(order[:k]) | set(roots)
    return len(chosen), sum(1 for i in chosen if labels[i] == POSITIVE)


def metrics(scores, labels, roots):
    """Full-recall metrics of R = roots UNION top-K ranked cells.

    frf: ranking cut that covers every positive cell with no roots; residual_frf: same with roots (positives that sit in
    a root cell need no rank); retained: |R| / cells at that cut (root overhead included); c_outside_r(b): uncovered
    positive cells at ranking budget b; recall budgets are retained fractions (roots included).
    """
    n = len(scores)
    order = z1.ranking(scores)
    rank_of = {c: r + 1 for r, c in enumerate(order)}
    pos = [i for i, l in enumerate(labels) if l == POSITIVE]
    rootset = set(roots)
    res_pos = [i for i in pos if i not in rootset]
    frf = max((rank_of[i] for i in pos), default=0) / n
    k_full = max((rank_of[i] for i in res_pos), default=0)
    out = {"cells": n, "positive_cells": len(pos), "positive_fraction": round(len(pos) / n, 4),
           "root_cells": len(rootset), "positives_in_roots": len(pos) - len(res_pos),
           "frf": round(frf, 4), "residual_frf": round(k_full / n, 4),
           "retained_full_recall": round(len(set(order[:k_full]) | rootset) / n, 4)}
    worst = sorted(res_pos, key=lambda i: -rank_of[i])[:4]
    out["worst_residual_rank_fractions"] = [round(rank_of[i] / n, 4) for i in worst]
    for q in (0.5, 0.9, 0.99, 0.999):
        need = math.ceil(q * len(pos) - 1e-9)
        lo = 0
        for k in range(0, n + 1):
            if _cover_count(order, labels, rootset, k)[1] >= need:
                lo = k
                break
        out[f"retained_for_recall_{q}"] = round(_cover_count(order, labels, rootset, lo)[0] / n, 4)
    return out


def missed_at_budget(scores, labels, roots, budget: float):
    """(uncovered positive cells, positives, retained fraction) for R = roots UNION top ceil(budget*n) ranked cells."""
    n = len(scores)
    order = z1.ranking(scores)
    k = min(n, max(1, math.ceil(budget * n - 1e-12)))
    chosen = set(order[:k]) | set(roots)
    pos = [i for i, l in enumerate(labels) if l == POSITIVE]
    return sum(1 for i in pos if i not in chosen), len(pos), len(chosen) / n


def canonical_digest(obj) -> str:
    return hashlib.sha256(json.dumps(obj, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
