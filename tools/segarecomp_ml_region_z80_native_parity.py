#!/usr/bin/env python3
"""SEG-057-T002 (ADR 0119): synthetic PyTorch reference fixture for the native D512HP evaluator.

  segarecomp_ml_region_z80_native_parity.py <fixture-out.smsmlp>            (needs numpy + PyTorch; never run in CI)

REGISTERED TOLERANCE (fixed before any parity number was looked at): per byte logit |native - reference| <= 1e-4 + 1e-4 * |reference|
(binary32; the two evaluators differ only in summation order), and the retained cell set at the frozen 65% budget must be EXACTLY equal.
Any cell-set difference is investigated, not tolerated.

The reference is the unmodified research model (`segarecomp_ml_region_z80_gen2_train.build_model` + `infer`: 4096-byte central windows with a
512-byte PAD-token context, exactly as in SEG-056) loaded from the committed weight artifact. Inputs are synthetic and public: seeded random
bytes, an impulse (receptive-field / dilation / stride-4 probe), a constant image, tiny images that are mostly PAD, a size that is not a
multiple of 4 or 32, and an image crossing several window boundaries. No ROM byte is involved.

Fixture (little endian): "SMSMLP01", u32 cases; per case: u32 n, n bytes, n float32 reference logits, u32 retained, retained u32 cells.
"""
import importlib.util
import math
import pathlib
import struct
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import numpy as np  # noqa: E402
import torch  # noqa: E402
import segarecomp_ml_region_z80_gen2_weights as weights  # noqa: E402

spec = importlib.util.spec_from_file_location("train", HERE / "segarecomp_ml_region_z80_gen2_train.py")
train = importlib.util.module_from_spec(spec)
spec.loader.exec_module(train)

ROOTS = (0x0000, 0x0038, 0x0066)


def cases():
    rng = np.random.default_rng(57)
    out = {}
    out["random_3000"] = rng.integers(0, 256, 3000, dtype=np.int64)
    impulse = np.zeros(1500, dtype=np.int64)
    impulse[700] = 0xC3
    out["impulse_1500"] = impulse
    out["tiny_37"] = rng.integers(0, 256, 37, dtype=np.int64)
    out["zeros_256"] = np.zeros(256, dtype=np.int64)
    out["ff_65"] = np.full(65, 0xFF, dtype=np.int64)
    code = np.array([0x3E, 0x01, 0xC3, 0x00, 0x10, 0xCD, 0x38, 0x00, 0xED, 0xB0, 0xDD, 0x21, 0xFF, 0xC9], dtype=np.int64)
    mixed = np.concatenate([np.tile(code, 300), rng.integers(0, 256, 1800, dtype=np.int64)])
    out["mixed_6000"] = mixed
    out["random_9001"] = rng.integers(0, 256, 9001, dtype=np.int64)
    return out


def main():
    blob = (HERE / "segarecomp_ml_region_z80_gen2.weights.sgz80w").read_bytes()
    nets = []
    for _seed, tensors in weights.decode(blob):
        net = train.build_model(torch, "D512HP")
        net.load_state_dict({n: torch.from_numpy(np.frombuffer(d, dtype="<f4").reshape(sh).copy()) for n, sh, d in tensors})
        net.eval()
        nets.append(net)
    torch.set_num_threads(1)
    parts = [b"SMSMLP01"]
    items = cases()
    parts.append(struct.pack("<I", len(items)))
    for name, data in items.items():
        n = len(data)
        item = {"n": n, "byte": data}
        logits = np.mean(np.stack([train.infer(np, torch, net, item) for net in nets]), axis=0, dtype=np.float32)
        cells = (n + 31) // 32
        scores = []
        for c in range(cells):
            seg = sorted(logits[c * 32:(c + 1) * 32].tolist(), reverse=True)[:2]
            scores.append(sum(seg) / len(seg))
        order = sorted(range(cells), key=lambda i: (-scores[i], i))
        k = min(cells, max(1, -(-cells * 65 // 100)))
        chosen = sorted(set(order[:k]) | {a // 32 for a in ROOTS if a < n})
        parts += [struct.pack("<I", n), bytes(data.astype(np.uint8).tolist()), logits.astype("<f4").tobytes(),
                  struct.pack("<I", len(chosen)), struct.pack("<%dI" % len(chosen), *chosen)]
        print(name, n, "cells", cells, "retained", len(chosen), file=sys.stderr)
    pathlib.Path(sys.argv[1]).write_bytes(b"".join(parts))


if __name__ == "__main__":
    main()
