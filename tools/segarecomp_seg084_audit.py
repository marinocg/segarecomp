#!/usr/bin/env python3
"""SEG-084-T001: M68K training-corpus audit (OFFLINE research tooling; standard library only).

Inventories every eligible dataset from the live private workspace and verifies presence and integrity instead of assuming it:

  seg072   the SEG-072 Genesis development registry (50 images: 14 Genesis ROM images + 36 Sonic CD modules, 11 qualified lineages)
  seg075   the SEG-075 cross-platform (Atari / Amiga relocatable) development snapshot
  tas      SEG-080 / SEG-081 / SEG-083 TAS-derived labels (local sample directories and the retained private snapshots)
  seg077   the SEG-077 exploratory commercial samples (Chuck Rock pilot) - status only
  sealed   opaque bindings of the protected cohorts: entry COUNTS from public manifests only; no sealed file is opened

Output is aggregate and opaque (counts, digests, booleans, closed vocabularies); no path, title or per-address datum is written. The sealed
directories are never listed, hashed or read (`refuse_sealed` guards every path). Run: `segarecomp_seg084_audit.py inventory --out <json>`.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import sys

CELL = 128
HERE = pathlib.Path(__file__).resolve().parent
HARNESS = pathlib.Path(os.environ.get("SEGARECOMP_HARNESS_ROOT", "/root/dev/segarecomp-harness"))
SCHEMA = "segarecomp.seg084.audit.v1"
SEALED_NAMES = ("sealed", "reserved-unsealed", "blind", "blind-driver")


def sha(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def sha_file(p: pathlib.Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def refuse_sealed(p) -> None:
    parts = {x.casefold() for x in pathlib.Path(p).parts}
    if parts & {n.casefold() for n in SEALED_NAMES}:
        raise SystemExit(f"audit barrier: sealed/reserved path refused: {p}")


def class_counts(cls: bytes) -> dict:
    return {c: cls.count(c.encode()) for c in "IcDU"}


def cell_counts(cls: bytes) -> dict:
    pos = neg = masked = 0
    for c in range((len(cls) + CELL - 1) // CELL):
        ch = cls[c * CELL:(c + 1) * CELL]
        if b"I" in ch:
            pos += 1
        elif ch.count(b"D") == len(ch):
            neg += 1
        else:
            masked += 1
    return {"pos": pos, "neg": neg, "masked": masked, "cells": pos + neg + masked}


def opaque(*parts) -> str:
    return sha("|".join(str(p) for p in parts).encode())[:16]


# ------------------------------------------------------------------------------------------------------------------------ SEG-072
def inventory_seg072(root: pathlib.Path, manifest: dict, handoff_shas: set) -> dict:
    """Walk the registry exactly as `segarecomp_ml_v3_train.registry` does (meta.json with an `audit` block)."""
    refuse_sealed(root)
    qual = {d["lineage"] for d in manifest["development"] if d["qualification"] == "QUALIFIED"}
    man_img = {}
    for d in manifest["development"]:
        for r in d.get("image_records", []):
            man_img[r["id"]] = r
    images, lineages = [], {}
    for meta_p in sorted(root.rglob("meta.json")):
        refuse_sealed(meta_p)
        m = json.loads(meta_p.read_text())
        if "audit" not in m:
            continue
        d = meta_p.parent
        cls = (d / "classes.bin").read_bytes()
        tok = (d / "tokens.bin").read_bytes()
        img = pathlib.Path(m["image"])
        img_ok = img.is_file() and img.stat().st_size == m["size"] and sha_file(img) == m["sha256"]
        seeds = json.loads((d / "seeds.json").read_text()) if (d / "seeds.json").is_file() else []
        rec = {"key": opaque("seg072", m["id"]), "lineage": m["lineage"], "qualified_lineage": m["lineage"] in qual, "kind": m["kind"],
               "platform": "genesis" if m["kind"] == "genesis_rom" else "sega_cd", "cpu": "MC68000", "size": m["size"],
               "image_sha256": m["sha256"], "image_present": img.is_file(), "image_identity_ok": img_ok,
               "classes_sha256": m["classes_sha256"], "classes_ok": sha(cls) == m["classes_sha256"],
               "tokens_sha256": m["tokens_sha256"], "tokens_ok": sha(tok) == m["tokens_sha256"],
               "bytes": class_counts(cls), "cells": cell_counts(cls), "seed_cells": len(seeds),
               "seed_address_file": (d / "d.txt").is_file(), "broad_aot": m.get("broad_aot"), "in_manifest": m["id"] in man_img,
               "manifest_classes_match": bool(m["id"] in man_img and man_img[m["id"]].get("classes_sha256") == m["classes_sha256"]),
               "preserved_in_handoff_bundle": m["classes_sha256"] in handoff_shas,
               "label_origin": "independent source-backed truth (assembler listing / reproducible build map)",
               "provenance_class": "reproducible_build" if "build" in m.get("provenance", "") else
               "module_reuse" if m["kind"] == "scd_module" else "community_disassembly_of_commercial_title"}
        images.append(rec)
        lineages.setdefault(m["lineage"], {"images": 0, "pos": 0, "neg": 0, "masked": 0})
        L = lineages[m["lineage"]]
        L["images"] += 1
        for k in ("pos", "neg", "masked"):
            L[k] += rec["cells"][k]
    return {"images": images, "lineages": lineages, "image_count": len(images), "qualified_lineages": sorted(qual),
            "unqualified": sorted(d["lineage"] for d in manifest["development"] if d["qualification"] != "QUALIFIED"),
            "sealed_entries_in_public_manifest": len(manifest["sealed"].get("entries", [])) if isinstance(manifest.get("sealed"), dict) else None,
            "all_present_and_verified": all(i["image_identity_ok"] and i["classes_ok"] and i["tokens_ok"] for i in images)}


# ------------------------------------------------------------------------------------------------------------------------ SEG-075
def inventory_seg075(snapshot: pathlib.Path, handoff_shas: set) -> dict:
    refuse_sealed(snapshot / "dev")
    snap = json.loads((snapshot / "SNAPSHOT.json").read_text())
    dev = snap["dev"]
    root = snapshot / "dev"
    images, lineages = [], {}
    for key, rec in sorted(dev.items()):
        d = root / key.replace("#", "@")
        present = d.is_dir()
        ok = present
        cls = b""
        if present:
            for name, want in rec["files"].items():
                f = d / name
                if not f.is_file() or sha_file(f) != want:
                    ok = False
            cls = (d / "classes.bin").read_bytes() if (d / "classes.bin").is_file() else b""
        relocs = json.loads((d / "relocs.json").read_text()) if present and (d / "relocs.json").is_file() else []
        seeds = json.loads((d / "seeds.json").read_text()) if present and (d / "seeds.json").is_file() else []
        meta = json.loads((d / "meta.json").read_text()) if present and (d / "meta.json").is_file() else {}
        r = {"key": opaque("seg075", key), "lineage": rec["lineage"], "representative": rec["representative"], "role": rec["role"],
             "platform": meta.get("platform"), "cpu": meta.get("cpu_variant"), "kind": meta.get("kind"), "files_present_and_verified": ok,
             "image_sha256": rec["sha256"], "classes_sha256": rec["classes_sha256"], "tokens_sha256": rec["tokens_sha256"],
             "bytes": class_counts(cls) if cls else None, "cells": cell_counts(cls) if cls else None, "relocations": len(relocs), "seed_cells": len(seeds),
             "preserved_in_handoff": rec["classes_sha256"] in handoff_shas,
             "label_origin": "independent source-backed truth (assembler/linker map, per-statement re-assembly oracle)",
             "provenance": str(meta.get("provenance", ""))[:60]}
        images.append(r)
        L = lineages.setdefault(rec["lineage"], {"images": 0, "representatives": 0, "pos": 0, "neg": 0, "masked": 0, "relocations": 0})
        L["images"] += 1
        L["representatives"] += int(rec["representative"])
        L["relocations"] += len(relocs)
        if r["cells"]:
            for k in ("pos", "neg", "masked"):
                L[k] += r["cells"][k]
    return {"images": images, "lineages": lineages, "image_count": len(images), "sealed_entries_in_snapshot_index": len(snap.get("sealed", {})),
            "all_present_and_verified": all(i["files_present_and_verified"] for i in images)}


# ---------------------------------------------------------------------------------------------------------------------------- TAS
def inventory_tas(private_corpus: pathlib.Path, product_cache: pathlib.Path) -> dict:
    out = {"snapshots": {}, "local_sample_dirs": {}}
    for snap in sorted(private_corpus.glob("seg08*-v*")):
        man = json.loads((snap / "MANIFEST.json").read_text())
        games = {}
        for g, rec in man["games"].items():
            conds = {}
            for c, cr in rec["conditions"].items():
                cn = cr["counts"]
                conds[c] = {"strong_cells": cn["strong_cells"], "weak_cells": cn["weak_cells"], "strong_bytes": cn["strong_bytes"], "weak_bytes": cn["weak_bytes"],
                            "tier_bytes": cn["tier_bytes"], "decoder_cli_sha256": cr["decoder_cli_sha256"], "frames": cr["frames"]}
            games[g] = {"lineage": rec["lineage"], "rom_sha256": rec["rom"]["sha256"], "rom_size": rec["rom"]["size"], "conditions": conds}
        out["snapshots"][snap.name] = {"tree_sha256": man["tree_sha256"], "games": games, "policy": man["policy"], "status": man["status"]}
    for name in ("seg080", "seg081", "seg083"):
        d = product_cache / name
        out["local_sample_dirs"][name] = {"present": d.is_dir()}
    return out


# ---------------------------------------------------------------------------------------------------------------------------- SEG-077
def inventory_seg077(corpus: pathlib.Path) -> dict:
    titles = corpus / "titles"
    out = {"samples": []}
    if not titles.is_dir():
        return out
    for d in sorted(titles.iterdir()):
        if not d.is_dir() or (d / "classes.bin").exists() is False:
            continue
        refuse_sealed(d)
        cls = (d / "classes.bin").read_bytes()
        meta = json.loads((d / "meta.json").read_text()) if (d / "meta.json").is_file() else {}
        out["samples"].append({"key": opaque("seg077", d.name), "bytes": class_counts(cls), "cells": cell_counts(cls), "classes_sha256": sha(cls),
                               "variant": "t009_static_first" if d.name.endswith("t009") or d.name.endswith("t009v2") else "t001_execution_evidence",
                               "provenance": str(meta.get("provenance", ""))[:80], "truth": str(meta.get("truth", ""))[:120]})
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("inventory")
    p.add_argument("--harness", default=str(HARNESS)), p.add_argument("--out", required=True)
    a = ap.parse_args(argv)
    h = pathlib.Path(a.harness)
    manifest = json.loads((HERE / "segarecomp_ml_v3.corpus_manifest.json").read_text())
    hs72 = {v["sha256"] for v in json.loads((h / ".cache/seg072-handoff/bundle-v1/MANIFEST.json").read_text())["files"].values()}
    hs75 = {e["sha256"] for e in json.loads((h / ".cache/seg075-handoff/manifests/seg076-c7-final.json").read_text())["entries"] if "sha256" in e}
    res = {"schema": SCHEMA,
           "seg072": inventory_seg072(h / ".cache/seg072/data", manifest, hs72 | hs75),
           "seg075": inventory_seg075(h / ".cache/seg075/t003/snapshot", hs75),
           "tas": inventory_tas(h / "private-corpus", h / "segarecomp/.cache"),
           "seg077": inventory_seg077(h / ".cache/m68k-corpus")}
    pathlib.Path(a.out).write_text(json.dumps(res, indent=1, sort_keys=True))
    print(json.dumps({"seg072_images": res["seg072"]["image_count"], "seg072_ok": res["seg072"]["all_present_and_verified"],
                      "seg075_images": res["seg075"]["image_count"], "seg075_ok": res["seg075"]["all_present_and_verified"],
                      "tas_snapshots": sorted(res["tas"]["snapshots"]), "seg077_samples": len(res["seg077"]["samples"])}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
