#!/usr/bin/env python3
"""SEG-072 (ADR 0152): private per-image research registry for M68K ML v3 (OFFLINE tooling, standard library only).

  register  from a private JSON config, bind every image to its per-byte class string (I/c/D/U), compute CPU-owned decode tokens
            (`segarecomp m68k-decode-token-report`), production seed cells and the broad universe (Genesis cartridge images, via
            `emit-general-startup-bridge-c`), and audit the labels against the decoder (an independent cross-check: every labelled start
            must decode as an ordinary instruction whose length equals the labelled extent)
  summary   aggregate, non-reconstructable per-image / per-lineage statistics (counts, fractions, digests) for the ADR
  shingles  lineage-independence statistics (12-byte shingle containment of labelled application-code runs; raw-byte variant)

Every artifact lives under the ignored private root; the committed outputs are aggregates only. Sealed images are refused here.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_ml_v3 as v3  # noqa: E402

TOKEN_FIELDS = ("status", "length", "family", "control", "ea_src", "ea_dst", "width", "flags")


def sha256_bytes(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def parse_token_report(data: bytes, size: int) -> bytes:
    """The gen2 validator (header, body length, `end` trailer, per-field ranges)."""
    import segarecomp_ml_region_gen2 as gen2
    return gen2.parse_token_report(data, size)


def audit(classes: bytes, tokens: bytes) -> dict:
    """Decoder cross-check of the labels (CPU-owned decoder; not used to create labels)."""
    nf = len(TOKEN_FIELDS)
    starts = [i for i, b in enumerate(classes) if b == ord("I")]
    bad_status = bad_len = odd = 0
    for a in starts:
        if a & 1:
            odd += 1
            continue
        w = a // 2
        status, length = tokens[w * nf], tokens[w * nf + 1]
        if status != 2:
            bad_status += 1
            continue
        extent = 1
        while a + extent < len(classes) and classes[a + extent] == ord("c"):
            extent += 1
        if length * 2 != extent:
            bad_len += 1
    return {"starts": len(starts), "odd_starts": odd, "not_ordinary_decode": bad_status, "length_mismatch": bad_len}


def run_cli(cli: str, args: list[str], timeout: int = 3600) -> None:
    subprocess.run([cli] + args, check=True, capture_output=True, timeout=timeout)


def cmd_register(a) -> int:
    cfg = json.loads(pathlib.Path(a.config).read_text())
    root = pathlib.Path(a.root)
    for entry in cfg["images"]:
        iid = entry["id"]
        v3.assert_no_sealed([iid.split("/")[0]])
        image_path = pathlib.Path(entry["image"])
        v3.refuse_sealed_path(image_path)
        img = image_path.read_bytes()
        cls = pathlib.Path(entry["classes"]).read_bytes()
        if len(cls) != len(img):
            raise SystemExit(f"{iid}: class string length differs from image")
        if entry.get("sha256") and sha256_bytes(img) != entry["sha256"]:
            raise SystemExit(f"{iid}: image identity mismatch")
        d = root / iid
        d.mkdir(parents=True, exist_ok=True)
        (d / "classes.bin").write_bytes(cls)
        tok = d / "tokens.bin"
        if not tok.exists() or a.force:
            report = d / "tok.report"
            run_cli(a.cli, ["m68k-decode-token-report", "--rom", str(image_path), "--output", str(report)])
            tok.write_bytes(parse_token_report(report.read_bytes(), len(img)))
            report.unlink()
        tokens = tok.read_bytes()
        seeds = set()
        if entry["kind"] == "genesis_rom":
            dtxt, utxt = d / "d.txt", d / "u.txt"
            rejected = d / "bridge_rejected.json"
            if (not dtxt.exists() and not rejected.exists()) or a.force:
                probe = d / "probe.c"
                proc = subprocess.run([a.cli, "emit-general-startup-bridge-c", "--rom", str(image_path), "--reset-entry", "--rom-sha256", sha256_bytes(img),
                                       "--immutable-rom-aot", "--direct-control-address-report", str(dtxt), "--immutable-aot-address-report", str(utxt),
                                       "--generated-c-output", str(probe)], capture_output=True, text=True, timeout=3600)
                probe.unlink(missing_ok=True)
                if proc.returncode != 0:
                    cat = next((json.loads(ln).get("category", "") for ln in proc.stderr.splitlines() if ln.startswith("{") and "category" in ln), "")
                    rejected.write_text(json.dumps({"category": cat or "unknown"}))
                    dtxt.unlink(missing_ok=True)
                    utxt.unlink(missing_ok=True)
            for t in (dtxt.read_text().split() if dtxt.exists() else []):
                addr = int(t, 16)
                if 0 <= addr < len(img):
                    seeds.add(addr // v3.CELL_BYTES)
        elif entry["kind"] == "scd_module":
            for t in pathlib.Path(entry["seed_starts"]).read_text().split():
                seeds.add(int(t, 16) // v3.CELL_BYTES)
        else:
            raise SystemExit("unknown image kind")
        (d / "seeds.json").write_text(json.dumps(sorted(seeds)))
        au = audit(cls, tokens)
        meta = {"id": iid, "lineage": entry["lineage"], "kind": entry["kind"], "image": str(image_path), "sha256": sha256_bytes(img), "size": len(img),
                "classes_sha256": sha256_bytes(cls), "tokens_sha256": sha256_bytes(tokens), "seed_cells": len(seeds), "audit": au,
                "module_class": entry.get("module_class"), "provenance": entry.get("provenance", ""),
                "broad_aot": (json.loads((d / "bridge_rejected.json").read_text())["category"] if (d / "bridge_rejected.json").exists() else "accepted")
                if entry["kind"] == "genesis_rom" else "not_applicable"}
        (d / "meta.json").write_text(json.dumps(meta, indent=1, sort_keys=True))
        print(json.dumps({"id": iid, "size": len(img), "starts": au["starts"], "audit": au, "seed_cells": len(seeds)}), flush=True)
    return 0


def load_registry(root: pathlib.Path, ids=None) -> list[dict]:
    out = []
    for meta in sorted(root.rglob("meta.json")):
        m = json.loads(meta.read_text())
        if "classes_sha256" not in m:
            continue
        if ids and m["id"] not in ids:
            continue
        m["dir"] = str(meta.parent)
        out.append(m)
    return out


def cmd_summary(a) -> int:
    root = pathlib.Path(a.root)
    rows = []
    for m in load_registry(root):
        cls = pathlib.Path(m["dir"], "classes.bin").read_bytes()
        labels = v3.cell_labels(cls)
        rows.append({"id": m["id"], "lineage": m["lineage"], "kind": m["kind"], "bytes": m["size"], "starts": cls.count(b"I"),
                     "bytes_I_c": cls.count(b"I") + cls.count(b"c"), "bytes_D": cls.count(b"D"), "bytes_U": cls.count(b"U"),
                     "cells": len(labels), "positive_cells": labels.count(1), "negative_cells": labels.count(0), "masked_cells": labels.count(-1),
                     "seed_cells": m["seed_cells"], "audit": m["audit"], "module_class": m.get("module_class")})
    lineages = {}
    for r in rows:
        L = lineages.setdefault(r["lineage"], {"images": 0, "starts": 0, "bytes": 0, "positive_cells": 0, "cells": 0, "masked_cells": 0,
                                               "audit_not_ordinary": 0, "audit_length_mismatch": 0})
        L["images"] += 1
        for k in ("starts", "bytes", "positive_cells", "cells", "masked_cells"):
            L[k] += r[k]
        L["audit_not_ordinary"] += r["audit"]["not_ordinary_decode"]
        L["audit_length_mismatch"] += r["audit"]["length_mismatch"]
    out = {"images": rows if a.per_image else len(rows), "lineages": lineages}
    print(json.dumps(out, indent=1, sort_keys=True))
    return 0


def shingles(data: bytes, mask: bytes | None, k: int = 12) -> set:
    out = set()
    run = []
    for i, b in enumerate(data):
        keep = mask is None or mask[i] in (ord("I"), ord("c"))
        if keep:
            run.append(b)
            if len(run) >= k:
                out.add(bytes(run[-k:]))
        else:
            run = []
    return out


def cmd_shingles(a) -> int:
    root = pathlib.Path(a.root)
    regs = [m for m in load_registry(root) if m["lineage"] in set(a.lineages)]
    per = {}
    for m in regs:
        img = pathlib.Path(m["image"]).read_bytes()
        cls = pathlib.Path(m["dir"], "classes.bin").read_bytes()
        per.setdefault(m["lineage"], set()).update(shingles(img, cls))
    res = {}
    keys = sorted(per)
    for i, x in enumerate(keys):
        for y in keys[i + 1:]:
            inter = len(per[x] & per[y])
            res[f"{x}~{y}"] = {"containment_x_in_y": inter / max(1, len(per[x])), "containment_y_in_x": inter / max(1, len(per[y]))}
    worst = max(res.items(), key=lambda kv: max(kv[1].values())) if res else None
    print(json.dumps({"pairs": len(res), "max_pair": worst, "threshold": 0.30,
                      "over_threshold": {k: v for k, v in res.items() if max(v.values()) >= 0.30}}, indent=1, sort_keys=True))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("register")
    r.add_argument("--config", required=True)
    r.add_argument("--root", required=True)
    r.add_argument("--cli", required=True)
    r.add_argument("--force", action="store_true")
    r.set_defaults(func=cmd_register)
    s = sub.add_parser("summary")
    s.add_argument("--root", required=True)
    s.add_argument("--per-image", action="store_true")
    s.set_defaults(func=cmd_summary)
    h = sub.add_parser("shingles")
    h.add_argument("--root", required=True)
    h.add_argument("--lineages", nargs="+", required=True)
    h.set_defaults(func=cmd_shingles)
    a = ap.parse_args()
    return a.func(a)


if __name__ == "__main__":
    raise SystemExit(main())
