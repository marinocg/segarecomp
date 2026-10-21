#!/usr/bin/env python3
"""SEG-076-T005 (exploratory, OFFLINE research code): C-v2, a deterministic, vectorized implementation of the SEG-075 graph-guided
Transformer C (segarecomp_xplat_models.build_bc(graph=True)), which is left unchanged.

Same network function, parameters (identical module names, so a C state_dict loads into C-v2), M4 training recipe, candidate edges
(segarecomp_xplat_models.window_graph) and PROVEN flag. Only the typed message passing is re-implemented:

  frozen C   per layer and per round, per (batch item, edge type, block) Python loops building masked index tensors, then one
             `index_add` per piece (many tiny kernels, CPU-bound). It was observed NOT to be run-to-run deterministic on ROCm (T001
             post-hoc refit); the root cause is not established (torch documents index_add as deterministic under
             use_deterministic_algorithms on CUDA)
  C-v2       the edge index is built ONCE per forward in numpy; ONE padded in-edge table over all three edge types (unique destination x
             max in-degree, rows into the stacked per-type message tensor in a stable sorted order, zero mask for padding) turns aggregation
             into gather + masked sum over a fixed axis, written with a unique-index copy: a fixed summation order. Its run-to-run
             determinism is an EMPIRICAL claim (a 150-step ROCm probe and the pre-registered full-length refit), not a proof

Mathematically the aggregated message is the same sum; only the floating-point summation order differs, so C-v2 equals C within float
tolerance (verified on identical weights and batches), not bitwise.
"""
from __future__ import annotations

import pathlib
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))


def build_cv2(torch):
    import segarecomp_ml_v3_train as t
    import segarecomp_xplat_models as xm
    np = xm.np_()
    nn = torch.nn
    A = xm.B_ARCH

    class Net(nn.Module):
        def __init__(self):
            super().__init__()
            self.emb_hi, self.emb_lo = nn.Embedding(257, 8), nn.Embedding(257, 8)
            self.tok = nn.ModuleList([nn.Embedding(t.TOKEN_CARD[f] + 1, t.TOKEN_DIMS[f]) for f in t.TOKEN_FIELDS])
            cin = 16 + sum(t.TOKEN_DIMS.values()) + 1
            self.inp = nn.Linear(cin, A["d_model"])
            self.pos = nn.Embedding(A["block"] + 2 * A["context"], A["d_model"])
            self.layers = nn.ModuleList([nn.TransformerEncoderLayer(A["d_model"], A["heads"], A["ff"], dropout=0.0, batch_first=True)
                                         for _ in range(A["layers"])])
            self.edge = nn.ModuleList([nn.ModuleList([nn.Linear(A["d_model"], A["d_model"], bias=False) for _ in range(3)])
                                       for _ in range(A["layers"])])
            self.head = nn.Linear(A["d_model"], 1)

        def forward(self, hi, lo, tok, graph_in):
            m = t.M4["margin"]
            parts = [self.emb_hi(hi), self.emb_lo(lo)] + [e(tok[:, :, i]) for i, e in enumerate(self.tok)]
            parts.append(graph_in[0].unsqueeze(2).float())
            x = self.inp(torch.cat(parts, dim=2))
            Bsz, L, D = x.shape
            blk, ctx = A["block"], A["context"]
            starts = list(range(m, L - m, blk))
            seqs = torch.stack([x[:, s - ctx:s + blk + ctx] for s in starts], dim=1).reshape(Bsz * len(starts), blk + 2 * ctx, D)
            seqs = seqs + self.pos.weight.unsqueeze(0)
            table = padded_table(np, torch, graph_in[1], starts, Bsz, seqs.device)
            for li, layer in enumerate(self.layers):
                seqs = layer(seqs)
                seqs = self._message(seqs, li, table)
            out = seqs[:, ctx:ctx + blk].reshape(Bsz, len(starts) * blk, D)
            return self.head(out).squeeze(2)[:, :L - 2 * m]

        def _message(self, seqs, li, table):
            flat = seqs.reshape(-1, seqs.shape[-1])
            if table is None:
                return seqs
            dst, src_pad, mask = table
            for _round in range(2):
                msg = torch.cat([self.edge[li][typ](flat) for typ in range(3)], dim=0)   # (3N, D): row typ*N + node
                summed = (msg[src_pad] * mask.unsqueeze(2)).sum(dim=1)                 # (n_dst, D) fixed-order sum per destination
                agg = torch.zeros_like(flat).index_copy(0, dst, summed)                # unique destinations: no accumulation
                flat = flat + torch.relu(agg) / 4.0
            return flat.reshape(seqs.shape)

    return Net()


def padded_table(np, torch, edges, starts, Bsz, device):
    """ONE padded in-edge table over all three edge types: (unique destination flat indices, (n_dst, K) source rows into the stacked
    (3N, D) message tensor in a stable sorted order, mask). Same edge selection as the frozen C (both endpoints inside the block span),
    built once per forward. Returns None when no edge exists."""
    import segarecomp_xplat_models as xm
    A = xm.B_ARCH
    blk, ctx = A["block"], A["context"]
    span, nb = blk + 2 * ctx, len(starts)
    N = Bsz * nb * span
    srcs, dsts = [], []
    for typ in range(3):
        for b in range(Bsz):
            s_all = edges[b][typ][0].cpu().numpy()
            d_all = edges[b][typ][1].cpu().numpy()
            for k, s in enumerate(starts):
                lo_, hi_ = s - ctx, s + blk + ctx
                sel = (s_all >= lo_) & (s_all < hi_) & (d_all >= lo_) & (d_all < hi_)
                if sel.any():
                    base = (b * nb + k) * span
                    srcs.append(typ * N + s_all[sel] - lo_ + base)
                    dsts.append(d_all[sel] - lo_ + base)
    if not srcs:
        return None
    src, dst = np.concatenate(srcs), np.concatenate(dsts)
    order = np.lexsort((src, dst))                        # by destination, then (type, source): a fixed, data-determined order
    src, dst = src[order], dst[order]
    udst, first, counts = np.unique(dst, return_index=True, return_counts=True)
    K = int(counts.max())
    pos = np.arange(len(dst)) - np.repeat(first, counts)
    row = np.repeat(np.arange(len(udst)), counts)
    pad = np.zeros((len(udst), K), dtype=np.int64)
    mask = np.zeros((len(udst), K), dtype=np.float32)
    pad[row, pos] = src
    mask[row, pos] = 1.0
    return (torch.from_numpy(udst.astype(np.int64)).to(device), torch.from_numpy(pad).to(device), torch.from_numpy(mask).to(device))


def fit_cv2(images: list, seed: int, device, log=None):
    """The frozen M4 recipe (line-for-line as segarecomp_xplat_models.fit_bc / the T001 amendment's fit_bc_device), with build_cv2."""
    import torch
    import segarecomp_ml_v3_train as t
    import segarecomp_xplat_models as xm
    np = xm.np_()
    M4 = t.M4
    torch.set_num_threads(M4["threads"])
    torch.use_deterministic_algorithms(True)
    torch.manual_seed(seed)
    rng = np.random.default_rng(seed)
    model = build_cv2(torch).to(device)
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
            r, e = xm.window_graph(np, torch, im, start, central)
            rs.append(r), es.append(e)
        hb, lb, tb, yb = (torch.from_numpy(np.stack(a)).to(device) for a in (hs, ls, ts, ys))
        gi = (torch.from_numpy(np.stack(rs)).to(device), es)
        logits = model(hb, lb, tb, gi)
        mask = yb >= 0
        if not bool(mask.any()):
            continue
        lg, tgt = logits[mask], yb[mask].float()
        one = torch.ones((), device=device)
        wgt = torch.where(tgt > 0.5, one * pos_weight, one)
        loss = (torch.nn.functional.binary_cross_entropy_with_logits(lg, tgt, reduction="none") * wgt).sum() / wgt.sum()
        opt.zero_grad()
        loss.backward()
        opt.step()
        if log and (step + 1) % 500 == 0:
            log(f"step {step + 1}/{total} loss {float(loss):.5f} t={time.time() - t0:.0f}s")
    model.eval()
    return model


def infer_cv2(model, im, device):
    import torch
    import segarecomp_ml_v3_train as t
    import segarecomp_xplat_models as xm
    np = xm.np_()
    out = np.zeros(im.n_words, dtype=np.float32)
    c = t.M4["infer_central"]
    with torch.no_grad():
        for start in range(0, im.n_words, c):
            h, l, tk, _ = t.m4_window(np, im, start, c)
            r, e = xm.window_graph(np, torch, im, start, c)
            lg = model(torch.from_numpy(h[None]).to(device), torch.from_numpy(l[None]).to(device), torch.from_numpy(tk[None]).to(device),
                       (torch.from_numpy(r[None]).to(device), [e]))[0].float().cpu().numpy()
            k = min(c, im.n_words - start)
            out[start:start + k] = lg[:k]
    return out
