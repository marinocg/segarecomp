#!/usr/bin/env python3
"""SEG-087-T001 (ADR 0199, checkpoint C1): low-tail executable-cell diagnosis and independent-witness coverage (OFFLINE research diagnostics).

Question: of the rare executable 128-byte cells that structural model A scores poorly (non-seed verified-positive cells, negative logit), how many are
independently indicated by existing deterministic, label-free structural evidence?

Nothing is fitted or tuned here. Inputs are the stored out-of-fold model-A scores (SEG-076/084), the SEG-085 inner-refit scores, the unchanged CPU decode tokens and the
development-image labels. Labels are used ONLY to describe the cells being diagnosed ("what is this cell?") and to evaluate witnesses ("is the witness right?"); every
witness definition below reads only tokens, image words, seed roots and relocation fields, i.e. it is computable on an unseen ROM.

Witnesses (all label-free):
  W_REACH_ROOTED     a word of the cell is statically reachable from the independently established seed roots by fall-through and direct (PC-relative / absolute) edges
                     over decoder-ordinary words. INDEPENDENT: it is rooted in seeds. It is already an input feature of model A (`reach`), so a cell that still scores
                     low despite this witness is a ranking limit of the model, not missing evidence.
  W_REACH_TARGET     a direct edge whose SOURCE word is seed-reached lands in the cell (subset of the above by construction; reported for completeness).
  W_RELOC            a trusted relocation field lies in the cell (no Genesis development image carries any; the field exists for relocatable images).
  W_VECTOR_ROOTED    a word of the cell is reachable (same edges) from the program-counter entries of the 68000 exception-vector table in the image header
                     (longwords at offsets 4..0xFF that are even and inside the image). Applied only to images that carry the cartridge header
                     ("SEGA" at 0x100); a CD module or relocatable blob yields none. Relocation offsets outside the image are ignored, never clamped. INDEPENDENT (header metadata, no labels); added after review because the
                     three unseeded images have no seed roots, so seed-rooted reach is undefined there.
  W_UNROOTED_TARGET  a direct edge from ANY decoder-ordinary word (reached or not) lands in the cell. NOT independent: arbitrary decodable bytes produce such edges.
  W_DECODE_CHAIN     a run of at least 8 consecutive decoder-ordinary words starts in the cell. NOT independent (the decoder accepts most data).
Only the first three can serve as a code root. The other two are measured so the report can show how non-specific they are.

Pure functions first (unit-tested on synthetic data), then `run`. Product evidence is aggregate only: per-cell arrays stay in the ignored cache.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import pathlib
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_ml_v3 as v3  # noqa: E402

SCHEMA = "segarecomp.seg087.diag.v1"
HARNESS = pathlib.Path(os.environ.get("SEGARECOMP_HARNESS_ROOT", "/root/dev/segarecomp-harness"))
CACHE = pathlib.Path(os.environ.get("SEG087_CACHE", str(HARNESS / ".cache" / "seg087")))
S85 = pathlib.Path(os.environ.get("SEG085_CACHE", str(HARNESS / ".cache" / "seg085")))
S72 = pathlib.Path(os.environ.get("SEG072_CACHE", str(HARNESS / ".cache" / "seg072")))
RESULTS = HERE / "segarecomp_seg087_diag.results.json"
CELL = v3.CELL_BYTES
GENESIS = ("D01", "D02", "D03", "D04", "D05", "D06", "D07", "D09", "D10", "D11", "D12")
CHAIN_RUN = 8
TERMINATING = (2, 4, 6)       # token control: direct jump, dynamic jump, return
CATEGORIES = ("AMBIGUOUS_UNKNOWN", "PURE_CODE", "SINGLE_START_FRAGMENT", "DATA_DOMINATED_FRAGMENT", "MIXED_CODE_DATA")
ENTRIES = ("IMAGE_START", "AFTER_DATA", "AFTER_UNKNOWN", "AFTER_TERMINATOR", "FALLTHROUGH_FROM_CODE", "NO_START")
INDEPENDENT = ("W_REACH_ROOTED", "W_REACH_TARGET", "W_RELOC", "W_VECTOR_ROOTED")
HEURISTIC = ("W_UNROOTED_TARGET", "W_DECODE_CHAIN")


def r4(x):
    return None if x is None else round(float(x), 4)


def frac(a, b):
    return None if b == 0 else r4(a / b)


# ------------------------------------------------------------------------------------------------------------------------------- pure
def cell_profile(chunk: bytes) -> dict:
    """Byte-class composition of one cell (classes: I instruction start, c instruction continuation, D data, U unknown/disputed)."""
    return {"starts": chunk.count(b"I"), "code": chunk.count(b"I") + chunk.count(b"c"), "data": chunk.count(b"D"), "unknown": chunk.count(b"U"), "bytes": len(chunk)}


def classify_cell(p: dict) -> str:
    """Mutually exclusive structural category of a verified-positive cell, evaluated in this order. Unknown bytes make a cell ambiguous: it is never
    reinterpreted as clean evidence."""
    if p["unknown"] > 0:
        return "AMBIGUOUS_UNKNOWN"
    if p["data"] == 0:
        return "PURE_CODE"
    if p["starts"] == 1:
        return "SINGLE_START_FRAGMENT"
    if p["data"] * 2 > p["bytes"]:
        return "DATA_DOMINATED_FRAGMENT"
    return "MIXED_CODE_DATA"


def entry_kind(classes: bytes, cell: int, controls_by_start: dict, cell_bytes: int = CELL) -> str:
    """How control enters the first instruction of a cell, from the byte classes alone (offline description of the cell, NOT a witness):
    after data, after unknown bytes, after a terminating instruction (so only a branch can reach it), by plain fall-through from code, or no start at all."""
    lo = cell * cell_bytes
    chunk = classes[lo:lo + cell_bytes]
    k = chunk.find(b"I")
    if k < 0:
        return "NO_START"
    a = lo + k
    if a == 0:
        return "IMAGE_START"
    prev = classes[a - 1:a]
    if prev == b"D":
        return "AFTER_DATA"
    if prev == b"U":
        return "AFTER_UNKNOWN"
    p = classes.rfind(b"I", 0, a)
    if p >= 0 and controls_by_start.get(p, -1) in TERMINATING:
        return "AFTER_TERMINATOR"
    return "FALLTHROUGH_FROM_CODE"


def subcell_profiles(chunk: bytes, sub: int) -> list:
    return [cell_profile(chunk[i:i + sub]) for i in range(0, len(chunk), sub)]


def subcell_resolution(chunk: bytes, sub: int) -> dict:
    """Does halving the unit separate code from data inside a positive cell? Per sub-cell: pure code / pure data / mixed / contains unknown.
    `resolved` = no sub-cell is mixed or unknown (every sub-cell can be decided without a boundary inside it)."""
    ps = subcell_profiles(chunk, sub)
    pure_code = sum(1 for p in ps if p["starts"] + p["code"] > 0 and p["data"] == 0 and p["unknown"] == 0)
    pure_data = sum(1 for p in ps if p["starts"] == 0 and p["code"] == 0 and p["unknown"] == 0 and p["data"] > 0)
    unknown = sum(1 for p in ps if p["unknown"] > 0)
    mixed = len(ps) - pure_code - pure_data - unknown
    code_subcells = sum(1 for p in ps if p["starts"] > 0)
    return {"subcells": len(ps), "pure_code": pure_code, "pure_data": pure_data, "mixed": mixed, "unknown": unknown,
            "start_bearing": code_subcells, "resolved": mixed == 0 and unknown == 0}


def has_cartridge_header(rom: bytes) -> bool:
    """Label-free applicability test for the 68000 vector-table root: the image carries the Mega Drive / Genesis cartridge header ("SEGA" console name at 0x100).
    A Sega CD module, a relocatable executable or any other blob has no such table and yields no vector roots (fail closed)."""
    return len(rom) >= 0x200 and (rom[0x100:0x104] == b"SEGA" or rom[0x101:0x105] == b"SEGA")


def vector_roots(rom: bytes, n_words: int) -> list:
    """Word indices of the exception-vector program-counter entries (longwords 1..63 of the header; the initial stack pointer is not code). Even, in-image only."""
    out = []
    if not has_cartridge_header(rom):
        return out
    for i in range(1, 64):
        if 4 * i + 4 > len(rom):
            break
        a = int.from_bytes(rom[4 * i:4 * i + 4], "big")
        if a % 2 == 0 and 0 < a // 2 < n_words:
            out.append(a // 2)
    return sorted(set(out))


def reach_from(roots, ordinary, length, control, target) -> list:
    """Words reachable from `roots` by fall-through (unless the instruction terminates) and direct edges, over decoder-ordinary words (same rule as model A's `reach`)."""
    n = len(ordinary)
    seen = [False] * n
    stack = [w for w in roots if 0 <= w < n]
    while stack:
        w = stack.pop()
        if w < 0 or w >= n or seen[w] or not ordinary[w]:
            continue
        seen[w] = True
        if control[w] not in TERMINATING:
            stack.append(w + max(1, int(length[w])))
        if target[w] >= 0:
            stack.append(int(target[w]))
    return seen


def witness_flags(cell_words: int, n_cells: int, reach, target, ordinary, chain, relocs, reloc_cell_bytes: int = CELL, vreach=None) -> dict:
    """Per-cell boolean witness arrays (lists) from per-word structural arrays (pure; `reach`, `ordinary`: bool lists/arrays, `target`: word index or -1, `chain`: run length).
    Words are mapped to cells with the model-A convention (cell = word // cell_words, clamped)."""
    def cw(w):
        return min(w // cell_words, n_cells - 1)

    out = {k: [False] * n_cells for k in INDEPENDENT + HEURISTIC}
    for w, r in enumerate(reach):
        if r:
            out["W_REACH_ROOTED"][cw(w)] = True
    for w, t in enumerate(target):
        if t < 0 or t >= len(ordinary):
            continue
        out["W_UNROOTED_TARGET"][cw(int(t))] = True
        if reach[w]:
            out["W_REACH_TARGET"][cw(int(t))] = True
    for w, c in enumerate(chain):
        if c >= CHAIN_RUN and ordinary[w]:
            out["W_DECODE_CHAIN"][cw(w)] = True
    for w, r in enumerate(vreach if vreach is not None else ()):
        if r:
            out["W_VECTOR_ROOTED"][cw(w)] = True
    for off, _w in relocs:
        c = int(off) // reloc_cell_bytes if int(off) >= 0 else -1
        if 0 <= c < n_cells:                    # an out-of-range relocation is invalid input: ignored, never evidence for another cell
            out["W_RELOC"][c] = True
    return out


def usable_witness(flags_at_cell: dict) -> bool:
    """A cell has a usable independent witness iff at least one INDEPENDENT (seed- or relocation-rooted) witness fires."""
    return any(flags_at_cell[k] for k in INDEPENDENT)


def lineage_minima_driver(minima: dict) -> dict:
    """SEG-085 DYN_SCORE threshold = T0 - shortfall: T0 is the lineage-minimum weakest positive; the shortfall is the largest leave-one-lineage-out miss of that
    plain minimum rule. Returns which lineage sets T0 and which ordered (held-out, calibration-minimum) pair sets the shortfall (pure)."""
    vals = {L: m for L, m in minima.items() if m is not None}
    if len(vals) < 2:
        return {"t0": None}
    t0_l = min(vals, key=lambda L: (vals[L], L))
    best = None
    for L in sorted(vals):
        others = {k: v for k, v in vals.items() if k != L}
        lo = min(others, key=lambda k: (others[k], k))
        miss = others[lo] - vals[L]
        if best is None or miss > best[0] + 1e-12:
            best = (miss, L, lo)
    return {"t0": r4(vals[t0_l]), "t0_lineage": t0_l, "shortfall": r4(best[0]), "shortfall_held_out": best[1], "shortfall_calibration_minimum_lineage": best[2]}


def retained_by_budget(scores, seeds, pct: int) -> set:
    """Fixed ranked-budget region (the unchanged SEG-072 rule): top ceil(pct%) cells by score with stable tie order, united with the seed cells."""
    return set(v3.rank_order(scores)[:v3.budget_cell_count(len(scores), pct)]) | set(seeds)


def stability(inner_scores: list, oof_score: float) -> dict:
    """Over the refits of one cell: how many keep the cell below zero (plus the out-of-fold score itself)."""
    vals = [float(oof_score)] + [float(x) for x in inner_scores]
    return {"fits": len(vals), "below_zero": sum(1 for x in vals if x < 0.0), "min": r4(min(vals)), "max": r4(max(vals))}


def rank_percentile(scores, c: int) -> float:
    """Fraction of cells scoring strictly higher than cell c (0 = best). Label-free description of where a cell sits in its own image's ranking."""
    s = float(scores[c])
    return sum(1 for x in scores if float(x) > s) / max(1, len(scores))


def summarize_flags(rows: list, keys) -> dict:
    n = len(rows)
    return {"cells": n, **{k: {"count": sum(1 for r in rows if r["w"][k]), "share": frac(sum(1 for r in rows if r["w"][k]), n)} for k in keys},
            "usable_independent": {"count": sum(1 for r in rows if usable_witness(r["w"])), "share": frac(sum(1 for r in rows if usable_witness(r["w"])), n)}}


# ------------------------------------------------------------------------------------------------------------------------------- run
SGDK_IMAGES = ("d05", "d06", "d07", "d09", "d10", "d11", "d12")   # modern SGDK images (d10-d12 carry no independently established seed universe); every other development image is historical commercial software
LABEL_AUDIT_NOTE = "a direct edge from a labelled instruction start that lands on a labelled continuation or data byte contradicts the reconstruction labels (ADR 0191)"


def group_of(img_id: str) -> str:
    return "sgdk" if img_id in SGDK_IMAGES else "historical"


def _load(guard_install=None, loader=None):
    """Install the SEG-086 sample-role guard (ADR 0198) on the training-image constructor BEFORE any development sample is loaded, then use the unchanged SEG-084 loader.
    Evaluation-only, exploratory, contradictory-role, frozen-evaluation and reserved identities are refused from metadata alone; an authorization failure raises."""
    if guard_install is None:
        import segarecomp_sample_guard as sg
        guard_install = sg.install
    guard_install()
    if loader is None:
        import segarecomp_seg084_a as s84
        loader = s84.load_registry_images
    imgs, _ = loader()
    import numpy as np
    import segarecomp_seg084_a as s84
    import segarecomp_xplat_transfer as xt
    oof = {}
    for L in GENESIS:
        oof.update(xt.load_npz(s84.score_path(L, "A0")))
    return np, xt, imgs, oof


def _controls_by_start(im, np) -> dict:
    ctl = im.tokens[:, 3]
    return {int(a): int(ctl[a // 2]) for a in im.starts if a % 2 == 0 and a // 2 < im.n_words}


def label_audit_cells(im, st, np) -> tuple:
    """Cells that contain (a) a labelled start whose direct edge lands on a continuation/data byte, or (b) such a landing. Counts of cells, not addresses."""
    src, dst = set(), set()
    for a in im.starts:
        if a % 2:
            continue
        w = a // 2
        if w >= im.n_words or not st["ordinary"][w]:
            continue
        t = int(st["target"][w])
        if t < 0:
            continue
        tb = 2 * t
        if tb < len(im.classes) and im.classes[tb:tb + 1] in (b"c", b"D"):
            src.add(min(a // CELL, im.n_cells - 1))
            dst.add(min(tb // CELL, im.n_cells - 1))
    return src, dst


def cmd_run(a) -> int:
    np, xt, imgs, oof = _load()
    import segarecomp_seg085_dyn as dyn
    import segarecomp_xplat_models as xm
    import segarecomp_ml_v3_train as tr
    t0 = time.process_time()
    nested = json.loads((S85 / "nested.json").read_text())
    rows, neg_flags, seed_pos, neg_feat = [], [], [], []
    per_img = {}
    m4_ok = 0
    for im in imgs:
        sc = [float(x) for x in oof[im.id]]
        lab = [int(x) for x in im.labels]
        seeds = {c for c in im.seeds if 0 <= c < im.n_cells}
        st = xm.structure(im)
        relocs = xm.relocs_of(im)
        vr = reach_from(vector_roots(im.rom, im.n_words), st["ordinary"], st["length"], st["control"], st["target"])
        fl = witness_flags(64, im.n_cells, st["reach"], st["target"], st["ordinary"], st["chain"], relocs, vreach=vr)
        ctl = _controls_by_start(im, np)
        la_src, la_dst = label_audit_cells(im, st, np)
        reg = {p: retained_by_budget(sc, seeds, p) for p in (35, 45, 50)}
        nonseed_pos = [c for c in range(im.n_cells) if lab[c] == 1 and c not in seeds]
        weakest = min(nonseed_pos, key=lambda c: (sc[c], c)) if nonseed_pos else None
        per_img[im.id] = {"lineage": im.lineage, "group": group_of(im.id), "cells": im.n_cells, "weakest_cell": weakest, "weakest_logit": None if weakest is None else sc[weakest]}
        # optional dense M4 word logits (held-out, 3 seeds averaged) for the descriptive 64-byte comparison
        m4 = None
        fs = [S72 / "scores" / f"M4.{im.lineage}.s{k}.npz" for k in (1, 2, 3)]
        if all(f.is_file() for f in fs):
            zs = [np.load(f) for f in fs]
            if all(im.id.replace("/", "@@") in z.files or im.id in z.files for z in zs):
                words = [z[im.id if im.id in z.files else im.id.replace("/", "@@")] for z in zs]
                if all(len(w) == im.n_words for w in words):
                    wl = np.mean(words, axis=0).astype(np.float32)
                    m4 = {128: tr.cell_max(wl, im.size, 128), 64: tr.cell_max(wl, im.size, 64)}
                    m4_ok += 1
        ordn = np.asarray(st["ordinary"], dtype=float)
        cow = np.minimum(np.arange(im.n_words) // 64, im.n_cells - 1)
        ord_share = np.bincount(cow, weights=ordn, minlength=im.n_cells) / 64.0
        chain_max = np.zeros(im.n_cells)
        np.maximum.at(chain_max, cow, np.asarray(st["chain"], dtype=float))
        raw = np.empty(im.n_words * 2, dtype=np.int64)
        raw[0::2], raw[1::2] = im.hi, im.lo
        bc = np.minimum(np.arange(im.n_words * 2) // 128, im.n_cells - 1)
        zero_share = np.bincount(bc, weights=(raw == 0).astype(float), minlength=im.n_cells) / 128.0
        ascii_share = np.bincount(bc, weights=((raw >= 32) & (raw < 127)).astype(float), minlength=im.n_cells) / 128.0
        fch = {"ordinary_word_share": ord_share, "decode_chain_max": chain_max, "zero_byte_share": zero_share, "ascii_byte_share": ascii_share}
        inner = [np.asarray(xt.load_npz(dyn.CACHE / "inner" / f"{H}.{im.lineage}.npz")[im.id]) for H in GENESIS if H != im.lineage]
        for c in range(im.n_cells):
            w = {k: bool(fl[k][c]) for k in fl}
            if lab[c] == 0:
                neg_flags.append((group_of(im.id), w))
                if im.lineage in ("D01", "D03", "D12") and len(neg_feat) < 10**7:
                    neg_feat.append({k: float(v[c]) for k, v in fch.items()})
                continue
            if lab[c] != 1:
                continue
            chunk = im.classes[c * CELL:(c + 1) * CELL]
            p = cell_profile(chunk)
            if c in seeds:
                seed_pos.append({"group": group_of(im.id), "neg": sc[c] < 0.0, "cat": classify_cell(p)})
                continue
            row = {"img": im.id, "L": im.lineage, "group": group_of(im.id), "c": c, "logit": sc[c], "cat": classify_cell(p), "profile": p,
                   "entry": entry_kind(im.classes, c, ctl), "w": w, "seeded": bool(seeds), "weakest_in_image": c == weakest,
                   "feat": {k: float(v[c]) for k, v in fch.items()},
                   "kept": {f"B{q}": c in reg[q] for q in reg}, "audit_src": c in la_src, "audit_dst": c in la_dst}
            if sc[c] < 0.0:
                row["stab"] = stability([float(x[c]) for x in inner], sc[c])
                row["rank_pct"] = rank_percentile(sc, c)
                row["res64"], row["res32"] = subcell_resolution(chunk, 64), subcell_resolution(chunk, 32)
                if m4 is not None:
                    row["m4_rank128"] = rank_percentile(m4[128], c)
                    row["m4_rank64"] = min(rank_percentile(m4[64], 2 * c), rank_percentile(m4[64], min(2 * c + 1, len(m4[64]) - 1)))
            rows.append(row)
    low = [r for r in rows if r["logit"] < 0.0]
    out = {"schema": SCHEMA, "cell_bytes": CELL, "images": len(imgs), "data_boundary": "aggregates only; per-cell arrays stay in the ignored cache", "label_audit_definition": LABEL_AUDIT_NOTE}
    out["population"] = {"nonseed_positive_cells": len(rows), "low_tail_negative_logit_cells": len(low), "seed_positive_cells": len(seed_pos),
                         "seed_positive_cells_with_negative_logit": sum(1 for s in seed_pos if s["neg"]),
                         "by_group": {g: {"nonseed_positive": sum(1 for r in rows if r["group"] == g), "low_tail": sum(1 for r in low if r["group"] == g)} for g in ("historical", "sgdk")},
                         "low_tail_in_images_without_seed_universe": sum(1 for r in low if not r["seeded"]),
                         "by_lineage": {L: sum(1 for r in low if r["L"] == L) for L in GENESIS}}
    # (A) binding cases
    minima = {}
    for L in GENESIS:
        vals = [v["weakest_logit"] for v in per_img.values() if v["lineage"] == L and v["weakest_logit"] is not None]
        minima[L] = min(vals) if vals else None
    out["threshold_drivers"] = {"lineage_weakest_positive": {L: r4(v) for L, v in minima.items()}, "seg085_dyn_score_T_final": r4(nested["final"]["T"]), "decomposition": lineage_minima_driver(minima)}
    binding = []
    for L in GENESIS:
        cand = [r for r in rows if r["L"] == L]
        if cand:
            binding.append(min(cand, key=lambda r: (r["logit"], r["img"], r["c"])))
    img_binding = [r for r in rows if r["weakest_in_image"] and r["logit"] < 0.0]

    def brief(r):
        return {"lineage": r["L"], "group": r["group"], "category": r["cat"], "entry": r["entry"], "starts": r["profile"]["starts"], "data_bytes": r["profile"]["data"],
                "logit": r4(r["logit"]), "usable_independent_witness": usable_witness(r["w"]), "kept": r["kept"]}
    out["binding"] = {"lineage_weakest_cells": [brief(r) for r in binding],
                      "image_weakest_negative_cells": {"count": len(img_binding), "images": sorted({r["img"] for r in img_binding}),
                                                       "usable_independent_witness": sum(1 for r in img_binding if usable_witness(r["w"]))}}
    out["retention_of_low_tail"] = {b: {"retained": sum(1 for r in low if r["kept"][b]), "of": len(low), "by_group": {g: [sum(1 for r in low if r["group"] == g and r["kept"][b]), sum(1 for r in low if r["group"] == g)] for g in ("historical", "sgdk")},
                                        "by_lineage": {L: [sum(1 for r in low if r["L"] == L and r["kept"][b]), sum(1 for r in low if r["L"] == L)] for L in ("D01", "D03", "D12")}} for b in ("B35", "B45", "B50")}
    out["stability_over_fits"] = {"always_below_zero": sum(1 for r in low if r["stab"]["below_zero"] == r["stab"]["fits"]),
                                  "below_zero_in_at_least_half": sum(1 for r in low if r["stab"]["below_zero"] * 2 >= r["stab"]["fits"]),
                                  "never_below_zero_in_inner_fits": sum(1 for r in low if r["stab"]["below_zero"] == 1), "cells": len(low),
                                  "fits_per_cell": low[0]["stab"]["fits"] if low else 0,
                                  "note": "fits = the out-of-fold model plus the ten SEG-085 inner refits that do not train on the cell's lineage; the in-sample final model is not used (optimistic)"}
    out["rank_in_image_of_low_tail"] = {q: r4(sorted(r["rank_pct"] for r in low)[int(q * (len(low) - 1))]) for q in (0.0, 0.25, 0.5, 0.75, 1.0)} if low else {}
    # (B0) why model A scores them low: median label-free cell character against other positives and negatives of the same lineages
    def med(vals):
        v = sorted(vals)
        return r4(v[len(v) // 2]) if v else None

    def feat_summary(sel_feats):
        return {k: med([f[k] for f in sel_feats]) for k in ("ordinary_word_share", "decode_chain_max", "zero_byte_share", "ascii_byte_share")} | {"cells": len(sel_feats)}
    out["feature_character_medians"] = {
        "low_tail_D01": feat_summary([r["feat"] for r in low if r["L"] == "D01"]), "low_tail_D03": feat_summary([r["feat"] for r in low if r["L"] == "D03"]),
        "low_tail_D12": feat_summary([r["feat"] for r in low if r["L"] == "D12"]),
        "other_positive_D01": feat_summary([r["feat"] for r in rows if r["L"] == "D01" and r["logit"] >= 0.0]),
        "other_positive_D03": feat_summary([r["feat"] for r in rows if r["L"] == "D03" and r["logit"] >= 0.0]),
        "other_positive_D12": feat_summary([r["feat"] for r in rows if r["L"] == "D12" and r["logit"] >= 0.0]),
        "negatives_D01_D03_D12": feat_summary(neg_feat)}
    # (B1) threshold-relevant effect of a rescue: lineage-weakest positives and the SEG-085 decomposition AFTER removing the witnessed cells
    def minima_without(pred):
        m = {}
        for L in GENESIS:
            v = [r["logit"] for r in rows if r["L"] == L and not pred(r)]
            m[L] = min(v) if v else None
        return m
    out["threshold_effect_of_witness_removal"] = {
        "baseline": lineage_minima_driver(minima),
        "remove_independent_witnessed": lineage_minima_driver(minima_without(lambda r: usable_witness(r["w"]))),
        "remove_independent_witnessed_seed_roots_only": lineage_minima_driver(minima_without(lambda r: r["w"]["W_REACH_ROOTED"] or r["w"]["W_REACH_TARGET"] or r["w"]["W_RELOC"])),
        "remove_unrooted_target_witnessed_NON_INDEPENDENT_UPPER_BOUND": lineage_minima_driver(minima_without(lambda r: r["w"]["W_UNROOTED_TARGET"] or usable_witness(r["w"]))),
        "remove_all_five_witnesses_NON_INDEPENDENT_UPPER_BOUND": lineage_minima_driver(minima_without(lambda r: any(r["w"].values()))),
        "note": "removing a witnessed cell from the lineage minimum is what a perfect rescue of those cells would do to the calibration; the threshold is set by the weakest UNRESCUED positive"}
    out["witness_density_by_lineage"] = {L: {"seed_cells": sum(len(im.seeds) for im in imgs if im.lineage == L), "cells": sum(im.n_cells for im in imgs if im.lineage == L),
                                              "nonseed_positive_with_independent_witness": [sum(1 for r in rows if r["L"] == L and usable_witness(r["w"])), sum(1 for r in rows if r["L"] == L)]} for L in GENESIS}
    # (B) taxonomy
    def tax(sel):
        t = {c: sum(1 for r in sel if r["cat"] == c) for c in CATEGORIES}
        t["entries"] = {e: sum(1 for r in sel if r["entry"] == e) for e in ENTRIES}
        t["label_audit_flagged"] = sum(1 for r in sel if r["audit_src"] or r["audit_dst"])
        t["cells"] = len(sel)
        return t
    out["taxonomy"] = {"low_tail": tax(low), "low_tail_by_lineage": {L: tax([r for r in low if r["L"] == L]) for L in ("D01", "D03", "D12")},
                       "low_tail_by_image_family": {k: tax([r for r in low if r["img"].startswith(k)]) for k in ("s1", "s2", "sk", "scd/")},
                       "other_nonseed_positive": tax([r for r in rows if r["logit"] >= 0.0])}
    # (C) witness coverage
    keys = INDEPENDENT + HEURISTIC
    out["witness_coverage"] = {"low_tail": summarize_flags(low, keys), "low_tail_seeded_images_only": summarize_flags([r for r in low if r["seeded"]], keys),
                               "low_tail_unseeded_images_only": summarize_flags([r for r in low if not r["seeded"]], keys),
                               "low_tail_by_group": {g: summarize_flags([r for r in low if r["group"] == g], keys) for g in ("historical", "sgdk")},
                               "image_weakest_negative_seeded_only": summarize_flags([r for r in img_binding if r["seeded"]], keys), "low_tail_by_lineage": {L: summarize_flags([r for r in low if r["L"] == L], keys) for L in ("D01", "D03", "D12")},
                               "image_weakest_negative": summarize_flags(img_binding, keys), "lineage_weakest": summarize_flags(binding, keys),
                               "all_nonseed_positive": summarize_flags(rows, keys), "other_nonseed_positive": summarize_flags([r for r in rows if r["logit"] >= 0.0], keys),
                               "negatives_false_witness": {g: summarize_flags([{"w": w} for gg, w in neg_flags if gg == g], keys) for g in ("historical", "sgdk")},
                               "low_tail_categories_with_usable_witness": {c: [sum(1 for r in low if r["cat"] == c and usable_witness(r["w"])), sum(1 for r in low if r["cat"] == c)] for c in CATEGORIES}}
    # (D) granularity
    def gran(sel, key):
        res = [r[key] for r in sel]
        return {"cells": len(res), "resolved_without_mixed_subcell": sum(1 for x in res if x["resolved"]), "with_mixed_subcell": sum(1 for x in res if x["mixed"] > 0),
                "start_bearing_subcells_mean": r4(sum(x["start_bearing"] for x in res) / max(1, len(res))), "subcells_per_cell": res[0]["subcells"] if res else 0,
                "mixed_subcells_total": sum(x["mixed"] for x in res), "pure_data_subcells_total": sum(x["pure_data"] for x in res), "pure_code_subcells_total": sum(x["pure_code"] for x in res)}
    out["granularity"] = {"low_tail_64": gran(low, "res64"), "low_tail_32": gran(low, "res32"),
                          "low_tail_64_by_lineage": {L: gran([r for r in low if r["L"] == L], "res64") for L in ("D01", "D03", "D12")},
                          "low_tail_32_by_lineage": {L: gran([r for r in low if r["L"] == L], "res32") for L in ("D01", "D03", "D12")},
                          "caveat": "descriptive truth-based composition only; no 64- or 32-byte model was trained or scored. `resolved` means every sub-cell is pure code or pure data."}
    m4rows = [r for r in low if "m4_rank128" in r]
    out["m4_dense_descriptive"] = {"images_with_m4": m4_ok, "low_tail_cells_covered": len(m4rows),
                                   "top35_at_128": sum(1 for r in m4rows if r["m4_rank128"] < 0.35), "top35_at_64_either_half": sum(1 for r in m4rows if r["m4_rank64"] < 0.35),
                                   "model_a_top35": sum(1 for r in m4rows if r["rank_pct"] < 0.35),
                                   "note": "M4-Dense (SEG-072 stored held-out word logits, 3 seeds averaged) is a different model; reported only to see whether a finer unit changes the ranking of these cells"}
    out["cpu_seconds"] = round(time.process_time() - t0, 1)
    RESULTS.write_text(json.dumps(out, indent=1, sort_keys=True) + "\n")
    print(json.dumps({"low_tail": len(low), "usable": out["witness_coverage"]["low_tail"]["usable_independent"], "cpu": out["cpu_seconds"]}))
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("run").set_defaults(func=cmd_run)
    a = ap.parse_args(argv)
    return a.func(a)


if __name__ == "__main__":
    raise SystemExit(main())
