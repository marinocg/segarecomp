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
    "M3_FROZEN": "production gen2 M3 weights, unchanged: zero-shot reference on every Genesis development image (SEG-072's stored "
                 "scores) and every cross-platform development representative (inference)",
    "M1_HGB": "SEG-072 M1 control (HistGradientBoosting over the gen2 aggregates, random_state 49), all conditions",
    "M3_R": "SEG-072 M3-R recipe unchanged (seed 49, 30 epochs), all conditions; condition C0 additionally with seed 50 (noise replicate)",
    "M4_DENSE": "SEG-072 M4-Dense recipe unchanged (seeds 1-3, word logits averaged then cell max at 128 and 64 bytes), conditions C0 and C2",
    "C0_reuse": "condition C0 genesis_lolo fold scores ARE the preserved SEG-072 development fold scores (M3R / M4 s1-s3 / M1; same images, "
                "recipes and environment), admitted only when `reproduce` retrained one fold of EACH of M3_R, M4_DENSE seed 1 and M1_HGB "
                "under this plan digest and all three equal the preserved arrays bit for bit (reproduce.json); otherwise every C0 fold "
                "is retrained; the runner refuses reuse without a passing reproduce.json",
}
FOLDS = {
    "genesis_lolo": "leave-one-lineage-out over the 11 qualified SEG-072 development lineages (all 50 images); the held-out Genesis "
                    "lineage is scored; the condition's cross-platform development representatives are added to every training set",
    "xplat_lolo": "for each cross-platform development lineage x: C2-minus-x (Genesis + the other three) versus C0-all (Genesis only), "
                  "both scoring the representatives of x; this measures transfer to an unseen cross-platform lineage",
}
METRICS = {
    "er": "excess retention ER(image) = (cells_needed - positive_cells) / (cells - positive_cells), cells_needed = the FRF cell count (rank "
          "order, ties with the weakest positive counted), 128-byte cells (M4@64: 64-byte); the fraction of the non-positive cells that "
          "must be retained to keep every labelled start; 0 = perfect ranking, 1 = no better than retaining everything",
    "informative": "an image is informative iff it has >= 1 positive and >= 1 non-positive cell; ER is defined on informative images "
                   "only; the number of trivial (all-positive or no-positive) images per lineage is reported",
    "primary": "Genesis transfer: mean over the 11 Genesis lineages of the per-lineage max ER over its informative held-out images",
    "co_primary": "worst-lineage ER (max over the 11 Genesis lineages)",
    "secondary": ["FRF (reported, not decisive; degenerate on all-positive images)", "per-lineage B35/B50/B60 misses (labelled starts "
                  "outside R; false negatives) and negative (all-D) cells admitted (false positives), reported separately",
                  "residual ER/FRF (seeds excluded)", "AUROC over labelled cells", "short/mixed stratum: positive cells holding <= 8 starts "
                  "or any D byte (missed cells)", "xplat_lolo: mean / worst ER and misses over the four cross-platform lineages",
                  "per toolchain family and per platform", "trivial-image counts"],
    "aggregation": "per lineage first, then mean over lineages (no image- or start-weighted pooling across lineages; EmuTOS counts once "
                   "in metrics - in M3_R training it dominates the cross-platform segments, see disclosure)",
    "seeds": "seed cells come from seed ADDRESSES (Genesis: the SEG-072 registry d.txt / seed_starts; cross-platform: meta "
             "seed_addresses), mapped address // cell_bytes for each cell size",
    "retained": "retained fraction = retained cells / cells (cell units; SEG-072 reported cells too for policies)",
}
NOISE = {"tau": "tau = max(0.01, 2 * mean over the 11 Genesis lineages of |ER_l(M3_R C0 seed 49) - ER_l(M3_R C0 seed 50)|)",
         "miss_allowance": "a_l = |misses_B50_l(seed 49) - misses_B50_l(seed 50)| per Genesis lineage l (M3_R, C0); applied to every model",
         "insufficient_if": "tau > 0.05, or any planned fit missing / failed twice (the table then classifies EVIDENCE_INSUFFICIENT)",
         "disclosure": "one seed pair; conservative (biased toward NEUTRAL/HARMS); fits are deterministic, so tau is not adjustable"}
DECISIONS = {
    "transfer": {
        "per_model": "for M3_R, M4_DENSE@128, M4_DENSE@64 and M1_HGB, compare C2 (and C1 where fitted) with C0 on genesis_lolo; "
                     "d_l = ER_l(Cx) - ER_l(C0), d_mean = mean over lineages; e_l = max(0, misses_B50_l(Cx) - misses_B50_l(C0) - a_l)",
        "TRANSFER_HELPS": "d_mean <= -tau AND every d_l <= +tau AND every e_l = 0",
        "TRANSFER_HARMS": "d_mean >= +tau OR any d_l > +2*tau OR any e_l > 0",
        "TRANSFER_NEUTRAL": "otherwise",
        "EVIDENCE_INSUFFICIENT": "the noise / completeness rule fires",
        "overall": "the class of C2 under M3_R (primary model, matched to SEG-072's frozen selection); M4_DENSE, M1_HGB and C1 corroborate; "
                   "Amiga attribution (C1 vs C2) relies on M4_DENSE (M3_R's cross-platform segments are dominated by EmuTOS)",
    },
    "ordering": {
        "rule": "a configuration's development record = (genesis B50 misses summed over lineages, genesis mean ER, xplat_lolo mean ER, "
                "complexity); candidates are compared lexicographically: fewer B50 misses; then lower genesis mean ER unless within tau "
                "(treated equal); then lower xplat_lolo mean ER unless within tau; then the simpler configuration",
        "complexity": ["M3_FROZEN", "M1_HGB", "A", "M3_R", "M4_DENSE@128", "M4_DENSE@64", "B", "C"],
        "condition": "each configuration in its best transfer condition (C2 if its class is HELPS or NEUTRAL, else C0)",
    },
    "t005_scope": {
        "condition": "models A/B/C train on C2 unless the T004 overall class is TRANSFER_HARMS (then C0); same genesis_lolo and xplat_lolo "
                     "folds as T004; no hyper-parameter search (fixed configurations below) - nothing is tuned on the folds compared",
        "model_A": "always built: sklearn HistGradientBoostingClassifier(max_iter=300, max_depth=6, learning_rate=0.05, min_samples_leaf=40, "
                   "l2_regularization=1.0, max_bins=255, early_stopping=False, class_weight=balanced, random_state=49) over per-cell "
                   "features: counts per token field value (status/length/family/control/ea_src/ea_dst/width/flags) over the cell's words; "
                   "ordinary-decode chain features (longest chain of consecutive ordinary decodes following decoder lengths starting in "
                   "the cell; number of chain starts); direct control-flow candidates (branch/call targets computed from the opcode "
                   "displacement or absolute operand of ordinary direct-control decodes: count landing in the image on an ordinary "
                   "decode, count landing in this cell from anywhere); byte context (zero-byte fraction, printable-ASCII fraction, "
                   "byte-value entropy); relocation-field count in the cell (0 for Genesis); seed-cell flag; the same aggregates over "
                   "the +/-1 and +/-4 neighbour cells; masked cells excluded from training",
        "model_B": "built only if the ordering's best of {M1_HGB, A, M3_R, M4_DENSE} has genesis mean ER >= 0.10 AND T004 is not "
                   "EVIDENCE_INSUFFICIENT: a bounded local instruction-aware Transformer: per-word input = embed(hi byte) + embed(lo byte) "
                   "+ the M3/M4 decode-token embeddings, d_model 64, 2 encoder layers, 4 heads, feed-forward 128, learned positions, "
                   "attention restricted to non-overlapping 256-word windows with 64 words of real context (or PAD) on each side; per-word "
                   "logit -> cell max (M4 aggregation); training exactly M4's budget and recipe (4000 steps, batch 8, AdamW lr 0.002 x0.1 "
                   "after 80%, weight decay 1e-4, masked BCE with M4's pos_weight, lineage-uniform sampling, seeds 1-3 averaged)",
        "model_C": "built only if B is better than A by the ordering with genesis mean ER lower by >= tau: B plus 2 rounds of typed sparse "
                   "message passing per encoder layer over edges within the window+context: fall-through (ordinary decode -> next "
                   "position by decoder length), direct branch / call target (computed as for A), overlapping interpretation (positions "
                   "inside a decoded instruction's extent), each typed separately as CANDIDATE edges, plus a node flag for positions "
                   "statically reachable from seed roots by fall-through and direct edges (PROVEN); hypothetical edges never mark a "
                   "position reachable; same training budget as B",
        "preference": "the ordering rule (simpler wins within tau and without extra misses)",
        "code_binding": "the A/B/C implementation files are written after this freeze and are bound by digest in a T005 pre-fit addendum "
                        "(segarecomp_xplat_design.t005.json, committed and reviewed before any A/B/C fit)",
    },
    "t006": {
        "freeze": "before the sealed labels are opened: the ordering's best configuration (+ its condition) as the candidate, budget policy "
                  "B50, and the comparators M3_FROZEN and C0-all M3_R+B50 are written to segarecomp_xplat_design.frozen.json and "
                  "committed",
        "one_shot": "the frozen candidate and comparators are refit on all development data of their condition (fold 'all'), then score "
                    "the sealed reserved samples once; a one-shot marker is written to the private sealed directory; a second run "
                    "refuses",
        "evaluated_samples": "every sealed sample with >= 1 labelled start (all of them, not only representatives - no reserved image "
                             "trains); ER over informative samples; misses over all samples; per reserved lineage",
        "confident_starts": "the I-labelled starts of the sealed samples (ADR 0160 labels; U never counts as a miss)",
        "credibility": "LIMITED: two reserved lineages (platform held-out Neo Geo, its GCC 13 compiler family is in training via toslibc; "
                       "compiler held-out vbcc, object-only, small, shares vasm and the Atari ST platform with training); descriptive "
                       "cross-platform evidence; no Genesis generalization claim",
        "economics": "economic measure = complete budget: the smallest of B35/B50/B60 whose genesis_lolo B-misses summed over all 11 "
                     "lineages are <= the M3_R seed-replicate allowance sum (sum a_l); a candidate is economically better than the "
                     "comparator C0 M3_R iff its complete budget is smaller (retained cells -30%/-17%); when it is, the SEG-074 exact-code "
                     "benchmark (tools/segarecomp_oracle_bench.py, unchanged) measures generated C bytes and compile CPU on the Genesis "
                     "development images it supports, at both budgets, and the improvement is confirmed only if generated C falls by >= 10% "
                     "AND compile CPU by >= 10% (SEG-072's incumbent gate); never run on the reserved cross-platform lineages",
        "outcomes": ["CORPUS_IMPROVES_EXISTING", "LIGHTWEIGHT_STRUCTURAL_JUSTIFIED", "LOCAL_TRANSFORMER_JUSTIFIED",
                     "GRAPH_TRANSFORMER_JUSTIFIED", "CORPUS_EXPANSION_ONLY", "DATA_INSUFFICIENT", "NO_ECONOMIC_IMPROVEMENT"],
        "outcome_rule": [
            "1 DATA_INSUFFICIENT: T004 overall EVIDENCE_INSUFFICIENT",
            "2 contradiction: the one-shot contradicts the candidate if, on any reserved lineage, it misses more confident starts at B50 "
            "than the C0 M3_R comparator, or its reserved mean ER exceeds the comparator's by > tau; a contradicted candidate cannot "
            "carry classes 3-6",
            "3 CORPUS_IMPROVES_EXISTING: candidate is M1_HGB/M3_R/M4_DENSE trained on C1/C2, ordered better than C0 M3_R, economically "
            "better (confirmed), not contradicted",
            "4 LIGHTWEIGHT_STRUCTURAL_JUSTIFIED / 5 LOCAL_TRANSFORMER_JUSTIFIED / 6 GRAPH_TRANSFORMER_JUSTIFIED: candidate is A / B / C, "
            "ordered better than C0 M3_R, economically better (confirmed), not contradicted",
            "7 NO_ECONOMIC_IMPROVEMENT: a candidate is ordered better than C0 M3_R but is not economically better (or the benchmark does "
            "not confirm), or it is contradicted",
            "8 CORPUS_EXPANSION_ONLY: no candidate is ordered better than C0 M3_R on Genesis, but C2 improves xplat_lolo mean ER over "
            "C0-all by >= tau for M3_R",
            "9 otherwise NO_ECONOMIC_IMPROVEMENT",
            "rules apply in order; exactly one class; no production model is promoted",
        ],
    },
}
BACKEND = {"selected": "CPU", "basis": "SEG-072 hardware benchmark (ROCm 8.9% faster than the 8-process CPU farm, below its 25% rule); "
           "research venv rebuilt at the recorded versions (Python 3.13.14, numpy 2.5.3, scikit-learn 1.9.1, torch 2.14.1+cu130 on CPU, "
           "float32, deterministic algorithms)",
           "parallelism": "M3_R 4 processes x 4 threads; M4_DENSE 8 processes x 2 threads; M1 2 processes"}
BOUND_CODE = ("segarecomp_xplat_design.py", "segarecomp_xplat_transfer.py", "segarecomp_ml_v3.py", "segarecomp_ml_v3_train.py",
              "segarecomp_ml_v3_eval.py", "segarecomp_ml_region_gen2.py", "segarecomp_ml_region_gen2_train.py")
SUPERSEDES = {"plan_sha256": "f551fea791c307e15f5203ed30102d1b80db6a583128ae04a82df2565a053ad5",
              "reason": "C3 review (APPROVE_WITH_CONDITIONS, before any fit): FRF degenerate on all-positive images (ER adopted), B50 miss "
                        "rule without noise allowance, open selection/economics/outcome choices, runner not bound to the plan, C0 reuse "
                        "not gated, representatives omitted images without an 8-instruction run"}


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
    for iid, seqs, n in sorted(images, key=lambda t: (-t[2], t[0])):
        if not seqs:
            if n > 0:
                kept.append(iid)  # labelled code without an 8-instruction run: no evidence of duplication, kept
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
                            "representative": m["id"] in plan_lineages[m["candidate"]]["representatives"],
                            "files": {f.name: sha256(f.read_bytes()) for f in sorted(dst.iterdir())}}
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
    if a.refreeze and snapshot.exists():
        # allowed only before any fit: the superseded plan is recorded in the new one (SUPERSEDES)
        for f in sorted(snapshot.rglob("*"), reverse=True):
            if f.is_dir():
                os.chmod(f, 0o700)
        shutil.rmtree(snapshot)
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
        "backend": BACKEND, "feasibility": gate, "supersedes": SUPERSEDES,
        "bound_code": {n: sha256((HERE / n).read_bytes()) for n in BOUND_CODE},
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
    for name, want in plan.get("bound_code", {}).items():
        if sha256((HERE / name).read_bytes()) != want:
            raise SystemExit(f"{name} differs from the code bound by the frozen plan")
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
        present = {f.name for f in d.iterdir()} if d.is_dir() else set()
        if present != set(row["files"]):
            problems.append(f"{iid}: file set differs")
        for f, want in row["files"].items():
            if (d / f).is_file() and sha256((d / f).read_bytes()) != want:
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
        parts = set(pathlib.PurePosixPath(s).parts)
        try:
            p = pathlib.Path(s)
            if p.exists() or p.is_symlink():
                parts |= set(p.resolve().parts)  # symlinks into sealed/ or a reserved directory are resolved
        except OSError:
            pass
        names = {seg.split("#", 1)[0].split("@", 1)[0] for seg in parts}
        if SEALED_DIRNAME in names or names & res:
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
    f.add_argument("--refreeze", action="store_true", help="replace an unused earlier freeze (recorded as superseded)")
    f.set_defaults(func=cmd_freeze)
    c = sub.add_parser("check")
    c.add_argument("--snapshot", required=True)
    c.set_defaults(func=cmd_check)
    a = ap.parse_args(argv)
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())
