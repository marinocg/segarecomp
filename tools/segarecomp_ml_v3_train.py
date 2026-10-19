#!/usr/bin/env python3
"""SEG-072 (ADR 0151/0153): OFFLINE training and scoring of the M68K ML v3 candidates (private research environment).

numpy / scikit-learn / PyTorch are NOT product dependencies and are imported lazily. Every protocol definition (folds, labels, budgets,
selection) lives in the stdlib `segarecomp_ml_v3.py`; this file refuses to run when the committed plan differs from it. Inputs are the
private research registry (`segarecomp_ml_v3_data.py register`); outputs are private score arrays plus aggregate logs.

  frozen-m3    score every development image with the unchanged production gen2 M3 weights (M3_FROZEN zero-shot part)
  fit-fold     train one candidate on the training lineages of a leave-one-lineage-out fold and score the held-out lineage's images
  bench        bounded hardware benchmark of the frozen M4-Dense training step (CPU 2-thread vs ROCm), ADR 0151 `hardware`
  fit-final    refit the frozen configuration on all development lineages (after the freeze)

Candidates: M3R (gen2 M3 architecture/representation/recipe; masked cells carry no loss), M1 (gen2 M1 HistGradientBoosting over the 219
multiscale aggregates; masked cells excluded), M4 (dense per-word occupancy CNN, seeds 1-3; cell score = max word logit).
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

ROOT = pathlib.Path(os.environ.get("SEG072_ROOT", "/root/dev/segarecomp-harness/.cache/seg072"))
DATA = ROOT / "data"
SCORES = ROOT / "scores"

# ------------------------------------------------------------------------------------------------------------- frozen M4 recipe
M4 = {"margin": 512, "central": 2048, "infer_central": 4096, "batch": 8, "steps": 4000, "lr": 0.002, "lr_drop_at": 0.8, "lr_drop": 0.1,
      "weight_decay": 1e-4, "seeds": (1, 2, 3), "local_channels": 32, "context_channels": 32, "dilations": (1, 2, 4, 8, 16, 32), "start_quantum": 16,
      "threads": 2}
TOKEN_DIMS = {"status": 2, "length": 2, "family": 3, "control": 3, "ea_src": 3, "ea_dst": 3, "width": 2, "flags": 2}
TOKEN_CARD = {"status": 3, "length": 6, "family": 8, "control": 8, "ea_src": 13, "ea_dst": 13, "width": 4, "flags": 4}
TOKEN_FIELDS = ("status", "length", "family", "control", "ea_src", "ea_dst", "width", "flags")


def np_():
    import numpy
    return numpy


# ------------------------------------------------------------------------------------------------------------------- images
class Image:
    """One registered development image: bytes, CPU decode tokens, per-byte classes, cell/word labels, seed cells."""

    def __init__(self, meta_dir: pathlib.Path):
        np = np_()
        v3.refuse_sealed_path(meta_dir)
        meta = json.loads((meta_dir / "meta.json").read_text())
        self.id, self.lineage, self.kind = meta["id"], meta["lineage"], meta["kind"]
        v3.assert_no_sealed([self.id.split("/")[0]])
        self.rom = pathlib.Path(meta["image"]).read_bytes()
        if __import__("hashlib").sha256(self.rom).hexdigest() != meta["sha256"]:
            raise SystemExit(f"{self.id}: image identity mismatch")
        self.size = len(self.rom)
        self.classes = (meta_dir / "classes.bin").read_bytes()
        self.n_words = self.size // 2
        tok = (meta_dir / "tokens.bin").read_bytes()
        self.tokens = np.frombuffer(tok, dtype=np.uint8).reshape(self.n_words, len(TOKEN_FIELDS))
        arr = np.frombuffer(self.rom[:self.n_words * 2], dtype=np.uint8).reshape(self.n_words, 2)
        self.hi, self.lo = arr[:, 0], arr[:, 1]
        self.labels = np.array(v3.cell_labels(self.classes), dtype=np.int8)
        self.n_cells = len(self.labels)
        self.wlabels = np.array(v3.word_labels(self.classes), dtype=np.int8)
        self.seeds = set(json.loads((meta_dir / "seeds.json").read_text()))
        self.starts = v3.starts_of(self.classes)
        self._padded = None

    def padded(self):
        """gen2 representation: word-block symmetric reflection, `n_cells*64 + 448` positions."""
        if self._padded is None:
            import segarecomp_ml_region_gen2 as g
            np = np_()
            length = self.n_cells * g.CELL_WORDS + 2 * g.CONTEXT_PAD_WORDS
            idx = np.array([g.reflect_index(j - g.CONTEXT_PAD_WORDS, self.n_words) for j in range(length)], dtype=np.int64)
            self._padded = (self.hi[idx], self.lo[idx], self.tokens[idx])
        return self._padded


def registry(ids=None) -> list:
    out = []
    for meta in sorted(DATA.rglob("meta.json")):
        m = json.loads(meta.read_text())
        if "audit" not in m:
            continue
        if ids is not None and m["id"] not in ids and m["lineage"] not in ids:
            continue
        out.append(meta.parent)
    return out


def load_images(lineages) -> list:
    return [Image(d) for d in registry(set(lineages))]


def fold_lineages(fold: str) -> tuple[list, list]:
    qualified = qualified_lineages()
    folds = dict(v3.lolo_folds(qualified))
    if fold not in folds:
        raise SystemExit(f"unknown fold {fold}")
    held = fold.split("+")
    train = folds[fold]
    v3.assert_no_sealed(held + train)
    if set(held) & set(train):
        raise SystemExit("fold leak")
    return held, train


def qualified_lineages() -> list:
    man = json.loads((HERE / "segarecomp_ml_v3.corpus_manifest.json").read_text())
    return [d["lineage"] for d in man["development"] if d["qualification"] == "QUALIFIED"]


# ------------------------------------------------------------------------------------------------------------- frozen M3 / M3-R
def sigmoid(x):
    np = np_()
    return 1.0 / (1.0 + np.exp(-np.clip(x, -700.0, 700.0)))


def frozen_m3_model():
    import torch
    import segarecomp_ml_region_gen2_train as tr
    model = tr.build_model(True)
    w = json.loads((HERE / "segarecomp_ml_region_gen2.model.json").read_text())["weights"]
    sd = model.state_dict()
    missing = [k for k in sd if k not in w and not k.endswith("num_batches_tracked")]
    if missing:
        raise SystemExit(f"frozen model lacks {missing}")
    model.load_state_dict({k: (torch.tensor(w[k], dtype=torch.float32) if k in w else v) for k, v in sd.items()})
    model.eval()
    return model


def train_m3r(images: list, log=print):
    """gen2 `train_cnn` (M3, seed 49, 30 epochs, segments of 32 cells, batches of 8, AdamW + per-step cosine, grad clip 5, 4 threads) with one
    change fixed by ADR 0151: masked cells (label -1) carry loss weight 0 and pos_weight counts labelled cells only."""
    import torch
    import segarecomp_ml_region_gen2_train as tr
    np = np_()
    R = tr.RECIPE
    torch.set_num_threads(R["torch_threads"])
    torch.use_deterministic_algorithms(True)
    torch.manual_seed(R["seed"])
    rng = np.random.RandomState(R["seed"])
    model = tr.build_model(True)
    opt = torch.optim.AdamW(model.parameters(), lr=R["learning_rate"], weight_decay=R["weight_decay"], betas=tuple(R["betas"]))
    n_pos = sum(int((im.labels == 1).sum()) for im in images)
    n_neg = sum(int((im.labels == 0).sum()) for im in images)
    pos_weight = torch.tensor(n_neg / n_pos, dtype=torch.float32)
    seg, per_batch, epochs = R["segment_cells"], R["segments_per_batch"], R["epochs"]
    steps_per_epoch, step, t0 = None, 0, time.time()
    for epoch in range(1, epochs + 1):
        segments = []
        for k, im in enumerate(images):
            off = int(rng.randint(0, seg))
            segments += [(k, s) for s in range(off, im.n_cells - seg + 1, seg)]
        order = rng.permutation(len(segments))
        batches = [order[i:i + per_batch] for i in range(0, len(order) - per_batch + 1, per_batch)]
        steps_per_epoch = steps_per_epoch or len(batches)
        total = steps_per_epoch * epochs
        model.train()
        losses = []
        for b in batches:
            for gr in opt.param_groups:
                gr["lr"] = 0.5 * R["learning_rate"] * (1 + math.cos(math.pi * min(step, total) / total))
            hs, ls, ts, ys, ws = [], [], [], [], []
            for kk in b:
                k, s = segments[int(kk)]
                h, l, t = tr._batch(images[k], [s], seg, True)
                lab = images[k].labels[s:s + seg]
                hs.append(h), ls.append(l), ts.append(t)
                ys.append(torch.from_numpy((lab == 1).astype(np.float32)))
                ws.append(torch.from_numpy((lab >= 0).astype(np.float32)))
            logits = model(torch.cat(hs), torch.cat(ls), torch.cat(ts))
            y, w = torch.stack(ys), torch.stack(ws)
            loss = (torch.nn.functional.binary_cross_entropy_with_logits(logits, y, pos_weight=pos_weight, reduction="none") * w).mean()
            opt.zero_grad()
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), R["grad_clip_norm"])
            opt.step()
            losses.append(float(loss))
            step += 1
        log(f"epoch {epoch}/{epochs} loss {sum(losses) / max(1, len(losses)):.5f} t={time.time() - t0:.0f}s")
    return model


def score_m3(model, im) -> "np.ndarray":
    import segarecomp_ml_region_gen2_train as tr
    return tr.score_logits(model, im, True)


# --------------------------------------------------------------------------------------------------------------------------- M1
def fit_m1(images: list):
    import segarecomp_ml_region_gen2_train as tr
    from sklearn.ensemble import HistGradientBoostingClassifier
    np = np_()
    xs, ys = [], []
    for im in images:
        f = tr.m1_features(im)
        keep = im.labels >= 0
        xs.append(f[keep])
        ys.append((im.labels[keep] == 1).astype(np.int64))
    return HistGradientBoostingClassifier(**tr.HGB_CONFIG).fit(np.concatenate(xs), np.concatenate(ys))


def score_m1(clf, im):
    import segarecomp_ml_region_gen2_train as tr
    np = np_()
    p = clf.predict_proba(tr.m1_features(im))[:, 1]
    return np.log(np.clip(p, 1e-300, 1.0)) - np.log(np.clip(1.0 - p, 1e-300, 1.0))  # logit (rank-preserving); sigmoid restores p


# ---------------------------------------------------------------------------------------------------------------------- M4-Dense
def build_m4(torch):
    nn = torch.nn

    class Dense(nn.Module):
        def __init__(self):
            super().__init__()
            self.emb_hi, self.emb_lo = nn.Embedding(257, 8), nn.Embedding(257, 8)
            self.tok = nn.ModuleList([nn.Embedding(TOKEN_CARD[f] + 1, TOKEN_DIMS[f]) for f in TOKEN_FIELDS])
            cin = 16 + sum(TOKEN_DIMS.values())
            c, k = M4["local_channels"], M4["context_channels"]
            self.l1, self.l2 = nn.Conv1d(cin, c, 5, padding=2), nn.Conv1d(c, c, 5, padding=2)
            self.down = nn.Conv1d(cin, k, 4, stride=4)
            self.ctx = nn.ModuleList([nn.Conv1d(k, k, 5, padding=2 * d, dilation=d) for d in M4["dilations"]])
            self.fuse = nn.Conv1d(c + k, 32, 1)
            self.head = nn.Conv1d(32, 1, 1)

        def forward(self, hi, lo, tok):
            """hi, lo: (B, L) int64 with 256 = PAD; tok: (B, L, 8) int64 with cardinality = PAD. L multiple of 4. -> (B, L - 2*margin)."""
            parts = [self.emb_hi(hi), self.emb_lo(lo)] + [e(tok[:, :, i]) for i, e in enumerate(self.tok)]
            x = torch.cat(parts, dim=2).transpose(1, 2)
            loc = torch.relu(self.l2(torch.relu(self.l1(x))))
            c = torch.relu(self.down(x))
            for layer in self.ctx:
                c = c + torch.relu(layer(c))
            c = c.repeat_interleave(4, dim=2)
            y = self.head(torch.relu(self.fuse(torch.cat([loc, c], dim=1)))).squeeze(1)
            m = M4["margin"]
            return y[:, m:-m]

    return Dense()


def m4_window(np, im, start, central):
    """Word window [start - margin, start + central + margin); PAD outside the image; labels for the centre (-1 outside/masked)."""
    m = M4["margin"]
    n = im.n_words
    idx = np.arange(start - m, start + central + m)
    ok = (idx >= 0) & (idx < n)
    cl = np.clip(idx, 0, max(0, n - 1))
    hi = np.where(ok, im.hi[cl].astype(np.int64), 256)
    lo = np.where(ok, im.lo[cl].astype(np.int64), 256)
    tok = np.where(ok[:, None], im.tokens[cl].astype(np.int64), np.array([TOKEN_CARD[f] for f in TOKEN_FIELDS], dtype=np.int64)[None, :])
    cidx = np.arange(start, start + central)
    cok = (cidx >= 0) & (cidx < n)
    y = np.where(cok, im.wlabels[np.clip(cidx, 0, max(0, n - 1))].astype(np.int64), -1)
    return hi.astype(np.int64), lo.astype(np.int64), tok.astype(np.int64), y.astype(np.int64)


def m4_pos_weight(np, images):
    by = {}
    for im in images:
        p, q = int((im.wlabels == 1).sum()), int((im.wlabels == 0).sum())
        a = by.setdefault(im.lineage, [0, 0])
        a[0] += p
        a[1] += q
    ratios = [q / p for p, q in by.values() if p and q]
    return float(np.mean(ratios))


def fit_m4(images: list, seed: int, device=None, steps=None, log=None):
    import torch
    np = np_()
    dev = device or torch.device("cpu")
    torch.manual_seed(seed)
    rng = np.random.default_rng(seed)
    model = build_m4(torch).to(dev)
    opt = torch.optim.AdamW(model.parameters(), lr=M4["lr"], weight_decay=M4["weight_decay"])
    pos_weight = m4_pos_weight(np, images)
    lineages = sorted({im.lineage for im in images})
    by_lin = {L: [im for im in images if im.lineage == L] for L in lineages}
    sizes = {L: np.array([im.n_words for im in by_lin[L]], dtype=np.float64) for L in lineages}
    q, central = M4["start_quantum"], M4["central"]
    total = steps or M4["steps"]
    model.train()
    t0 = time.time()
    for step in range(total):
        for gr in opt.param_groups:
            gr["lr"] = M4["lr"] * (M4["lr_drop"] if step >= int(M4["lr_drop_at"] * total) else 1.0)
        hs, ls, ts, ys = [], [], [], []
        for _ in range(M4["batch"]):
            L = lineages[int(rng.integers(0, len(lineages)))]
            im = by_lin[L][int(rng.choice(len(by_lin[L]), p=sizes[L] / sizes[L].sum()))]
            lo_q, hi_q = -(central // 2) // q, (im.n_words - central // 2) // q
            start = int(rng.integers(lo_q, max(lo_q, hi_q) + 1)) * q
            h, l, t, y = m4_window(np, im, start, central)
            hs.append(h), ls.append(l), ts.append(t), ys.append(y)
        hb = torch.from_numpy(np.stack(hs)).to(dev)
        lb = torch.from_numpy(np.stack(ls)).to(dev)
        tb = torch.from_numpy(np.stack(ts)).to(dev)
        yb = torch.from_numpy(np.stack(ys)).to(dev)
        logits = model(hb, lb, tb)
        mask = yb >= 0
        if not bool(mask.any()):
            continue
        lg, tgt = logits[mask], yb[mask].float()
        one = torch.ones((), device=dev)
        w = torch.where(tgt > 0.5, one * pos_weight, one)
        loss = (torch.nn.functional.binary_cross_entropy_with_logits(lg, tgt, reduction="none") * w).sum() / w.sum()
        opt.zero_grad()
        loss.backward()
        opt.step()
        if log and (step + 1) % 500 == 0:
            log(f"step {step + 1}/{total} loss {float(loss):.5f} t={time.time() - t0:.0f}s")
    model.eval()
    return model


def infer_m4(model, im):
    import torch
    np = np_()
    dev = next(model.parameters()).device
    out = np.zeros(im.n_words, dtype=np.float32)
    c = M4["infer_central"]
    with torch.no_grad():
        for start in range(0, im.n_words, c):
            h, l, t, _ = m4_window(np, im, start, c)
            lg = model(torch.from_numpy(h[None]).to(dev), torch.from_numpy(l[None]).to(dev), torch.from_numpy(t[None]).to(dev))[0].float().cpu().numpy()
            k = min(c, im.n_words - start)
            out[start:start + k] = lg[:k]
    return out


def cell_max(word_logits, size: int, cell_bytes: int):
    """Cell score = max word logit over the cell's words (an odd trailing byte belongs to the last cell)."""
    np = np_()
    words = cell_bytes // 2
    n = v3.cell_count(size, cell_bytes)
    pad = n * words - len(word_logits)
    w = np.concatenate([word_logits, np.full(pad, -np.inf, dtype=np.float32)]) if pad > 0 else word_logits
    return w.reshape(n, words).max(axis=1).astype(np.float64)


# ----------------------------------------------------------------------------------------------------------------------- commands
def save_scores(path: pathlib.Path, arrays: dict) -> None:
    np = np_()
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp.npz")
    np.savez(tmp, **{k.replace("/", "@@"): v for k, v in arrays.items()})
    tmp.replace(path)


def cmd_frozen_m3(a) -> int:
    import torch
    v3.verify_plan()
    torch.set_num_threads(a.threads)
    model = frozen_m3_model()
    out = {}
    for d in registry():
        im = Image(d)
        out[im.id] = score_m3(model, im)
        print("frozen-m3", im.id, flush=True)
    save_scores(SCORES / "M3_FROZEN.production.npz", out)
    return 0


def cmd_fit_fold(a) -> int:
    v3.verify_plan()
    held, train = fold_lineages(a.fold)
    tr_imgs = load_images(train)
    held_imgs = load_images(held)
    if {im.lineage for im in tr_imgs} & set(held):
        raise SystemExit("held-out lineage in training data")
    t0 = time.time()
    log = (lambda m: print(f"[{a.model}/{a.fold}] {m}", flush=True))
    if a.model == "M3R":
        model = train_m3r(tr_imgs, log)
        scores = {im.id: score_m3(model, im) for im in held_imgs}
        path = SCORES / f"M3R.{a.fold}.npz"
    elif a.model == "M1":
        clf = fit_m1(tr_imgs)
        scores = {im.id: score_m1(clf, im) for im in held_imgs}
        path = SCORES / f"M1.{a.fold}.npz"
    elif a.model == "M4":
        import torch
        torch.set_num_threads(M4["threads"])
        torch.use_deterministic_algorithms(True)
        dev = torch.device(a.device)
        model = fit_m4(tr_imgs, a.seed, dev, log=log)
        scores = {im.id: infer_m4(model, im) for im in held_imgs}
        path = SCORES / f"M4.{a.fold}.s{a.seed}.npz"
    else:
        raise SystemExit("unknown model")
    save_scores(path, scores)
    rec = {"model": a.model, "fold": a.fold, "train": train, "held": held, "seconds": time.time() - t0, "images": len(held_imgs),
           "seed": a.seed if a.model == "M4" else (49 if a.model == "M3R" else 49)}
    (SCORES / (path.stem + ".json")).write_text(json.dumps(rec, sort_keys=True))
    print(json.dumps(rec), flush=True)
    return 0


def cmd_bench(a) -> int:
    """Hardware benchmark: warm-up + one representative epoch (500 steps) of the frozen M4 training step; repeated; plus reproducibility."""
    import torch
    import resource
    np = np_()
    v3.verify_plan()
    torch.set_num_threads(a.threads)
    torch.use_deterministic_algorithms(True, warn_only=a.device != "cpu")
    dev = torch.device(a.device)
    imgs = load_images(qualified_lineages())
    res = {"device": a.device, "torch": torch.__version__, "threads": a.threads, "runs": []}
    for r in range(a.repeats):
        if a.device != "cpu":
            torch.cuda.synchronize()
            torch.cuda.reset_peak_memory_stats()
        fit_m4(imgs, 1, dev, steps=a.warmup)
        if a.device != "cpu":
            torch.cuda.synchronize()
        t0 = time.time()
        model = fit_m4(imgs, 1, dev, steps=a.steps)
        if a.device != "cpu":
            torch.cuda.synchronize()
        dt = time.time() - t0
        probe = infer_m4(model, imgs[0])
        res["runs"].append({"epoch_seconds": dt, "steps": a.steps, "ms_per_step": 1000 * dt / a.steps,
                            "peak_rss_mb": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024,
                            "peak_vram_mb": (torch.cuda.max_memory_allocated() / 2 ** 20) if a.device != "cpu" else None,
                            "probe_digest": __import__("hashlib").sha256(probe.tobytes()).hexdigest()})
        print(json.dumps(res["runs"][-1]), flush=True)
    t_data = time.time()
    rng = np.random.default_rng(0)
    for _ in range(200):
        im = imgs[int(rng.integers(0, len(imgs)))]
        m4_window(np, im, 0, M4["central"])
    res["data_loading_ms_per_batch"] = (time.time() - t_data) / 200 * 1000 * M4["batch"]
    out = ROOT / "bench" / f"bench.{a.device}.{a.tag}.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(res, indent=1))
    print(json.dumps({k: v for k, v in res.items() if k != "runs"}))
    return 0


class ExternalImage(Image):
    """An image outside the development registry (used ONLY by the one-shot sealed evaluation after the freeze)."""

    def __init__(self, image_path, tokens_path, classes_path, seeds, iid, lineage):
        np = np_()
        self.id, self.lineage, self.kind = iid, lineage, "genesis_rom"
        self.rom = pathlib.Path(image_path).read_bytes()
        self.size = len(self.rom)
        self.classes = pathlib.Path(classes_path).read_bytes()
        self.n_words = self.size // 2
        self.tokens = np.frombuffer(pathlib.Path(tokens_path).read_bytes(), dtype=np.uint8).reshape(self.n_words, len(TOKEN_FIELDS))
        arr = np.frombuffer(self.rom[:self.n_words * 2], dtype=np.uint8).reshape(self.n_words, 2)
        self.hi, self.lo = arr[:, 0], arr[:, 1]
        self.labels = np.array(v3.cell_labels(self.classes), dtype=np.int8)
        self.n_cells = len(self.labels)
        self.wlabels = np.array(v3.word_labels(self.classes), dtype=np.int8)
        self.seeds = set(seeds)
        self.starts = v3.starts_of(self.classes)
        self._padded = None


FINAL = ROOT / "final"


def cmd_fit_final(a) -> int:
    """Refit the selected configuration on ALL qualified development lineages (same recipe/seeds) and persist private weights + digest."""
    import hashlib
    import torch
    v3.verify_plan()
    imgs = load_images(qualified_lineages())
    FINAL.mkdir(parents=True, exist_ok=True)
    digests = {}
    if a.model == "M3R":
        model = train_m3r(imgs, lambda m: print(f"[final M3R] {m}", flush=True))
        path = FINAL / "M3R.pt"
        torch.save(model.state_dict(), path)
        digests["M3R"] = hashlib.sha256(b"".join(t.detach().numpy().tobytes() for t in model.state_dict().values())).hexdigest()
    elif a.model == "M4":
        torch.set_num_threads(M4["threads"])
        torch.use_deterministic_algorithms(True)
        for seed in M4["seeds"]:
            model = fit_m4(imgs, seed, torch.device("cpu"), log=lambda m: print(f"[final M4 s{seed}] {m}", flush=True))
            path = FINAL / f"M4.s{seed}.pt"
            torch.save(model.state_dict(), path)
            digests[f"M4.s{seed}"] = hashlib.sha256(b"".join(t.detach().numpy().tobytes() for t in model.state_dict().values())).hexdigest()
    elif a.model == "M1":
        import pickle
        clf = fit_m1(imgs)
        path = FINAL / "M1.pkl"
        path.write_bytes(pickle.dumps(clf))
        digests["M1"] = hashlib.sha256(path.read_bytes()).hexdigest()
    else:
        raise SystemExit("unknown model")
    (FINAL / f"{a.model}.digests.json").write_text(json.dumps(digests, indent=1, sort_keys=True))
    print(json.dumps(digests))
    return 0


def final_scorer(configuration: str):
    """Returns f(image) -> (cell scores, cell bytes) for the frozen configuration's final (all-development) model."""
    import torch
    np = np_()
    if configuration == "M3_FROZEN":
        model = frozen_m3_model()
        return lambda im: (score_m3(model, im), 128)
    if configuration == "M3_R":
        import segarecomp_ml_region_gen2_train as tr
        model = tr.build_model(True)
        model.load_state_dict(torch.load(FINAL / "M3R.pt"))
        model.eval()
        return lambda im: (score_m3(model, im), 128)
    if configuration == "M1_HGB":
        import pickle
        clf = pickle.loads((FINAL / "M1.pkl").read_bytes())
        return lambda im: (score_m1(clf, im), 128)
    if configuration.startswith("M4_DENSE"):
        cell = int(configuration.split("@")[1])
        models = []
        for seed in M4["seeds"]:
            m = build_m4(torch)
            m.load_state_dict(torch.load(FINAL / f"M4.s{seed}.pt"))
            m.eval()
            models.append(m)

        def f(im):
            wl = np.mean([infer_m4(m, im) for m in models], axis=0).astype(np.float32)
            return cell_max(wl, im.size, cell), cell
        return f
    raise SystemExit("unknown configuration")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("frozen-m3")
    f.add_argument("--threads", type=int, default=4)
    f.set_defaults(func=cmd_frozen_m3)
    k = sub.add_parser("fit-fold")
    k.add_argument("--model", required=True, choices=["M3R", "M1", "M4"])
    k.add_argument("--fold", required=True)
    k.add_argument("--seed", type=int, default=1)
    k.add_argument("--device", default="cpu")
    k.set_defaults(func=cmd_fit_fold)
    ff = sub.add_parser("fit-final")
    ff.add_argument("--model", required=True, choices=["M3R", "M1", "M4"])
    ff.set_defaults(func=cmd_fit_final)
    b = sub.add_parser("bench")
    b.add_argument("--device", default="cpu")
    b.add_argument("--threads", type=int, default=2)
    b.add_argument("--warmup", type=int, default=50)
    b.add_argument("--steps", type=int, default=500)
    b.add_argument("--repeats", type=int, default=3)
    b.add_argument("--tag", default="run")
    b.set_defaults(func=cmd_bench)
    a = ap.parse_args()
    return a.func(a)


if __name__ == "__main__":
    raise SystemExit(main())
