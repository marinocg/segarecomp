#!/usr/bin/env python3
"""SEG-050-T003/T004 (ADR 0112): offline E0/E1 measurement of the frozen gen2 M3 + B35 model on an already-established materialized image.

OFFLINE RESEARCH TOOLING (stdlib only; the model is the shipped native evaluator reached through `segarecomp ml-gen2-cell-logits`, so the tool
contains no model, no decoder and no tuning). Nothing is trained or calibrated here and no threshold is a parameter.

The image under test is an ADR 0049 verbatim copy: image bytes = ROM[source : source + length]. The same bytes are scored in two ways and compared:

  direct-ROM   the whole ROM is scored once (context = the surrounding ROM); the cells of the source span are the "direct" scores;
  E1 (image)   ONLY the image bytes are scored (context = the image; ROM-edge rule = the frozen symmetric word-block reflection at the image ends).

Cells are the frozen 128-byte decision cells of the IMAGE (image offset 0 is cell 0). Position and execution base are never an input: the model sees
bytes and CPU-owned decode tokens of the image only (tokens are recomputed by the CLI on the image alone, so a truncated instruction at the image end
is a genuine boundary effect that is measured, not hidden).

Truth (private, never printed): the source-backed MC68000 instruction-start list of the ROM (`.u` artifact). Copied-image starts are DERIVED by byte
identity: a ROM truth start s with source <= s < source + length is an image start at s - source. Observed runtime coverage is never used.

Output: ONE sanitized JSON object: counts, fractions, booleans and ranks of aggregates only. No address, offset, byte, per-cell datum or listing.

usage: seg050_materialized_e1.py --segarecomp <cli> --rom <image> --truth <.u> --source <hex> --length <hex> --work <ignored-dir> [--budget-percent 35]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import pathlib
import struct
import subprocess
import sys

CELL = 128
BUDGET_PERCENT_FROZEN = 35  # B35 (ADR 0101): the only policy; the flag exists only so a test can prove it is refused when different.


def load_truth(path: pathlib.Path, rom_sha256: str) -> list[int]:
    lines = path.read_text().splitlines()
    if lines[0] != "segarecomp.m68k_source_universe.v1" or lines[1] != "rom_sha256 " + rom_sha256:
        raise SystemExit("truth does not belong to this ROM")
    count = int(lines[5].split()[1])
    starts = [int(x, 16) for x in lines[6:6 + count]]
    if len(starts) != count or starts != sorted(set(starts)):
        raise SystemExit("invalid truth list")
    return starts


def finite(values: list[float], what: str) -> list[float]:
    if any(math.isnan(v) or math.isinf(v) for v in values):
        raise SystemExit("non-finite logit in " + what)
    return values


def logits_of(cli: str, data: bytes, work: pathlib.Path, name: str) -> list[float]:
    source = work / (name + ".bin")
    sink = work / (name + ".logits")
    source.write_bytes(data)
    done = subprocess.run([cli, "ml-gen2-cell-logits", "--rom", str(source), "--output", str(sink)], capture_output=True, text=True, timeout=3600)
    if done.returncode != 0:
        raise SystemExit("cell logits failed for " + name + ": " + done.stderr.strip()[-200:])
    raw = sink.read_bytes()
    return finite(list(struct.unpack("<%df" % (len(raw) // 4), raw)), name)


def rank_select(scores: list[float], percent: int) -> list[int]:
    """B35 exactly as the shipped policy: score descending, cell ordinal ascending, N = ceil(cells * percent / 100) in integer arithmetic."""
    order = sorted(range(len(scores)), key=lambda c: (-scores[c], c))
    return order[:(len(scores) * percent + 99) // 100]


def spearman(a: list[float], b: list[float]) -> float:
    def ranks(values):
        order = sorted(range(len(values)), key=lambda i: (values[i], i))
        out = [0.0] * len(values)
        for rank, index in enumerate(order):
            out[index] = float(rank)
        return out
    ra, rb = ranks(a), ranks(b)
    n = len(a)
    ma, mb = sum(ra) / n, sum(rb) / n
    cov = sum((x - ma) * (y - mb) for x, y in zip(ra, rb))
    va = math.sqrt(sum((x - ma) ** 2 for x in ra))
    vb = math.sqrt(sum((y - mb) ** 2 for y in rb))
    return cov / (va * vb) if va > 0.0 and vb > 0.0 else float("nan")


def frf(scores: list[float], labels: list[int]) -> dict:
    """Full-recall fraction (ADR 0100 definition): the smallest fraction of cells, in descending score, containing every positive cell (ties pessimistic)."""
    positives = [s for s, y in zip(scores, labels) if y]
    if not positives:
        return {"frf": None, "cells_needed": None}
    worst = min(positives)
    needed = sum(1 for s in scores if s >= worst)
    return {"frf": round(needed / len(scores), 6), "cells_needed": needed}


def arm(name: str, scores: list[float], labels: list[int], starts: list[int], percent: int) -> dict:
    selected = set(rank_select(scores, percent))
    outside = sum(1 for s in starts if s // CELL not in selected)
    out = {"arm": name, "cells": len(scores), "positive_cells": sum(labels), "positive_cell_fraction": round(sum(labels) / len(labels), 6),
           "selected_cells": len(selected), "proposed_fraction": round(len(selected) / len(scores), 6), "truth_starts": len(starts),
           "c_outside_r": outside, "full_recall": outside == 0, "positive_cells_missed": sum(1 for c, y in enumerate(labels) if y and c not in selected)}
    out.update(frf(scores, labels))
    # more positive cells than the budget N = ceil(cells * percent / 100) can hold (integer arithmetic): no ranking can reach full recall
    out["density_limited"] = sum(labels) > (len(labels) * percent + 99) // 100
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--segarecomp", required=True)
    ap.add_argument("--rom", required=True)
    ap.add_argument("--truth", help="private .u truth artifact; omitted => structure-only mode (no recall claim)")
    ap.add_argument("--source", required=True, help="hex image source address in the ROM (even; 128-byte aligned when --truth is given)")
    ap.add_argument("--length", required=True, help="hex image length in bytes (even)")
    ap.add_argument("--work", required=True)
    ap.add_argument("--budget-percent", type=int, default=BUDGET_PERCENT_FROZEN)
    args = ap.parse_args()
    if args.budget_percent != BUDGET_PERCENT_FROZEN:
        raise SystemExit("the budget policy is frozen (B35)")
    work = pathlib.Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    rom = pathlib.Path(args.rom).read_bytes()
    rom_sha = hashlib.sha256(rom).hexdigest()
    source, length = int(args.source, 16), int(args.length, 16)
    if source % 2 or length % 2 or length <= 0 or source + length > len(rom) or (args.truth and source % CELL):
        raise SystemExit("invalid image descriptor")
    image = rom[source:source + length]
    image_cells = (length + CELL - 1) // CELL
    percent = args.budget_percent
    e1 = logits_of(args.segarecomp, image, work, "image")
    again = logits_of(args.segarecomp, image, work, "image_repeat")
    deterministic = e1 == again and rank_select(e1, percent) == rank_select(again, percent)  # D1
    budget = (image_cells * percent + 99) // 100
    result = {"image_sha256": hashlib.sha256(image).hexdigest(), "image_bytes": length, "image_cells": image_cells, "budget_cells": budget,
              "budget_fraction": round(budget / image_cells, 6), "deterministic_d1": deterministic, "e0_broad": {"proposed_fraction": 1.0, "c_outside_r": 0}}
    if not args.truth:
        result["mode"] = "structure_only_no_truth"
        print(json.dumps(result, sort_keys=True))
        return 0
    truth = load_truth(pathlib.Path(args.truth), rom_sha)
    image_starts = [s - source for s in truth if source <= s < source + length]
    labels = [0] * image_cells
    for start in image_starts:
        labels[start // CELL] = 1
    rom_logits = logits_of(args.segarecomp, rom, work, "rom")
    first = source // CELL
    direct = rom_logits[first:first + image_cells]
    if len(direct) != image_cells or len(e1) != image_cells:
        raise SystemExit("cell count mismatch")
    result.update({"truth_starts": len(image_starts),
                   "e1_image_only": arm("e1_image_only", e1, labels, image_starts, percent),
                   "direct_rom_scores_in_image": arm("direct_rom", direct, labels, image_starts, percent),
                   "spearman_image_vs_direct": round(spearman(e1, direct), 6)})
    sel_e1, sel_direct = set(rank_select(e1, percent)), set(rank_select(direct, percent))
    result["selected_set_jaccard"] = round(len(sel_e1 & sel_direct) / len(sel_e1 | sel_direct), 6)
    edge = min(4, image_cells // 2)
    border = list(range(edge)) + list(range(image_cells - edge, image_cells))
    result["boundary"] = {"edge_cells_each_side": edge,
                          "mean_abs_logit_difference_edges": round(sum(abs(e1[c] - direct[c]) for c in border) / len(border), 4),
                          "mean_abs_logit_difference_interior": round(sum(abs(e1[c] - direct[c]) for c in range(edge, image_cells - edge)) / max(1, image_cells - 2 * edge), 4)}
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
