#!/usr/bin/env python3
"""SEG-056 (ADR 0117): OFFLINE dense byte-level Z80 executable-occupancy CNN (private research tooling).

numpy and PyTorch are NOT product dependencies and are imported lazily. All label/root/aggregation/metric definitions are in the
stdlib `segarecomp_ml_region_z80_gen2.py`; the protocol is frozen in `segarecomp_ml_region_z80_gen2.plan.json` (digest checked
before anything is fitted). Output is aggregates and private ignored score arrays only.

  fit-fold   one leave-one-lineage-out development fold: train the frozen candidates on the other development lineages and
             write per-byte logits of the held-out lineage for every (candidate, seed)
  eval       aggregate private logits into the frozen metric table (no fitting)
  final      fit the SELECTED candidate on all development lineages and score the sealed lineages EXACTLY ONCE
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_ml_region_z80 as z1  # noqa: E402
import segarecomp_ml_region_z80_gen2 as g  # noqa: E402

PLAN = HERE / "segarecomp_ml_region_z80_gen2.plan.json"
DIGEST = HERE / "segarecomp_ml_region_z80_gen2.plan.digest"
PAD = 256
MARGIN = 512      # context bytes per side (>= receptive-field radius of every candidate, asserted by the test)
CENTRAL = 2048    # training central window
INFER_CENTRAL = 4096
BATCH = 8
STEPS = 3000
LR = 0.002
SEEDS = (1, 2, 3)
CANDIDATES = {
    # name: (context dilations at stride 4 | None for the C1-shaped local net, hard-positive reweighting)
    "B1": (None, False),
    "D256": ((1, 2, 4, 8, 16), False),
    "D512": ((1, 2, 4, 8, 16, 32), False),
    "D512HP": ((1, 2, 4, 8, 16, 32), True),
}


def check_plan():
    plan = json.loads(PLAN.read_text())
    if g.canonical_digest(plan) != DIGEST.read_text().strip():
        raise SystemExit("plan digest mismatch: the frozen plan was modified")
    return plan


def build_model(torch, name):
    nn = torch.nn
    dil, _ = CANDIDATES[name]

    class Dense(nn.Module):
        def __init__(self):
            super().__init__()
            self.emb = nn.Embedding(257, 16)
            if dil is None:  # B1: the SEG-051 C1 trunk (radius 30) with a dense per-byte head
                chans = [16, 32, 32, 32, 32]
                self.convs = nn.ModuleList([nn.Conv1d(chans[i], chans[i + 1], 5, padding=2 * d, dilation=d)
                                            for i, d in enumerate((1, 2, 4, 8))])
                self.head = nn.Conv1d(32, 1, 1)
            else:
                self.l1 = nn.Conv1d(16, 24, 5, padding=2)
                self.l2 = nn.Conv1d(24, 24, 5, padding=2)
                self.down = nn.Conv1d(16, 32, 4, stride=4)
                self.ctx = nn.ModuleList([nn.Conv1d(32, 32, 5, padding=2 * d, dilation=d) for d in dil])
                self.fuse = nn.Conv1d(56, 32, 1)
                self.head = nn.Conv1d(32, 1, 1)

        def forward(self, b):
            e = self.emb(b).transpose(1, 2)
            if dil is None:
                x = e
                for c in self.convs:
                    x = torch.relu(c(x))
                return self.head(x).squeeze(1)[:, MARGIN:-MARGIN]
            loc = torch.relu(self.l2(torch.relu(self.l1(e))))
            c = torch.relu(self.down(e))
            for layer in self.ctx:
                c = c + torch.relu(layer(c))
            c = c.repeat_interleave(4, dim=2)
            x = torch.relu(self.fuse(torch.cat([loc, c], dim=1)))
            return self.head(x).squeeze(1)[:, MARGIN:-MARGIN]

    return Dense()


def window(np, item, start, central):
    """Input (central + 2*MARGIN) tokens beginning MARGIN before `start`; PAD outside the image. Label window for the centre."""
    n = len(item["byte"])
    lo = start - MARGIN
    idx = np.arange(lo, start + central + MARGIN)
    ok = (idx >= 0) & (idx < n)
    x = np.where(ok, item["byte"][np.clip(idx, 0, n - 1)], PAD)
    cidx = np.arange(start, start + central)
    cok = (cidx >= 0) & (cidx < n)
    y = np.where(cok, item["y"][np.clip(cidx, 0, n - 1)], -1)
    return x.astype(np.int64), y.astype(np.int64)


def load(np, lid, img_dir, truth_dir):
    img = (pathlib.Path(img_dir) / f"{lid}.sms").read_bytes()
    truth = json.loads((pathlib.Path(truth_dir) / f"{lid}.final.json").read_text())
    if truth["rom_sha256"] != hashlib.sha256(img).hexdigest():
        raise SystemExit(f"{lid}: image does not match its truth")
    return {"name": lid, "byte": np.frombuffer(img, dtype=np.uint8).astype(np.int64),
            "y": np.array(g.byte_labels(truth["classes"]), dtype=np.int64), "classes": truth["classes"], "n": len(img)}


def pick_device(torch, device):
    """'cpu' | 'cuda' | 'auto'. ROCm builds of PyTorch expose the GPU as 'cuda'. A requested-but-missing GPU fails visibly."""
    if device == "cpu":
        return torch.device("cpu")
    if device == "cuda":
        if not torch.cuda.is_available():
            raise SystemExit("--device cuda requested but torch.cuda.is_available() is False (no silent CPU fallback)")
        return torch.device("cuda")
    if device == "auto":
        return torch.device("cuda" if torch.cuda.is_available() else "cpu")
    raise SystemExit("unknown device")


def fit(np, torch, train, name, seed, device=None):
    dev = device or torch.device("cpu")
    torch.manual_seed(seed)
    rng = np.random.default_rng(seed)
    model = build_model(torch, name).to(dev)
    hp = CANDIDATES[name][1]
    opt = torch.optim.AdamW(model.parameters(), lr=LR, weight_decay=1e-4)
    # lineage-balanced class weight: mean over training lineages of (negative bytes / positive bytes)
    ratios = []
    for t in train:
        p, q = int((t["y"] == 1).sum()), int((t["y"] == 0).sum())
        if p and q:
            ratios.append(q / p)
    pos_weight = float(np.mean(ratios))
    model.train()
    for step in range(STEPS):
        for gr in opt.param_groups:
            gr["lr"] = LR * (0.1 if step >= int(0.8 * STEPS) else 1.0)
        xs, ys = [], []
        for _ in range(BATCH):
            t = train[int(rng.integers(0, len(train)))]  # uniform over independent lineages
            start = int(rng.integers(-CENTRAL // 2 // 16, (t["n"] - CENTRAL // 2) // 16 + 1)) * 16
            x, y = window(np, t, start, CENTRAL)
            xs.append(x)
            ys.append(y)
        xb = torch.from_numpy(np.stack(xs)).to(dev)
        yb = torch.from_numpy(np.stack(ys)).to(dev)
        logits = model(xb)
        mask = yb >= 0
        if not bool(mask.any()):
            continue
        lg, tgt = logits[mask], yb[mask].float()
        one = torch.ones((), device=dev)
        w = torch.where(tgt > 0.5, one * pos_weight, one)
        if hp:
            w = w * torch.where(tgt > 0.5, 1.0 + 2.0 * (1.0 - torch.sigmoid(lg)).detach(), one)
        loss = (torch.nn.functional.binary_cross_entropy_with_logits(lg, tgt, reduction="none") * w).sum() / w.sum()
        opt.zero_grad()
        loss.backward()
        opt.step()
    model.eval()
    return model


def infer(np, torch, model, item):
    dev = next(model.parameters()).device
    out = np.zeros(item["n"], dtype=np.float32)
    with torch.no_grad():
        for start in range(0, item["n"], INFER_CENTRAL):
            x, _ = window(np, {"byte": item["byte"], "y": np.full(item["n"], -1)}, start, INFER_CENTRAL)
            lg = model(torch.from_numpy(x[None, :]).to(dev))[0].float().cpu().numpy()
            m = min(INFER_CENTRAL, item["n"] - start)
            out[start:start + m] = lg[:m]
    return out


def valid_result(np, path, n):
    """A published score array must load, be float32 of shape (n,) and finite; anything else is recomputed."""
    try:
        a = np.load(path, allow_pickle=False)
    except Exception:
        return False
    return a.dtype == np.float32 and a.shape == (n,) and bool(np.isfinite(a).all())


def publish(np, path, arr):
    """Atomic publication: write a temp file in the same directory, then rename."""
    tmp = pathlib.Path(str(path) + ".tmp")
    with open(tmp, "wb") as f:
        np.save(f, arr, allow_pickle=False)
    tmp.replace(path)


def setup(threads):
    import numpy as np
    import torch
    torch.set_num_threads(threads)
    torch.use_deterministic_algorithms(True)
    return np, torch


def dev_ids(plan):
    return sorted(l["id"] for l in plan["lineages"] if l["role"] == "development")


def sealed_ids(plan):
    return sorted(l["id"] for l in plan["lineages"] if l["role"] == "sealed")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["fit-fold", "eval", "final"])
    ap.add_argument("--img-dirs", nargs="+", required=True)
    ap.add_argument("--truth-dirs", nargs="+", required=True)
    ap.add_argument("--state", required=True)
    ap.add_argument("--out")
    ap.add_argument("--held")
    ap.add_argument("--models", nargs="*", default=list(CANDIDATES))
    ap.add_argument("--threads", type=int, default=2)
    ap.add_argument("--device", choices=["auto", "cpu", "cuda"], default="cpu")
    a = ap.parse_args()
    plan = check_plan()
    np, torch = setup(a.threads)
    device = pick_device(torch, a.device)
    state = pathlib.Path(a.state)
    state.mkdir(parents=True, exist_ok=True)
    dev, sealed = dev_ids(plan), sealed_ids(plan)

    def find(lid):
        for idir, tdir in zip(a.img_dirs, a.truth_dirs):
            if (pathlib.Path(idir) / f"{lid}.sms").exists():
                return load(np, lid, idir, tdir)
        raise SystemExit(f"missing private inputs for {lid}")

    if a.mode == "fit-fold":
        if a.held not in dev:
            raise SystemExit("held-out lineage must be a development lineage")
        train = [find(i) for i in dev if i != a.held]
        held = find(a.held)
        assert not ({t["name"] for t in train} & set(sealed)) and held["name"] not in sealed
        for name in a.models:
            for seed in SEEDS:
                path = state / f"logit_{name}_{a.held}_s{seed}.npy"
                if valid_result(np, path, held["n"]):
                    continue
                model = fit(np, torch, train, name, seed, device)
                publish(np, path, infer(np, torch, model, held))
                print("fit", a.held, name, seed, str(device), flush=True)
        return
    raise SystemExit("eval/final: see segarecomp_ml_region_z80_gen2_eval.py")


if __name__ == "__main__":
    main()
