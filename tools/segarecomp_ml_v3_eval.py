#!/usr/bin/env python3
"""SEG-072 (ADR 0151/0153/0154): evaluation of M68K ML v3 configurations under the frozen budget policies (OFFLINE research tooling).

  table      per-image / per-lineage metrics of every available configuration under B35/B45/B50/B60/ADAPT (development OOF scores)
  kstage     structural closure (unchanged production prune + validator) for chosen configuration/policy pairs on Genesis images
  select     apply the frozen selection rule to the development table
  sealed     the ONE-SHOT sealed evaluation of the committed frozen configuration (refuses without the frozen record / a second time)

Inputs: the private registry, private score arrays (`segarecomp_ml_v3_train.py`), historical gen2 outer-fold arrays (M3_FROZEN). Outputs:
private JSON results plus aggregate-only records committed by hand. Everything protocol-related comes from `segarecomp_ml_v3.py`.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import segarecomp_ml_v3 as v3  # noqa: E402

ROOT = pathlib.Path(os.environ.get("SEG072_ROOT", "/root/dev/segarecomp-harness/.cache/seg072"))
DATA, SCORES, RESULTS = ROOT / "data", ROOT / "scores", ROOT / "results"
SEG049 = pathlib.Path(os.environ.get("SEG049_ROOT", "/root/dev/segarecomp-harness/.cache/seg049"))
HISTORICAL = ("s1", "flicky", "sf1", "ps2", "sk")
POLICIES = ("B35", "B45", "B50", "B60", "ADAPT")
CONFIGS = ("M3_FROZEN", "M1_HGB", "M3_R", "M4_DENSE@128", "M4_DENSE@64")


def np_():
    import numpy
    return numpy


def sigmoid(x: float) -> float:
    return 1.0 / (1.0 + math.exp(-max(-700.0, min(700.0, x))))


# ------------------------------------------------------------------------------------------------------------------ inputs
def registry() -> dict:
    out = {}
    for meta in sorted(DATA.rglob("meta.json")):
        m = json.loads(meta.read_text())
        if "audit" in m:
            m["dir"] = str(meta.parent)
            out[m["id"]] = m
    return out


def seed_addresses(m: dict) -> list[int]:
    d = pathlib.Path(m["dir"])
    if m["kind"] == "genesis_rom":
        p = d / "d.txt"
        return [int(t, 16) for t in p.read_text().split()] if p.exists() else []
    p = d / "seed_starts.txt"
    return [int(t, 16) for t in p.read_text().split()] if p.exists() else []


def load_npz(path: pathlib.Path) -> dict:
    np = np_()
    with np.load(path) as z:
        return {k.replace("@@", "/"): z[k] for k in z.files}


def config_scores(config: str, reg: dict, folds: list) -> dict:
    """{image id: (cell scores (rank key), cell probabilities, cell_bytes)} out-of-fold for every development image the config covers."""
    np = np_()
    out = {}
    if config == "M3_FROZEN":
        prod = load_npz(SCORES / "M3_FROZEN.production.npz")
        for iid in reg:
            if iid in HISTORICAL:
                p = np.load(SEG049 / "scores" / f"M3.{iid}.npy")
                out[iid] = (p.astype(np.float64), p.astype(np.float64), 128)  # historical outer folds store probabilities
            else:
                lg = prod[iid]
                out[iid] = (lg, 1.0 / (1.0 + np.exp(-np.clip(lg, -700, 700))), 128)
        return out
    for fold, _ in folds:
        if config == "M3_R":
            path = SCORES / f"M3R.{fold}.npz"
        elif config == "M1_HGB":
            path = SCORES / f"M1.{fold}.npz"
        else:
            path = None
        if config.startswith("M4_DENSE"):
            cell = int(config.split("@")[1])
            seeds = [SCORES / f"M4.{fold}.s{s}.npz" for s in (1, 2, 3)]
            if not all(p.exists() for p in seeds):
                continue
            arrs = [load_npz(p) for p in seeds]
            import segarecomp_ml_v3_train as tr
            for iid in arrs[0]:
                wl = np.mean([a[iid] for a in arrs], axis=0).astype(np.float32)
                cs = tr.cell_max(wl, reg[iid]["size"], cell)
                out[iid] = (cs, 1.0 / (1.0 + np.exp(-np.clip(cs, -700, 700))), cell)
            continue
        if not path.exists():
            continue
        for iid, lg in load_npz(path).items():
            out[iid] = (lg.astype(np.float64), 1.0 / (1.0 + np.exp(-np.clip(lg.astype(np.float64), -700, 700))), 128)
    return out


# ------------------------------------------------------------------------------------------------------------------- metrics
def image_metrics(m: dict, scores, probs, cell: int, margin: float | None) -> dict:
    cls = pathlib.Path(m["dir"], "classes.bin").read_bytes()
    labels = v3.cell_labels(cls, cell)
    starts = v3.starts_of(cls)
    seeds = {a // cell for a in seed_addresses(m) if 0 <= a < m["size"]}
    s, p = [float(x) for x in scores], [float(x) for x in probs]
    if len(s) != len(labels):
        raise SystemExit(f"{m['id']}: score/cell count mismatch")
    f = v3.frf(s, labels)
    rec = {"id": m["id"], "lineage": m["lineage"], "kind": m["kind"], "cells": len(labels), "cell_bytes": cell, "starts": len(starts),
           "positive_cells": labels.count(1), "frf": f["frf"], "residual_frf": v3.residual_frf(s, labels, seeds), "density_e": v3.density_estimate(p),
           "seed_cells": len(seeds), "policies": {}}
    for pol in POLICIES:
        if pol == "ADAPT" and margin is None:
            continue
        r = v3.policy_region(pol, s, p, seeds, margin)
        region = r["region"]
        order = v3.rank_order(s)
        if r["abstained"]:
            ml_only = set(range(len(s)))
        elif pol in v3.FIXED_BUDGETS:
            ml_only = set(order[:v3.budget_cell_count(len(s), v3.FIXED_BUDGETS[pol])])
        else:
            ml_only = set(order[:min(len(s), math.ceil(round(len(s) * r["budget"], 9)))])
        rbytes = sum(min((c + 1) * cell, m["size"]) - c * cell for c in region)
        rec["policies"][pol] = {"retained": rbytes / m["size"], "budget": r["budget"], "abstained": r["abstained"], "c_outside_r": v3.misses(starts, region, cell),
                                "c_outside_ml_only": v3.misses(starts, ml_only, cell), "_region": sorted(region)}
    return rec


def lineage_aggregate(records: list) -> dict:
    by = {}
    for r in records:
        by.setdefault(r["lineage"], []).append(r)
    out = {}
    for L, rs in by.items():
        agg = {"images": len(rs), "max_frf": max(r["frf"] for r in rs), "max_residual_frf": max(r["residual_frf"] for r in rs),
               "starts": sum(r["starts"] for r in rs), "policies": {}}
        for pol in POLICIES:
            vals = [r["policies"][pol] for r in rs if pol in r["policies"]]
            if vals:
                agg["policies"][pol] = {"mean_retained": sum(v["retained"] for v in vals) / len(vals), "misses": sum(v["c_outside_r"] for v in vals),
                                        "images_missing": sum(1 for v in vals if v["c_outside_r"]), "abstained": sum(1 for v in vals if v["abstained"])}
        out[L] = agg
    return out


def evaluate_config(config: str, reg: dict, folds: list) -> dict | None:
    sc = config_scores(config, reg, folds)
    if not sc:
        return None
    covered = [iid for iid in reg if iid in sc]
    # nested adaptive margin: for held lineage L, fit on the OOF records of the other lineages
    base = {}
    for iid in covered:
        s, p, cell = sc[iid]
        base[iid] = image_metrics(reg[iid], s, p, cell, None)
    margins = {}
    lineages = sorted({reg[i]["lineage"] for i in covered})
    for L in lineages:
        fit = [(base[i]["residual_frf"], base[i]["density_e"]) for i in covered if reg[i]["lineage"] != L]
        margins[L] = v3.fit_adaptive_margin(fit) if fit else None
    records = []
    for iid in covered:
        s, p, cell = sc[iid]
        records.append(image_metrics(reg[iid], s, p, cell, margins[reg[iid]["lineage"]]))
    agg = lineage_aggregate(records)
    pairs = {}
    for pol in POLICIES:
        if any(pol not in r["policies"] for r in records):
            continue
        pairs[pol] = {"complete": all(r["policies"][pol]["c_outside_r"] == 0 for r in records),
                      "misses": sum(r["policies"][pol]["c_outside_r"] for r in records),
                      "worst_frf": max(a["max_frf"] for a in agg.values()),
                      "mean_retained": sum(a["policies"][pol]["mean_retained"] for a in agg.values()) / len(agg),
                      "lineages": len(agg), "images": len(records)}
    return {"config": config, "images": len(records), "lineages": lineages, "margins": margins, "records": records, "lineage": agg, "pairs": pairs}


def public(obj):
    """Strip private per-cell regions from a result before it is written to a committed record."""
    if isinstance(obj, dict):
        return {k: public(v) for k, v in obj.items() if not k.startswith("_")}
    if isinstance(obj, list):
        return [public(v) for v in obj]
    return obj


def cmd_table(a) -> int:
    v3.verify_plan()
    reg = {k: v for k, v in registry().items()}
    import segarecomp_ml_v3_train as tr
    folds = v3.lolo_folds(tr.qualified_lineages())
    RESULTS.mkdir(parents=True, exist_ok=True)
    summary = {}
    for config in a.configs:
        res = evaluate_config(config, reg, folds)
        if res is None:
            print(config, "no scores")
            continue
        (RESULTS / f"table.{config}.json").write_text(json.dumps(res))
        summary[config] = {"pairs": res["pairs"], "margins": res["margins"],
                           "lineage": {L: {"max_frf": v["max_frf"], "max_residual_frf": v["max_residual_frf"],
                                           "policies": v["policies"]} for L, v in res["lineage"].items()}}
        print(config, json.dumps({p: {k: (round(x, 4) if isinstance(x, float) else x) for k, x in v.items()} for p, v in res["pairs"].items()}), flush=True)
    (RESULTS / "table.summary.json").write_text(json.dumps(summary, indent=1, sort_keys=True))
    return 0


# --------------------------------------------------------------------------------------------------------------------- K stage
def regions_text(sha: str, size: int, cells, cell: int) -> str:
    runs = []
    for c in sorted(cells):
        b, e = c * cell, min((c + 1) * cell, size)
        if b >= size or c < 0:
            raise SystemExit("region cell outside the image")
        if runs and runs[-1][1] == b:
            runs[-1][1] = e
        else:
            runs.append([b, e])
    return ("segarecomp.m68k_executable_regions.v1\n" f"rom_sha256 {sha}\n" + "".join(f"range {b:08x} {e:08x}\n" for b, e in runs) + "end\n")


def cmd_kstage(a) -> int:
    import segarecomp_ml_region_v2 as v2
    v3.verify_plan()
    reg = registry()
    res = json.loads((RESULTS / f"table.{a.config}.json").read_text())
    runner = v2.PruneRunner(a.cli, str(ROOT / "prune"))
    out = {}
    for r in res["records"]:
        m = reg[r["id"]]
        if m["kind"] != "genesis_rom" or m.get("broad_aot") != "accepted" or a.policy not in r["policies"]:
            continue
        pol = r["policies"][a.policy]
        if pol["abstained"]:
            out[r["id"]] = {"abstained": True}
            continue

        class T:
            pass
        t = T()
        t.sha, t.rom_path = m["sha256"], m["image"]
        t.universe = sorted(int(x, 16) for x in pathlib.Path(m["dir"], "u.txt").read_text().split())
        stats = runner.run(t, regions_text(m["sha256"], m["size"], pol["_region"], r["cell_bytes"]))
        accepted = stats.get("returncode") == 0 and stats.get("validator") == "accepted" and "ranges" in stats
        cls = pathlib.Path(m["dir"], "classes.bin").read_bytes()
        starts = v3.starts_of(cls)
        if accepted:
            k = v2.admitted_set(t.universe, stats["ranges"])
            out[r["id"]] = {"validator": "accepted", "k_over_u": len(k) / len(t.universe), "c_outside_k": sum(1 for s in starts if s not in k),
                            "k": len(k), "u": len(t.universe)}
        else:
            out[r["id"]] = {"validator": "rejected", "reason": stats.get("reason", "")}
        print(r["id"], json.dumps(out[r["id"]]), flush=True)
    kus = [v["k_over_u"] for v in out.values() if v.get("validator") == "accepted"]
    summary = {"config": a.config, "policy": a.policy, "images": out, "mean_ku": sum(kus) / len(kus) if kus else None,
               "all_accepted": all(v.get("validator") == "accepted" or v.get("abstained") for v in out.values()),
               "c_outside_k_total": sum(v.get("c_outside_k", 0) for v in out.values())}
    (RESULTS / f"kstage.{a.config}.{a.policy}.json").write_text(json.dumps(summary, indent=1, sort_keys=True))
    print(json.dumps({k: v for k, v in summary.items() if k != "images"}))
    return 0


def cmd_diagnose(a) -> int:
    """Aggregate diagnosis of one configuration/policy: missed positive cells (code coverage, mixed code/data fragments, starts per cell,
    image code density) and candidate false admissions (ordinary-decoding even offsets inside R that are labelled non-start, U excluded)."""
    np = np_()
    reg = registry()
    res = json.loads((RESULTS / f"table.{a.config}.json").read_text())
    missed, fa, fa_inside, pos_total = [], 0, 0, 0
    for r in res["records"]:
        if a.policy not in r["policies"]:
            continue
        m = reg[r["id"]]
        cls = pathlib.Path(m["dir"], "classes.bin").read_bytes()
        tok = np.frombuffer(pathlib.Path(m["dir"], "tokens.bin").read_bytes(), dtype=np.uint8).reshape(-1, 8)
        cell = r["cell_bytes"]
        region = set(r["policies"][a.policy]["_region"])
        labels = v3.cell_labels(cls, cell)
        pos_total += labels.count(1)
        density = labels.count(1) / max(1, labels.count(1) + labels.count(0))
        for c, y in enumerate(labels):
            chunk = cls[c * cell:(c + 1) * cell]
            if y == 1 and c not in region:
                cov = (chunk.count(b"I") + chunk.count(b"c")) / len(chunk)
                missed.append({"coverage": cov, "mixed": 0.25 <= cov <= 0.75, "starts": chunk.count(b"I"), "image_density": density,
                               "lineage": r["lineage"]})
            if c in region:
                for off in range(0, len(chunk) - 1, 2):
                    b = chunk[off]
                    if b in (ord("U"),) or b == ord("I"):
                        continue
                    w = (c * cell + off) // 2
                    if w < len(tok) and tok[w, 0] == 2:
                        fa += 1
                        fa_inside += b == ord("c")
    out = {"config": a.config, "policy": a.policy, "missed_positive_cells": len(missed), "positive_cells": pos_total,
           "missed_by_lineage": {L: sum(1 for x in missed if x["lineage"] == L) for L in sorted({x["lineage"] for x in missed})},
           "missed_mean_coverage": (sum(x["coverage"] for x in missed) / len(missed)) if missed else None,
           "missed_mixed_fraction": (sum(x["mixed"] for x in missed) / len(missed)) if missed else None,
           "missed_mean_starts": (sum(x["starts"] for x in missed) / len(missed)) if missed else None,
           "missed_mean_image_density": (sum(x["image_density"] for x in missed) / len(missed)) if missed else None,
           "candidate_false_admissions": fa, "candidate_false_inside_labelled_instruction": fa_inside}
    (RESULTS / f"diagnose.{a.config}.{a.policy}.json").write_text(json.dumps(out, indent=1, sort_keys=True))
    print(json.dumps(out))
    return 0


def make_plan(cli: str, m: dict, regions: str, out: pathlib.Path) -> dict:
    """Unchanged production prune + validator on a region proposal; keeps the validated admission plan for the bridge."""
    import subprocess
    out.mkdir(parents=True, exist_ok=True)
    rp, pp = out / "regions.txt", out / "admission.plan"
    rp.write_text(regions, newline="\n")
    done = subprocess.run([cli, "emit-general-startup-bridge-c", "--rom", m["image"], "--reset-entry", "--rom-sha256", m["sha256"], "--immutable-rom-aot",
                           "--immutable-aot-region-proposal", str(rp), "--region-admission-plan-output", str(pp)], capture_output=True, text=True)
    return {"accepted": done.returncode == 0 and pp.exists(), "plan": str(pp)}


def bridge(cli: str, m: dict, out: pathlib.Path, frames: int, extra: list, timeout: int = 7200) -> dict:
    """One bounded bridge run (explicit instruction budget and wall timeout) - the gen2 differential's measurement, unchanged."""
    import genesis_ml_gen2_differential as diff
    rec = diff.variant(cli, pathlib.Path(m["image"]), out, frames, extra, "clang", timeout)
    return {k: rec.get(k) for k in ("ok", "wall_seconds", "cpu_seconds", "peak_rss_kib", "generated_bytes", "outcome", "frames", "final_state", "coverage",
                                    "frame_stream")}


def cmd_econ(a) -> int:
    v3.verify_plan()
    reg = registry()
    res = json.loads((RESULTS / f"table.{a.config}.json").read_text())
    rows = {r["id"]: r for r in res["records"]}
    work = pathlib.Path(a.work).resolve()
    import subprocess
    if subprocess.run(["git", "check-ignore", "-q", "--", str(work)], cwd=str(HERE.parent)).returncode != 0:
        raise SystemExit("--work must be an ignored directory inside the product checkout (commercial privacy guard)")
    out = {}
    for iid in a.images:
        m = reg[iid]
        if m.get("broad_aot") != "accepted":
            out[iid] = {"status": "broad_aot_rejected:" + str(m.get("broad_aot"))}
            continue
        broad_cache = work / "broad" / iid / "econ.json"
        if broad_cache.exists():
            b = json.loads(broad_cache.read_text())
        else:
            b = bridge(a.cli, m, work / "broad" / iid / "run", a.frames, [])
            if not b.get("ok"):
                raise SystemExit(f"{iid}: broad bridge run produced no coverage summary (out dir must be an ignored product path)")
            broad_cache.parent.mkdir(parents=True, exist_ok=True)
            broad_cache.write_text(json.dumps(b))
        r = rows[iid]
        pol = r["policies"][a.policy]
        if pol["abstained"]:
            out[iid] = {"status": "abstained (broad)", "broad": b}
            continue
        plan = make_plan(a.cli, m, regions_text(m["sha256"], m["size"], pol["_region"], r["cell_bytes"]), work / a.config / a.policy / iid / "plan")
        if not plan["accepted"]:
            out[iid] = {"status": "prune_rejected (broad fallback)", "broad": b}
            continue
        v = bridge(a.cli, m, work / a.config / a.policy / iid / "run", a.frames, ["--admission-plan", plan["plan"]])
        if not v.get("ok"):
            raise SystemExit(f"{iid}: selective bridge run produced no coverage summary")
        equal = all(b.get(k) == v.get(k) for k in ("outcome", "frames", "final_state", "coverage", "frame_stream"))
        out[iid] = {"status": "measured", "equal_digests": equal, "broad": {k: b[k] for k in ("wall_seconds", "cpu_seconds", "peak_rss_kib", "generated_bytes")},
                    "selective": {k: v[k] for k in ("wall_seconds", "cpu_seconds", "peak_rss_kib", "generated_bytes")}, "outcome": v.get("outcome"),
                    "frames": v.get("frames"), "retained": pol["retained"]}
        print(iid, json.dumps(out[iid]), flush=True)
    meas = [x for x in out.values() if x.get("status") == "measured"]
    agg = {}
    if meas:
        agg = {"images": len(meas), "generated_c_reduction_mean": sum(1 - x["selective"]["generated_bytes"] / x["broad"]["generated_bytes"] for x in meas) / len(meas),
               "bridge_cpu_reduction_mean": sum(1 - x["selective"]["cpu_seconds"] / x["broad"]["cpu_seconds"] for x in meas) / len(meas),
               "bridge_wall_reduction_mean": sum(1 - x["selective"]["wall_seconds"] / x["broad"]["wall_seconds"] for x in meas) / len(meas),
               "all_equal_digests": all(x["equal_digests"] for x in meas)}
    summary = {"config": a.config, "policy": a.policy, "frames": a.frames, "images": out, "aggregate": agg}
    (RESULTS / f"econ.{a.config}.{a.policy}.json").write_text(json.dumps(summary, indent=1, sort_keys=True))
    print(json.dumps(agg))
    return 0


SEALED_PATHS = {  # private inputs of the qualified sealed entries (identities are verified against the committed manifest)
    "E01": ("games/Alien Soldier (Japan).md", "sealed/truth/e01.1.cls"), "E03": ("games/Kid Chameleon (JUE) [!].bin", "sealed/truth/e03.1.cls"),
    "E05": ("games/Sonic Spinball (USA).md", "sealed/truth/e05.1.cls"), "E06": ("@sealed/hb/e06/inst/out/rom.bin", "sealed/truth/e06.cls"),
    "E07": ("@sealed/hb/e07/inst/out/rom.bin", "sealed/truth/e07.cls")}
GAMES = pathlib.Path(os.environ.get("SEG072_GAMES", "/root/dev/segarecomp-harness/segarecomp/games"))


def evaluate_external(im, scorer, policy, margin, seeds_addr, tokens_path, broad_aot) -> dict:
    """Per-image sealed metrics (shared by the one-shot sealed run and its development rehearsal)."""
    import time
    image_bytes = im.rom
    t0 = time.process_time()
    scores, cell = scorer(im)
    infer_cpu = time.process_time() - t0
    scores2, _ = scorer(im)
    deterministic = hashlib.sha256(scores.tobytes()).hexdigest() == hashlib.sha256(scores2.tobytes()).hexdigest()
    s = [float(x) for x in scores]
    probs = [sigmoid(x) for x in s]
    cls = im.classes
    labels = v3.cell_labels(cls, cell)
    starts = v3.starts_of(cls)
    seeds = {x // cell for x in seeds_addr if 0 <= x < len(image_bytes)}
    r = v3.policy_region(policy, s, probs, seeds, margin)
    region = r["region"]
    rbytes = sum(min((c + 1) * cell, len(image_bytes)) - c * cell for c in region)
    tok = np_().frombuffer(pathlib.Path(tokens_path).read_bytes(), dtype=np_().uint8).reshape(-1, 8)
    fa = 0
    for c in region:
        for off in range(c * cell, min((c + 1) * cell, len(image_bytes)) - 1, 2):
            if cls[off] not in (ord("U"), ord("I")) and tok[off // 2, 0] == 2:
                fa += 1
    return {"qualification": "QUALIFIED", "starts_labelled": len(starts), "starts_missed": v3.misses(starts, region, cell), "retained": rbytes / len(image_bytes),
            "abstained": r["abstained"], "budget": r["budget"], "frf": v3.frf(s, labels)["frf"], "residual_frf": v3.residual_frf(s, labels, seeds),
            "false_admission_candidates": fa, "inference_cpu_seconds": infer_cpu, "deterministic": deterministic, "broad_aot": broad_aot,
            "_region": sorted(region), "_cell": cell}


def cmd_rehearse(a) -> int:
    """Dry run of the sealed per-image code on DEVELOPMENT images with a given configuration (no marker, no sealed data)."""
    import segarecomp_ml_v3_train as tr
    reg = registry()
    scorer = tr.final_scorer(a.config) if a.config == "M3_FROZEN" or (tr.FINAL / "M3R.pt").exists() else None
    if scorer is None:
        raise SystemExit("no final model available for this configuration")
    for iid in a.images:
        m = reg[iid]
        im = tr.Image(pathlib.Path(m["dir"]))
        row = evaluate_external(im, scorer, a.policy, a.margin, seed_addresses(m), pathlib.Path(m["dir"]) / "tokens.bin", m.get("broad_aot"))
        print(iid, json.dumps(public(row)))
    return 0


def cmd_sealed(a) -> int:
    """ONE-SHOT sealed evaluation of the committed frozen configuration and policy (ADR 0151 `sealed`)."""
    import time
    import segarecomp_ml_v3_train as tr
    v3.verify_plan()
    rec = v3.unseal_once(ROOT / "sealed" / "UNSEALED.marker.json")  # refuses without the committed frozen record, or a second time
    config, policy, margin = rec["configuration"], rec["policy"], rec.get("adaptive_margin")
    manifest = json.loads((HERE / "segarecomp_ml_v3.corpus_manifest.json").read_text())["sealed"]
    scorer = tr.final_scorer(config)
    out = {}
    for e in manifest["entries"]:
        L = e["lineage"]
        if e["qualification"] != "QUALIFIED":
            out[L] = {"qualification": "UNQUALIFIED"}
            continue
        img_rel, cls_rel = SEALED_PATHS[L]
        img = (ROOT / img_rel[1:]) if img_rel.startswith("@") else GAMES / img_rel.split("/", 1)[1]
        cls_path = ROOT / cls_rel
        data = ROOT / "sealed" / "data" / L.lower()
        image_bytes = img.read_bytes()
        if hashlib.sha256(image_bytes).hexdigest() != e["image_sha256"] or hashlib.sha256(cls_path.read_bytes()).hexdigest() != e["opaque_classes_sha256"]:
            raise SystemExit(f"{L}: sealed identity mismatch")
        seeds_addr = [int(t, 16) for t in (data / "d.txt").read_text().split()] if (data / "d.txt").exists() else []
        im = tr.ExternalImage(img, data / "tokens.bin", cls_path, [x // 128 for x in seeds_addr if x < len(image_bytes)], L, L)
        row = evaluate_external(im, scorer, policy, margin, seeds_addr, data / "tokens.bin", e["broad_aot"])
        out[L] = row
        print(L, json.dumps(public(row)), flush=True)
    priv = {"frozen": rec, "lineages": out}
    (RESULTS / "sealed.private.json").write_text(json.dumps(priv))
    pub = {"configuration": config, "policy": policy, "lineages": public(out),
           "zero_miss_gate": all(v.get("starts_missed", 0) == 0 for v in out.values() if v.get("qualification") == "QUALIFIED"),
           "qualified": sum(1 for v in out.values() if v.get("qualification") == "QUALIFIED")}
    (RESULTS / "sealed.json").write_text(json.dumps(pub, indent=1, sort_keys=True))
    print(json.dumps({k: v for k, v in pub.items() if k != "lineages"}))
    return 0


def cmd_freeze(a) -> int:
    """Write tools/segarecomp_ml_v3.frozen.json from the development selection (before any sealed truth is opened)."""
    import platform
    v3.verify_plan()
    sel = json.loads((RESULTS / "selection.json").read_text())
    config, policy = sel["selected"].split("+")
    res = json.loads((RESULTS / f"table.{config}.json").read_text())
    margin = None
    if policy == "ADAPT":
        margin = v3.fit_adaptive_margin([(r["residual_frf"], r["density_e"]) for r in res["records"]])
    final = ROOT / "final"
    if config == "M3_FROZEN":
        weights = {"production_model_sha256": hashlib.sha256((HERE / "segarecomp_ml_region_gen2.model.json").read_bytes()).hexdigest()}
    else:
        model = {"M3_R": "M3R", "M1_HGB": "M1"}.get(config, "M4")
        weights = json.loads((final / f"{model}.digests.json").read_text())
    hw = json.loads((RESULTS / "hardware.json").read_text())
    import numpy
    import sklearn
    import torch
    rec = {"status": "FROZEN", "plan_sha256": v3.DIGEST_PATH.read_text().strip(), "configuration": config, "policy": policy, "adaptive_margin": margin,
           "selection_rule": sel["rule"], "incumbent_retained": sel["incumbent_retained"],
           "selection_table_sha256": hashlib.sha256(json.dumps(sel["table"], sort_keys=True).encode()).hexdigest(),
           "final_weights": weights, "training_backend": {"selected": hw["selected"], "improvement_estimate": hw["improvement"], "rule": hw["rule"]},
           "environment": {"python": platform.python_version(), "numpy": numpy.__version__, "scikit_learn": sklearn.__version__,
                           "torch": torch.__version__, "device": "cpu", "precision": "float32", "deterministic_algorithms": True,
                           "threads": {"M3_R": 4, "M4_DENSE": 2, "M3_FROZEN": "n/a (inference only)"}},
           "sealed_store": "private sealed/; opened once by `segarecomp_ml_v3_eval.py sealed` after this record is committed"}
    v3.FROZEN_PATH.write_text(json.dumps(rec, indent=1, sort_keys=True) + "\n")
    print(json.dumps(rec, sort_keys=True))
    return 0


def cmd_select(a) -> int:
    v3.verify_plan()
    table = {}
    for config in CONFIGS:
        p = RESULTS / f"table.{config}.json"
        if not p.exists():
            continue
        res = json.loads(p.read_text())
        for pol, row in res["pairs"].items():
            k = RESULTS / f"kstage.{config}.{pol}.json"
            mean_ku = json.loads(k.read_text())["mean_ku"] if k.exists() else None
            table[(config, pol)] = dict(row, mean_ku=mean_ku)
    sel = v3.select_pair(table)
    out = {"table": {f"{c}+{p}": v for (c, p), v in table.items()}, "selected": "+".join(sel["pair"]), "rule": sel["rule"],
           "incumbent_retained": sel["incumbent_retained"]}
    (RESULTS / "selection.json").write_text(json.dumps(out, indent=1, sort_keys=True))
    print(json.dumps({k: v for k, v in out.items() if k != "table"}))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("table")
    t.add_argument("--configs", nargs="+", default=list(CONFIGS))
    t.set_defaults(func=cmd_table)
    k = sub.add_parser("kstage")
    k.add_argument("--config", required=True)
    k.add_argument("--policy", required=True)
    k.add_argument("--cli", required=True)
    k.set_defaults(func=cmd_kstage)
    s = sub.add_parser("select")
    s.set_defaults(func=cmd_select)
    e = sub.add_parser("econ")
    e.add_argument("--config", required=True)
    e.add_argument("--policy", required=True)
    e.add_argument("--cli", required=True)
    e.add_argument("--images", nargs="+", required=True)
    e.add_argument("--frames", type=int, default=120)
    e.add_argument("--work", required=True)
    e.set_defaults(func=cmd_econ)
    fz = sub.add_parser("freeze")
    fz.set_defaults(func=cmd_freeze)
    rh = sub.add_parser("rehearse")
    rh.add_argument("--config", required=True)
    rh.add_argument("--policy", required=True)
    rh.add_argument("--margin", type=float)
    rh.add_argument("--images", nargs="+", required=True)
    rh.set_defaults(func=cmd_rehearse)
    se = sub.add_parser("sealed")
    se.set_defaults(func=cmd_sealed)
    d = sub.add_parser("diagnose")
    d.add_argument("--config", required=True)
    d.add_argument("--policy", required=True)
    d.set_defaults(func=cmd_diagnose)
    a = ap.parse_args()
    return a.func(a)


if __name__ == "__main__":
    raise SystemExit(main())
