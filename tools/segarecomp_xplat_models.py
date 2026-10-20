#!/usr/bin/env python3
"""SEG-075-T005 (ADR 0161 decisions.t005_scope): the pre-registered code-recognition models A, B and C (OFFLINE research code).

  A  lightweight structural model: sklearn HistGradientBoosting (fixed configuration) over per-cell aggregates of CPU-owned decode
     tokens, ordinary-decode chains, PC-relative direct-control targets, byte context, relocation fields, the seed flag, and the same
     aggregates averaged over the +/-1 and +/-4 neighbour cells
  B  bounded local instruction-aware Transformer: per-word embeddings (bytes + M3/M4 decode tokens) -> 64-wide encoder (2 layers, 4 heads,
     feed-forward 128, learned positions) over 256-word blocks with 64 words of context on each side; per-word logit -> cell max.
     Trained with EXACTLY M4-Dense's recipe (window, margin, steps, batch, optimiser, schedule, pos_weight, lineage-uniform sampling,
     seeds 1-3 averaged)
  C  B plus two rounds of typed sparse message passing after each encoder layer over CANDIDATE edges inside the block+context: fall-through
     (ordinary decode -> next position by decoder length), PC-relative direct branch/call/DBcc target, overlapping interpretation
     (positions inside a decoded extent); a node flag marks positions statically reachable from the independently established seed roots
     by fall-through and direct edges (PROVEN); candidate edges never set that flag

No address, platform, lineage or game identity is an input. Edges and flags are derived from the CPU-owned decode tokens plus the
instruction words themselves (displacement fields); no symbol table, listing or label is read.
"""
from __future__ import annotations

import json
import math
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

TOKEN_FIELDS = ("status", "length", "family", "control", "ea_src", "ea_dst", "width", "flags")
TOKEN_CARD = {"status": 3, "length": 6, "family": 8, "control": 8, "ea_src": 13, "ea_dst": 13, "width": 4, "flags": 4}
CELL_WORDS = 64
A_CONFIG = {"max_iter": 300, "max_depth": 6, "learning_rate": 0.05, "min_samples_leaf": 40, "l2_regularization": 1.0, "max_bins": 255,
            "early_stopping": False, "class_weight": "balanced", "random_state": 49}
B_ARCH = {"d_model": 64, "layers": 2, "heads": 4, "ff": 128, "block": 256, "context": 64}
TERMINATING = (2, 4, 6)       # token control: direct jump, dynamic jump, return
DIRECT = (1, 2, 3)            # token control: conditional, direct jump, direct call


def np_():
    import numpy
    return numpy


# ------------------------------------------------------------------------------------------------------------------ structure
def structure(im) -> dict:
    """Per-word structural arrays of an image (cached on the image): ordinary flag, decoder length (words), control class, PC-relative
    direct target word (-1 none / outside), chain length, PROVEN reachability from seed roots."""
    if getattr(im, "_xplat_structure", None) is not None:
        return im._xplat_structure
    np = np_()
    n = im.n_words
    tok = im.tokens
    status, length, control = tok[:, 0].astype(np.int64), tok[:, 1].astype(np.int64), tok[:, 3].astype(np.int64)
    ordinary = status == 2
    length = np.where(ordinary, np.maximum(length, 1), 0)
    words = (im.hi.astype(np.int64) << 8) | im.lo.astype(np.int64)
    nxt = np.concatenate([words[1:], [0]])
    target = np.full(n, -1, dtype=np.int64)
    idx = np.arange(n)
    is_bcc = ordinary & np.isin(control, DIRECT) & ((words & 0xF000) == 0x6000)
    d8 = words & 0xFF
    disp = np.where(d8 == 0, ((nxt + 0x8000) & 0xFFFF) - 0x8000, ((d8 + 0x80) & 0xFF) - 0x80)
    tb = idx + 1 + disp // 2  # word index: pc + 2 + disp (bytes) -> (2*i + 2 + disp) / 2
    target = np.where(is_bcc & (disp % 2 == 0), tb, target)
    is_db = ordinary & ((words & 0xF0F8) == 0x50C8)
    dd = ((nxt + 0x8000) & 0xFFFF) - 0x8000
    target = np.where(is_db & (dd % 2 == 0), idx + 1 + dd // 2, target)
    # JMP / JSR: (d16,PC) -> pc + 2 + disp16; absolute .W / .L operand -> interpreted as an IMAGE OFFSET when it lies inside the image
    # (Genesis cartridge addresses equal file offsets; in relocatable Atari/Amiga images the stored operand is section-relative, so the
    # candidate is exact only for intra-section targets - disclosed in the T005 addendum)
    is_jpc = ordinary & np.isin(words, (0x4EFA, 0x4EBA))
    target = np.where(is_jpc & (dd % 2 == 0), idx + 1 + dd // 2, target)
    nxt2 = np.concatenate([words[2:], [0, 0]])
    abs_w = ((nxt + 0x8000) & 0xFFFF) - 0x8000
    abs_l = (nxt << 16) | nxt2
    is_aw = ordinary & np.isin(words, (0x4EF8, 0x4EB8))
    is_al = ordinary & np.isin(words, (0x4EF9, 0x4EB9))
    target = np.where(is_aw & (abs_w >= 0) & (abs_w % 2 == 0), abs_w // 2, target)
    target = np.where(is_al & (abs_l % 2 == 0), abs_l // 2, target)
    target = np.where((target >= 0) & (target < n), target, -1)
    chain = np.zeros(n + 6, dtype=np.int64)
    for w in range(n - 1, -1, -1):
        if ordinary[w]:
            chain[w] = 1 + min(63, chain[w + length[w]] if w + length[w] < n else 0)
    chain = chain[:n]
    reach = np.zeros(n, dtype=bool)
    roots = [a // 2 for a in getattr(im, "seed_addresses", []) if 0 <= a // 2 < n]
    stack = list(roots)
    while stack:
        w = stack.pop()
        if w < 0 or w >= n or reach[w] or not ordinary[w]:
            continue
        reach[w] = True
        if control[w] not in TERMINATING:
            stack.append(w + length[w])
        if target[w] >= 0:
            stack.append(int(target[w]))
    s = {"ordinary": ordinary, "length": length, "control": control, "target": target, "chain": chain, "reach": reach}
    im._xplat_structure = s
    return s


# ---------------------------------------------------------------------------------------------------------------------- model A
def a_features(im, relocs=()) -> "np.ndarray":
    """Per-cell feature matrix (cells x F) for model A (pure given the image; unit-tested shape/determinism)."""
    np = np_()
    s = structure(im)
    n, cells = im.n_words, im.n_cells
    cell_of = np.minimum(np.arange(n) // CELL_WORDS, cells - 1)
    cols = []
    for f, card in TOKEN_CARD.items():
        col = im.tokens[:, TOKEN_FIELDS.index(f)].astype(np.int64)
        cols.append(np.bincount(cell_of * card + col, minlength=cells * card).reshape(cells, card))
    ch = s["chain"]
    chain_max = np.zeros(cells)
    np.maximum.at(chain_max, cell_of, ch)
    chain_sum = np.bincount(cell_of, weights=ch, minlength=cells)
    long_chains = np.bincount(cell_of, weights=(ch >= 8).astype(float), minlength=cells)
    t = s["target"]
    out_ok = (t >= 0) & s["ordinary"][np.maximum(t, 0)]
    out_ok &= t >= 0
    targets_out = np.bincount(cell_of, weights=out_ok.astype(float), minlength=cells)
    tc = np.minimum(np.maximum(t, 0) // CELL_WORDS, cells - 1)
    targets_in = np.bincount(tc[t >= 0], minlength=cells)
    raw = np.empty(n * 2, dtype=np.int64)
    raw[0::2], raw[1::2] = im.hi, im.lo
    byte_cell = np.minimum(np.arange(n * 2) // (CELL_WORDS * 2), cells - 1)
    zeros = np.bincount(byte_cell, weights=(raw == 0).astype(float), minlength=cells)
    ascii_ = np.bincount(byte_cell, weights=((raw >= 32) & (raw < 127)).astype(float), minlength=cells)
    hist = np.bincount(byte_cell * 256 + raw, minlength=cells * 256).reshape(cells, 256).astype(float)
    tot = np.maximum(hist.sum(axis=1, keepdims=True), 1)
    p = hist / tot
    ent = -(np.where(p > 0, p * np.log2(np.where(p > 0, p, 1)), 0)).sum(axis=1)
    rel = np.zeros(cells)
    for off, _w in relocs:
        c = min(off // (CELL_WORDS * 2), cells - 1)
        rel[c] += 1
    seed = np.zeros(cells)
    for c in im.seeds:
        if 0 <= c < cells:
            seed[c] = 1
    reach = np.bincount(cell_of, weights=s["reach"].astype(float), minlength=cells)
    base = np.concatenate([np.concatenate(cols, axis=1).astype(float),
                           np.stack([chain_max, chain_sum / CELL_WORDS, long_chains, targets_out, targets_in, zeros / 128, ascii_ / 128, ent,
                                     rel, seed, reach], axis=1)], axis=1)

    def neigh(r):
        k = 2 * r + 1
        pad = np.pad(base, ((r, r), (0, 0)), mode="edge")
        cs = np.cumsum(np.vstack([np.zeros((1, base.shape[1])), pad]), axis=0)
        return (cs[k:] - cs[:-k]) / k

    return np.concatenate([base, neigh(1), neigh(4)], axis=1)


def relocs_of(im) -> list:
    d = getattr(im, "_dir", None)
    if d is not None and (pathlib.Path(d) / "relocs.json").is_file():
        return [tuple(x) for x in json.loads((pathlib.Path(d) / "relocs.json").read_text())]
    return []


def fit_a(images: list):
    from sklearn.ensemble import HistGradientBoostingClassifier
    np = np_()
    xs, ys = [], []
    for im in images:
        f = a_features(im, relocs_of(im))
        keep = im.labels >= 0
        xs.append(f[keep])
        ys.append((im.labels[keep] == 1).astype(np.int64))
    return HistGradientBoostingClassifier(**A_CONFIG).fit(np.concatenate(xs), np.concatenate(ys))


def score_a(clf, im):
    np = np_()
    p = clf.predict_proba(a_features(im, relocs_of(im)))[:, 1]
    return np.log(np.clip(p, 1e-300, 1.0)) - np.log(np.clip(1.0 - p, 1e-300, 1.0))


# ----------------------------------------------------------------------------------------------------------------- models B / C
def build_bc(torch, graph: bool):
    import segarecomp_ml_v3_train as t
    nn = torch.nn
    A = B_ARCH

    class Net(nn.Module):
        def __init__(self):
            super().__init__()
            self.emb_hi, self.emb_lo = nn.Embedding(257, 8), nn.Embedding(257, 8)
            self.tok = nn.ModuleList([nn.Embedding(t.TOKEN_CARD[f] + 1, t.TOKEN_DIMS[f]) for f in t.TOKEN_FIELDS])
            cin = 16 + sum(t.TOKEN_DIMS.values()) + (1 if graph else 0)
            self.inp = nn.Linear(cin, A["d_model"])
            self.pos = nn.Embedding(A["block"] + 2 * A["context"], A["d_model"])
            self.layers = nn.ModuleList([nn.TransformerEncoderLayer(A["d_model"], A["heads"], A["ff"], dropout=0.0, batch_first=True)
                                         for _ in range(A["layers"])])
            if graph:
                self.edge = nn.ModuleList([nn.ModuleList([nn.Linear(A["d_model"], A["d_model"], bias=False) for _ in range(3)])
                                           for _ in range(A["layers"])])
            self.head = nn.Linear(A["d_model"], 1)

        def forward(self, hi, lo, tok, graph_in=None):
            """hi, lo (B, L), tok (B, L, 8); graph_in: (reach (B, L), edges list per batch item of (type, src, dst) LongTensors in
            window coordinates). Returns logits for the central L - 2*margin words."""
            m = t.M4["margin"]
            parts = [self.emb_hi(hi), self.emb_lo(lo)] + [e(tok[:, :, i]) for i, e in enumerate(self.tok)]
            if graph:
                parts.append(graph_in[0].unsqueeze(2).float())
            x = self.inp(torch.cat(parts, dim=2))
            Bsz, L, D = x.shape
            blk, ctx = A["block"], A["context"]
            starts = list(range(m, L - m, blk))
            seqs = torch.stack([x[:, s - ctx:s + blk + ctx] for s in starts], dim=1).reshape(Bsz * len(starts), blk + 2 * ctx, D)
            seqs = seqs + self.pos.weight.unsqueeze(0)
            for li, layer in enumerate(self.layers):
                seqs = layer(seqs)
                if graph:
                    seqs = self._message(seqs, li, graph_in[1], starts, Bsz)
            out = seqs[:, ctx:ctx + blk].reshape(Bsz, len(starts) * blk, D)
            return self.head(out).squeeze(2)[:, :L - 2 * m]

        def _message(self, seqs, li, edges, starts, Bsz):
            """Two rounds of typed sparse message passing (sum of per-type linear messages from in-window edge sources)."""
            blk, ctx = A["block"], A["context"]
            span = blk + 2 * ctx
            nb = len(starts)
            index = []
            for b in range(Bsz):
                for typ in range(3):
                    src, dst = edges[b][typ]
                    for k, s in enumerate(starts):
                        lo_, hi_ = s - ctx, s + blk + ctx
                        sel = (src >= lo_) & (src < hi_) & (dst >= lo_) & (dst < hi_)
                        if bool(sel.any()):
                            base = (b * nb + k) * span
                            index.append((typ, src[sel] - lo_ + base, dst[sel] - lo_ + base))
            flat = seqs.reshape(Bsz * nb * span, -1)
            for _round in range(2):
                agg = torch.zeros_like(flat)
                for typ, si, di in index:
                    agg = agg.index_add(0, di, self.edge[li][typ](flat[si]))
                flat = flat + torch.relu(agg) / 4.0
            return flat.reshape(seqs.shape)

    return Net()


def window_graph(np, torch, im, start, central):
    """PROVEN reach flags and CANDIDATE typed edges (fall-through, direct target, overlap) for the M4 window starting at word `start`."""
    import segarecomp_ml_v3_train as t
    s = structure(im)
    m = t.M4["margin"]
    lo, hi = start - m, start + central + m
    n = im.n_words
    idx = np.arange(lo, hi)
    ok = (idx >= 0) & (idx < n)
    cl = np.clip(idx, 0, max(0, n - 1))
    reach = np.where(ok, s["reach"][cl], False).astype(np.float32)
    w = idx[ok]
    ln, tg, od = s["length"][w], s["target"][w], s["ordinary"][w]
    ft_src, ft_dst = w[od], (w + ln)[od]
    dt_src, dt_dst = w[od & (tg >= 0)], tg[od & (tg >= 0)]
    ov_src, ov_dst = [], []
    for k in range(1, 5):
        sel = od & (ln > k)
        ov_src.append(w[sel])
        ov_dst.append(w[sel] + k)
    ov_src, ov_dst = np.concatenate(ov_src), np.concatenate(ov_dst)
    edges = []
    for a, b in ((ft_src, ft_dst), (dt_src, dt_dst), (ov_src, ov_dst)):
        keep = (b >= lo) & (b < hi)
        edges.append((torch.from_numpy((a[keep] - lo).astype(np.int64)), torch.from_numpy((b[keep] - lo).astype(np.int64))))
    return reach, edges


def fit_bc(images: list, seed: int, graph: bool, log=None):
    """M4-Dense's exact training recipe with the B/C network (same window, sampling, schedule, loss and pos_weight)."""
    import time
    import torch
    import segarecomp_ml_v3_train as t
    np = np_()
    M4 = t.M4
    torch.set_num_threads(M4["threads"])
    torch.use_deterministic_algorithms(True)
    torch.manual_seed(seed)
    rng = np.random.default_rng(seed)
    model = build_bc(torch, graph)
    opt = torch.optim.AdamW(model.parameters(), lr=M4["lr"], weight_decay=M4["weight_decay"])
    pos_weight = t.m4_pos_weight(np, images)
    lineages = sorted({im.lineage for im in images})
    by_lin = {L: [im for im in images if im.lineage == L] for L in lineages}
    sizes = {L: np.array([im.n_words for im in by_lin[L]], dtype=np.float64) for L in lineages}
    q, central, total = M4["start_quantum"], M4["central"], M4["steps"]
    model.train()
    t0 = time.time()
    for step in range(total):
        for gr in opt.param_groups:
            gr["lr"] = M4["lr"] * (M4["lr_drop"] if step >= int(M4["lr_drop_at"] * total) else 1.0)
        hs, ls, ts, ys, rs, es = [], [], [], [], [], []
        for _ in range(M4["batch"]):
            L = lineages[int(rng.integers(0, len(lineages)))]
            im = by_lin[L][int(rng.choice(len(by_lin[L]), p=sizes[L] / sizes[L].sum()))]
            lo_q, hi_q = -(central // 2) // q, (im.n_words - central // 2) // q
            start = int(rng.integers(lo_q, max(lo_q, hi_q) + 1)) * q
            h, l, tk, y = t.m4_window(np, im, start, central)
            hs.append(h), ls.append(l), ts.append(tk), ys.append(y)
            if graph:
                r, e = window_graph(np, torch, im, start, central)
                rs.append(r), es.append(e)
        hb, lb, tb, yb = (torch.from_numpy(np.stack(a)) for a in (hs, ls, ts, ys))
        gi = (torch.from_numpy(np.stack(rs)), es) if graph else None
        logits = model(hb, lb, tb, gi)
        mask = yb >= 0
        if not bool(mask.any()):
            continue
        lg, tgt = logits[mask], yb[mask].float()
        one = torch.ones(())
        wgt = torch.where(tgt > 0.5, one * pos_weight, one)
        loss = (torch.nn.functional.binary_cross_entropy_with_logits(lg, tgt, reduction="none") * wgt).sum() / wgt.sum()
        opt.zero_grad()
        loss.backward()
        opt.step()
        if log and (step + 1) % 500 == 0:
            log(f"step {step + 1}/{total} loss {float(loss):.5f} t={time.time() - t0:.0f}s")
    model.eval()
    return model


def infer_bc(model, im, graph: bool):
    import torch
    import segarecomp_ml_v3_train as t
    np = np_()
    out = np.zeros(im.n_words, dtype=np.float32)
    c = t.M4["infer_central"]
    with torch.no_grad():
        for start in range(0, im.n_words, c):
            h, l, tk, _ = t.m4_window(np, im, start, c)
            gi = None
            if graph:
                r, e = window_graph(np, torch, im, start, c)
                gi = (torch.from_numpy(r[None]), [e])
            lg = model(torch.from_numpy(h[None]), torch.from_numpy(l[None]), torch.from_numpy(tk[None]), gi)[0].float().numpy()
            k = min(c, im.n_words - start)
            out[start:start + k] = lg[:k]
    return out
