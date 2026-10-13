#!/usr/bin/env python3
"""SEG-056 weight RECOVERY (private research tooling; needs numpy + PyTorch, which are NOT product dependencies).

Re-runs ONLY the three final D512HP fits of `segarecomp_ml_region_z80_gen2_final.py` (all eight development lineages, seeds 1,2,3,
T.fit unchanged), computes the original ensemble identity and, only if it equals the recorded digest, writes the portable artifact.
It never opens sealed truth, scores nothing, and writes no result files. On a digest mismatch nothing is written to the output
directory: the models are a DIFFERENT model and must not be substituted."""
import argparse
import hashlib
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_ml_region_z80_gen2_train as T  # noqa: E402
import segarecomp_ml_region_z80_gen2_weights as W  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img-dirs", nargs="+", required=True)
    ap.add_argument("--truth-dirs", nargs="+", required=True)
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--threads", type=int, default=8, help="the original final fit used 8")
    a = ap.parse_args()
    plan = T.check_plan()
    if T.DIGEST.read_text().strip() != W.PLAN_SHA256:
        raise SystemExit("frozen plan digest differs from the recorded SEG-056 digest")
    np, torch = T.setup(a.threads)
    device = T.pick_device(torch, "cpu")
    dev = T.dev_ids(plan)
    sealed = T.sealed_ids(plan)
    if len(dev) != 8:
        raise SystemExit("expected exactly eight development lineages")

    def find(lid):
        for idir, tdir in zip(a.img_dirs, a.truth_dirs):
            if (pathlib.Path(idir) / f"{lid}.sms").exists():
                return T.load(np, lid, idir, tdir)  # verifies image SHA-256 against its truth record
        raise SystemExit(f"missing private inputs for {lid}")

    train = [find(i) for i in dev]
    assert not ({t["name"] for t in train} & set(sealed))
    for t in train:  # image identity against the frozen plan
        pinned = next(l for l in plan["lineages"] if l["id"] == t["name"])
        sha = pinned.get("rom_sha256") or pinned.get("image_sha256")
        if sha and sha != hashlib.sha256(bytes(t["byte"].astype(np.uint8))).hexdigest():
            raise SystemExit(f"{t['name']}: image differs from the frozen plan")
    models = []
    for seed in T.SEEDS:
        m = T.fit(np, torch, train, "D512HP", seed, device)
        tensors = [(k, tuple(v.shape), v.detach().cpu().numpy().astype("<f4", copy=False).tobytes())
                   for k, v in m.state_dict().items()]
        assert all(v.dtype == torch.float32 for v in m.state_dict().values())
        models.append((seed, tensors))
        print("fitted seed", seed, flush=True)
    blob = W.encode(models)
    digest = W.ensemble_digest(W.decode(blob))
    # cross-check with the literal original algorithm (torch state_dict bytes)
    print("recovered weights_sha256", digest)
    print("expected  weights_sha256", W.ORIGINAL_WEIGHTS_SHA256)
    if digest != W.ORIGINAL_WEIGHTS_SHA256:
        raise SystemExit("MISMATCH: these are NOT the original SEG-056 weights; nothing written")
    out = pathlib.Path(a.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    meta = {
        "format": "SGZ80W01", "format_version": W.VERSION, "status": "experimental-not-production-adopted",
        "note": "SEG-056 research model. Not adopted: no Z80 selective-admission contract exists and the literal sealed retained-fraction clause failed (ADR 0117). Weights recovered by deterministic refit (ADR 0117 recovery note).",
        "architecture": "D512HP", "parameters_per_model": W.PARAMS_PER_MODEL, "seeds": list(W.SEEDS),
        "byte_order": "little-endian", "tensor_dtype": "float32",
        "weights_sha256": digest, "weights_sha256_algorithm": "per model in seed order, per tensor sorted by name: name bytes then raw little-endian float32 bytes",
        "plan_digest": W.PLAN_SHA256, "blob_sha256": hashlib.sha256(blob).hexdigest(), "blob_bytes": len(blob),
        "input_schema": {"tokens": "image bytes 0..255; PAD token 256 outside the image", "embedding": [257, 16],
                         "context_margin_bytes": T.MARGIN, "output": "one float32 logit per image byte",
                         "ensemble": "arithmetic mean of the three per-byte logits",
                         "image_independent": True},
        "cell_aggregation": {"cell_bytes": 32, "reduction": "top2 (mean of the two largest byte logits in the cell)",
                             "structural_root_addresses": ["0x0000", "0x0038", "0x0066"],
                             "frozen_ranking_budget": 0.65},
        "forward": "see segarecomp_ml_region_z80_gen2_train.build_model (D512HP: stride-4 dilated residual context path (1,2,4,8,16,32), local conv path, 1x1 fuse, 1x1 head)",
        "training": {"device": "cpu", "steps": T.STEPS, "batch": T.BATCH, "lineages": "eight development lineages", "threads": a.threads},
    }
    (out / W.BLOB.name).write_bytes(blob)
    (out / W.META.name).write_text(json.dumps(meta, indent=1, sort_keys=True) + "\n")
    print("written", len(blob), "bytes blob_sha256", meta["blob_sha256"])


if __name__ == "__main__":
    main()
