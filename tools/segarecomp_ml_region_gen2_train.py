#!/usr/bin/env python3
"""SEG-049 (ADR 0100): OFFLINE training / evaluation of the second-generation executable-region candidates M1..M4 (and the M0 comparator).

Research tooling in a PRIVATE environment (numpy / scikit-learn / PyTorch are NOT product dependencies and are imported lazily; the
release never needs them). Every definition (cells, context, edges, labels, FRF, policies, selection, leakage guards) lives in the
stdlib-only `segarecomp_ml_region_gen2.py`; this file only vectorises the same rules and runs the fixed, pre-registered plan.

No title-specific model, no held-out-title tuning: every command that fits a model receives an explicit training-title list and the
held-out title is rejected by `assert_split` before any array is built.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import pathlib
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import segarecomp_ml_region as v1  # noqa: E402
import segarecomp_ml_region_gen2 as g  # noqa: E402

PLAN_NAME = "segarecomp_ml_region_gen2.plan.json"
TOOLS = pathlib.Path(__file__).resolve().parent

# ----------------------------------------------------------------------------------------------------------- frozen plan constants
RECIPE = {
    "epochs": 30, "optimizer": "AdamW", "learning_rate": 0.002, "weight_decay": 0.0001, "betas": [0.9, 0.999],
    "lr_schedule": "cosine annealing to 0 over all steps (per-step)", "segment_cells": 32, "segments_per_batch": 8,
    "loss": "BCEWithLogits, pos_weight = (#negative cells)/(#positive cells) over the TRAINING titles, per-cell weight",
    "grad_clip_norm": 5.0, "seed": g.SEED, "torch_threads": 4, "dropout": 0.0,
    "epoch_definition": "per training title a random cell offset in [0, 32) (seeded) partitions the title into 32-cell segments; the "
                        "segment list of all training titles is shuffled and consumed in batches of 8",
    "hard_positive": {"applies_to": "M4 only", "mining_epochs": [10, 20], "quantile": 0.10, "loss_multiplier": 4.0,
                      "rule": "at the end of epochs 10 and 20, score the TRAINING titles only (eval mode); training positive cells whose "
                              "score is below the 10th percentile of the pooled training-positive scores get loss multiplier 4.0 "
                              "(weights are reset to 1 and re-mined at each mining epoch); cell identities are runtime-only"},
}
HGB_CONFIG = {"max_iter": 200, "max_depth": 4, "learning_rate": 0.08, "min_samples_leaf": 40, "l2_regularization": 1.0, "max_bins": 255,
              "early_stopping": False, "class_weight": "balanced", "random_state": g.SEED}
CNN_ARCH = {
    "byte_embedding": {"hi": [256, 8], "lo": [256, 8]},
    "token_embedding_dims": {"status": 2, "length": 2, "family": 3, "control": 3, "ea_src": 3, "ea_dst": 3, "width": 2, "flags": 2},
    "input": "word-resolution sequence of 512 positions (1024-byte context); per position concat(embed(hi byte), embed(lo byte)[, token embeddings])",
    "blocks": [
        {"name": "stem", "op": "Conv1d valid", "kernel": 5, "dilation": 1, "channels": 48, "norm": "BatchNorm1d", "act": "ReLU"},
        {"name": "b2", "op": "Conv1d valid", "kernel": 5, "dilation": 2, "channels": 48, "norm": "BatchNorm1d", "act": "ReLU"},
        {"name": "pool", "op": "MaxPool1d", "kernel": 2, "stride": 2},
        {"name": "b3", "op": "Conv1d valid", "kernel": 5, "dilation": 2, "channels": 64, "norm": "BatchNorm1d", "act": "ReLU"},
        {"name": "b4", "op": "Conv1d valid", "kernel": 5, "dilation": 4, "channels": 64, "norm": "BatchNorm1d", "act": "ReLU"}],
    "valid_geometry": "context 512 words -> stem 508 -> b2 500 -> pool 250 -> b3 242 -> b4 226 final positions; the cell occupies final "
                      "positions [97, 129) (32 positions = 64 words)",
    "head": "concat(mean over cell positions, max over cell positions, mean over all 226 context positions, max over all 226) = 256 -> "
            "Linear(256,64) -> ReLU -> Linear(64,1) -> logit; probability = sigmoid(logit)",
    "equivalence": "a valid-convolution trunk makes every cell's logit a function of exactly its 512-word context; whole-ROM/segment "
                   "evaluation is therefore identical to per-cell evaluation (checked by test)",
}
FINAL_LEN, CELL_LO, CELL_LEN = 226, 97, 32
EVAL_CELLS = 64
HARD_QUANTILE = RECIPE["hard_positive"]["quantile"]
MODELS = ("M1", "M2", "M3", "M4")
COMPLEXITY = {"M1": (0, 0), "M2": (1, 0), "M3": (1, 1), "M4": (1, 2)}  # sortable simplicity order (HGB < raw CNN < token CNN < hard-positive)
ROOT = pathlib.Path(os.environ.get("SEG049_ROOT", "/root/dev/segarecomp-harness/.cache/seg049"))
SEG048 = pathlib.Path(os.environ.get("SEG048_ROOT", "/root/dev/segarecomp-harness/.cache/seg048"))
GAMES = pathlib.Path(os.environ.get("SEG049_GAMES", "/root/dev/segarecomp-harness/segarecomp/games"))
CLI = pathlib.Path(os.environ.get("SEG049_CLI", str(ROOT / "build-rel/apps/segarecomp/segarecomp")))
ROM_FILES = {"s1": "Sonic The Hedgehog (USA, Europe).md", "flicky": "Flicky (JUE) [!].bin", "sf1": "Shining Force (U) [!].bin",
             "ps2": "Phantasy Star II (UE) (REV 02) [!].bin", "sk": "Sonic and Knuckles (JUE) [!].bin"}


def np_():
    import numpy
    return numpy


# --------------------------------------------------------------------------------------------------------------------------- titles
class Title:
    """One ROM with CPU-owned decode tokens, source-backed cell labels and the (private) broad-universe / seed artifacts."""

    def __init__(self, tid: str, with_truth: bool = True):
        np = np_()
        meta = g.CORPUS[tid]
        self.id = tid
        self.rom = (GAMES / ROM_FILES[tid]).read_bytes()
        self.sha = hashlib.sha256(self.rom).hexdigest()
        if self.sha != meta["rom_sha256"]:
            raise SystemExit(f"{tid}: ROM identity mismatch")
        self.size = len(self.rom)
        self.n_words = self.size // 2
        self.n_cells = g.cell_count(self.size)
        tok_path = ROOT / "tok" / f"{tid}.tok"
        if not tok_path.exists():
            tok_path.parent.mkdir(parents=True, exist_ok=True)
            subprocess.run([str(CLI), "m68k-decode-token-report", "--rom", str(GAMES / ROM_FILES[tid]), "--output", str(tok_path)], check=True)
        body = g.parse_token_report(tok_path.read_bytes(), self.size)
        self.tokens = np.frombuffer(body, dtype=np.uint8).reshape(self.n_words, len(g.TOKEN_FIELDS))
        arr = np.frombuffer(self.rom[:self.n_words * 2], dtype=np.uint8).reshape(self.n_words, 2)
        self.hi, self.lo = arr[:, 0], arr[:, 1]
        art = SEG048 / "art" / tid
        self.seeds = g.seed_cells((art / "d.txt").read_text(), self.size)
        self.universe = sorted(int(t, 16) for t in (art / "u.txt").read_text().split())
        self.truth = None
        self.labels = None
        self.truth_sha = None
        if with_truth:
            text = (SEG048 / "truth" / f"{tid}.1.u").read_text()
            truth = v1.read_universe(text, self.sha, self.size)
            if (len(truth) != meta["truth_starts"] or not truth <= set(self.universe)
                    or hashlib.sha256(text.encode()).hexdigest() != meta["truth_sha256"]):
                raise SystemExit(f"{tid}: truth identity/sanity mismatch")
            self.truth = sorted(truth)
            self.truth_sha = hashlib.sha256(text.encode()).hexdigest()
            self.labels = np.array(g.cell_labels(truth, self.size), dtype=np.uint8)
        self._padded = None

    def padded(self):
        """hi, lo (L,) and tokens (L,8) arrays with `n_cells*64 + 448` word positions; position j holds source word reflect(j - 224)."""
        if self._padded is None:
            np = np_()
            length = self.n_cells * g.CELL_WORDS + 2 * g.CONTEXT_PAD_WORDS
            idx = np.array([g.reflect_index(j - g.CONTEXT_PAD_WORDS, self.n_words) for j in range(length)], dtype=np.int64)
            self._padded = (self.hi[idx], self.lo[idx], self.tokens[idx])
        return self._padded


# ------------------------------------------------------------------------------------------------------------------ M1 features
# (name, blocks in window, blocks before the cell's first block). Cell = 2 blocks (128 B); mid = cell +-3 blocks (512 B); ctx = cell +-7 blocks (1024 B).
M1_SCALES = (("cell", 2, 0), ("mid", 8, 3), ("ctx", 16, 7))
TOKEN_BINS = sum(g.TOKEN_CARDINALITY[f] for f in g.TOKEN_FIELDS)


def m1_feature_names() -> list[str]:
    base = ["entropy", "zero", "ff", "printable", "unique", "word_repeat"] + [f"hist{i}" for i in range(8)]
    base += [f"tok_{f}_{v}" for f in g.TOKEN_FIELDS for v in range(g.TOKEN_CARDINALITY[f])]
    names = [f"{scale}_{b}" for scale, _, _ in M1_SCALES for b in base]
    for name in names:
        if any(frag in name for frag in g.FORBIDDEN_FRAGMENTS):
            raise SystemExit(f"forbidden M1 feature name: {name}")
    return names


def m1_features(title: Title):
    """Multi-scale aggregates of the 128-B cell / 512-B mid / 1024-B context windows, from 64-byte block statistics of the padded ROM."""
    np = np_()
    hi, lo, tok = title.padded()
    bw = g.BLOCK_WORDS
    nb = len(hi) // bw
    hist = np.zeros((nb, 256), dtype=np.int64)
    block_of_word = np.repeat(np.arange(nb), bw)
    for part in (hi, lo):
        hist += np.bincount(block_of_word * 256 + part.astype(np.int64), minlength=nb * 256).reshape(nb, 256)
    tokc = np.zeros((nb, TOKEN_BINS), dtype=np.int64)
    base = 0
    for col, f in enumerate(g.TOKEN_FIELDS):
        card = g.TOKEN_CARDINALITY[f]
        tokc[:, base:base + card] = np.bincount(block_of_word * card + tok[:, col].astype(np.int64), minlength=nb * card).reshape(nb, card)
        base += card
    words = (hi.astype(np.int64) << 8) | lo
    same = (words[1:] == words[:-1])
    same = np.concatenate([[False], same])
    same[::bw] = False  # pairs never cross a block start
    rep = same.reshape(nb, bw).sum(axis=1)

    def csum(a):
        out = np.zeros((a.shape[0] + 1,) + a.shape[1:], dtype=np.int64)
        np.cumsum(a, axis=0, out=out[1:])
        return out

    ch, ct, cr = csum(hist), csum(tokc), csum(rep)
    cells = title.n_cells
    feats = []
    for _, blocks, lead in M1_SCALES:
        start = 7 + 2 * np.arange(cells) - lead  # padded block index of the cell's first block is 7 + 2*cell
        end = start + blocks
        h = (ch[end] - ch[start]).astype(np.float64)
        t = (ct[end] - ct[start]).astype(np.float64)
        r = (cr[end] - cr[start]).astype(np.float64)
        n = float(blocks * bw * 2)
        p = h / n
        with np.errstate(divide="ignore", invalid="ignore"):
            ent = -np.where(p > 0, p * np.log2(np.where(p > 0, p, 1.0)), 0.0).sum(axis=1) / 8.0
        printable = h[:, 0x20:0x7F].sum(axis=1) / n
        uniq = (h > 0).sum(axis=1) / 256.0
        rep_frac = r / (blocks * bw - blocks)
        buckets = np.stack([h[:, i * 32:(i + 1) * 32].sum(axis=1) / n for i in range(8)], axis=1)
        tk = t / float(blocks * bw)
        feats.append(np.column_stack([ent, h[:, 0] / n, h[:, 255] / n, printable, uniq, rep_frac, buckets, tk]))
    return np.concatenate(feats, axis=1).astype(np.float64)


# -------------------------------------------------------------------------------------------------------------------------- CNN
def build_model(use_tokens: bool):
    import torch
    from torch import nn

    class Cell(nn.Module):
        def __init__(self):
            super().__init__()
            self.use_tokens = use_tokens
            self.emb_hi, self.emb_lo = nn.Embedding(256, 8), nn.Embedding(256, 8)
            cin = 16
            if use_tokens:
                self.tok = nn.ModuleList([nn.Embedding(g.TOKEN_CARDINALITY[f], CNN_ARCH["token_embedding_dims"][f]) for f in g.TOKEN_FIELDS])
                cin += sum(CNN_ARCH["token_embedding_dims"][f] for f in g.TOKEN_FIELDS)
            self.stem, self.bn1 = nn.Conv1d(cin, 48, 5), nn.BatchNorm1d(48)
            self.b2, self.bn2 = nn.Conv1d(48, 48, 5, dilation=2), nn.BatchNorm1d(48)
            self.b3, self.bn3 = nn.Conv1d(48, 64, 5, dilation=2), nn.BatchNorm1d(64)
            self.b4, self.bn4 = nn.Conv1d(64, 64, 5, dilation=4), nn.BatchNorm1d(64)
            self.fc1, self.fc2 = nn.Linear(256, 64), nn.Linear(64, 1)

        def trunk(self, hi, lo, tok):
            parts = [self.emb_hi(hi), self.emb_lo(lo)]
            if self.use_tokens:
                parts += [emb(tok[:, :, i]) for i, emb in enumerate(self.tok)]
            x = torch.cat(parts, dim=2).transpose(1, 2)
            x = torch.relu(self.bn1(self.stem(x)))
            x = torch.relu(self.bn2(self.b2(x)))
            x = torch.nn.functional.max_pool1d(x, 2)
            x = torch.relu(self.bn3(self.b3(x)))
            return torch.relu(self.bn4(self.b4(x)))

        def forward(self, hi, lo, tok):
            """hi, lo: (B, L) int64; tok: (B, L, 8) int64 (ignored without tokens). L = 64*S + 448 words -> (B, S) logits."""
            f = self.trunk(hi, lo, tok)
            s = (f.shape[2] - FINAL_LEN) // CELL_LEN + 1
            cell = f[:, :, CELL_LO:CELL_LO + CELL_LEN * s]
            feat = torch.cat([torch.nn.functional.avg_pool1d(cell, CELL_LEN, CELL_LEN), torch.nn.functional.max_pool1d(cell, CELL_LEN, CELL_LEN),
                              torch.nn.functional.avg_pool1d(f, FINAL_LEN, CELL_LEN), torch.nn.functional.max_pool1d(f, FINAL_LEN, CELL_LEN)],
                             dim=1).transpose(1, 2)
            return self.fc2(torch.relu(self.fc1(feat))).squeeze(-1)

    return Cell()


def parameter_count(model) -> int:
    return sum(p.numel() for p in model.parameters() if p.requires_grad)


def _batch(title: Title, starts, cells: int, use_tokens: bool):
    import torch
    np = np_()
    hi, lo, tok = title.padded()
    span = cells * g.CELL_WORDS + 2 * g.CONTEXT_PAD_WORDS
    idx = [slice(s * g.CELL_WORDS, s * g.CELL_WORDS + span) for s in starts]
    h = torch.from_numpy(np.stack([hi[i] for i in idx]).astype(np.int64))
    l = torch.from_numpy(np.stack([lo[i] for i in idx]).astype(np.int64))
    t = torch.from_numpy(np.stack([tok[i] for i in idx]).astype(np.int64)) if use_tokens else None
    return h, l, t


def score_logits(model, title: Title, use_tokens: bool):
    """Whole-ROM logits (float64 array, one per cell), evaluated in chunks of EVAL_CELLS cells (identical to per-cell evaluation)."""
    import torch
    np = np_()
    model.eval()
    out = np.empty(title.n_cells, dtype=np.float64)
    with torch.no_grad():
        for start in range(0, title.n_cells, EVAL_CELLS):
            n = min(EVAL_CELLS, title.n_cells - start)
            h, l, t = _batch(title, [start], n, use_tokens)
            out[start:start + n] = model(h, l, t).squeeze(0).double().numpy()
    return out


def sigmoid(x):
    np = np_()
    return 1.0 / (1.0 + np.exp(-np.clip(x, -700.0, 700.0)))


def train_cnn(titles: dict, train_ids, use_tokens: bool, hard_positive: bool, log=print):
    import torch
    np = np_()
    torch.set_num_threads(RECIPE["torch_threads"])
    torch.use_deterministic_algorithms(True)
    torch.manual_seed(RECIPE["seed"])
    rng = np.random.RandomState(RECIPE["seed"])
    model = build_model(use_tokens)
    opt = torch.optim.AdamW(model.parameters(), lr=RECIPE["learning_rate"], weight_decay=RECIPE["weight_decay"], betas=tuple(RECIPE["betas"]))
    train_ids = list(train_ids)
    n_pos = sum(int(titles[t].labels.sum()) for t in train_ids)
    n_neg = sum(int(titles[t].n_cells - titles[t].labels.sum()) for t in train_ids)
    pos_weight = torch.tensor(n_neg / n_pos, dtype=torch.float32)
    weights = {t: np.ones(titles[t].n_cells, dtype=np.float32) for t in train_ids}
    seg, per_batch, epochs = RECIPE["segment_cells"], RECIPE["segments_per_batch"], RECIPE["epochs"]
    steps_per_epoch = None
    step = 0
    t0 = time.time()
    for epoch in range(1, epochs + 1):
        segments = []
        for t in train_ids:
            off = int(rng.randint(0, seg))
            segments += [(t, s) for s in range(off, titles[t].n_cells - seg + 1, seg)]
        order = rng.permutation(len(segments))
        batches = [order[i:i + per_batch] for i in range(0, len(order) - per_batch + 1, per_batch)]
        steps_per_epoch = steps_per_epoch or len(batches)
        total_steps = steps_per_epoch * epochs
        model.train()
        losses = []
        for b in batches:
            for g_ in opt.param_groups:
                g_["lr"] = 0.5 * RECIPE["learning_rate"] * (1 + math.cos(math.pi * min(step, total_steps) / total_steps))
            # segments of one batch may belong to different titles: build per segment then stack (equal length)
            hs, ls, ts, ys, ws = [], [], [], [], []
            for k in b:
                t, s = segments[int(k)]
                h, l, tk = _batch(titles[t], [s], seg, use_tokens)
                hs.append(h), ls.append(l), ts.append(tk)
                ys.append(torch.from_numpy(titles[t].labels[s:s + seg].astype(np.float32)))
                ws.append(torch.from_numpy(weights[t][s:s + seg]))
            h, l = torch.cat(hs), torch.cat(ls)
            tk = torch.cat(ts) if use_tokens else None
            y, w = torch.stack(ys), torch.stack(ws)
            logits = model(h, l, tk)
            loss = (torch.nn.functional.binary_cross_entropy_with_logits(logits, y, pos_weight=pos_weight, reduction="none") * w).mean()
            opt.zero_grad()
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), RECIPE["grad_clip_norm"])
            opt.step()
            losses.append(float(loss))
            step += 1
        log(f"epoch {epoch}/{epochs} loss {sum(losses) / len(losses):.5f} t={time.time() - t0:.0f}s")
        if hard_positive and epoch in RECIPE["hard_positive"]["mining_epochs"]:
            scores = {t: score_logits(model, titles[t], use_tokens) for t in train_ids}
            hard = g.hard_positive_cells(scores, {t: titles[t].labels for t in train_ids}, train_ids=train_ids, held_out="__none__",
                                         quantile=HARD_QUANTILE)
            for t in train_ids:
                weights[t][:] = 1.0
                weights[t][list(hard[t])] = RECIPE["hard_positive"]["loss_multiplier"]
            log(f"hard-positive mining at epoch {epoch}: {sum(len(v) for v in hard.values())} cells emphasised")
    return model


# -------------------------------------------------------------------------------------------------------------------------- HGB
def train_hgb(titles: dict, train_ids, feats: dict):
    np = np_()
    from sklearn.ensemble import HistGradientBoostingClassifier
    x = np.concatenate([feats[t] for t in train_ids])
    y = np.concatenate([titles[t].labels for t in train_ids])
    return HistGradientBoostingClassifier(**HGB_CONFIG).fit(x, y)


# ------------------------------------------------------------------------------------------------------------------------ scoring
def fit_and_score(model_name: str, titles: dict, train_ids, score_ids, log=print) -> dict:
    """Fit `model_name` on `train_ids` only and score `score_ids` (probabilities, float64 per cell). Returns {title: scores, '_meta': ...}."""
    for t in train_ids:
        if titles[t].labels is None:
            raise SystemExit("training title without labels")
    out = {}
    if model_name == "M1":
        feats = {t: m1_features(titles[t]) for t in set(train_ids) | set(score_ids)}
        clf = train_hgb(titles, train_ids, feats)
        for t in score_ids:
            out[t] = clf.predict_proba(feats[t])[:, 1]
        out["_meta"] = {"params": "trees"}
        return out
    use_tokens = model_name in ("M3", "M4")
    model = train_cnn(titles, train_ids, use_tokens, model_name == "M4", log)
    for t in score_ids:
        out[t] = sigmoid(score_logits(model, titles[t], use_tokens))
    out["_meta"] = {"params": parameter_count(model)}
    out["_model"] = model
    return out


# ------------------------------------------------------------------------------------------------------- v1 (M0) and characterization
def v1_scores(title: Title):
    """Frozen production v1 (M0): per-512-byte-window logit margins, zero-shot (no retraining). Returns (window logit-threshold margins, selected windows)."""
    model = json.loads((TOOLS / "segarecomp_ml_region.model.json").read_text())
    report = v1.parse_window_report((SEG048 / "art" / title.id / "w512.txt").read_text(), 512)
    rows, count = v1.window_matrix(title.rom, report, 512)
    logits = [v1.canonical_logit(model, r) for r in rows]
    prob = [1.0 / (1.0 + math.exp(-x)) if x > -700 else 0.0 for x in logits]
    return logits, prob, model["logit_threshold"], model["probability_threshold"]


def v1_cell_scores(title: Title):
    """M0 scores expanded to 128-byte cells (a cell inherits the probability of its 512-byte window)."""
    _, prob, _, threshold = v1_scores(title)
    return [prob[c // 4] for c in range(title.n_cells)], threshold


def characterize(titles: dict) -> dict:
    """Aggregate diagnosis of what v1's representation cannot expose: source-positive cells v1 selected by score vs cells it missed."""
    np = np_()
    groups = {"selected": [], "missed": []}
    per_title = {}
    for tid in g.TITLE_ORDER:
        t = titles[tid]
        logits, prob, lthr, pthr = v1_scores(t)
        win_selected = {w for w, x in enumerate(logits) if x >= lthr}
        win_seed = {c // 4 for c in t.seeds}
        code = np.zeros(t.size, dtype=bool)
        starts_by_cell = {}
        for a in t.truth:
            w = a // 2
            length = int(t.tokens[w, 1]) * 2 if t.tokens[w, 0] else 2
            code[a:min(a + max(length, 2), t.size)] = True
            starts_by_cell.setdefault(a // g.CELL_BYTES, []).append(a)
        pos = np.nonzero(t.labels)[0]
        neg = np.nonzero(t.labels == 0)[0]
        miss = [int(c) for c in pos if (c // 4) not in win_selected and (c // 4) not in win_seed]
        seed_only = [int(c) for c in pos if (c // 4) not in win_selected and (c // 4) in win_seed]
        sel = [int(c) for c in pos if (c // 4) in win_selected]
        per_title[tid] = {"positive_cells": int(len(pos)), "selected_by_score": len(sel), "seed_only": len(seed_only), "missed": len(miss),
                          "c_outside_r_v1": sum(len(starts_by_cell[c]) for c in miss), "zero_shot": tid != "s1"}
        neg_sorted = neg
        for name, cells in (("selected", sel), ("missed", miss)):
            for c in cells:
                a0 = c * g.CELL_BYTES
                chunk = t.rom[a0:a0 + g.CELL_BYTES]
                n = len(chunk)
                counts = np.bincount(np.frombuffer(chunk, dtype=np.uint8), minlength=256)
                p = counts[counts > 0] / n
                starts = starts_by_cell[c]
                tok = [t.tokens[a // 2] for a in starts]
                w0 = c * g.CELL_WORDS
                cell_tokens = t.tokens[w0:w0 + g.CELL_WORDS]
                covered = float(code[a0:a0 + n].mean())
                i = int(np.searchsorted(neg_sorted, c))
                dist = min([abs(int(neg_sorted[j]) - c) for j in (i - 1, i) if 0 <= j < len(neg_sorted)] or [10 ** 6])
                neigh = [t.labels[c + d] for d in range(-4, 5) if d and 0 <= c + d < t.n_cells]
                win_cells = [t.labels[(c // 4) * 4 + k] for k in range(4) if (c // 4) * 4 + k < t.n_cells]
                groups[name].append({
                    "title": tid, "instr_density": len(starts) / g.CELL_WORDS, "valid_decode": float((cell_tokens[:, 0] == 2).mean()),
                    "exception_form": float((cell_tokens[:, 0] == 1).mean()), "rejected": float((cell_tokens[:, 0] == 0).mean()),
                    "control_density": sum(1 for x in tok if x[3] != 0) / len(tok), "zero": float(counts[0] / n), "ff": float(counts[255] / n),
                    "entropy": float(-(p * np.log2(p)).sum() / 8.0), "code_coverage": covered, "embedded_data": 1.0 - covered,
                    "mixed": float(0.25 <= covered <= 0.75), "dist_to_noncode_cells": min(dist, 64), "quarter": c % 4,
                    "neighbour_exec_density": float(np.mean(neigh)) if neigh else 0.0, "window_positive_cells": int(sum(win_cells)),
                    "hist_family": [sum(1 for x in tok if x[2] == v) for v in range(8)],
                    "hist_length": [sum(1 for x in tok if x[1] == v) for v in range(6)],
                    "hist_ea_src": [sum(1 for x in tok if x[4] == v) for v in range(13)],
                    "hist_ea_dst": [sum(1 for x in tok if x[5] == v) for v in range(13)],
                    "hist_control": [sum(1 for x in tok if x[3] == v) for v in range(8)]})
    scalars = ["instr_density", "valid_decode", "exception_form", "rejected", "control_density", "zero", "ff", "entropy", "code_coverage",
               "embedded_data", "mixed", "dist_to_noncode_cells", "neighbour_exec_density", "window_positive_cells"]
    hists = ["hist_family", "hist_length", "hist_ea_src", "hist_ea_dst", "hist_control"]

    def summarize(rows):
        out = {"cells": len(rows)}
        if not rows:
            return out
        for k in scalars:
            v = sorted(r[k] for r in rows)
            out[k] = {"mean": sum(v) / len(v), "median": v[len(v) // 2]}
        for k in hists:
            total = [sum(r[k][i] for r in rows) for i in range(len(rows[0][k]))]
            s = sum(total) or 1
            out[k] = [x / s for x in total]
        out["quarter_distribution"] = [sum(1 for r in rows if r["quarter"] == q) / len(rows) for q in range(4)]
        return out

    zero_shot = {n: [r for r in rows if r["title"] != "s1"] for n, rows in groups.items()}
    return {"per_title_v1": per_title, "pooled_zero_shot": {n: summarize(r) for n, r in zero_shot.items()},
            "per_title_groups": {tid: {n: summarize([r for r in rows if r["title"] == tid]) for n, rows in groups.items()} for tid in g.TITLE_ORDER},
            "note": "aggregate only; 'selected' = source-positive 128-B cells whose 512-B window v1 selected by score, 'missed' = neither "
                    "selected by score nor a certain-code seed window (= C outside R); diagnosis, not a rule source"}


def v1_frf(titles: dict) -> dict:
    out = {}
    for tid in g.TITLE_ORDER:
        scores, _ = v1_cell_scores(titles[tid])
        out[tid] = g.frf(scores, titles[tid].labels.tolist())
    return out


def cmd_characterize(args) -> int:
    titles = {t: Title(t) for t in g.TITLE_ORDER}
    result = {"characterization": characterize(titles), "m0_frf": v1_frf(titles)}
    out = ROOT / "results" / "characterization.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(result, indent=1, sort_keys=True))
    print(json.dumps({"per_title_v1": result["characterization"]["per_title_v1"], "m0_frf": {k: round(v["frf"], 4) for k, v in result["m0_frf"].items()}}, indent=1))
    return 0


# ------------------------------------------------------------------------------------------------------------------- frozen plan
def build_plan() -> dict:
    """The canonical pre-registered experiment plan (written before any M1..M4 held-out result is read)."""
    import platform
    import numpy
    import sklearn
    import torch
    m2, m3 = build_model(False), build_model(True)
    return {
        "schema": "segarecomp.ml_region_gen2_plan.v1", "experiment": "SEG-049-T001", "adr": "0100",
        "status": "FROZEN BEFORE ANY M1-M4 TITLE-HELD-OUT RESULT",
        "scope": "OFFLINE research; production Optimized AOT and SEG-047 v1 artifacts are unchanged",
        "representation": {"version": g.REPRESENTATION_VERSION, "sha256": g.representation_sha256(), "cell_bytes": g.CELL_BYTES,
                           "context_bytes": g.CONTEXT_BYTES, "edge": "word-block symmetric reflection; no positional input",
                           "label": "cell positive iff it contains >= 1 source-backed MC68000 instruction start",
                           "byte_channels": list(g.BYTE_CHANNELS), "token_channels": list(g.TOKEN_CHANNELS),
                           "token_schema": g.TOKEN_SCHEMA_NAME, "token_schema_sha256": g.token_schema_sha256(),
                           "token_fields": list(g.TOKEN_FIELDS), "token_cardinality": g.TOKEN_CARDINALITY,
                           "token_definition": g.TOKEN_DEFINITION},
        "corpus": g.CORPUS, "loto": [{"held_out": h, "train": tr} for h, tr in g.loto_folds()],
        "models": {
            "M0": {"role": "comparator only", "what": "frozen production SEG-047 v1 (512-B windows, 141 features, Sonic-1-trained logistic, threshold "
                                                         "0.00835406801187952), no retraining; each 128-B cell inherits its window probability"},
            "M1": {"role": "richer-feature boosted-tree control", "estimator": "sklearn HistGradientBoostingClassifier", "config": HGB_CONFIG,
                   "features": {"count": len(m1_feature_names()), "scales": [s[0] for s in M1_SCALES],
                                "per_scale": "6 byte statistics + 8 byte-bucket fractions + 57 token-class fractions (from the same "
                                             "128-B cell / 1024-B context representation)", "names": m1_feature_names()}},
            "M2": {"role": "raw-byte CNN", "architecture": CNN_ARCH, "token_channels": False, "trainable_parameters": parameter_count(m2)},
            "M3": {"role": "raw-byte + MC68000 token CNN (primary)", "architecture": CNN_ARCH, "token_channels": True,
                   "trainable_parameters": parameter_count(m3)},
            "M4": {"role": "M3 + training-title hard-positive emphasis", "architecture": CNN_ARCH, "token_channels": True,
                   "trainable_parameters": parameter_count(m3), "hard_positive": RECIPE["hard_positive"]}},
        "recipe": RECIPE,
        "environment": {"python": platform.python_version(), "numpy": numpy.__version__, "scikit_learn": sklearn.__version__,
                        "torch": torch.__version__.split("+")[0], "device": "CPU only", "deterministic": "torch.use_deterministic_algorithms(True), "
                                                                                                          "fixed seed and fixed thread count"},
        "complexity_order": "M1 < M2 < M3 < M4",
        "metrics": {"primary": ["source recall (C outside R)", "FRF (full-recall fraction)", "R/ROM"], "secondary_only": ["precision", "accuracy", "AUROC"],
                    "frf": "smallest fraction of cells, taken in descending model score, containing every positive cell; ties with the lowest "
                           "positive are all counted"},
        "selection": {"viable": f"FRF <= {g.FRF_VIABLE} on every one of the five held-out titles", "preferred": f"FRF <= {g.FRF_PREFERRED} on every title",
                      "strong_mean": f"mean FRF <= {g.FRF_STRONG_MEAN}",
                      "order": "viable -> lowest worst-title FRF -> lowest mean FRF -> smaller/simpler model",
                      "materiality": f"candidates whose worst-title FRF is within {g.MATERIAL_FRF_MARGIN} of the best form the tie set; a more complex "
                                     f"candidate replaces a simpler one only if its mean (or worst) FRF is lower by >= {g.MATERIAL_FRF_MARGIN}; "
                                     "otherwise the simpler model wins",
                      "model_size": "complexity order, not artifact bytes"},
        "calibration": {"after": "representation selection (never simultaneously)", "score": "model probability",
                        "inner": "for each outer held-out title H, each of the 4 training titles is scored by a model trained on the other 3 "
                                 "training titles; m_H = minimum positive-cell score over those inner held-out predictions; H contributes nothing",
                        "policies": g.POLICY_FACTORS, "threshold": "factor * m_H", "choice": "highest factor passing all five outer folds",
                        "r_gate": f"C outside R = 0 and R/ROM <= {g.R_OVER_ROM_GATE} (preferred {g.R_OVER_ROM_PREFERRED}) on every outer title",
                        "r_definition": "R = ML-selected cells UNION certain-code seed cells (cells holding a precise direct-control-discovery identity)"},
        "stage_separation": "C outside R is computed from the ML proposal first; K comes from the unchanged structural prune; C in R but outside K "
                            "is POST_ML_PRUNING_BLOCKER and never an ML false negative",
        "classes": ["GEN2_ML_READY_FOR_NATIVE_SPIKE", "GEN2_ML_VALID_POSTPROCESS_BLOCKED", "GEN2_REPRESENTATION_VALID_CALIBRATION_UNSOLVED",
                    "GEN2_ML_INSUFFICIENT"],
        "v1_artifact_sha256": g.V1_ARTIFACT_SHA256,
    }


def plan_digest(plan: dict) -> str:
    return g.sha256_bytes(g.canonical_json(plan).encode())


def verify_plan(path=None) -> dict:
    """Fail closed if the committed pre-registered plan differs from the constants this code would train with."""
    path = pathlib.Path(path) if path else TOOLS / PLAN_NAME
    committed = json.loads(path.read_text())
    current = build_plan()
    committed_core = {k: v for k, v in committed.items() if k != "environment"}
    current_core = {k: v for k, v in current.items() if k != "environment"}
    if committed_core != current_core:
        raise SystemExit("plan drift: training constants differ from the frozen pre-registered plan")
    for key in ("python", "numpy", "scikit_learn", "torch"):
        if committed["environment"][key] != current["environment"][key]:
            raise SystemExit(f"plan drift: environment {key} differs from the frozen plan")
    return committed


def cmd_plan(args) -> int:
    plan = build_plan()
    out = TOOLS / PLAN_NAME
    if out.exists() and not args.force:
        raise SystemExit("plan already frozen (refusing to overwrite)")
    out.write_text(json.dumps(plan, indent=1, sort_keys=True) + "\n")
    print(json.dumps({"plan_sha256": plan_digest(plan), "m1_features": len(m1_feature_names()),
                      "M2_params": plan["models"]["M2"]["trainable_parameters"], "M3_params": plan["models"]["M3"]["trainable_parameters"]}))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("characterize").set_defaults(func=cmd_characterize)
    plan = sub.add_parser("plan")
    plan.add_argument("--force", action="store_true")
    plan.set_defaults(func=cmd_plan)
    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
