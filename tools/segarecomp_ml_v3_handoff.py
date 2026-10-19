#!/usr/bin/env python3
"""SEG-072 research handoff: build and verify the private, checksum-bound bundle of reusable M68K ML v3 artifacts (OFFLINE tooling).

  bundle  copy the required private artifacts (registry labels/tokens/seeds, truth, fold scores, final weights, results, sealed diagnostic store
          including the original one-shot marker, SGDK build outputs, gen2 historical folds) into an ignored bundle directory; record every file's
          SHA-256, every EXTERNAL input (operator-held commercial images, Sonic CD module files) by path + SHA-256 without copying it, the
          environment, the model identities and a reference inference sample computed with the preserved weights (no training)
  verify  in a fresh process: re-hash every bundled file, check external inputs, reload the final weights and compare their state-dict digests
          with the recorded identities, reproduce the reference inference sample bit for bit, check the frozen production M3 against the stored
          production scores, check fold-score files against the corpus manifest and lineage folds, and regenerate the stored one-shot sealed
          M3-R B50 regions from the preserved weights WITHOUT reading any sealed label (the one-shot evaluation is never rerun)

The bundle holds commercial-derived material (labels, listings-derived classes, scores) and must stay in ignored private storage; nothing here is
ever committed. Only this tool and a non-sensitive handoff document are public.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import platform
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
SCHEMA = "segarecomp.ml_v3_handoff.v1"
SAMPLE_DEV = "d09"          # small project-authored development image (bundled, no external input needed)
SAMPLE_SEALED = ("E01", "E05")  # frozen M3-R regions regenerated label-free; E05 is the exposed diagnostic lineage

# relative to the harness `.cache` directory
INCLUDE = [
    "seg072/data", "seg072/truth", "seg072/scores", "seg072/final", "seg072/results", "seg072/bench", "seg072/m3hist", "seg072/scripts",
    "seg072/sealed/UNSEALED.marker.json", "seg072/sealed/binding.json", "seg072/sealed/manifest_private.json", "seg072/sealed/truth",
    "seg072/sealed/data", "seg072/dev_config.json", "seg072/shingles_dev.json", "seg072/sealed_vs_dev_raw.json",
    "seg072/m1.log", "seg072/m3r.log", "seg072/m4.log", "seg072/final_m3r.log", "seg072/final_m3r_run2.log", "seg072/final_m4.log",
    "seg072/frozen_m3.log", "seg072/register.out", "seg072/sealed_post.log", "seg072/kstage_m3f_b35.log", "seg072/econ_m3f_b35.log",
    "seg049/scores", "seg049/results", "seg049/tok", "seg048/truth", "seg048/art",
]
HB_FILES = ("build.json", "classes.bin", "units.json", "app.nm", "lib.nm", "inst/out/rom.bin", "inst/out/rom.map")
HB_DEV = ("d05", "d06", "d07", "d09", "d10", "d11", "d12")
HB_SEALED = ("e06", "e07")


def sha256_file(path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def state_digest(state_dict) -> str:
    return hashlib.sha256(b"".join(t.detach().numpy().tobytes() for t in state_dict.values())).hexdigest()


def collect(cache: pathlib.Path) -> list[str]:
    rels = []
    for inc in INCLUDE:
        p = cache / inc
        if not p.exists():
            raise SystemExit(f"required artifact missing: {inc}")
        rels += [str(f.relative_to(cache)) for f in sorted(p.rglob("*")) if f.is_file()] if p.is_dir() else [inc]
    for hb in HB_DEV:
        rels += [f"seg072/hb/{hb}/{f}" for f in HB_FILES if (cache / f"seg072/hb/{hb}/{f}").exists()]
    for hb in HB_SEALED:
        rels += [f"seg072/sealed/hb/{hb}/{f}" for f in HB_FILES if (cache / f"seg072/sealed/hb/{hb}/{f}").exists()]
    return sorted(set(r for r in rels if "/__pycache__/" not in r and not r.endswith(".pyc")))


def externals(cache: pathlib.Path) -> dict:
    """Inputs referenced by the registry / sealed store that are NOT copied (operator-held commercial images, Sonic CD module files)."""
    out = {}
    for meta in sorted((cache / "seg072/data").rglob("meta.json")):
        m = json.loads(meta.read_text())
        if "image" not in m:
            continue
        img = pathlib.Path(m["image"])
        if str(img).startswith(str(cache / "seg072/hb")):
            continue  # project-authored SGDK build, bundled
        out[m["id"]] = {"path": str(img), "sha256": m["sha256"], "bytes": m["size"]}
    man = json.loads((HERE / "segarecomp_ml_v3.corpus_manifest.json").read_text())["sealed"]["entries"]
    import segarecomp_ml_v3_eval as ev
    for e in man:
        if e["qualification"] != "QUALIFIED":
            continue
        rel = ev.SEALED_PATHS[e["lineage"]][0]
        if rel.startswith("@"):
            continue  # project-authored, bundled
        out["sealed/" + e["lineage"]] = {"path": str(ev.GAMES / rel.split("/", 1)[1]), "sha256": e["image_sha256"], "bytes": e["image_bytes"]}
    return out


def environment() -> dict:
    venv = pathlib.Path(sys.executable)
    freeze = subprocess.run([sys.executable, "-m", "pip", "freeze"], capture_output=True, text=True).stdout.split()
    keep = [p for p in freeze if p.split("==")[0].lower() in ("numpy", "scikit-learn", "scipy", "torch", "joblib", "threadpoolctl")]
    return {"python": platform.python_version(), "interpreter": str(venv), "packages": keep, "device": "cpu", "precision": "float32",
            "deterministic_algorithms": True, "threads": {"M3_R": 4, "M4_DENSE": 2},
            "rocm_benchmark_environment": "torch 2.6.0+rocm6.4.2 (/root/.venvs/torch-rocm64) - benchmark only, never used for results",
            "sgdk_image": "ghcr.io/stephane-d/sgdk@sha256:b0bb8a7171f8dcbf1625685b3a839d23c5c32000019a2023d0e039dd6eadcf31",
            "wine": subprocess.run(["wine", "--version"], capture_output=True, text=True).stdout.strip() if shutil.which("wine") else None}


def reference_sample(root: pathlib.Path) -> dict:
    """Inference with the preserved weights only (no training): final M3-R and the final M4-Dense ensemble on one bundled dev image."""
    import numpy as np
    import segarecomp_ml_v3_train as tr
    meta_dir = root / "data" / SAMPLE_DEV
    m = json.loads((meta_dir / "meta.json").read_text())
    image = root / "hb" / SAMPLE_DEV / "inst/out/rom.bin"
    im = tr.ExternalImage(image, meta_dir / "tokens.bin", meta_dir / "classes.bin", [], SAMPLE_DEV, m["lineage"])
    out = {}
    for config in ("M3_R", "M4_DENSE@64", "M3_FROZEN"):
        scores, cell = tr.final_scorer(config)(im)
        out[config] = {"cells": int(len(scores)), "cell_bytes": cell, "sha256": hashlib.sha256(np.asarray(scores, dtype=np.float64).tobytes()).hexdigest()}
    return out


def sealed_regions(root: pathlib.Path, cache: pathlib.Path) -> dict:
    """Label-free regeneration of the frozen pair's one-shot regions (scores + seeds + B50 rank budget; no class string is read)."""
    import segarecomp_ml_v3 as v3
    import segarecomp_ml_v3_eval as ev
    import segarecomp_ml_v3_train as tr
    man = {e["lineage"]: e for e in json.loads((HERE / "segarecomp_ml_v3.corpus_manifest.json").read_text())["sealed"]["entries"]}
    scorer = tr.final_scorer("M3_R")
    out = {}
    for L in SAMPLE_SEALED:
        rel = ev.SEALED_PATHS[L][0]
        img = (root / rel[1:]) if rel.startswith("@") else ev.GAMES / rel.split("/", 1)[1]
        if hashlib.sha256(img.read_bytes()).hexdigest() != man[L]["image_sha256"]:
            raise SystemExit(f"{L}: sealed image identity mismatch")
        data = root / "sealed" / "data" / L.lower()
        # build the image without its class string: tokens + bytes are all the scorer needs
        import numpy as np
        rom = img.read_bytes()
        im = tr.ExternalImage.__new__(tr.ExternalImage)
        im.id = im.lineage = L
        im.kind = "genesis_rom"
        im.rom, im.size, im.n_words = rom, len(rom), len(rom) // 2
        im.tokens = np.frombuffer((data / "tokens.bin").read_bytes(), dtype=np.uint8).reshape(im.n_words, 8)
        arr = np.frombuffer(rom[:im.n_words * 2], dtype=np.uint8).reshape(im.n_words, 2)
        im.hi, im.lo = arr[:, 0], arr[:, 1]
        im.n_cells = v3.cell_count(im.size)
        im._padded = None
        scores, cell = scorer(im)
        seeds = {int(t, 16) // cell for t in (data / "d.txt").read_text().split()} if (data / "d.txt").exists() else set()
        seeds = {c for c in seeds if c < len(scores)}
        region = v3.policy_region("B50", [float(x) for x in scores], [0.0] * len(scores), seeds)["region"]
        out[L] = {"cells": len(scores), "region_sha256": hashlib.sha256(json.dumps(sorted(region)).encode()).hexdigest()}
    return out


def cmd_bundle(a) -> int:
    cache = pathlib.Path(a.cache).resolve()
    out = pathlib.Path(a.out).resolve()
    if out.exists():
        raise SystemExit("bundle directory exists (refusing to overwrite)")
    payload = out / "payload"
    files = collect(cache)
    records = {}
    for rel in files:
        src, dst = cache / rel, payload / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        records[rel] = {"sha256": sha256_file(dst), "bytes": dst.stat().st_size}
        if records[rel]["sha256"] != sha256_file(src):
            raise SystemExit(f"copy mismatch {rel}")
    os.environ["SEG072_ROOT"] = str(payload / "seg072")
    sys.path.insert(0, str(HERE))
    frozen = json.loads((HERE / "segarecomp_ml_v3.frozen.json").read_text())
    ids = {"plan_sha256": (HERE / "segarecomp_ml_v3.plan.digest").read_text().strip(),
           "frozen_record_sha256": sha256_file(HERE / "segarecomp_ml_v3.frozen.json"), "frozen_configuration": frozen["configuration"],
           "frozen_policy": frozen["policy"], "m3r_weights": frozen["final_weights"]["M3R"],
           "m4_weights": json.loads((payload / "seg072/final/M4.digests.json").read_text()),
           "production_m3_model_sha256": sha256_file(HERE / "segarecomp_ml_region_gen2.model.json"),
           "corpus_manifest_sha256": sha256_file(HERE / "segarecomp_ml_v3.corpus_manifest.json"),
           "historical_outer_score_digests": json.loads((HERE / "segarecomp_ml_region_gen2_rank.plan.json").read_text())["frozen_from"]["outer_score_digests"]}
    manifest = {"schema": SCHEMA, "product_commit": subprocess.run(["git", "-C", str(HERE), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip(),
                "layout": "payload/<path relative to the harness .cache directory>", "files": records, "external_inputs": externals(cache),
                "identities": ids, "environment": environment(), "reference_inference": reference_sample(payload / "seg072"),
                "sealed_regions_label_free": sealed_regions(payload / "seg072", cache),
                "not_preserved": {
                    "M1_HGB final model / fold models": "never fitted or never saved (selection used out-of-fold scores only); recompute: ~30 s for the final fit, ~22 s per fold",
                    "per-cell M4-Dense post-hoc sealed predictions": "not saved (aggregates only in results/posthoc.m4_sealed.json); recompute: ~1 min inference with final/M4.s*.pt, no training",
                    "per-cell one-shot sealed scores": "not saved (frozen regions are, in results/sealed.private.json); recompute label-free with final/M3R.pt in seconds",
                    "toolchains and sources": "public pinned inputs (SGDK trees/fork, disassembly clones, Wine prefix, JRE, Python 2.7) are rebuildable; see the handoff document",
                    "sealed build trees": "commercial-derived split/assembly trees under sealed/build are not bundled; sealed truth and tokens are"}}
    (out / "MANIFEST.json").write_text(json.dumps(manifest, indent=1, sort_keys=True))
    (out / "MANIFEST.sha256").write_text(sha256_file(out / "MANIFEST.json") + "  MANIFEST.json\n")
    print(json.dumps({"files": len(records), "bytes": sum(r["bytes"] for r in records.values()), "external_inputs": len(manifest["external_inputs"]),
                      "manifest_sha256": sha256_file(out / "MANIFEST.json")}))
    return 0


def cmd_verify(a) -> int:
    bundle = pathlib.Path(a.bundle).resolve()
    expect = (bundle / "MANIFEST.sha256").read_text().split()[0]
    if sha256_file(bundle / "MANIFEST.json") != expect:
        raise SystemExit("MANIFEST.json does not match MANIFEST.sha256")
    man = json.loads((bundle / "MANIFEST.json").read_text())
    payload = bundle / "payload"
    report = {"checks": {}}
    bad = [rel for rel, r in man["files"].items() if not (payload / rel).exists() or sha256_file(payload / rel) != r["sha256"]]
    report["checks"]["bundled_files"] = {"files": len(man["files"]), "mismatched_or_missing": bad}
    ext_bad = [k for k, r in man["external_inputs"].items() if not pathlib.Path(r["path"]).exists() or sha256_file(r["path"]) != r["sha256"]]
    report["checks"]["external_inputs"] = {"inputs": len(man["external_inputs"]), "mismatched_or_missing": ext_bad}
    os.environ["SEG072_ROOT"] = str(payload / "seg072")
    os.environ["SEG049_ROOT"] = str(payload / "seg049")
    sys.path.insert(0, str(HERE))
    import torch
    import numpy as np
    import segarecomp_ml_v3 as v3
    v3.verify_plan()
    ids = man["identities"]
    w = {"M3R": state_digest(torch.load(payload / "seg072/final/M3R.pt"))}
    for s in (1, 2, 3):
        w[f"M4.s{s}"] = state_digest(torch.load(payload / f"seg072/final/M4.s{s}.pt"))
    report["checks"]["weights"] = {"M3R_matches_frozen_record": w["M3R"] == ids["m3r_weights"],
                                   "M4_matches_recorded": all(w[k] == v for k, v in ids["m4_weights"].items())}
    report["checks"]["frozen_record"] = {"sha256_matches": sha256_file(HERE / "segarecomp_ml_v3.frozen.json") == ids["frozen_record_sha256"],
                                         "marker_binds_it": json.loads((payload / "seg072/sealed/UNSEALED.marker.json").read_text())["frozen_record_sha256"]
                                         == ids["frozen_record_sha256"]}
    ref = reference_sample(payload / "seg072")
    report["checks"]["reference_inference_bitwise"] = {k: ref[k]["sha256"] == man["reference_inference"][k]["sha256"] for k in ref}
    prod = np.load(payload / "seg072/scores/M3_FROZEN.production.npz")[SAMPLE_DEV]
    report["checks"]["frozen_m3_vs_stored_production_scores"] = (hashlib.sha256(np.asarray(prod, dtype=np.float64).tobytes()).hexdigest()
                                                                 == ref["M3_FROZEN"]["sha256"])
    hist_ok = {}
    import segarecomp_ml_region_gen2 as g
    for t, d in ids["historical_outer_score_digests"].items():
        hist_ok[t] = g.prediction_digest({t: np.load(payload / f"seg049/scores/M3.{t}.npy").tolist()}) == d
    report["checks"]["historical_gen2_folds"] = hist_ok
    # fold score files vs registry identities and folds
    reg = {}
    for meta in (payload / "seg072/data").rglob("meta.json"):
        m = json.loads(meta.read_text())
        if "audit" in m:
            reg[m["id"]] = m
            cls = (meta.parent / "classes.bin").read_bytes()
            if hashlib.sha256(cls).hexdigest() != m["classes_sha256"] or hashlib.sha256((meta.parent / "tokens.bin").read_bytes()).hexdigest() != m["tokens_sha256"]:
                report.setdefault("registry_mismatch", []).append(m["id"])
    folds = dict(v3.lolo_folds([d["lineage"] for d in json.loads((HERE / "segarecomp_ml_v3.corpus_manifest.json").read_text())["development"]
                                if d["qualification"] == "QUALIFIED"]))
    fold_issues = []
    for f in sorted((payload / "seg072/scores").glob("*.json")):
        rec = json.loads(f.read_text())
        npz = f.with_suffix(".npz")
        if rec["train"] != folds.get(rec["fold"]):
            fold_issues.append(f.name + ": training lineages differ from the frozen folds")
        with np.load(npz) as z:
            for k in z.files:
                iid = k.replace("@@", "/")
                m = reg.get(iid)
                if m is None or m["lineage"] not in rec["held"]:
                    fold_issues.append(f"{f.name}: {iid} not a held-out image")
                    continue
                n = (m["size"] // 2) if rec["model"] == "M4" else v3.cell_count(m["size"])
                if len(z[k]) != n:
                    fold_issues.append(f"{f.name}: {iid} length")
    report["checks"]["fold_scores"] = {"files": len(list((payload / "seg072/scores").glob("*.json"))), "issues": fold_issues,
                                       "registry_images": len(reg), "registry_mismatch": report.pop("registry_mismatch", [])}
    sealed_priv = json.loads((payload / "seg072/results/sealed.private.json").read_text())["lineages"]
    regen = sealed_regions(payload / "seg072", None)
    report["checks"]["sealed_regions_regenerated_label_free"] = {
        L: regen[L]["region_sha256"] == hashlib.sha256(json.dumps(sorted(sealed_priv[L]["_region"])).encode()).hexdigest() for L in regen}
    sealed_man = {e["lineage"]: e for e in json.loads((HERE / "segarecomp_ml_v3.corpus_manifest.json").read_text())["sealed"]["entries"]}
    report["checks"]["sealed_truth_bound"] = {L: sha256_file(payload / f"seg072/sealed/truth/{f}") == sealed_man[L]["opaque_classes_sha256"]
                                             for L, f in (("E01", "e01.1.cls"), ("E03", "e03.1.cls"), ("E05", "e05.1.cls"), ("E06", "e06.cls"), ("E07", "e07.cls"))}
    flat = []

    def walk(x):
        if isinstance(x, dict):
            for v in x.values():
                walk(v)
        elif isinstance(x, list):
            flat.append(len(x) == 0)
        elif isinstance(x, bool):
            flat.append(x)
    walk(report["checks"])
    report["verdict"] = "PASS" if all(flat) else "FAIL"
    print(json.dumps(report, indent=1, sort_keys=True))
    return 0 if report["verdict"] == "PASS" else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("bundle")
    b.add_argument("--cache", default="/root/dev/segarecomp-harness/.cache")
    b.add_argument("--out", required=True)
    b.set_defaults(func=cmd_bundle)
    v = sub.add_parser("verify")
    v.add_argument("--bundle", required=True)
    v.set_defaults(func=cmd_verify)
    a = ap.parse_args()
    return a.func(a)


if __name__ == "__main__":
    raise SystemExit(main())
