#!/usr/bin/env python3
"""SEG-056 weight artifact (ADR 0117 recovery note): EXPERIMENTAL, not production-adopted. Pure stdlib reader/writer/verifier.

Blob `segarecomp_ml_region_z80_gen2.weights.sgz80w` (all integers and float32 little-endian):
  magic "SGZ80W01"(8) | u32 version=1 | u32 n_models
  per model (seed order): u32 seed | u32 n_tensors
    per tensor (sorted by name, bytewise UTF-8): u16 name_len | name | u8 dtype (0 = float32) | u8 rank | u32 dims[rank]
                                                | float32 data[prod(dims)] (row-major, as torch .numpy().tobytes())
Metadata `segarecomp_ml_region_z80_gen2.weights.json` binds the blob (blob_sha256), the original ensemble identity
(weights_sha256 = SHA-256 over, for each model in seed order and each tensor sorted by name, name bytes then raw data) and the
frozen plan digest. No PyTorch/NumPy is required to read or verify. CLI: `verify [--blob P] [--meta P]`."""
from __future__ import annotations

import hashlib
import json
import pathlib
import struct
import sys

HERE = pathlib.Path(__file__).resolve().parent
BLOB = HERE / "segarecomp_ml_region_z80_gen2.weights.sgz80w"
META = HERE / "segarecomp_ml_region_z80_gen2.weights.json"
PLAN_DIGEST = HERE / "segarecomp_ml_region_z80_gen2.plan.digest"
MAGIC = b"SGZ80W01"
VERSION = 1
SEEDS = (1, 2, 3)
ORIGINAL_WEIGHTS_SHA256 = "6bee77726a93ad33504522c35b84aaa6d89ed3c5327ff5fae6bff1e9558a4f11"
PLAN_SHA256 = "21db1afb2634e4d1765ecd5a0da8578a0d388029ef1d75f55bb87b0a3ab1a852"
# D512HP tensor layout (torch state_dict names, sorted): the architecture contract of the 43,809-parameter network
SHAPES = {
    "ctx.0.bias": (32,), "ctx.0.weight": (32, 32, 5), "ctx.1.bias": (32,), "ctx.1.weight": (32, 32, 5),
    "ctx.2.bias": (32,), "ctx.2.weight": (32, 32, 5), "ctx.3.bias": (32,), "ctx.3.weight": (32, 32, 5),
    "ctx.4.bias": (32,), "ctx.4.weight": (32, 32, 5), "ctx.5.bias": (32,), "ctx.5.weight": (32, 32, 5),
    "down.bias": (32,), "down.weight": (32, 16, 4), "emb.weight": (257, 16),
    "fuse.bias": (32,), "fuse.weight": (32, 56, 1), "head.bias": (1,), "head.weight": (1, 32, 1),
    "l1.bias": (24,), "l1.weight": (24, 16, 5), "l2.bias": (24,), "l2.weight": (24, 24, 5),
}
PARAMS_PER_MODEL = 43809


class FormatError(ValueError):
    pass


def _prod(dims):
    n = 1
    for d in dims:
        n *= d
    return n


def encode(models):
    """models: list of (seed, [(name, shape, float32 LE bytes)]) -> blob bytes. Tensors are written sorted by name."""
    out = [MAGIC, struct.pack("<II", VERSION, len(models))]
    for seed, tensors in models:
        out.append(struct.pack("<II", seed, len(tensors)))
        for name, shape, data in sorted(tensors, key=lambda t: t[0].encode()):
            nb = name.encode()
            if len(data) != 4 * _prod(shape):
                raise FormatError(f"{name}: data length does not match shape")
            out.append(struct.pack("<H", len(nb)) + nb + struct.pack("<BB", 0, len(shape)))
            out.append(struct.pack(f"<{len(shape)}I", *shape) + data)
    return b"".join(out)


def decode(blob):
    """Strict parse -> list of (seed, [(name, shape, bytes)]). Rejects truncation, trailing bytes and unknown dtype/version."""
    pos = 0

    def take(n):
        nonlocal pos
        if n < 0 or pos + n > len(blob):
            raise FormatError("truncated weight blob")
        v = blob[pos:pos + n]
        pos += n
        return v

    if take(8) != MAGIC:
        raise FormatError("bad magic")
    version, n_models = struct.unpack("<II", take(8))
    if version != VERSION:
        raise FormatError(f"unsupported version {version}")
    if n_models > 64:
        raise FormatError("implausible model count")
    models = []
    for _ in range(n_models):
        seed, n_t = struct.unpack("<II", take(8))
        if n_t > 4096:
            raise FormatError("implausible tensor count")
        tensors, prev = [], None
        for _ in range(n_t):
            (nl,) = struct.unpack("<H", take(2))
            name = take(nl).decode("utf-8")
            dtype, rank = struct.unpack("<BB", take(2))
            if dtype != 0:
                raise FormatError(f"{name}: unsupported dtype {dtype}")
            if rank > 8:
                raise FormatError(f"{name}: implausible rank")
            shape = struct.unpack(f"<{rank}I", take(4 * rank))
            if prev is not None and name.encode() <= prev:
                raise FormatError("tensors not strictly sorted by name")
            prev = name.encode()
            tensors.append((name, tuple(shape), take(4 * _prod(shape))))
        models.append((seed, tensors))
    if pos != len(blob):
        raise FormatError("trailing bytes after weight blob")
    return models


def ensemble_digest(models):
    """The ORIGINAL SEG-056 identity: per model (seed order), per tensor sorted by name: name bytes then raw float32 bytes."""
    h = hashlib.sha256()
    for _, tensors in models:
        for name, _, data in tensors:
            h.update(name.encode())
            h.update(data)
    return h.hexdigest()


def verify(blob_path=BLOB, meta_path=META, plan_digest_path=PLAN_DIGEST):
    """Full integrity check; raises FormatError/ValueError on any defect, returns the metadata on success."""
    blob = pathlib.Path(blob_path).read_bytes()
    meta = json.loads(pathlib.Path(meta_path).read_text())
    if hashlib.sha256(blob).hexdigest() != meta.get("blob_sha256") or len(blob) != meta.get("blob_bytes"):
        raise FormatError("artifact digest/size does not match metadata")
    models = decode(blob)
    if tuple(s for s, _ in models) != SEEDS or meta.get("seeds") != list(SEEDS):
        raise FormatError("model/seed order is not (1, 2, 3)")
    for seed, tensors in models:
        if {n: s for n, s, _ in tensors} != SHAPES:
            raise FormatError(f"seed {seed}: tensor names/shapes differ from the D512HP architecture")
        if sum(_prod(s) for _, s, _ in tensors) != PARAMS_PER_MODEL:
            raise FormatError(f"seed {seed}: parameter count mismatch")
        for name, _, data in tensors:
            vals = struct.unpack(f"<{len(data) // 4}f", data)
            if any(v != v or v in (float("inf"), float("-inf")) for v in vals):
                raise FormatError(f"seed {seed} {name}: non-finite value")
    d = ensemble_digest(models)
    if d != meta.get("weights_sha256") or d != ORIGINAL_WEIGHTS_SHA256:
        raise FormatError("ensemble digest differs from the original SEG-056 weights identity")
    if meta.get("plan_digest") != PLAN_SHA256:
        raise FormatError("metadata plan digest is not the frozen SEG-056 plan digest")
    pd = pathlib.Path(plan_digest_path)
    if pd.exists() and pd.read_text().strip() != PLAN_SHA256:
        raise FormatError("committed plan digest differs from the frozen plan digest")
    if meta.get("status") != "experimental-not-production-adopted":
        raise FormatError("metadata status must state the artifact is experimental and not production-adopted")
    return meta


def main(argv):
    if len(argv) >= 1 and argv[0] == "verify":
        blob, meta = BLOB, META
        if "--blob" in argv:
            blob = argv[argv.index("--blob") + 1]
        if "--meta" in argv:
            meta = argv[argv.index("--meta") + 1]
        m = verify(blob, meta)
        print("OK weights_sha256", m["weights_sha256"], "blob_sha256", m["blob_sha256"])
        return 0
    print("usage: segarecomp_ml_region_z80_gen2_weights.py verify [--blob P] [--meta P]", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
