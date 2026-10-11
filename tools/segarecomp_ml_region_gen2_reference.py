#!/usr/bin/env python3
"""SEG-049 (ADR 0100): dependency-free (Python standard library only) reference evaluator of the gen2 CNN artifact.

Purpose: artifact / parity verification and a readable specification for the later native build-time inference task. NOT a production
inference path and not a neural framework: it evaluates exactly the one frozen architecture (`CNN_ARCH` in
`segarecomp_ml_region_gen2_train.py`) in eval mode, one 512-word (1024-byte) context at a time, with IEEE-754 doubles.

Weights are the PyTorch state dict as nested lists under their state-dict names (decimal text with 9 significant digits, which round
trips every float32 weight exactly).
"""
from __future__ import annotations

import math
import operator

BN_EPS = 1e-5
CONTEXT_WORDS = 512
FINAL_LEN, CELL_LO, CELL_LEN = 226, 97, 32
TOKEN_FIELDS = ("status", "length", "family", "control", "ea_src", "ea_dst", "width", "flags")


def conv1d_valid(x, weight, bias, dilation):
    """x: channel-major lists [cin][L]; weight [cout][cin][k]; valid padding."""
    cin, k = len(weight[0]), len(weight[0][0])
    length = len(x[0]) - dilation * (k - 1)
    flat = [[w for c in range(cin) for w in row[c]] for row in weight]  # [cout][cin*k] in (c, tap) order
    out = [[0.0] * length for _ in weight]
    for t in range(length):
        vec = [x[c][t + tap * dilation] for c in range(cin) for tap in range(k)]
        for o, wo in enumerate(flat):
            out[o][t] = bias[o] + sum(map(operator.mul, wo, vec))
    return out


def batch_norm_relu(x, gamma, beta, mean, var):
    out = []
    for c, row in enumerate(x):
        scale = gamma[c] / math.sqrt(var[c] + BN_EPS)
        shift = beta[c] - mean[c] * scale
        out.append([v * scale + shift if v * scale + shift > 0.0 else 0.0 for v in row])
    return out


def cnn_logit(w: dict, hi, lo, tok=None) -> float:
    """Logit of the 128-byte cell at words [224, 288) of one 512-word context. `tok`: 512 rows of 8 token values (None = no token channels)."""
    if len(hi) != CONTEXT_WORDS or len(lo) != CONTEXT_WORDS or (tok is not None and len(tok) != CONTEXT_WORDS):
        raise ValueError("context must be 512 words")
    rows = []
    for p in range(CONTEXT_WORDS):
        vec = list(w["emb_hi.weight"][hi[p]]) + list(w["emb_lo.weight"][lo[p]])
        if tok is not None:
            for i in range(len(TOKEN_FIELDS)):
                vec += w[f"tok.{i}.weight"][tok[p][i]]
        rows.append(vec)
    x = [[rows[p][c] for p in range(CONTEXT_WORDS)] for c in range(len(rows[0]))]
    x = batch_norm_relu(conv1d_valid(x, w["stem.weight"], w["stem.bias"], 1), w["bn1.weight"], w["bn1.bias"], w["bn1.running_mean"], w["bn1.running_var"])
    x = batch_norm_relu(conv1d_valid(x, w["b2.weight"], w["b2.bias"], 2), w["bn2.weight"], w["bn2.bias"], w["bn2.running_mean"], w["bn2.running_var"])
    x = [[max(row[2 * i], row[2 * i + 1]) for i in range(len(row) // 2)] for row in x]
    x = batch_norm_relu(conv1d_valid(x, w["b3.weight"], w["b3.bias"], 2), w["bn3.weight"], w["bn3.bias"], w["bn3.running_mean"], w["bn3.running_var"])
    x = batch_norm_relu(conv1d_valid(x, w["b4.weight"], w["b4.bias"], 4), w["bn4.weight"], w["bn4.bias"], w["bn4.running_mean"], w["bn4.running_var"])
    if len(x[0]) != FINAL_LEN:
        raise ValueError("unexpected valid-convolution geometry")
    feat = []
    for row in x:
        cell = row[CELL_LO:CELL_LO + CELL_LEN]
        feat += [sum(cell) / CELL_LEN]
    for row in x:
        feat += [max(row[CELL_LO:CELL_LO + CELL_LEN])]
    for row in x:
        feat += [sum(row) / FINAL_LEN]
    for row in x:
        feat += [max(row)]
    hidden = [max(0.0, b + sum(map(operator.mul, wr, feat))) for wr, b in zip(w["fc1.weight"], w["fc1.bias"])]
    return w["fc2.bias"][0] + sum(map(operator.mul, w["fc2.weight"][0], hidden))


def probability(logit: float) -> float:
    return 1.0 / (1.0 + math.exp(-max(-700.0, min(700.0, logit))))
