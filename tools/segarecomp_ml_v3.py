#!/usr/bin/env python3
"""SEG-072 (ADR 0151): M68K ML v3 - expanded fixed corpus, dense ranking and adaptive budgets (OFFLINE research only).

Standard-library-only definitions of the frozen protocol: the fixed corpus manifest, lineage folds, label/mask rules, metrics,
budget policies (fixed and the single adaptive policy), candidate selection, final classification and the sealed-corpus
barrier. Training/evaluation code (`segarecomp_ml_v3_train.py`, `segarecomp_ml_v3_eval.py`) imports these definitions and
refuses to run when the committed plan (`segarecomp_ml_v3.plan.json` + `.plan.digest`) differs from them.

Nothing here changes production behaviour: production M3 (`gen2`), Optimized v1, the validator, the structural prune, the
launcher and the Sega CD admission policy are untouched. No commercial byte, address, label or per-cell value is ever written to
a committed file; private artifacts live under the ignored harness cache.

  plan        write the frozen plan + digest (refuses to overwrite)
  verify      check the committed plan against these constants
  seal-check  fail if any sealed truth artifact is reachable from a development/training root
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
PLAN_PATH = HERE / "segarecomp_ml_v3.plan.json"
DIGEST_PATH = HERE / "segarecomp_ml_v3.plan.digest"
FROZEN_PATH = HERE / "segarecomp_ml_v3.frozen.json"
PLAN_ID = "seg072-m68k-ml-v3"
PRIVATE_ROOT = pathlib.Path(os.environ.get("SEG072_ROOT", "/root/dev/segarecomp-harness/.cache/seg072"))
SEALED_DIRNAME = "sealed"  # PRIVATE_ROOT/sealed: the only place sealed truth may exist; development tooling refuses it

# ------------------------------------------------------------------------------------------------------------------- the corpus
# Fixed manifest (SEG-072 brief, sections 2-4). Identities only; the brief forbids additions, substitutions and reassignment.
DEVELOPMENT = [
    {"lineage": "D01", "name": "Sonic engine lineage", "members": [
        {"id": "s1", "title": "Sonic the Hedgehog (USA/Europe REV00)", "source": "sonicretro/s1disasm",
         "revision": "7ebe4b3d0c182b2566026b6d3f423d33539558da", "config": "Revision0",
         "image_sha256": "46160baa06362c711c9f1a5017cb7371026444936c8af5e93a78996cf32ff2a6", "truth": "reuse ADR 0099 (digest re-verified)"},
        {"id": "sk", "title": "Sonic & Knuckles (JUE standalone)", "source": "sonicretro/skdisasm",
         "revision": "044fa46725c71187399e13f5ddb70e11d32dc024", "config": "Sonic3_Complete=0",
         "image_sha256": "6e12e6b33c26ebfcd0be433251d21cf6284eafe9f71b027bda3767ae59affec1", "truth": "reuse ADR 0099 (digest re-verified)"},
        {"id": "scd", "title": "Sonic CD (USA) verified application modules", "source": "sonicretro/scddisasm",
         "revision": "73a6a358496116bc892c7c068189b0771f273c58", "config": "REGION=1",
         "image_sha256": "operator CUE/BIN (ADR 0140 identity)", "truth": "reuse ADR 0140: 36 admitted modules, 96,276 starts; module classes kept"},
        {"id": "s2", "title": "Sonic the Hedgehog 2 (Genesis)", "source": "sonicretro/s2disasm",
         "revision": "4ef457c18fc4902396c43ab06fabc119ac6ecb64", "config": "documented build.lua procedure",
         "image_sha256": "the operator image byte-identical to the build", "truth": "new: listing extraction"}]},
    {"lineage": "D02", "name": "Flicky", "members": [
        {"id": "flicky", "title": "Flicky (JUE)", "source": "oranguthang/flicky_src", "revision": "d9a0d4799c25fb23ba6251306508ee40a1ce9a89",
         "config": "default", "image_sha256": "4df1a91e08376ae773a6eb8e5abe310f94ca39d8f81583b91a9477fedcb68c11",
         "truth": "reuse ADR 0099 (digest re-verified)"}]},
    {"lineage": "D03", "name": "Shining Force lineage", "members": [
        {"id": "sf1", "title": "Shining Force (USA)", "source": "ShiningForceCentral/SF1DISASM", "revision": "1f466bf038a64f6bdf3113e484bace0c3c66c6f7",
         "config": "default", "image_sha256": "6c43b41b60ae8147945b4420caeb5eb266e1dd57a0d125b7496fa8fb6fde522d",
         "truth": "reuse ADR 0099 (rebuild + digest re-verified)"},
        {"id": "sf2", "title": "Shining Force II (USA)", "source": "ShiningForceCentral/SF2DISASM", "revision": "77e5d6432444cef06e703db4791d6f63fc9a63cd",
         "config": "historical master original-ROM split/assembly", "image_sha256": "the operator image byte-identical to the build",
         "truth": "new: listing extraction"}]},
    {"lineage": "D04", "name": "Phantasy Star II", "members": [
        {"id": "ps2", "title": "Phantasy Star II (USA/Europe REV02)", "source": "wyatt8740/ps2disasm", "revision": "c23877b8aa090473562ae2ed571ace2869289167",
         "config": "rev2", "image_sha256": "a0bd97f5aaa67301923ccc367511606bd3471ea9542919165311d4a23a2b696b",
         "truth": "reuse ADR 0099 (rebuild + digest re-verified)"}]},
] + [
    {"lineage": lid, "name": name, "members": [{"id": lid.lower(), "title": name, "source": repo, "revision": rev,
                                                  "config": cfg, "image_sha256": "reproducible project build", "truth": "new: SGDK LTO listing"}]}
    for lid, name, repo, rev, cfg in (
        ("D05", "Genesis 2048", "atamurad/sega-2048", "931008f890292afec936699821ed0d5de705b2df", "SGDK"),
        ("D06", "TetrisMD", "NeroJin/TetrisMD", "6169fb4af2c1c5fb27ea59b5a98997a022b34843", "SGDK"),
        ("D07", "240p Test Suite (Genesis)", "ArtemioUrbina/240pTestSuite", "89adfdd66853746fa9db9105104314ac58021791", "Genesis version"),
        ("D08", "Mega Car Wars", "realbrucest/Mega-Car-Wars", "f6238c55d974bbecc25e5e05da355884265642ba", "SGDK"),
        ("D09", "MegaPong", "And-0/MegaPong", "1064468a42553fb851abe536c314457269a762fa", "SGDK"),
        ("D10", "Genesis Megatetris", "danisc23/genesis-megatetris", "7895531e68cbc46169d3b33379856c7023790039", "SGDK"),
        ("D11", "Penguin World", "alicesim1/Penguin-World", "8fd19587eb8513611a98059bf6914c69a9c91e83", "SGDK"),
        ("D12", "MegaDriving (streeter)", "radioation/MegaDriving", "2e8c98862d8c1282e890786e7117152d1ebe072a", "streeter program only"))
]
SEALED = [
    {"lineage": "E01", "id": "e01", "title": "Alien Soldier (original Japanese)", "source": "RetrolariumLab/alien_soldier_src",
     "revision": "4f3d7e852aed6edd7a869afe6c252813c4acf316", "image_sha1": "8f6eb584ed9487b8504fbc21d86783f58e6c9cd6",
     "disclosure": "historical ROM-only experiments used a US/EU image of this game (ADR 0099); not entirely unseen"},
    {"lineage": "E02", "id": "e02", "title": "Landstalker (original USA)", "source": "lordmir/landstalker_disasm",
     "revision": "fb8a9c6c64ec27c05acea75f9556c928d2b9f081", "config": "US",
     "disclosure": "historical ROM-only experiments used the local image; exact US rebuild match required (header-byte variant not qualified)"},
    {"lineage": "E03", "id": "e03", "title": "Kid Chameleon", "source": "sonicretro/kid-chameleon-disasm",
     "revision": "5856daef633cd7162acdf9ddf1a56fa81aedc65b", "config": "bit-perfect build"},
    {"lineage": "E04", "id": "e04", "title": "QuackShot", "source": "lab313ru/quackshot_src",
     "revision": "90f7be9961c78ca4df2deede0790789aea944f6e"},
    {"lineage": "E05", "id": "e05", "title": "Sonic Spinball", "source": "sonicretro/spindisasm", "revision": "d835779e55a36fa204edfa34c1a332d9e3836f6c",
     "disclosure": "Sonic-related authorship/ecosystem overlap; IDA-derived disassembly (FIXME inline-hex rows are not authoritative)"},
    {"lineage": "E06", "id": "e06", "title": "Tank Battle (genesis/tank-battle only)", "source": "monteslu/retro-homebrew-games",
     "revision": "eb7dc3044f9d2f76047f3b3573971bf86575ca52", "disclosure": "SGDK/GCC ecosystem shared with development homebrew"},
    {"lineage": "E07", "id": "e07", "title": "The Weave", "source": "Ganso/The-Weave", "revision": "0bdba1e0ea3d7a2e2166f1dde9636a72722c333a",
     "disclosure": "SGDK/GCC ecosystem shared with development homebrew"},
]
DIAGNOSTIC = {"id": "tmss", "title": "Mega Drive/Genesis TMSS boot ROM", "source": "OrionNavattan/TMSS-Disassembly",
              "revision": "6bef50130248f57c5eaa88b32507a52cfcd4b62f", "role": "diagnostic only: never training, never selection, never counted",
              "unavailable_class": "TMSS_DIAGNOSTIC_UNAVAILABLE"}
MIN_DEVELOPMENT_LINEAGES = 12
MIN_SEALED_LINEAGES = 7
FROZEN_M3_TRAINING_IDS = ("s1", "flicky", "sf1", "ps2", "sk")  # exposed development material, never fresh blind evidence

# --------------------------------------------------------------------------------------------------------------- representation
CELL_BYTES = 128              # production gen2 decision cell (all budgets and the frozen-M3/M3-R/M1 unit)
M4_CELL_BYTES = (128, 64)     # the pre-registered M4-Dense decision-cell set (aggregation of dense word logits only)
MAX_RETENTION = 0.60          # pre-registered maximum retention budget
FIXED_BUDGETS = {"B35": 35, "B45": 45, "B50": 50, "B60": 60}
ADAPTIVE = {
    "id": "ADAPT",
    "inputs": "cell probabilities of the candidate (label-free), seed cells, decision-cell count; nothing else",
    "forbidden_inputs": ["source truth", "game identity", "lineage identity", "held-out instruction positions", "runtime traces", "ROM digest", "address"],
    "density_estimate": "e = |{cells: probability >= 0.5}| / cells (the model's own code-density estimate at its balanced decision boundary)",
    "margin_fit": "m = max over FITTING images i of (residual_frf_i - e_i) + 0.05, floored at 0; residual_frf_i = smallest fraction of the image's "
                  "cells, taken in rank order (seeds excluded from the count), that together with the seed cells holds every labelled start",
    "budget": "b = ceil_to_0.01(e + m); N = ceil(cells * b) ranked cells UNION seed cells; retained = |R| / cells",
    "abstention": "if b > MAX_RETENTION (0.60): broad AOT for that image (retained = 1.0, recall complete by construction)",
    "fitting_images": "development: the out-of-fold records of the OTHER development lineages (nested by lineage; no inner refits - "
                      "their fold models included the held-out lineage in training, a disclosed limitation); sealed: all development OOF records",
}
PROB_THRESHOLD_DENSITY = 0.5
ADAPTIVE_SAFETY = 0.05

# ----------------------------------------------------------------------------------------------------------------- labels
LABELS = {
    "classes": {"I": "MC68000 instruction-start byte of a source-backed instruction row", "c": "continuation byte of that instruction (decoder length)",
                "D": "known data (data rows, project resources, padding explained by the build)",
                "U": "unknown/masked: shared SDK/runtime/libgcc/startup code and data, vectors/header of SDK startup, unverifiable or ambiguous bytes"},
    "cell": "positive iff it holds >= 1 I; negative iff every byte is D; otherwise masked (excluded from loss and from FRF positives/negatives)",
    "dense_word": "M4-Dense word label: 1 if the word holds an I or c byte, 0 if both bytes are D, masked otherwise",
    "recall_scope": "only I starts are scored for recall (confidently labelled starts); U never counts as a miss or a negative",
    "historical_titles": "s1/flicky/sf1/ps2/sk: every byte is explained by the listing, so their cell labels equal the frozen gen2 labels",
    "sgdk": "LTO link-time assembly kept with -save-temps and re-assembled with listings; sections placed with the linker map; app functions/objects = "
            "I/c/D; SGDK (libmd, sega.s, rom_head), libgcc and functions whose origin is ambiguous after LTO = U; app resources (rescomp) = D; "
            "instrumented build must be byte-identical to the plain build and two clean builds byte-identical",
    "disassembly": "tools/segarecomp_source_map_extract.py listing semantics (ADR 0099 adapters; fails closed); non-instruction explained bytes = D",
    "ida_derived": "E05: FIXME/inline-hex rows and non-instruction bytes = U (only instruction rows are confident labels)",
}

# --------------------------------------------------------------------------------------------------------------- independence
INDEPENDENCE = {
    "development_app_code": "pairwise containment of 12-byte shingles of labelled app-code instruction runs (I/c bytes); >= 0.30 in either direction "
                            "= substantially shared application code -> the two projects count as ONE lineage (merged fold), reported",
    "sealed_vs_development": "raw-byte 12-byte shingle containment over whole images excluding uniform fill runs (no sealed labels used); >= 0.30 = "
                             "closely related binary -> reported as correlated (no replacement)",
    "sonic_family": "all Sonic-family development titles share fold D01 (similar routines never cross a train/test boundary)",
}

# ------------------------------------------------------------------------------------------------------------------- candidates
CANDIDATES = {
    "M3_FROZEN": {"role": "baseline (no change)", "what": "production gen2 M3 weights + representation + token contract, unchanged",
                  "predictions": "historical title-held-out outer folds for s1/flicky/sf1/ps2/sk (reproduced, digest-verified); the production model "
                                 "(trained on those five) for every other image (zero-shot for D05-D12; lineage-correlated for s2/scd (D01) and sf2 (D03), flagged)"},
    "M3_R": {"role": "isolate data", "what": "gen2 M3 architecture, representation and recipe unchanged (seed 49, 30 epochs, AdamW, cosine), trained on the "
                                             "training lineages' images; masked cells carry zero loss weight; pos_weight over labelled cells",
             "complexity": 2},
    "M4_DENSE": {"role": "M68K byte-dense", "complexity": 3,
                 "what": "dense per-word occupancy CNN: per-word input = embed(hi byte,8) + embed(lo byte,8) + the gen2 MC68000 decode-token embeddings "
                         "(CPU-owned tokens, same dims as M3) with a distinct PAD index for every channel outside the image; local path 2x Conv1d k5 "
                         "(32 ch) at word resolution; context path Conv1d k4 stride 4 (32 ch) + residual dilated Conv1d k5 dilations 1,2,4,8,16,32 "
                         "(radius 504 words = 1008 bytes); fuse 1x1 (64->32) + ReLU + 1x1 head -> per-word logit; cell score = max word logit over the "
                         "cell; cells {128, 64} bytes evaluated as aggregation only (the same networks)",
                 "training": "margin 512 words of real context or PAD on each side, central 2048 words, batch 8, 4000 steps, AdamW lr 0.002 (x0.1 after 80%), "
                             "weight decay 1e-4, masked BCE with pos_weight = mean over training lineages of negative/positive labelled words; sampling "
                             "uniform over training lineages, then images by size within the lineage, window starts multiple of 16 words including "
                             "image edges; seeds 1, 2, 3; ensemble = mean of the three word logits; no hard-positive emphasis"},
    "M1_HGB": {"role": "simpler control", "complexity": 1,
               "what": "gen2 M1: HistGradientBoostingClassifier(max_iter=200, max_depth=4, learning_rate=0.08, min_samples_leaf=40, l2_regularization=1.0, "
                       "max_bins=255, early_stopping=False, class_weight=balanced, random_state=49) over the 219 gen2 multiscale aggregates; masked cells excluded"},
}
COMPLEXITY_ORDER = ["M3_FROZEN", "M1_HGB", "M3_R", "M4_DENSE@128", "M4_DENSE@64"]

# ------------------------------------------------------------------------------------------------------------------ evaluation
FOLDS = "leave-one-lineage-out over the qualified development lineages (all images of the held-out lineage leave together; merged lineages per INDEPENDENCE)"
METRICS = {
    "per_image": ["labelled starts", "positive/negative/masked cells", "FRF (rank order, ties with the weakest positive counted)",
                  "residual FRF (seeds excluded)", "per policy: retained fraction (seeds + ceil included), C outside R, C outside ML-only, abstained",
                  "Genesis cartridge images: K/U and C outside K from the unchanged prune + validator (structural closure)"],
    "aggregates": {"worst_lineage_frf": "max over lineages of the max FRF over the lineage's images",
                   "mean_retained": "mean over lineages of the mean retained fraction over the lineage's images",
                   "complete": "C outside R = 0 on every image of every development lineage"},
    "secondary": ["AUROC", "precision"],
}
SELECTION = {
    "pairs": "every candidate configuration in COMPLEXITY_ORDER x every policy in {B35, B45, B50, B60, ADAPT}",
    "order": ["1 complete development recall (filter)", "2 lowest worst-lineage FRF of the configuration (tie set within 0.02)",
              "3 lowest mean retained fraction (tie set within 0.02)", "4 lowest mean K/U over Genesis development images (tie set within 0.02)",
              "5 simplest configuration (COMPLEXITY_ORDER), then fixed before adaptive, then the smaller fixed budget"],
    "incumbent": "M3_FROZEN+B35 is retained unless the best alternative pair is complete AND (M3_FROZEN+B35 is incomplete OR the alternative lowers mean "
                 "retained by >= 0.05 absolute without raising worst-lineage FRF by more than 0.02)",
    "no_complete_pair": "if no pair is complete, freeze the pair with the fewest development misses (then lowest mean retained) for descriptive sealed evidence; "
                        "the outcome is RECALL_FAILED",
    "m4_internal": "M4_DENSE@128 and @64 are configurations of one candidate; both obey the same rule (disclosed: two aggregations of one network)",
    "frozen_before_sealed": "exactly one configuration and one policy, plus the training backend/environment, are recorded in segarecomp_ml_v3.frozen.json "
                            "and committed before the sealed truth is opened",
}
FINAL_FIT = "the frozen configuration refit once on ALL qualified development lineages (same recipe/seeds); M3_FROZEN uses the production weights unchanged; " \
            "an adaptive policy's margin is fitted on all development OOF records of that configuration"
SEALED_RULES = {
    "binding": "before any training: source commit, build inputs, image digest and an opaque truth digest per qualified entry in the sealed manifest; "
               "truth artifacts only under PRIVATE_ROOT/sealed; development tooling refuses that directory and every sealed id",
    "unseal": "segarecomp_ml_v3_eval.py sealed: requires segarecomp_ml_v3.frozen.json committed in an ancestor of HEAD and byte-identical, verifies every "
              "sealed truth digest, writes a one-shot marker; a second run refuses",
    "gate": "zero-miss: any confidently labelled start of a qualified sealed lineage outside R = failure; no refit, recalibration or widening",
    "retention": "retained fraction reported per lineage (no separate selectivity pass/fail; MAX_RETENTION bounds every policy)",
    "runtime": "runnable Genesis images: broad vs frozen pair, identical inputs, explicit instruction budget and wall timeout; runtime-equivalent short "
               "traces are never evidence of static recall",
}
ECONOMICS = {
    "platforms": ["Genesis immutable ROMs (general bridge, explicit admission plan)", "Sonic CD verified application modules (R-only; no Sega CD "
                  "R-admission path exists - ADR 0131/0133 - and none is built)", "broad materialized M68K images only where an existing safe admission "
                  "contract permits (none admits partial ML proposals today -> reported as not comparable)"],
    "measures": ["selected code cells", "emitted instruction functions", "generated C bytes", "compiler CPU and wall", "peak compiler RSS",
                 "final binary size", "ML inference CPU", "net benefit after inference", "cache-reuse note"],
    "gate_vs_broad": "generated C reduction >= 30% and compile CPU reduction >= 25% (mean over sealed Genesis images), net of inference",
    "gate_vs_incumbent": "an alternative must reduce generated C by >= 10% and compile CPU by >= 10% versus M3_FROZEN+B35 on the same images, OR be a "
                         "reliability improvement (incumbent incomplete where it is complete) with generated C <= +10% versus the incumbent",
}
OUTCOMES = ["V3_READY_FOR_OPT_IN", "M3_RETRAIN_SUFFICIENT", "ADAPTIVE_ONLY_JUSTIFIED", "RECALL_FAILED", "ECONOMICS_NOT_JUSTIFIED", "CORPUS_INSUFFICIENT"]
CLASSIFICATION = [
    "1 RECALL_FAILED: no complete development pair, or the frozen pair misses >= 1 confidently labelled start on any qualified sealed lineage",
    "2 CORPUS_INSUFFICIENT: fewer than 12 qualified development or 7 qualified sealed lineages (qualified-entry results reported separately; no promotion)",
    "3 ECONOMICS_NOT_JUSTIFIED: the incumbent M3_FROZEN+B35 is retained, or the frozen alternative fails an economics gate",
    "4 ADAPTIVE_ONLY_JUSTIFIED: frozen pair = M3_FROZEN + ADAPT",
    "5 M3_RETRAIN_SUFFICIENT: frozen pair uses M3_R",
    "6 V3_READY_FOR_OPT_IN: frozen pair uses M4_DENSE or M1_HGB",
    "positive outcomes (4-6) additionally require the independent adversarial review's concurrence; otherwise the conservative class stands",
]
HARDWARE = {
    "default": "CPU",
    "benchmark": "before M4-Dense training: verify the available ROCm/PyTorch device without driver changes; final M4-Dense architecture and batch; "
                 "representative qualified development data; warm-up then one representative epoch (= 500 steps) on CPU (2 threads) and ROCm, repeated 3x; "
                 "epoch time, data-loading share, throughput, peak RSS/VRAM; estimated total cross-validation time for the actual parallel-CPU farm "
                 "(8 concurrent 2-thread fits) versus ROCm; ROCm same-seed repeat reproducibility of the trained outputs",
    "rule": "ROCm only if >= 25% estimated end-to-end improvement over the parallel CPU farm AND reproducible; else parallel CPU; unavailable/unstable ROCm "
            "falls back explicitly; no kernel optimisation, no architecture change for GPU speed; one backend for every like-for-like comparison",
}
ENVIRONMENT_RULE = "Python/numpy/scikit-learn/torch versions, device, precision (float32), threads and deterministic-algorithm settings are recorded in " \
                   "the frozen record before the sealed truth is opened"


def build_plan() -> dict:
    return {
        "schema": "segarecomp.ml_v3_plan.v1", "plan_id": PLAN_ID, "experiment": "SEG-072", "adr": "0151",
        "status": "FROZEN BEFORE ANY NEW TRUTH EXTRACTION, MODEL FIT OR SEALED QUALIFICATION",
        "scope": "OFFLINE research; production gen2 M3, Optimized v1, validator, prune, launcher defaults and Sega CD admission policy unchanged",
        "corpus": {"development": DEVELOPMENT, "sealed": SEALED, "diagnostic": DIAGNOSTIC, "min_development_lineages": MIN_DEVELOPMENT_LINEAGES,
                   "min_sealed_lineages": MIN_SEALED_LINEAGES, "frozen_m3_training_ids": list(FROZEN_M3_TRAINING_IDS),
                   "rule": "fixed manifest; qualification failures are recorded and never shrink the denominator; no replacements"},
        "labels": LABELS, "independence": INDEPENDENCE,
        "representation": {"cell_bytes": CELL_BYTES, "m4_cell_bytes": list(M4_CELL_BYTES), "token_schema": "segarecomp.m68k_decode_tokens.v1 (gen2, unchanged)"},
        "budgets": {"fixed": FIXED_BUDGETS, "max_retention": MAX_RETENTION, "adaptive": ADAPTIVE,
                    "accounting": "N = ceil(cells * percent / 100) ranked cells (score desc, cell ordinal asc) UNION seed cells; retained = |R| / cells; "
                                  "post-validator K/U reported separately (structural closure)",
                    "seeds": "Genesis: production direct-control-discovery seed cells; Sonic CD modules: ADR 0140 seed definition"},
        "candidates": CANDIDATES, "complexity_order": COMPLEXITY_ORDER, "folds": FOLDS, "metrics": METRICS, "selection": SELECTION,
        "final_fit": FINAL_FIT, "sealed": SEALED_RULES, "economics": ECONOMICS, "outcomes": OUTCOMES, "classification": CLASSIFICATION,
        "hardware": HARDWARE, "environment_rule": ENVIRONMENT_RULE,
        "order": ["C1 this plan", "T001 development qualification + truth", "T002 sealed/TMSS qualification + binding", "T003 frozen M3 reproduction + baseline",
                  "T004 adaptive vs fixed on frozen M3", "T005 hardware benchmark, M3-R/M4-Dense/M1-HGB, freeze", "T006 one-shot sealed + economics",
                  "T007 independent review + decision"],
    }


# ----------------------------------------------------------------------------------------------------------------- canonical I/O
def canonical_json(obj) -> str:
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def plan_digest(plan: dict) -> str:
    return sha256_bytes(canonical_json(plan).encode())


def verify_plan() -> dict:
    committed = json.loads(PLAN_PATH.read_text())
    digest = DIGEST_PATH.read_text().strip()
    if plan_digest(committed) != digest:
        raise SystemExit("plan digest mismatch: the frozen plan file was modified")
    if committed != build_plan():
        raise SystemExit("plan drift: protocol constants differ from the frozen plan")
    return committed


# --------------------------------------------------------------------------------------------------------------------- lineages
def development_lineages() -> list[str]:
    return [d["lineage"] for d in DEVELOPMENT]


def sealed_ids() -> set[str]:
    return {e["id"] for e in SEALED} | {e["lineage"] for e in SEALED}


def lineage_of(image_id: str) -> str:
    for d in DEVELOPMENT:
        for m in d["members"]:
            if image_id == m["id"] or image_id.startswith(m["id"] + "/"):
                return d["lineage"]
    raise SystemExit(f"unknown development image {image_id!r}")


def lolo_folds(qualified: list[str], merges: list[list[str]] = ()) -> list[tuple[str, list[str]]]:
    """[(held-out fold id, training lineages)]; merged lineages leave together and count once."""
    groups: list[list[str]] = []
    seen: set[str] = set()
    for merge in merges:
        g = sorted(set(merge) & set(qualified))
        if g:
            groups.append(g)
            seen |= set(g)
    groups += [[l] for l in qualified if l not in seen]
    out = []
    for g in groups:
        fold = "+".join(g)
        out.append((fold, sorted(l for l in qualified if l not in g)))
    return out


def assert_no_sealed(ids) -> None:
    bad = sorted(set(ids) & sealed_ids())
    if bad:
        raise SystemExit(f"sealed barrier: sealed identity in a development/training set: {bad}")


def refuse_sealed_path(path) -> None:
    p = pathlib.Path(path).resolve()
    sealed = (PRIVATE_ROOT / SEALED_DIRNAME).resolve()
    if p == sealed or sealed in p.parents:
        raise SystemExit("sealed barrier: development tooling may not read the sealed store")


# ------------------------------------------------------------------------------------------------------------------- cells/labels
def cell_count(size: int, cell: int = CELL_BYTES) -> int:
    if size <= 0:
        raise SystemExit("invalid image size")
    return (size + cell - 1) // cell


def cell_labels(classes: bytes, cell: int = CELL_BYTES) -> list[int]:
    """Per-cell label from a per-byte class string (b'I', b'c', b'D', b'U'): 1 positive, 0 negative, -1 masked (LABELS['cell'])."""
    out = []
    for c in range(cell_count(len(classes), cell)):
        chunk = classes[c * cell:(c + 1) * cell]
        if b"I" in chunk:
            out.append(1)
        elif chunk.count(b"D") == len(chunk):
            out.append(0)
        else:
            out.append(-1)
    return out


def word_labels(classes: bytes) -> list[int]:
    """M4-Dense per-word label: 1 if either byte is I/c, 0 if both are D, -1 otherwise."""
    out = []
    for w in range(0, len(classes) - 1, 2):
        pair = classes[w:w + 2]
        if pair[0] in b"Ic" or pair[1] in b"Ic":
            out.append(1)
        elif pair == b"DD":
            out.append(0)
        else:
            out.append(-1)
    return out


def starts_of(classes: bytes) -> list[int]:
    return [i for i, b in enumerate(classes) if b == ord("I")]


# ------------------------------------------------------------------------------------------------------------------------ metrics
def rank_order(scores) -> list[int]:
    scores = list(scores)
    for s in scores:
        if isinstance(s, bool) or not isinstance(s, (int, float)) or math.isnan(s):
            raise SystemExit("rank: score is not a number")
    return sorted(range(len(scores)), key=lambda c: (-scores[c], c))


def frf(scores, labels) -> dict:
    """Full-recall fraction over all cells (masked cells occupy rank positions but are never positives). Ties with the weakest positive counted."""
    pos = [s for s, y in zip(scores, labels) if y == 1]
    if not pos:
        return {"frf": 0.0, "cells_needed": 0, "cells": len(scores), "positive_cells": 0}
    worst = min(pos)
    needed = sum(1 for s in scores if s >= worst)
    return {"frf": needed / len(scores), "cells_needed": needed, "cells": len(scores), "positive_cells": len(pos)}


def residual_frf(scores, labels, seeds) -> float:
    """Smallest fraction of cells in rank order (seed cells not counted) that, with the seeds, holds every positive cell."""
    seeds = set(seeds)
    rest = [c for c, y in enumerate(labels) if y == 1 and c not in seeds]
    if not rest:
        return 0.0
    order = rank_order(scores)
    need = set(rest)
    for k, c in enumerate(order, 1):
        need.discard(c)
        if not need:
            return k / len(scores)
    return 1.0


def budget_cell_count(cells: int, percent: int) -> int:
    if cells <= 0 or not 0 < percent <= 100:
        raise SystemExit("invalid budget")
    return (cells * percent + 99) // 100


def ceil_to(x: float, step: float) -> float:
    k = math.ceil(round(x / step, 9))
    return round(k * step, 10)


def density_estimate(probabilities) -> float:
    p = list(probabilities)
    return sum(1 for v in p if v >= PROB_THRESHOLD_DENSITY) / len(p)


def fit_adaptive_margin(records) -> float:
    """records: iterable of (residual_frf, density_estimate) of FITTING images only."""
    recs = list(records)
    if not recs:
        raise SystemExit("adaptive: no fitting records")
    return max(0.0, max(r - e for r, e in recs) + ADAPTIVE_SAFETY)


def adaptive_budget(probabilities, margin: float) -> tuple[float, bool]:
    """(budget fraction, abstained). Label-free: probabilities and the fitted margin only."""
    b = ceil_to(density_estimate(probabilities) + margin, 0.01)
    return (1.0, True) if b > MAX_RETENTION + 1e-12 else (b, False)


def select_region(scores, seeds, fraction: float) -> set[int]:
    order = rank_order(scores)
    n = len(order)
    k = min(n, math.ceil(round(n * fraction, 9)))
    return set(order[:k]) | set(seeds)


def policy_region(policy: str, scores, probabilities, seeds, margin: float | None = None) -> dict:
    n = len(scores)
    if policy in FIXED_BUDGETS:
        k = budget_cell_count(n, FIXED_BUDGETS[policy])
        region = set(rank_order(scores)[:k]) | set(seeds)
        return {"region": region, "retained": len(region) / n, "abstained": False, "budget": FIXED_BUDGETS[policy] / 100}
    if policy == "ADAPT":
        b, abst = adaptive_budget(probabilities, margin)
        if abst:
            return {"region": set(range(n)), "retained": 1.0, "abstained": True, "budget": 1.0}
        region = select_region(scores, seeds, b)
        return {"region": region, "retained": len(region) / n, "abstained": False, "budget": b}
    raise SystemExit(f"unknown policy {policy}")


def misses(starts, region, cell: int = CELL_BYTES) -> int:
    return sum(1 for a in starts if a // cell not in region)


# ---------------------------------------------------------------------------------------------------------------------- selection
def select_pair(table: dict) -> dict:
    """table[(config, policy)] = {'complete': bool, 'worst_frf': float, 'mean_retained': float, 'mean_ku': float|None, 'misses': int}.
    Returns {'pair': (config, policy), 'rule': ..., 'incumbent_retained': bool}."""
    pol_order = ["B35", "B45", "B50", "B60", "ADAPT"]

    def simple_key(pair):
        return (COMPLEXITY_ORDER.index(pair[0]), pol_order.index(pair[1]))

    complete = [p for p, r in table.items() if r["complete"]]
    if not complete:
        best = min(table, key=lambda p: (table[p]["misses"], table[p]["mean_retained"], simple_key(p)))
        return {"pair": best, "rule": "no_complete_pair", "incumbent_retained": False}
    pool = complete
    for key in ("worst_frf", "mean_retained", "mean_ku"):
        vals = [table[p][key] for p in pool if table[p].get(key) is not None]
        if not vals:
            continue
        lo = min(vals)
        pool = [p for p in pool if table[p].get(key) is None or table[p][key] <= lo + 0.02 + 1e-12]
    best = min(pool, key=simple_key)
    inc = ("M3_FROZEN", "B35")
    if best != inc and inc in table:
        i, b = table[inc], table[best]
        better = (not i["complete"]) or (b["mean_retained"] <= i["mean_retained"] - 0.05 + 1e-12 and b["worst_frf"] <= i["worst_frf"] + 0.02 + 1e-12)
        if not better and i["complete"]:
            return {"pair": inc, "rule": "incumbent_retained", "incumbent_retained": True}
    return {"pair": best, "rule": "selection_order", "incumbent_retained": best == inc}


def classify(*, dev_complete_exists: bool, sealed_misses: int, dev_qualified: int, sealed_qualified: int, pair: tuple,
             incumbent_retained: bool, economics_ok: bool, review_concurs: bool) -> str:
    if not dev_complete_exists or sealed_misses > 0:
        return "RECALL_FAILED"
    if dev_qualified < MIN_DEVELOPMENT_LINEAGES or sealed_qualified < MIN_SEALED_LINEAGES:
        return "CORPUS_INSUFFICIENT"
    if incumbent_retained or not economics_ok:
        return "ECONOMICS_NOT_JUSTIFIED"
    config, policy = pair
    if config == "M3_FROZEN" and policy == "ADAPT":
        positive = "ADAPTIVE_ONLY_JUSTIFIED"
    elif config == "M3_R":
        positive = "M3_RETRAIN_SUFFICIENT"
    else:
        positive = "V3_READY_FOR_OPT_IN"
    return positive if review_concurs else "ECONOMICS_NOT_JUSTIFIED"


# ------------------------------------------------------------------------------------------------------------------- sealed barrier
def seal_check(roots) -> list[str]:
    """Paths under development roots that look like sealed truth (a file naming a sealed id). Empty list = intact."""
    hits = []
    pat = re.compile(r"(^|[^a-z0-9])(e0[1-7])([^0-9]|$)")
    sealed = (PRIVATE_ROOT / SEALED_DIRNAME).resolve()
    for root in roots:
        root = pathlib.Path(root)
        if not root.exists():
            continue
        for p in root.rglob("*"):
            rp = p.resolve()
            if sealed == rp or sealed in rp.parents:
                continue
            if p.is_file() and "truth" in str(p).lower() and pat.search(p.name.lower()):
                hits.append(str(p))
    return hits


def verify_frozen_committed(frozen_path=FROZEN_PATH) -> dict:
    """The frozen record must exist, be committed (HEAD byte-identical) and name exactly one configuration and policy."""
    if not frozen_path.exists():
        raise SystemExit("no frozen record: the sealed corpus cannot be opened")
    data = frozen_path.read_bytes()
    shown = subprocess.run(["git", "-C", str(HERE), "show", f"HEAD:tools/{frozen_path.name}"], capture_output=True)
    if shown.returncode != 0 or shown.stdout != data:
        raise SystemExit("frozen record is not committed byte-identically at HEAD")
    rec = json.loads(data)
    for key in ("configuration", "policy", "environment", "plan_sha256", "status"):
        if key not in rec:
            raise SystemExit(f"frozen record lacks {key}")
    if rec["status"] != "FROZEN" or rec["plan_sha256"] != DIGEST_PATH.read_text().strip():
        raise SystemExit("frozen record status/plan mismatch")
    return rec


def unseal_once(marker_path, frozen_path=FROZEN_PATH, verify=verify_frozen_committed) -> dict:
    """The one-shot sealed gate: the committed frozen record must verify and no previous unseal marker may exist; the marker is written
    BEFORE any sealed truth is read, so an interrupted run can never be repeated silently."""
    marker = pathlib.Path(marker_path)
    if marker.exists():
        raise SystemExit("sealed corpus already opened once (marker present): refusing a second evaluation")
    rec = verify(frozen_path)
    marker.parent.mkdir(parents=True, exist_ok=True)
    marker.write_text(json.dumps({"frozen_record_sha256": sha256_bytes(pathlib.Path(frozen_path).read_bytes()),
                                  "configuration": rec["configuration"], "policy": rec["policy"]}, sort_keys=True))
    return rec


# --------------------------------------------------------------------------------------------------------------------------- CLI
def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("plan")
    p.add_argument("--force", action="store_true")
    sub.add_parser("verify")
    s = sub.add_parser("seal-check")
    s.add_argument("roots", nargs="*")
    a = ap.parse_args()
    if a.cmd == "plan":
        if PLAN_PATH.exists() and not a.force:
            raise SystemExit("plan already frozen (refusing to overwrite)")
        plan = build_plan()
        PLAN_PATH.write_text(json.dumps(plan, indent=1, sort_keys=True) + "\n")
        DIGEST_PATH.write_text(plan_digest(plan) + "\n")
        print(plan_digest(plan))
    elif a.cmd == "verify":
        verify_plan()
        print("plan OK", DIGEST_PATH.read_text().strip())
    else:
        roots = a.roots or [str(PRIVATE_ROOT / d) for d in ("truth", "scores", "work")]
        hits = seal_check(roots)
        print("INTACT" if not hits else f"VIOLATED {len(hits)}")
        return 1 if hits else 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
