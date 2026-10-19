#!/usr/bin/env python3
"""SEG-075-T003 (ADR 0161): cross-platform corpus curation, lineage grouping, evaluation barrier and frozen pre-registration (OFFLINE).

  lineages  aggregate lineage table from the canonical T002 corpus: platform, toolchain family, workload, label fractions, within-lineage
            near-duplicate groups (instruction-sequence containment) and cross-lineage independence (12-byte code shingles, the SEG-072
            measure, and instruction-sequence containment), against each other and against the available SEG-072 Genesis development set
  freeze    write the pre-registration (tools/segarecomp_xplat_design.plan.json + .digest): corpus manifest digest, the frozen
            development snapshot, lineage assignments, the evaluation barrier, conditions, models, budgets, metrics and every decision
            rule consumed by T004-T006; seal the reserved evaluation samples (moved out of the development snapshot into the private
            sealed directory, bound by opaque digests only). Nothing here reads a model or trains
  check     verify that the committed plan matches its digest and that the private snapshot / sealed binding still match the plan
  dev       (library) development image lists for T004/T005: refuses every sealed sample, path and lineage

Durable output is aggregate-only. Sealed labels are never summarized beyond the counts already recorded at freeze time.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import shutil
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
PLAN = HERE / "segarecomp_xplat_design.plan.json"
DIGEST = HERE / "segarecomp_xplat_design.plan.digest"
SCHEMA = "segarecomp.xplat_design.plan.v1"
SEALED_DIRNAME = "sealed"
K_SEQ = 8          # instruction-sequence shingle length (opcode word + instruction length per element)
DUP_CONTAINMENT = 0.5   # an image whose instruction sequences are >= 50% contained in the lineage's kept images is a near-duplicate
LINEAGE_MERGE = 0.30    # SEG-072 INDEPENDENCE threshold: >= 0.30 containment either way = one lineage

# ------------------------------------------------------------------------------------------------------------- pre-registration
# Lineage identity is author x toolchain x shared-code family; every T002 program is one lineage (measured independence below).
ASSIGNMENT = {
    "toslibc": {"role": "development", "platform_group": "atari-st", "toolchain_family": "gcc-13",
                "rationale": "Atari ST C runtime library + examples (GCC 13.3); training material in conditions C1/C2"},
    "emutos": {"role": "development", "platform_group": "atari-st", "toolchain_family": "gcc-4.6+gas",
               "rationale": "one firmware lineage (four configurations); one representative image, the other configurations are near-duplicates "
                            "(diagnostic only); counted as Atari ST (its primary platform) in every condition"},
    "BattleIsleEditor": {"role": "development", "platform_group": "amiga", "toolchain_family": "a68k",
                         "rationale": "six editor variants of one author's code base; near-duplicates reduced to representatives"},
    "amiga-realtime3d": {"role": "development", "platform_group": "amiga", "toolchain_family": "vasm",
                         "rationale": "nine incremental programs of one tutorial code base; one representative"},
    "neogeosdk": {"role": "reserved_evaluation", "platform_group": "neo-geo", "toolchain_family": "gcc-13",
                  "rationale": "platform held-out: the only Neo Geo lineage (never in any training or selection condition)"},
    "space-invaders": {"role": "reserved_evaluation", "platform_group": "atari-st", "toolchain_family": "vbcc",
                       "rationale": "compiler held-out: the only vbcc lineage (object-only, small)"},
    "HelloAmi": {"role": "diagnostics_only", "platform_group": "amiga", "toolchain_family": "vasm",
                 "rationale": "82 labelled starts in total: too small to be a training or evaluation lineage"},
}
EXCLUDED_T001 = {
    "licensing_review_required": ["perihelion-m68k-tutorials", "AtariST", "GODLIB", "Atari_ST_Sources", "x68000dev", "amiga-vasm-template"],
    "commercial_derived": ["galaga", "xevious", "mpatrol", "pacman500", "scramble500"],
    "reconstruction": ["amiga-boing", "monomental", "atari_reverse"],
    "blocked": ["amiga_asmdev_workflow", "AmiGalaga"],
    "infrastructure": ["atarist-toolkit-docker"],
}
CONDITIONS = {
    "C0": {"name": "Genesis-only", "xplat_training": []},
    "C1": {"name": "Genesis + Atari ST", "xplat_training": ["toslibc", "emutos"]},
    "C2": {"name": "Genesis + Atari ST + Amiga", "xplat_training": ["toslibc", "emutos", "BattleIsleEditor", "amiga-realtime3d"]},
}
MODELS = {
    "M3_FROZEN": "production gen2 M3 weights, unchanged (zero-shot reference on every development image)",
    "M1_HGB": "SEG-072 M1 control (HistGradientBoosting over the gen2 aggregates, random_state 49), all conditions",
    "M3_R": "SEG-072 M3-R recipe unchanged (seed 49, 30 epochs), all conditions; condition C0 additionally with seed 50 (noise floor)",
    "M4_DENSE": "SEG-072 M4-Dense recipe unchanged (seeds 1-3 ensemble, cells 128 and 64), conditions C0 and C2",
    "C0_reuse": "condition C0 genesis_lolo fold scores ARE the preserved SEG-072 development fold scores (M3R/M4 s1-s3/M1; same images, recipe "
                "and environment), admitted only after a bitwise reproduction check: one M3_R fold and one M4_DENSE seed-1 fold are "
                "retrained and must equal the preserved arrays exactly; otherwise every C0 fold is retrained",
}
FOLDS = {
    "genesis_lolo": "leave-one-lineage-out over the 11 qualified SEG-072 development lineages (available images only, see genesis); the "
                    "held-out Genesis lineage is scored; the condition's cross-platform development representatives are added to every "
                    "training set (never held out here)",
    "xplat_lolo": "for each cross-platform development lineage x: C2-minus-x (Genesis + the other three) versus C0-all (Genesis only), "
                  "both scoring x; this measures transfer to an unseen cross-platform lineage",
}
METRICS = {
    "primary": "Genesis transfer: mean over the 11 Genesis lineages of the per-lineage max FRF (required retention fraction at full "
               "recall, 128-byte cells, ties with the weakest positive counted) of the held-out fold",
    "co_primary": "worst-lineage FRF (max over Genesis lineages)",
    "secondary": ["misses (C outside R) at B35/B50/B60 per lineage", "false-positive admissions: negative (all-D) cells inside R at B50",
                  "residual FRF (seeds excluded)", "AUROC over labelled cells", "M4 cell 64 aggregation",
                  "short/mixed stratum: positive cells holding <= 8 starts or any D byte (misses among them)",
                  "xplat_lolo FRF / misses per cross-platform lineage", "per toolchain family and per platform"],
    "aggregation": "per lineage first, then mean over lineages (no image- or start-weighted pooling across lineages; EmuTOS counts once)",
}
NOISE = {"rule": "noise floor n = max over Genesis lineages of |FRF(M3_R seed 49) - FRF(M3_R seed 50)| in condition C0, but the decision "
                 "threshold tau = max(0.01, 2 * mean over lineages of that absolute difference)",
         "insufficient_if": "tau > 0.05 or any planned fit fails twice"}
DECISIONS = {
    "transfer": {
        "per_model": "for each of M3_R and M4_DENSE, compare C2 (and C1) with C0 on genesis_lolo; d_mean = mean FRF(Cx) - mean FRF(C0), "
                     "d_l = per-lineage difference",
        "TRANSFER_HELPS": "d_mean <= -tau AND no lineage d_l > +tau AND B50 misses not increased",
        "TRANSFER_HARMS": "d_mean >= +tau OR any lineage d_l > +2*tau OR B50 misses increased by > 0",
        "TRANSFER_NEUTRAL": "otherwise",
        "EVIDENCE_INSUFFICIENT": "the noise rule fires",
        "overall": "the class of C2 under M3_R (primary model, matched to SEG-072's frozen selection); M4_DENSE and C1 reported as "
                   "corroboration; disagreement between M3_R and M4_DENSE is reported, the M3_R class stands",
    },
    "t005_scope": {
        "model_A": "always built (cheap): a lightweight structural gradient-boosted model over CPU-owned decode features and local context, "
                   "evaluated on genesis_lolo and xplat_lolo with the best transfer condition's training set (C2 unless T004 is HARMS, "
                   "then C0)",
        "model_B": "built only if headroom exists: the best of {M3_R, M4_DENSE, A} has genesis_lolo mean FRF - mean positive-cell fraction "
                   ">= 0.05 (the ranking is materially worse than perfect) AND T004 is not EVIDENCE_INSUFFICIENT",
        "model_C": "built only if B beats A by >= tau in mean FRF without any lineage regressing by > tau",
        "preference": "if a simpler model is within tau of a more complex one (mean FRF and no extra misses at B50), the simpler is selected",
    },
    "t006": {
        "freeze": "before the sealed labels are opened: one candidate (configuration + training condition + budget policy B50 unless the "
                  "development table shows a smaller complete fixed budget) and the comparators M3_FROZEN and C0-all M3_R are written to "
                  "segarecomp_xplat_design.frozen.json and committed",
        "one_shot": "the sealed reserved lineages are scored once with the frozen models refit on all development data of the frozen "
                    "condition; a one-shot marker is written to the private sealed directory; a second run refuses",
        "credibility": "LIMITED: two reserved lineages (platform held-out Neo Geo / GCC 13.3; compiler held-out vbcc, object-only, small); "
                       "results are descriptive cross-platform evidence; no Genesis generalization claim (no fresh Genesis held-out "
                       "exists; the SEG-072 sealed cohort is exposed)",
        "economics": "SEG-074's exact-code benchmark applies to Genesis images only: candidate versus C0 region proposals on Genesis "
                     "development images (out-of-fold, development evidence), never on the reserved cross-platform lineages",
        "outcomes": ["CORPUS_IMPROVES_EXISTING", "LIGHTWEIGHT_STRUCTURAL_JUSTIFIED", "LOCAL_TRANSFORMER_JUSTIFIED",
                     "GRAPH_TRANSFORMER_JUSTIFIED", "CORPUS_EXPANSION_ONLY", "DATA_INSUFFICIENT", "NO_ECONOMIC_IMPROVEMENT"],
        "outcome_rule": "the simplest outcome supported by development evidence AND not contradicted by the one-shot evaluation (a frozen "
                        "candidate that misses more confident starts at B50 than the C0 comparator on a reserved lineage cannot be "
                        "recommended); no production model is promoted",
    },
}
BACKEND = {"selected": "CPU", "basis": "SEG-072 hardware benchmark (ROCm 8.9% faster than the 8-process CPU farm, below its 25% rule); "
           "same environment (Python 3.13 venv, numpy 2.5.3, scikit-learn 1.9.1, torch 2.14.1 CPU, float32, deterministic algorithms)",
           "parallelism": "M3_R 4 processes x 4 threads; M4_DENSE 8 processes x 2 threads; M1 sequential"}


def sha256(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def canonical(obj) -> bytes:
    return (json.dumps(obj, indent=1, sort_keys=True) + "\n").encode()


# ---------------------------------------------------------------------------------------------------------------- measurement
def instruction_sequences(image: bytes, classes: bytes, k: int = K_SEQ) -> set:
    """Shingles of k consecutive labelled instructions (opcode word, length): robust to relocated addresses (pure; unit-tested)."""
    out, run, end = set(), [], None
    for i, x in enumerate(classes):
        if x != ord("I"):
            continue
        n = 1
        while i + n < len(classes) and classes[i + n] == ord("c"):
            n += 1
        if end is not None and end != i:
            run = []
        run.append((image[i:i + 2], n))
        end = i + n
        if len(run) >= k:
            out.add(tuple(run[-k:]))
    return out


def representatives(images: list[tuple[str, set, int]], threshold: float = DUP_CONTAINMENT) -> tuple[list[str], dict[str, float]]:
    """Greedy, deterministic near-duplicate reduction (largest first by labelled starts, then id): keep an image unless >= threshold of
    its instruction sequences are contained in the kept images. Returns (kept ids, containment of every dropped id) (pure; tested)."""
    kept, union, dropped = [], set(), {}
    for iid, seqs, _n in sorted(images, key=lambda t: (-t[2], t[0])):
        if not seqs:
            continue
        c = len(seqs & union) / len(seqs)
        if c < threshold:
            kept.append(iid)
            union |= seqs
        else:
            dropped[iid] = round(c, 4)
    return kept, dropped


def containment(a: set, b: set) -> tuple[float, float]:
    inter = len(a & b)
    return (inter / len(a) if a else 0.0, inter / len(b) if b else 0.0)


def load_corpus(corpus: pathlib.Path) -> list[dict]:
    out = []
    for m in sorted(corpus.rglob("meta.json")):
        d = json.loads(m.read_text())
        d["dir"] = str(m.parent)
        out.append(d)
    return out


def genesis_registry(seg072: pathlib.Path) -> tuple[list[dict], list[dict]]:
    """SEG-072 qualified development registry split into (available, unavailable) images by physical presence + identity."""
    man = json.loads((HERE / "segarecomp_ml_v3.corpus_manifest.json").read_text())
    qualified = {d["lineage"] for d in man["development"] if d["qualification"] == "QUALIFIED"}
    avail, missing = [], []
    for m in sorted((seg072 / "data").rglob("meta.json")):
        d = json.loads(m.read_text())
        if "classes_sha256" not in d or d["lineage"] not in qualified:
            continue
        p = pathlib.Path(d["image"])
        row = {"id": d["id"], "lineage": d["lineage"], "kind": d["kind"], "sha256": d["sha256"], "dir": str(m.parent)}
        if p.is_file() and p.stat().st_size == d["size"]:
            avail.append(row)
        else:
            missing.append(row)
    return avail, missing


def lineage_table(corpus: pathlib.Path, seg072: pathlib.Path | None) -> dict:
    sys.path.insert(0, str(HERE))
    import segarecomp_ml_v3_data as v3d
    metas = load_corpus(corpus)
    by = {}
    for m in metas:
        img = pathlib.Path(m["dir"], "image.bin").read_bytes()
        cls = pathlib.Path(m["dir"], "classes.bin").read_bytes()
        L = by.setdefault(m["candidate"], {"samples": [], "seqs": set(), "shingles": set(), "bytes": {c: 0 for c in "IcDU"}, "size": 0})
        s = instruction_sequences(img, cls)
        L["samples"].append((m["id"], s, cls.count(b"I")))
        L["seqs"] |= s
        L["shingles"] |= v3d.shingles(img, cls)
        for c in "IcDU":
            L["bytes"][c] += m["bytes"][c]
        L["size"] += m["size"]
        L.update(platform=m["platform"], toolchain=m["toolchain"], workload=m["workload"])
    genesis_seq, genesis_sh = set(), set()
    if seg072 is not None:
        for g in genesis_registry(seg072)[0]:
            meta = json.loads(pathlib.Path(g["dir"], "meta.json").read_text())
            img = pathlib.Path(meta["image"]).read_bytes()
            cls = pathlib.Path(g["dir"], "classes.bin").read_bytes()
            genesis_seq |= instruction_sequences(img, cls)
            genesis_sh |= v3d.shingles(img, cls)
    rows = {}
    for cid, L in sorted(by.items()):
        kept, dropped = representatives(L["samples"])
        starts_kept = sum(n for i, _s, n in L["samples"] if i in kept)
        rows[cid] = {"platform": L["platform"], "toolchain": L["toolchain"], "workload": L["workload"], "samples": len(L["samples"]),
                     "bytes": L["size"], "starts": sum(n for _i, _s, n in L["samples"]),
                     "fractions": {c: round(L["bytes"][c] / L["size"], 4) for c in "IcDU"},
                     "representatives": kept, "near_duplicates": dropped, "starts_in_representatives": starts_kept,
                     "size_class": "large" if starts_kept >= 20000 else "medium" if starts_kept >= 2000 else "small",
                     "genesis_containment": {"code_shingles": round(containment(L["shingles"], genesis_sh)[0], 4),
                                             "instruction_sequences": round(containment(L["seqs"], genesis_seq)[0], 4)}}
    pairs = {}
    keys = sorted(by)
    for i, x in enumerate(keys):
        for y in keys[i + 1:]:
            a = containment(by[x]["shingles"], by[y]["shingles"])
            b = containment(by[x]["seqs"], by[y]["seqs"])
            pairs[f"{x}~{y}"] = {"code_shingles": [round(v, 4) for v in a], "instruction_sequences": [round(v, 4) for v in b]}
    worst = max(max(max(p["code_shingles"]), max(p["instruction_sequences"])) for p in pairs.values()) if pairs else 0.0
    return {"lineages": rows, "pairs": pairs, "max_pair_containment": round(worst, 4), "merge_threshold": LINEAGE_MERGE}


# ----------------------------------------------------------------------------------------------------------------- freeze/seal
def feasibility(table: dict, genesis_available: int, genesis_missing: int) -> dict:
    """The T003 feasibility gate (pure; unit-tested): evaluated on aggregates only."""
    L = table["lineages"]
    dev = [c for c, a in ASSIGNMENT.items() if a["role"] == "development" and c in L]
    res = [c for c, a in ASSIGNMENT.items() if a["role"] == "reserved_evaluation" and c in L]
    plat = {}
    for c in dev:
        plat.setdefault(ASSIGNMENT[c]["platform_group"], []).append(c)
    fams = sorted({ASSIGNMENT[c]["toolchain_family"] for c in dev})
    max_u = max(L[c]["fractions"]["U"] for c in dev + res)
    checks = {
        "independent_lineages_no_merge": table["max_pair_containment"] < LINEAGE_MERGE,
        "atari_training_lineages_ge_2": len(plat.get("atari-st", [])) >= 2,
        "amiga_training_lineages_ge_2": len(plat.get("amiga", [])) >= 2,
        "training_toolchain_families_ge_3": len(fams) >= 3,
        "label_unknown_fraction_le_0.10": max_u <= 0.10,
        "mixed_code_data_present": any(L[c]["fractions"]["D"] >= 0.3 and L[c]["fractions"]["I"] >= 0.1 for c in dev),
        "reserved_lineages_ge_2": len(res) >= 2,
        "reserved_platform_or_compiler_held_out": any(ASSIGNMENT[c]["platform_group"] not in plat for c in res)
                                                  and any(ASSIGNMENT[c]["toolchain_family"] not in fams for c in res),
        "genesis_folds_intact": genesis_available > 0,
    }
    admit_t004 = all(v for k, v in checks.items() if k != "reserved_platform_or_compiler_held_out")
    return {"checks": checks, "development_lineages": dev, "reserved_lineages": res, "training_toolchain_families": fams,
            "genesis_images_available": genesis_available, "genesis_images_unavailable": genesis_missing,
            "admit_t004": admit_t004, "admit_t006_one_shot": admit_t004 and checks["reserved_platform_or_compiler_held_out"],
            "evaluation_credibility": "LIMITED" if len(res) < 4 else "ADEQUATE"}


def seal(corpus: pathlib.Path, snapshot: pathlib.Path, plan_lineages: dict) -> dict:
    """Copy development samples into the frozen snapshot; MOVE nothing out of the corpus, but copy reserved samples only into
    snapshot/sealed (development tooling refuses that directory). Returns the binding: per reserved sample an opaque digest."""
    if snapshot.exists():
        raise SystemExit(f"snapshot {snapshot} already exists: the freeze is one-time")
    dev_dir = snapshot / "dev"
    sealed_dir = snapshot / SEALED_DIRNAME
    binding = {}
    dev = {}
    for m in load_corpus(corpus):
        role = ASSIGNMENT[m["candidate"]]["role"]
        rel = pathlib.Path(m["dir"]).relative_to(corpus)
        if role == "reserved_evaluation":
            dst = sealed_dir / rel
            shutil.copytree(m["dir"], dst)
            h = hashlib.sha256()
            for f in sorted(dst.iterdir()):
                h.update(f.name.encode() + b"\0" + f.read_bytes())
            binding[m["id"]] = {"lineage": m["candidate"], "opaque_sha256": h.hexdigest()}
        else:
            dst = dev_dir / rel
            shutil.copytree(m["dir"], dst)
            meta = json.loads((dst / "meta.json").read_text())
            meta["image"] = str((dst / "image.bin").resolve())
            meta["lineage"] = m["candidate"]
            (dst / "meta.json").write_text(json.dumps(meta, indent=1, sort_keys=True))
            dev[m["id"]] = {"lineage": m["candidate"], "role": role, "sha256": m["sha256"], "classes_sha256": m["classes_sha256"],
                            "tokens_sha256": m["tokens_sha256"],
                            "representative": m["id"] in plan_lineages[m["candidate"]]["representatives"]}
    for d in (sealed_dir,):
        os.chmod(d, 0o700)
    return {"dev": dev, "sealed": binding}


def cmd_lineages(a) -> int:
    t = lineage_table(pathlib.Path(a.corpus), pathlib.Path(a.seg072) if a.seg072 else None)
    print(json.dumps(t, indent=1, sort_keys=True))
    return 0


def cmd_freeze(a) -> int:
    corpus = pathlib.Path(a.corpus).resolve()
    snapshot = pathlib.Path(a.snapshot).resolve()
    seg072 = pathlib.Path(a.seg072).resolve()
    corpus_manifest = sha256((corpus / "MANIFEST.json").read_bytes())
    table = lineage_table(corpus, seg072)
    avail, missing = genesis_registry(seg072)
    gate = feasibility(table, len(avail), len(missing))
    binding = seal(corpus, snapshot, table["lineages"])
    snap_manifest = canonical({"dev": binding["dev"], "sealed": binding["sealed"]})
    (snapshot / "SNAPSHOT.json").write_bytes(snap_manifest)
    lineage_rows = {}
    for c, r in table["lineages"].items():
        lineage_rows[c] = {k: r[k] for k in ("platform", "toolchain", "workload", "samples", "starts", "fractions", "size_class",
                                             "starts_in_representatives", "genesis_containment")}
        lineage_rows[c]["representatives"] = len(r["representatives"])
        lineage_rows[c]["near_duplicates"] = len(r["near_duplicates"])
        lineage_rows[c].update(ASSIGNMENT[c])
        if ASSIGNMENT[c]["role"] == "reserved_evaluation":
            lineage_rows[c]["fractions"] = "sealed (quality checked at freeze: decoder agreement and U fraction within the gate)"
    plan = {
        "schema": SCHEMA, "task": "SEG-075-T003", "adr": "0161",
        "corpus": {"t002_manifest_sha256": corpus_manifest, "snapshot_sha256": sha256(snap_manifest),
                   "snapshot_dev_samples": len(binding["dev"]), "snapshot_sealed_samples": len(binding["sealed"])},
        "genesis": {"source": "SEG-072 qualified development registry (ADR 0152), images physically available at freeze",
                    "available_images": len(avail), "available_by_lineage": _count(avail), "unavailable_images": len(missing),
                    "unavailable_by_lineage": _count(missing),
                    "restored": "the 36 Sonic CD modules (lineage D01) referenced by the SEG-072 registry were restored from the "
                                "operator's disc image (ISO 9660 files of track 1); every restored file equals the SHA-256 the SEG-072 "
                                "handoff bundle recorded",
                    "unavailable_rule": "an image that is unavailable at freeze is excluded from EVERY condition (including C0, then "
                                        "retrained), so comparisons stay matched",
                    "available_ids_sha256": sha256(canonical(sorted((g["id"], g["sha256"]) for g in avail)))},
        "lineages": lineage_rows, "independence": {"max_pair_containment": table["max_pair_containment"],
                                                   "merge_threshold": LINEAGE_MERGE, "measures": ["code_shingles(12 bytes)",
                                                                                                  f"instruction_sequences(k={K_SEQ})"]},
        "dedup": {"rule": f"within a lineage, greedy largest-first: an image whose instruction sequences are >= {DUP_CONTAINMENT} contained "
                          "in the kept images is a near-duplicate; only representatives train; near-duplicates are diagnostic only"},
        "excluded_t001": EXCLUDED_T001, "seg072_sealed_cohort": "exposed by SEG-072: development/diagnostic only, never fresh evidence",
        "conditions": CONDITIONS, "models": MODELS, "folds": FOLDS, "metrics": METRICS, "noise": NOISE, "decisions": DECISIONS,
        "budgets": {"fixed": ["B35", "B50", "B60"], "max_retention": 0.60, "cell_bytes": 128,
                    "training_effort": "every fit uses its unchanged SEG-072 recipe (epochs/steps/seeds/batch); conditions differ only in the "
                                       "training image list"},
        "backend": BACKEND, "feasibility": gate,
        "barrier": {"sealed_dir": f"<HARNESS_ROOT>/.cache/seg075/t003/snapshot/{SEALED_DIRNAME}",
                    "rule": "development, tuning and selection tooling refuses the sealed directory, every sealed sample id and every "
                            "reserved lineage; the binding holds opaque digests only; the sealed labels are opened once by T006 after the "
                            "frozen candidate is committed",
                    "sealed_samples": len(binding["sealed"])},
    }
    data = canonical(plan)
    PLAN.write_bytes(data)
    DIGEST.write_text(sha256(data) + "\n")
    print(json.dumps({"plan_sha256": sha256(data), "snapshot_sha256": plan["corpus"]["snapshot_sha256"], "feasibility": gate}, indent=1))
    return 0


def _count(rows):
    out = {}
    for r in rows:
        out[r["lineage"]] = out.get(r["lineage"], 0) + 1
    return dict(sorted(out.items()))


def verify_plan() -> dict:
    data = PLAN.read_bytes()
    if DIGEST.read_text().split()[0] != sha256(data):
        raise SystemExit("design plan differs from its committed digest")
    plan = json.loads(data)
    if plan.get("schema") != SCHEMA:
        raise SystemExit("unexpected design plan schema")
    return plan


def cmd_check(a) -> int:
    plan = verify_plan()
    snap = pathlib.Path(a.snapshot).resolve()
    sm = (snap / "SNAPSHOT.json").read_bytes()
    problems = []
    if sha256(sm) != plan["corpus"]["snapshot_sha256"]:
        problems.append("snapshot manifest differs from the plan")
    s = json.loads(sm)
    for iid, row in s["dev"].items():
        d = snap / "dev" / iid.replace("#", "@")
        for f, key in (("image.bin", "sha256"), ("classes.bin", "classes_sha256"), ("tokens.bin", "tokens_sha256")):
            if not (d / f).is_file() or sha256((d / f).read_bytes()) != row[key]:
                problems.append(f"{iid}: {f} differs")
    for iid, row in s["sealed"].items():
        d = snap / SEALED_DIRNAME / iid.replace("#", "@")
        h = hashlib.sha256()
        for f in sorted(d.iterdir()) if d.is_dir() else []:
            h.update(f.name.encode() + b"\0" + f.read_bytes())
        if h.hexdigest() != row["opaque_sha256"]:
            problems.append(f"{iid}: sealed binding differs")
    print(json.dumps({"plan": "PASS", "dev_samples": len(s["dev"]), "sealed_samples": len(s["sealed"]), "problems": problems,
                      "verdict": "PASS" if not problems else "FAIL"}, indent=1))
    return 0 if not problems else 1


# ------------------------------------------------------------------------------------------------------------- development API
def reserved_ids(plan: dict | None = None) -> set[str]:
    plan = plan or verify_plan()
    return {c for c, r in plan["lineages"].items() if r["role"] == "reserved_evaluation"}


def refuse_sealed(paths_or_ids) -> None:
    """Barrier for development tooling: no sealed directory, reserved lineage or sealed sample may be touched."""
    res = reserved_ids()
    for x in paths_or_ids:
        s = str(x)
        if f"/{SEALED_DIRNAME}/" in s or s.endswith(f"/{SEALED_DIRNAME}") or any(s == r or f"/{r}/" in s or s.startswith(f"xp/{r}/") for r in res):
            raise SystemExit(f"evaluation barrier: sealed/reserved material refused: {s}")


def dev_sample_dirs(snapshot: pathlib.Path, lineages: list[str], representatives_only: bool = True) -> list[pathlib.Path]:
    """Development sample directories of the given cross-platform lineages from the frozen snapshot (barrier enforced)."""
    plan = verify_plan()
    refuse_sealed(lineages)
    s = json.loads((snapshot / "SNAPSHOT.json").read_bytes())
    if sha256((snapshot / "SNAPSHOT.json").read_bytes()) != plan["corpus"]["snapshot_sha256"]:
        raise SystemExit("snapshot differs from the frozen plan")
    out = []
    for iid, row in sorted(s["dev"].items()):
        if row["lineage"] in lineages and row["role"] == "development" and (row["representative"] or not representatives_only):
            d = snapshot / "dev" / iid.replace("#", "@")
            refuse_sealed([d])
            out.append(d)
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("lineages")
    p.add_argument("--corpus", required=True)
    p.add_argument("--seg072")
    p.set_defaults(func=cmd_lineages)
    f = sub.add_parser("freeze")
    f.add_argument("--corpus", required=True)
    f.add_argument("--snapshot", required=True)
    f.add_argument("--seg072", required=True)
    f.set_defaults(func=cmd_freeze)
    c = sub.add_parser("check")
    c.add_argument("--snapshot", required=True)
    c.set_defaults(func=cmd_check)
    a = ap.parse_args(argv)
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())
