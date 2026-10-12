#!/usr/bin/env python3
"""SEG-051 (ADR 0114): stdlib-only definitions of the Z80 executable-region ranking experiment.

Everything label-sensitive that must be reproducible lives here and is shared by the offline trainer
(`segarecomp_ml_region_z80_train.py`, which imports numpy/PyTorch lazily; neither is a product dependency) and by
`tests/segarecomp_ml_region_z80_test.py`: cells, labels, ranking, FRF, the fixed and adaptive budget policies, the
lineage split and its leakage guard.

Truth is PRIVATE. It is a per-image class string produced from WLA-DX linked listings and never leaves the ignored
local store: `I` source instruction start, `c` continuation byte of an instruction, `D` confident data directive, `A`
ambiguous `.db` (never a negative), `U` not listed, `Q` quarantined 4 KiB block where the product Z80 decoder and the
listing disagree on an instruction length. Only aggregates are ever published.

The Z80 CPU decoder (`z80-decode-token-report`, `cpu::z80::decode_at`) is the sole authority for forms, prefixes and
lengths; nothing here decodes Z80.
"""
from __future__ import annotations

import hashlib
import json
import math
import struct

CELL_BYTES = 32  # decision cell: aligned, short Z80 instructions (1-4 bytes) make 128-byte M68K cells inappropriate
CONTEXT_BYTES = 32  # per-side context; the 4-layer dilated convolution receptive-field radius is 30 <= 32
CELL_WITH_CONTEXT = CELL_BYTES + 2 * CONTEXT_BYTES
TOKEN_MAGIC = b"Z80TOK01"
FORM_CARDINALITY = 1024  # FormId < 1024 (kNoForm 0xFFFF maps to 0)

PLAN_SEED_LABEL = "seg051-sealed-v1:"
POSITIVE, NEGATIVE, MASKED = 1, 0, -1


# ---------------------------------------------------------------------------------------------------------------------
# lineage registry and split
# ---------------------------------------------------------------------------------------------------------------------
def sealed_order(lineage_ids):
    """Deterministic sealed-cohort order, fixed before any model exists: SHA-256 of a constant label and the lineage id."""
    return sorted(lineage_ids, key=lambda n: hashlib.sha256((PLAN_SEED_LABEL + n).encode()).hexdigest())


def split_lineages(lineage_ids, sealed_count=3):
    order = sealed_order(lineage_ids)
    return sorted(order[sealed_count:]), sorted(order[:sealed_count])


def assert_split(train, held_out, sealed, lineage_of):
    """Leakage guard: a source lineage never appears on both sides, and sealed lineages never train or calibrate."""
    train_l = {lineage_of[t] for t in train}
    test_l = {lineage_of[t] for t in held_out}
    sealed_l = {lineage_of[t] for t in sealed}
    if train_l & test_l:
        raise ValueError("lineage leakage: " + ",".join(sorted(train_l & test_l)))
    if train_l & sealed_l:
        raise ValueError("sealed lineage in the training set: " + ",".join(sorted(train_l & sealed_l)))
    return True


# ---------------------------------------------------------------------------------------------------------------------
# decode-token report (Z80 CPU owned) and cell labels
# ---------------------------------------------------------------------------------------------------------------------
def parse_token_report(blob: bytes):
    """-> (kind[], length[], form[]) per image position."""
    if blob[:8] != TOKEN_MAGIC:
        raise ValueError("bad token report magic")
    n = struct.unpack("<I", blob[8:12])[0]
    if len(blob) != 12 + 4 * n:
        raise ValueError("token report length mismatch")
    kind = blob[12::4][:n]
    length = blob[13::4][:n]
    form = [struct.unpack_from("<H", blob, 12 + 4 * i + 2)[0] for i in range(n)]
    return bytes(kind), bytes(length), form


def form_token(form: int) -> int:
    """0 = no form; 1.. = form id + 1 (bounded)."""
    return 0 if form >= FORM_CARDINALITY else form + 1


def cell_count(image_len: int) -> int:
    return (image_len + CELL_BYTES - 1) // CELL_BYTES


def cell_labels(classes: str):
    """Per-cell training/evaluation label from a truth class string.

    positive: contains at least one source-backed instruction start (`I`);
    negative: every byte is confident data (`D`);
    masked: anything else (continuation tails, ambiguous `.db`, unlisted, quarantined) - never a confident negative.
    """
    labels = []
    for c in range(cell_count(len(classes))):
        seg = classes[c * CELL_BYTES:(c + 1) * CELL_BYTES]
        if "I" in seg:
            labels.append(POSITIVE)
        elif seg and set(seg) == {"D"}:
            labels.append(NEGATIVE)
        else:
            labels.append(MASKED)
    return labels


# ---------------------------------------------------------------------------------------------------------------------
# ranking, FRF, policies
# ---------------------------------------------------------------------------------------------------------------------
def ranking(scores):
    """Cell indices best-first. Non-finite scores are rejected (fail closed); ties break by lower cell index."""
    for s in scores:
        if not math.isfinite(s):
            raise ValueError("non-finite score")
    return sorted(range(len(scores)), key=lambda i: (-scores[i], i))


def selected_cells(scores, budget: float):
    """Top ceil(budget * n) cells; a budget outside (0, 1] is rejected."""
    if not (0.0 < budget <= 1.0):
        raise ValueError("budget out of range")
    k = min(len(scores), max(1, math.ceil(budget * len(scores) - 1e-12)))
    return set(ranking(scores)[:k])


def frf(scores, labels):
    """Fraction of the ranking that must be kept to cover every positive cell (weakest positive's rank / cells)."""
    order = ranking(scores)
    rank_of = {cell: r + 1 for r, cell in enumerate(order)}
    worst = max((rank_of[i] for i, l in enumerate(labels) if l == POSITIVE), default=0)
    return worst / len(scores)


def recall_at(scores, labels, budget: float):
    chosen = selected_cells(scores, budget)
    pos = [i for i, l in enumerate(labels) if l == POSITIVE]
    missed = [i for i in pos if i not in chosen]
    return len(pos) - len(missed), len(pos)


def ceil_to(x: float, step: float) -> float:
    return min(1.0, math.ceil(x / step - 1e-9) * step)


FIXED_MARGIN = 0.05
FIXED_STEP = 0.05


def fixed_budget(dev_oof_frfs):
    """Fixed rank budget calibrated only from development out-of-fold FRFs: max + margin, rounded up to the step."""
    return ceil_to(max(dev_oof_frfs) + FIXED_MARGIN, FIXED_STEP)


ADAPT_TAU = 0.5
ADAPT_DELTA = 0.05
ADAPT_MULTIPLIERS = [1.0 + 0.25 * i for i in range(0, 13)]  # 1.0 .. 4.0
ADAPT_FLOOR = 0.20


def confident_fraction(probs, tau: float = ADAPT_TAU) -> float:
    """Inference-available statistic: fraction of cells whose score probability is at least tau."""
    return sum(1 for p in probs if p >= tau) / len(probs)


def adaptive_budget(probs, multiplier: float) -> float:
    return min(1.0, max(ADAPT_FLOOR, multiplier * confident_fraction(probs) + ADAPT_DELTA))


def calibrate_adaptive(dev_oof):
    """dev_oof: [(probs, frf)] -> smallest multiplier covering every development FRF, or None (not calibratable)."""
    for m in ADAPT_MULTIPLIERS:
        if all(adaptive_budget(p, m) >= f - 1e-12 for p, f in dev_oof):
            return m
    return None


# ---------------------------------------------------------------------------------------------------------------------
# plan identity
# ---------------------------------------------------------------------------------------------------------------------
def canonical_digest(obj) -> str:
    return hashlib.sha256(json.dumps(obj, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
