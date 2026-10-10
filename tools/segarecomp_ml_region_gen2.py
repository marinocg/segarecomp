#!/usr/bin/env python3
"""SEG-049 (ADR 0100): second-generation cross-title ML executable-region experiment -- stdlib-only core.

OFFLINE RESEARCH TOOLING. Never a production route, never runs in generated code, does not change the production Optimized AOT model
or policy (SEG-047 v1 artifacts are byte-identical, see `V1_ARTIFACT_SHA256`).

This module owns everything that must be exactly checkable without numpy/torch/scikit-learn (so it runs in hermetic CI):

  * the representation contract: 128-byte decision cells, a 1024-byte (512 word) context, ROM-edge word-block reflection, the
    CPU-owned decode-token schema (tokens come from the C++ `segarecomp m68k-decode-token-report`; Python never decodes M68K);
  * leakage guards: forbidden input channel names (title / address / ordinal / ROM position / decoded target ...), title-level split
    isolation, training-only mining and calibration;
  * the evaluation arithmetic: cell labels, the threshold-independent full-recall fraction (FRF), threshold policies Q0..Q3,
    inner-title-LOTO calibration, R-versus-K attribution, model selection order and the final classification.

The heavy numeric part (features, CNN, boosted trees) lives in `segarecomp_ml_region_gen2_train.py` and imports this module for every
definition, so there is exactly one definition of every rule.
"""
from __future__ import annotations

import hashlib
import json
import math
import pathlib
import re

REPRESENTATION_VERSION = "seg049-gen2-repr-v1"
CELL_BYTES = 128
CELL_WORDS = CELL_BYTES // 2
CONTEXT_BYTES = 1024
CONTEXT_WORDS = CONTEXT_BYTES // 2
CONTEXT_PAD_WORDS = (CONTEXT_WORDS - CELL_WORDS) // 2  # 224 words (448 bytes) on each side of the cell
BLOCK_WORDS = 32  # 64-byte blocks: the common alignment of the cell (2 blocks), the 512-B mid scale and the 1024-B context
SEED = 49

TOKEN_SCHEMA_NAME = "segarecomp.m68k_decode_tokens.v1"
TOKEN_FIELDS = ("status", "length", "family", "control", "ea_src", "ea_dst", "width", "flags")
TOKEN_CARDINALITY = {"status": 3, "length": 6, "family": 8, "control": 8, "ea_src": 13, "ea_dst": 13, "width": 4, "flags": 4}
TOKEN_HEADER = "{} positions {{n}} fields {}".format(TOKEN_SCHEMA_NAME, " ".join(TOKEN_FIELDS))
TOKEN_DEFINITION = {
    "status": "0 rejected by the decoder, 1 architectural exception form (illegal / line A / line F), 2 ordinary instruction",
    "length": "instruction length in words 1..4, 5 = five or more; 0 when status = 0",
    "family": "1 move, 2 arithmetic/compare, 3 logic/bit, 4 shift/rotate, 5 control-flow, 6 exception-raising, 7 other; 0 when status = 0",
    "control": ("0 none, 1 conditional (Bcc/DBcc), 2 direct jump, 3 direct call, 4 dynamic jump, 5 dynamic call, 6 return, "
                "7 always-exception or trap"),
    "ea_src": "M68kEaMode ordinal of the source operand (0 unused .. 12 pc_index8)",
    "ea_dst": "M68kEaMode ordinal of the destination operand (0 unused .. 12 pc_index8)",
    "width": "operand width when an operand EA exists: 1 byte, 2 word, 3 long; 0 otherwise",
    "flags": "bit0 privileged instruction, bit1 PC-relative displacement control form",
    "never_emitted": "decoded target address, ROM position, ordinal, title, symbol, source label, runtime frequency",
}
# Model input channels (names are what the leakage guard inspects). Raw bytes are the hi/lo byte of each word position.
BYTE_CHANNELS = ("byte_hi", "byte_lo")
TOKEN_CHANNELS = tuple("tok_" + f for f in TOKEN_FIELDS)
FORBIDDEN_FRAGMENTS = ("addr", "offset", "ordinal", "index", "window_start", "position", "pos_", "title", "path", "name", "symbol", "label",
                       "source", "exec", "coverage", "oracle", "pc_", "frame", "rom_size", "sha", "target", "absolute", "first", "last",
                       "edge", "rel_pos", "runtime", "truth", "cell_id", "ordinal")

SHA = re.compile(r"[0-9a-f]{64}")
POLICY_FACTORS = {"Q0": 1.0, "Q1": 0.5, "Q2": 0.25, "Q3": 0.1}
FRF_VIABLE = 0.60
FRF_PREFERRED = 0.45
FRF_STRONG_MEAN = 0.35
R_OVER_ROM_GATE = 0.60
R_OVER_ROM_PREFERRED = 0.50
MATERIAL_FRF_MARGIN = 0.02  # a more complex candidate must beat the simpler by this much (worst-title, then mean FRF) to be preferred

CORPUS = {  # the five SEG-048 exact source-backed titles (identities only; truth stays private)
    "s1": {"title": "Sonic the Hedgehog (REV00)", "rom_sha256": "46160baa06362c711c9f1a5017cb7371026444936c8af5e93a78996cf32ff2a6",
           "source": "github.com/sonicretro/s1disasm", "revision": "7ebe4b3d0c182b2566026b6d3f423d33539558da", "truth_starts": 24180, "truth_sha256": "fd21b96bc0d79d62152fa172e2609026822b577d126ddebe5e7f70888127fc13"},
    "flicky": {"title": "Flicky", "rom_sha256": "4df1a91e08376ae773a6eb8e5abe310f94ca39d8f81583b91a9477fedcb68c11",
               "source": "github.com/oranguthang/flicky_src", "revision": "d9a0d4799c25fb23ba6251306508ee40a1ce9a89", "truth_starts": 5707, "truth_sha256": "33916a8e6800a10aa3601a4c35ce5ca929067876994dd9e4c9b1c36e9cf8d6e0"},
    "sf1": {"title": "Shining Force", "rom_sha256": "6c43b41b60ae8147945b4420caeb5eb266e1dd57a0d125b7496fa8fb6fde522d",
            "source": "github.com/ShiningForceCentral/SF1DISASM", "revision": "1f466bf038a64f6bdf3113e484bace0c3c66c6f7",
            "truth_starts": 44740, "truth_sha256": "7f5289dce66deecde1d3a9aa18e5ec7992ef055deb2fe9bee6936a4fd9387059"},
    "ps2": {"title": "Phantasy Star II (REV02)", "rom_sha256": "a0bd97f5aaa67301923ccc367511606bd3471ea9542919165311d4a23a2b696b",
            "source": "github.com/wyatt8740/ps2disasm", "revision": "c23877b8aa090473562ae2ed571ace2869289167", "truth_starts": 19261, "truth_sha256": "87174c5685eddc09c7ea00ac826e2ec1d5d5c470548529a2b3d25a77a17dea96"},
    "sk": {"title": "Sonic & Knuckles", "rom_sha256": "6e12e6b33c26ebfcd0be433251d21cf6284eafe9f71b027bda3767ae59affec1",
           "source": "github.com/sonicretro/skdisasm", "revision": "044fa46725c71187399e13f5ddb70e11d32dc024", "truth_starts": 119319, "truth_sha256": "76be1cccdbb5160e4c61830fe29c41c01a213bc3c04944baded51e7cedf57b72"},
}
TITLE_ORDER = ("s1", "flicky", "sf1", "ps2", "sk")

# SHA-256 of the production v1 artifacts at the SEG-049 baseline (product a7d0d59); they must never change in this experiment.
V1_ARTIFACT_SHA256 = {
    "tools/segarecomp_ml_region.model.json": "5863742d3b84a934d7ab335695a3748426de53e43ddc9d42ca679a6fc9fab0a2",
    "tools/segarecomp_ml_region.frozen.json": "3ef087800bd90b1de312c901aa305f5aa4ca61e8fd013348bc0b4f5471f016e1",
    "tools/segarecomp_ml_region.parity.json": "2165e2522378496d2cfa402a503622a6b0208236efb41fbdbffbcad869235ac9",
    "tools/segarecomp_ml_region.py": "b4b801b8fe3b7ef6d3593ad454e2a4ac4123db8b760b62e084b84c34a14531e8",
    "tools/segarecomp_ml_region_native_data.py": "df8eb816e3b79d8fcd42c9d87222de5603cce269b1251e3fb87ef495a0210cc6",
    "tools/segarecomp_ml_region_native_parity.py": "61f2520b7322a5bebba6ef2807353d2411507618bb440721d0e17058d82da875",
}


# --------------------------------------------------------------------------------------------------------------------- identity
def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path) -> str:
    return sha256_bytes(pathlib.Path(path).read_bytes())


def canonical_json(obj) -> str:
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), allow_nan=False)


def token_schema_sha256() -> str:
    return sha256_bytes(canonical_json({"schema": TOKEN_SCHEMA_NAME, "fields": TOKEN_FIELDS, "cardinality": TOKEN_CARDINALITY,
                                        "definition": TOKEN_DEFINITION}).encode())


def representation_sha256() -> str:
    return sha256_bytes(canonical_json({
        "version": REPRESENTATION_VERSION, "cell_bytes": CELL_BYTES, "context_bytes": CONTEXT_BYTES, "pad_words": CONTEXT_PAD_WORDS,
        "edge": "word-block symmetric reflection (period 2n); bytes inside a word keep order", "token_schema": token_schema_sha256(),
        "byte_channels": BYTE_CHANNELS, "token_channels": TOKEN_CHANNELS}).encode())


def v1_artifacts_unchanged(root) -> list[str]:
    """Names of production v1 artifacts whose bytes differ from the baseline digests (must be empty)."""
    base = pathlib.Path(root)
    return sorted(name for name, want in V1_ARTIFACT_SHA256.items()
                  if not (base / name).is_file() or sha256_file(base / name) != want)


# ------------------------------------------------------------------------------------------------------------------- leakage guard
def assert_input_channels(names) -> None:
    """Reject any model input channel that could carry title / address / ordinal / ROM-position / decoded-target / label information."""
    names = list(names)
    if len(set(names)) != len(names):
        raise SystemExit("forbidden channel schema: duplicate channel")
    allowed = set(BYTE_CHANNELS) | set(TOKEN_CHANNELS)
    for name in names:
        lowered = name.lower()
        if any(fragment in lowered for fragment in FORBIDDEN_FRAGMENTS):
            raise SystemExit(f"forbidden input channel (leakage risk): {name}")
        if name not in allowed:
            raise SystemExit(f"unknown input channel (not part of {REPRESENTATION_VERSION}): {name}")


def assert_token_schema_clean() -> None:
    for field in TOKEN_FIELDS:
        if any(fragment in field for fragment in ("addr", "target", "offset", "position", "ordinal", "title", "symbol", "label")):
            raise SystemExit(f"token schema field carries forbidden information: {field}")
    if set(TOKEN_CARDINALITY) != set(TOKEN_FIELDS):
        raise SystemExit("token cardinality table does not match the token fields")


def assert_split(train_ids, held_out, *, calibration_ids=(), scaler_ids=(), mining_ids=(), early_stop_ids=()) -> None:
    """The held-out title contributes nothing to fitting, normalization, hard-positive mining, calibration or early stopping."""
    if not train_ids or held_out in train_ids:
        raise SystemExit("title split leak: held-out title is part of (or the split is missing) the training set")
    if len(set(train_ids)) != len(list(train_ids)):
        raise SystemExit("title split: duplicate training title")
    for what, ids in (("calibration", calibration_ids), ("normalization", scaler_ids), ("hard-positive mining", mining_ids),
                      ("early stopping", early_stop_ids)):
        if held_out in ids:
            raise SystemExit(f"title split leak: held-out title used for {what}")
        if not set(ids) <= set(train_ids):
            raise SystemExit(f"title split leak: {what} uses a title outside the training set")


def loto_folds(ids=TITLE_ORDER):
    """Five-way (n-way) title-held-out folds: (held_out, training ids)."""
    ids = list(ids)
    if len(set(ids)) != len(ids) or len(ids) < 2:
        raise SystemExit("invalid title set")
    return [(h, [t for t in ids if t != h]) for h in ids]


def inner_folds(train_ids):
    """Inner title-LOTO of one outer fold: each training title is scored by a model trained on the other training titles."""
    train_ids = list(train_ids)
    if len(train_ids) < 3:
        raise SystemExit("inner calibration needs at least three training titles")
    return [(t, [u for u in train_ids if u != t]) for t in train_ids]


# ---------------------------------------------------------------------------------------------------------------------- tokens
def parse_token_report(data: bytes, rom_size: int) -> bytes:
    """Validate the C++ token report and return the raw `positions * 8` bytes."""
    newline = data.find(b"\n")
    if newline < 0:
        raise SystemExit("invalid decode token report")
    positions = rom_size // 2
    if data[:newline].decode("ascii", "replace") != TOKEN_HEADER.format(n=positions):
        raise SystemExit("invalid decode token report header")
    body = data[newline + 1:newline + 1 + positions * len(TOKEN_FIELDS)]
    if len(body) != positions * len(TOKEN_FIELDS) or data[newline + 1 + len(body):] != b"end\n":
        raise SystemExit("invalid decode token report body")
    for column, field in enumerate(TOKEN_FIELDS):
        if any(v >= TOKEN_CARDINALITY[field] for v in set(body[column::len(TOKEN_FIELDS)])):
            raise SystemExit(f"decode token out of range for field {field}")
    return body


# ----------------------------------------------------------------------------------------------------------- cells, context, edges
def cell_count(rom_size: int) -> int:
    if rom_size <= 0 or rom_size % 2:
        raise SystemExit("invalid ROM size")
    return (rom_size + CELL_BYTES - 1) // CELL_BYTES


def cell_labels(truth, rom_size: int) -> list[int]:
    """1 iff the 128-byte decision cell contains at least one source-backed instruction start (evaluation/training label only)."""
    labels = [0] * cell_count(rom_size)
    for address in truth:
        if address & 1 or not 0 <= address < rom_size:
            raise SystemExit("invalid truth address")
        labels[address // CELL_BYTES] = 1
    return labels


def reflect_index(i: int, n: int) -> int:
    """Symmetric reflection with period 2n (defined for any integer, including ROMs shorter than the pad)."""
    if n <= 0:
        raise SystemExit("empty sequence")
    m = i % (2 * n)
    return m if m < n else 2 * n - 1 - m


def context_word_indices(cell: int, word_count: int) -> list[int]:
    """The 512 source word indices of the 1024-byte context of `cell` (cell words at positions 224..287), reflected at the ROM edges."""
    if not 0 <= cell < (word_count + CELL_WORDS - 1) // CELL_WORDS:
        raise SystemExit("cell outside ROM")
    start = cell * CELL_WORDS - CONTEXT_PAD_WORDS
    return [reflect_index(start + k, word_count) for k in range(CONTEXT_WORDS)]


def context_bytes_of(rom: bytes, cell: int) -> bytes:
    words = context_word_indices(cell, len(rom) // 2)
    return b"".join(rom[2 * w:2 * w + 2] for w in words)


def context_tokens_of(tokens: bytes, rom_size: int, cell: int) -> list[tuple[int, ...]]:
    n = len(TOKEN_FIELDS)
    return [tuple(tokens[w * n:(w + 1) * n]) for w in context_word_indices(cell, rom_size // 2)]


def seed_cells(address_text: str, rom_size: int) -> set[int]:
    """Cells holding a precise direct-control-discovery identity (the production 'certain code' union, at cell granularity)."""
    out = set()
    for token in address_text.split():
        address = int(token, 16)
        if 0 <= address < rom_size:
            out.add(address // CELL_BYTES)
    return out


def regions_text(rom_sha256: str, rom_size: int, cells, cell_bytes: int = CELL_BYTES) -> str:
    if not SHA.fullmatch(rom_sha256) or rom_size <= 0 or rom_size % 2 or not cells:
        raise SystemExit("invalid region proposal inputs")
    runs: list[list[int]] = []
    for c in sorted(cells):
        begin, end = c * cell_bytes, min((c + 1) * cell_bytes, rom_size)
        if begin >= rom_size:
            raise SystemExit("cell outside rom")
        if runs and runs[-1][1] == begin:
            runs[-1][1] = end
        else:
            runs.append([begin, end])
    return ("segarecomp.m68k_executable_regions.v1\n" f"rom_sha256 {rom_sha256}\n"
            + "".join(f"range {b:08x} {e:08x}\n" for b, e in runs) + "end\n")


# ---------------------------------------------------------------------------------------------------------------------- metrics
def frf(scores, labels) -> dict:
    """Full-recall fraction: the smallest fraction of cells (taken in descending model score) that contains every positive cell.

    Cells tied with the lowest-scoring positive are all counted (pessimistic, deterministic). Evaluation metric only: it reads labels.
    """
    scores, labels = list(scores), list(labels)
    if len(scores) != len(labels) or not scores:
        raise SystemExit("frf: score/label length mismatch")
    positives = [s for s, y in zip(scores, labels) if y]
    if not positives:
        raise SystemExit("frf: title has no positive cell")
    worst = min(positives)
    needed = sum(1 for s in scores if s >= worst)
    return {"frf": needed / len(scores), "cells_needed": needed, "cells": len(scores), "min_positive_score": worst,
            "positive_cells": len(positives)}


def selected_cells(scores, threshold: float) -> set[int]:
    return {c for c, s in enumerate(scores) if s >= threshold}


def c_outside_cells(truth, cells) -> list[int]:
    """Truth instruction starts whose 128-byte cell is not in `cells` (C outside R when `cells` is R)."""
    cells = set(cells)
    return sorted(a for a in truth if a // CELL_BYTES not in cells)


def region_bytes(cells, rom_size: int) -> int:
    return sum(min((c + 1) * CELL_BYTES, rom_size) - c * CELL_BYTES for c in cells)


def attribute(truth, r_cells, k_addresses, validator_accepted: bool) -> dict:
    """Keep model and post-ML failures apart. Only `ml_false_negatives` (C outside R) is a classifier miss."""
    outside_r = set(c_outside_cells(truth, r_cells))
    if not validator_accepted or k_addresses is None:
        return {"ml_false_negatives": len(outside_r), "c_outside_r": len(outside_r), "c_outside_k": None, "post_ml_losses": None,
                "post_ml_status": "VALIDATOR_REJECTED_PROPOSAL"}
    outside_k = set(truth) - set(k_addresses)
    post_ml = outside_k - outside_r
    return {"ml_false_negatives": len(outside_r), "c_outside_r": len(outside_r), "c_outside_k": len(outside_k),
            "post_ml_losses": len(post_ml), "post_ml_status": "POST_ML_PRUNING_BLOCKER" if post_ml else "OK"}


# ------------------------------------------------------------------------------------------------------------------ calibration
def threshold_for(min_positive_score: float, factor: float) -> float:
    if not (0.0 < factor <= 1.0) or not (min_positive_score > 0.0):
        raise SystemExit("invalid threshold policy input")
    return min_positive_score * factor


def inner_oof_min_positive(inner_scores: dict, inner_labels: dict, *, train_ids, held_out) -> float:
    """Minimum positive-source score over the inner title-held-out predictions of the OUTER TRAINING titles only.

    `inner_scores[t]` is the score vector of training title t from a model trained on the other training titles. The outer held-out
    title (and its scores) is rejected if it appears anywhere.
    """
    if held_out in inner_scores or held_out in inner_labels or held_out in train_ids:
        raise SystemExit("calibration leak: the outer held-out title appears in its own calibration")
    if set(inner_scores) != set(train_ids) or set(inner_labels) != set(train_ids):
        raise SystemExit("calibration needs exactly one inner held-out score vector per outer training title")
    best = math.inf
    for t in train_ids:
        if len(inner_scores[t]) != len(inner_labels[t]):
            raise SystemExit("calibration: score/label length mismatch")
        for s, y in zip(inner_scores[t], inner_labels[t]):
            if y:
                best = min(best, s)
    if not math.isfinite(best):
        raise SystemExit("calibration: no positive inner score")
    return best


def policy_passes(fold_records) -> dict:
    """Per policy: does it satisfy the R-stage gate on EVERY outer fold? (C outside R = 0 and R/ROM <= 0.60)."""
    out = {}
    for name in POLICY_FACTORS:
        records = [f["policies"][name] for f in fold_records]
        out[name] = all(r["c_outside_r"] == 0 and r["r_over_rom"] <= R_OVER_ROM_GATE for r in records)
    return out


def choose_policy(fold_records):
    """Highest factor (Q0 > Q1 > Q2 > Q3) passing all outer folds; None if no pre-registered policy qualifies."""
    passing = policy_passes(fold_records)
    for name in sorted(POLICY_FACTORS, key=lambda n: -POLICY_FACTORS[n]):
        if passing[name]:
            return name
    return None


# ----------------------------------------------------------------------------------------------------------------- selection/class
def select_model(table: dict, complexity: dict):
    """Pre-registered selection among the trainable candidates.

    `table[name] = {"frf": {title: value}}` (all five titles). Viable = every title FRF <= 0.60. Then lowest worst-title FRF; candidates
    within MATERIAL_FRF_MARGIN of the best worst-title FRF form a tie set in which a lower mean FRF only wins if it is lower by at least
    the margin; otherwise the smaller model (`complexity[name]`, a sortable tuple) wins.
    """
    viable = {n: v for n, v in table.items() if len(v["frf"]) == len(TITLE_ORDER) and max(v["frf"].values()) <= FRF_VIABLE}
    if not viable:
        return None
    worst = {n: max(v["frf"].values()) for n, v in viable.items()}
    mean = {n: sum(v["frf"].values()) / len(v["frf"]) for n, v in viable.items()}
    best = min(worst.values())
    tie = [n for n in viable if worst[n] <= best + MATERIAL_FRF_MARGIN]
    chosen = min(tie, key=lambda n: complexity[n])
    for n in sorted(tie, key=lambda n: complexity[n]):
        # a more complex candidate replaces the current choice only when materially better (mean, then worst) than every simpler one
        if complexity[n] > complexity[chosen]:
            if mean[n] <= mean[chosen] - MATERIAL_FRF_MARGIN or worst[n] <= worst[chosen] - MATERIAL_FRF_MARGIN:
                chosen = n
    return chosen


def classify(*, representation_viable: bool, policy: str | None, r_gate_all: bool, k_clean: bool, validator_all: bool) -> str:
    if not representation_viable:
        return "GEN2_ML_INSUFFICIENT"
    if policy is None or not r_gate_all:
        return "GEN2_REPRESENTATION_VALID_CALIBRATION_UNSOLVED"
    if k_clean and validator_all:
        return "GEN2_ML_READY_FOR_NATIVE_SPIKE"
    return "GEN2_ML_VALID_POSTPROCESS_BLOCKED"


# ----------------------------------------------------------------------------------------------------------------- hard positives
def hard_positive_cells(scores_by_title: dict, labels_by_title: dict, *, train_ids, held_out, quantile: float) -> dict:
    """Training-title positives whose score is in the lowest `quantile` of the pooled training-positive scores (M4 mining).

    Reads only the training titles; the held-out title is rejected if present. Returns `{title: set(cell)}` (runtime only: the cell
    identities are never persisted).
    """
    if held_out in scores_by_title or held_out in labels_by_title:
        raise SystemExit("hard-positive mining leak: held-out title scores supplied")
    if set(scores_by_title) != set(train_ids) or set(labels_by_title) != set(train_ids):
        raise SystemExit("hard-positive mining must use exactly the training titles")
    if not 0.0 < quantile < 1.0:
        raise SystemExit("invalid hard-positive quantile")
    pooled = sorted(s for t in train_ids for s, y in zip(scores_by_title[t], labels_by_title[t]) if y)
    if not pooled:
        raise SystemExit("no positive cells to mine")
    cutoff = pooled[min(len(pooled) - 1, int(quantile * len(pooled)))]
    return {t: {c for c, (s, y) in enumerate(zip(scores_by_title[t], labels_by_title[t])) if y and s < cutoff} for t in train_ids}


def prediction_digest(per_title_scores: dict) -> str:
    """Digest of float scores rounded to 9 decimals, title-ordered (deterministic across independent fits)."""
    h = hashlib.sha256()
    for t in sorted(per_title_scores):
        h.update(t.encode() + b"\0")
        for s in per_title_scores[t]:
            h.update(f"{s:.9e}".encode() + b"\n")
    return h.hexdigest()
