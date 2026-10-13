#!/usr/bin/env python3
"""SEG-056 weight artifact: stdlib-only integrity tests (no numpy/PyTorch/ROM/truth). Synthetic tensors exercise the codec;
the committed artifact is verified against the recorded original ensemble digest."""
import json
import os
import shutil
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import segarecomp_ml_region_z80_gen2_weights as W  # noqa: E402


def synthetic():
    models = []
    for seed in W.SEEDS:
        ts = []
        for i, (name, shape) in enumerate(sorted(W.SHAPES.items())):
            n = W._prod(shape)
            ts.append((name, shape, struct.pack(f"<{n}f", *[((j * 7 + i + seed) % 13) / 8.0 - 0.75 for j in range(n)])))
        models.append((seed, ts))
    return models


class Codec(unittest.TestCase):
    def test_round_trip_synthetic(self):
        m = synthetic()
        blob = W.encode(m)
        self.assertEqual(W.decode(blob), m)
        self.assertEqual(W.encode(W.decode(blob)), blob)
        self.assertEqual(W.ensemble_digest(W.decode(blob)), W.ensemble_digest(m))

    def test_architecture_parameter_count(self):
        self.assertEqual(sum(W._prod(s) for s in W.SHAPES.values()), W.PARAMS_PER_MODEL)

    def test_rejects_defects(self):
        blob = W.encode(synthetic())
        bad = [blob[:-1], blob + b"\0", blob[:7], b"XXXXXXXX" + blob[8:],
               blob[:8] + struct.pack("<I", 2) + blob[12:], blob[:12] + struct.pack("<I", 999) + blob[16:]]
        for b in bad:
            with self.assertRaises(W.FormatError):
                W.decode(b)

    def test_dtype_and_order_rejected(self):
        m = synthetic()
        blob = W.encode(m)
        off = 16 + 8 + 2 + len(m[0][1][0][0].encode())  # dtype byte of the first tensor
        with self.assertRaises(W.FormatError):
            W.decode(blob[:off] + b"\x01" + blob[off + 1:])
        with self.assertRaises(W.FormatError):
            W.encode([(1, [("a", (2,), b"\0" * 4)])])  # length does not match shape


class Committed(unittest.TestCase):
    def test_committed_artifact_matches_original_identity(self):
        meta = W.verify()
        self.assertEqual(meta["weights_sha256"], W.ORIGINAL_WEIGHTS_SHA256)
        self.assertEqual(meta["status"], "experimental-not-production-adopted")
        self.assertEqual(meta["architecture"], "D512HP")

    def test_corrupted_copy_is_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            blob, meta = os.path.join(d, "b.bin"), os.path.join(d, "m.json")
            shutil.copy(W.BLOB, blob)
            shutil.copy(W.META, meta)
            W.verify(blob, meta)
            raw = bytearray(open(blob, "rb").read())
            raw[-3] ^= 0x01
            open(blob, "wb").write(raw)
            with self.assertRaises(W.FormatError):  # artifact digest no longer matches
                W.verify(blob, meta)
            m = json.load(open(meta))  # re-bind the artifact digest: the ENSEMBLE identity must still reject it
            import hashlib
            m["blob_sha256"] = hashlib.sha256(bytes(raw)).hexdigest()
            json.dump(m, open(meta, "w"))
            with self.assertRaises(W.FormatError):
                W.verify(blob, meta)

    def test_metadata_must_stay_experimental(self):
        with tempfile.TemporaryDirectory() as d:
            meta = os.path.join(d, "m.json")
            m = json.load(open(W.META))
            m["status"] = "production"
            json.dump(m, open(meta, "w"))
            with self.assertRaises(W.FormatError):
                W.verify(W.BLOB, meta)


if __name__ == "__main__":
    unittest.main()
