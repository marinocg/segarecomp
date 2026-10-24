#!/usr/bin/env python3
"""SEG-084-T001: true SEG-076 structural model-A reassessment against the SEG-081 / SEG-083 TAS labels (OFFLINE research tooling).

Model A here is `segarecomp_xplat_models.a_features` + `A_CONFIG` (HistGradientBoosting over per-cell structural aggregates, token reports, relocation
and seed flags), NOT the SEG-072 M1-HGB used by the SEG-083 diagnostic. The frozen SEG-075/SEG-076 modules are imported UNCHANGED. The only addition is
the explicitly versioned optional sample-weight path `seg084.a_weighted.v1` (`fit_arrays(..., weights)`), which is NOT used for strong-only arms; with
`weights=None` it is byte-identical to `segarecomp_xplat_models.fit_a` (tested; and verified against the stored SEG-076 scores).

  prepare   regenerate the label-free seed addresses of each TAS ROM with the pinned CLI (the SEG-072 `d.txt` procedure), check image / lineage / code-shingle
            isolation, and write the private context
  run       one fold: fit every pre-registered arm (write-once cell scores + CPU cost) for a held-out SEG-072 Genesis lineage
  table     compute every metric (SEG-076 evaluation code, unchanged) from the saved scores -> aggregate-only results JSON
  freeze / verify-plan   the digest-bound protocol

Training inputs are RESTORED samples (private-corpus snapshots -> `corpus_snapshot.py restore`); the evaluation targets are the source-backed SEG-072
Genesis development lineages only (leave-one-lineage-out, the SEG-076 C0 folds). No sealed/reserved path is read; no B/C model is trained.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

SCHEMA = "segarecomp.seg084.a.v1"
PLAN = HERE / "segarecomp_seg084_a.plan.json"
PLAN_DIGEST = HERE / "segarecomp_seg084_a.plan.digest"
RESULTS = HERE / "segarecomp_seg084_a.results.json"
HARNESS = pathlib.Path(os.environ.get("SEGARECOMP_HARNESS_ROOT", "/root/dev/segarecomp-harness"))
GENESIS = ("D01", "D02", "D03", "D04", "D05", "D06", "D07", "D09", "D10", "D11", "D12")
WEIGHTED_VERSION = "seg084.a_weighted.v1"
CONTAINMENT_MAX = 0.30
NOISE_SEEDS = (84, 85, 86)
NOISE_FRACTION = 0.9
MATCH_SEED = 84

# games: key -> (snapshot label, lineage, qualified class). Sonic 2 (SEG-080) is excluded: the same image is the registry's D01 member.
GAMES = {
    "vm2": ("seg081-v1", "T81_VM2", "QUALIFIED_USEFUL"),
    "bnr": ("seg081-v1", "T81_BNR", "QUALIFIED_USEFUL"),
    "ewj2": ("seg081-v1", "T81_EWJ2", "QUALIFIED_USEFUL"),
    "shadowrun": ("seg083-v1", "T83_SHADOWRUN", "QUALIFIED_USEFUL"),
    "doraemon": ("seg083-v1", "T83_DORAEMON", "QUALIFIED_USEFUL"),
    "terminator": ("seg083-v1", "T83_TERMINATOR", "QUALIFIED_LOW_YIELD"),
}
G81, G83 = ["vm2", "bnr", "ewj2"], ["shadowrun", "doraemon", "terminator"]
ALL = G81 + G83
# arm -> list of (games, condition, variant, matched_to_control)
ARMS = {
    "A0": [],
    "A0_sub1": "sub", "A0_sub2": "sub", "A0_sub3": "sub",
    "A81": [(G81, "tas", "strong", False)],
    "A83": [(G83, "tas", "strong", False)],
    "A_all": [(ALL, "tas", "strong", False)],
    "A_control": [(ALL, "ctl", "strong", False)],
    "A_matched": [(ALL, "tas", "strong", True)],
    "A_all_W": [(ALL, "tas", "weak", False)],
    "A_control_W": [(ALL, "ctl", "weak", False)],
    "A_term": [(["terminator"], "tas", "strong", False)],
    "A_noterm": [(["vm2", "bnr", "ewj2", "shadowrun", "doraemon"], "tas", "strong", False)],
}
WEIGHTED_ARMS = ("A_all_W", "A_control_W")
READY = {"diversity_code_min_independent_useful": 5, "diversity_code_min_new_positive_fraction": 0.20, "diversity_negative_min_lineages": 3,
         "diversity_negative_min_strong_hard_cells": 300, "response_min_folds": 2, "dose_response": "mean ER(A_all) < mean ER(A_matched)", "residual_er": 0.05, "residual_min_folds": 2,
         "required_comparisons": ["A0", "A81", "A83", "A_all", "A_control", "A_matched"]}


# ---------------------------------------------------------------------------------------------------------------- pure rules
def readiness(m: dict) -> dict:
    """Frozen readiness decision (pure). Code diversity and negative-supervision diversity are judged independently.

    m: missing_arms (list), a0_reproduces (bool), code_independent_useful (int), code_new_positive_fraction (float; strong positive cells of the USEFUL games only),
    negative_lineages (int: independent qualified lineages with >= 300 strong hard-negative cells, regardless of code class), response_folds (folds where A_all
    improves ER beyond that fold's noise), response_mean_beyond_noise (bool), dose_response (bool: A_all's mean ER is below A_matched's, the same labels at the control's
    volume, so the gain grows with the amount of labelled evidence rather than being noise), residual_folds (folds where A_all still has ER >= 0.05 and its gain over A0
    is within noise). TAS-versus-control content (A_all vs A_matched / A_control) is a reported descriptor, not a gate."""
    R = READY
    if m["missing_arms"] or not m["a0_reproduces"]:
        return {"decision": "EVIDENCE_INSUFFICIENT", "reasons": [f"missing arms {m['missing_arms']}" if m["missing_arms"] else "A0 does not reproduce the stored SEG-076 scores"],
                "recommendation": "improve_evaluation_or_repair_inputs_before_acquisition"}
    div = []
    if m["code_independent_useful"] < R["diversity_code_min_independent_useful"]:
        div.append(f"code: independent useful lineages {m['code_independent_useful']} < {R['diversity_code_min_independent_useful']}")
    if m["code_new_positive_fraction"] < R["diversity_code_min_new_positive_fraction"]:
        div.append(f"code: new strong positive cells are {m['code_new_positive_fraction']:.3f} of the base < {R['diversity_code_min_new_positive_fraction']}")
    if m["negative_lineages"] < R["diversity_negative_min_lineages"]:
        div.append(f"negatives: independent lineages with >= {R['diversity_negative_min_strong_hard_cells']} strong hard-negative cells {m['negative_lineages']} < {R['diversity_negative_min_lineages']}")
    if div:
        return {"decision": "MORE_DIVERSITY_NEEDED", "reasons": div, "recommendation": "acquire_targeted_batch"}
    responds = m["response_mean_beyond_noise"] and m["response_folds"] >= R["response_min_folds"] and m["dose_response"]
    if not responds:
        return {"decision": "MODEL_OR_EVALUATION_LIMITATION",
                "reasons": ["diversity criteria met, but the structural model A's ER does not respond to the added labels beyond the noise reference, or the response is not dose-dependent"],
                "recommendation": "improve_evaluation_or_source_backed_evidence_before_more_acquisition"}
    if m["residual_folds"] >= R["residual_min_folds"]:
        return {"decision": "NEURAL_PILOT_JUSTIFIED", "reasons": [], "recommendation": "run_bounded_b_c_v2_pilot_now"}
    return {"decision": "MODEL_OR_EVALUATION_LIMITATION",
            "reasons": [f"model A responds but only {m['residual_folds']} development folds retain ER >= {R['residual_er']}: no discriminating headroom left for a pilot"],
            "recommendation": "improve_evaluation_or_source_backed_evidence_before_more_acquisition"}


def fold_noise(er_a0: dict, er_sub: list) -> dict:
    """Per-fold noise reference: the largest |ER(A0 on a random 90 % of the cells) - ER(A0)| over the noise seeds (pure)."""
    out = {}
    for f, v in er_a0.items():
        subs = [s[f] for s in er_sub if s.get(f) is not None]
        out[f] = max((abs(x - v) for x in subs), default=None) if v is not None else None
    return out


def classify_response(er_a0: dict, er_arm: dict, noise: dict) -> dict:
    """Folds where an arm improves / worsens ER beyond the fold's noise, and the mean change against the mean noise (pure)."""
    better = [f for f in er_a0 if er_a0[f] is not None and er_arm.get(f) is not None and noise.get(f) is not None and er_arm[f] - er_a0[f] < -noise[f]]
    worse = [f for f in er_a0 if er_a0[f] is not None and er_arm.get(f) is not None and noise.get(f) is not None and er_arm[f] - er_a0[f] > noise[f]]
    fs = [f for f in er_a0 if er_a0[f] is not None and er_arm.get(f) is not None]
    mean_delta = sum(er_arm[f] - er_a0[f] for f in fs) / len(fs) if fs else None
    mnoise = sum(noise[f] for f in fs if noise.get(f) is not None) / max(1, sum(1 for f in fs if noise.get(f) is not None))
    return {"folds_better": sorted(better), "folds_worse": sorted(worse), "mean_delta_er": None if mean_delta is None else round(mean_delta, 6),
            "mean_noise": round(mnoise, 6), "mean_beyond_noise": bool(mean_delta is not None and mean_delta < -mnoise)}


def matched_counts(tas_labels, ctl_labels) -> dict:
    """Target positive / negative cell counts of the volume-matched arm: the control's (pure)."""
    return {1: sum(1 for x in ctl_labels if x == 1), 0: sum(1 for x in ctl_labels if x == 0)}


# ------------------------------------------------------------------------------------------------------------ model A fitting
def fit_arrays(X, y, weights=None):
    """The `segarecomp_xplat_models.fit_a` fit on pre-assembled arrays. `weights=None` is byte-identical to fit_a; weights (seg084.a_weighted.v1) is the
    only addition and is used by the explicitly weighted arms only."""
    import segarecomp_xplat_models as xm
    from sklearn.ensemble import HistGradientBoostingClassifier
    clf = HistGradientBoostingClassifier(**xm.A_CONFIG)
    return clf.fit(X, y) if weights is None else clf.fit(X, y, sample_weight=weights)


def logit(clf, F):
    import numpy as np
    p = clf.predict_proba(F)[:, 1]
    return np.log(np.clip(p, 1e-300, 1.0)) - np.log(np.clip(1.0 - p, 1e-300, 1.0))


# ------------------------------------------------------------------------------------------------------------------------ plan
def bound_files() -> list:
    return sorted({"tools/segarecomp_seg084_a.py", "tools/segarecomp_seg084_audit.py", "tools/segarecomp_seg084_diag.py", "tests/segarecomp_seg084_test.py", "tools/segarecomp_xplat_models.py",
                   "tools/segarecomp_xplat_explore_eval.py", "tools/segarecomp_xplat_transfer.py", "tools/segarecomp_xplat_t005.py", "tools/segarecomp_ml_v3.py",
                   "tools/segarecomp_ml_v3_train.py", "tools/segarecomp_ml_v3_eval.py", "tools/segarecomp_ml_region_gen2_train.py", "tools/segarecomp_ml_region_gen2.py",
                   "tools/segarecomp_genesis_bizhawk.py", "tools/segarecomp_genesis_tasbatch_ml.py", "tools/segarecomp_ml_v3.corpus_manifest.json",
                   "tools/segarecomp_genesis_tasbatch5.readiness.results.json", "tools/segarecomp_genesis_tasbatch5.model_a.results.json",
                   "tools/segarecomp_genesis_evidence.py", "tools/segarecomp_xplat_design.py", "tools/segarecomp_genesis_tasbatch.py", "tools/segarecomp_ml_v3_data.py",
                   "tools/segarecomp_seg084_audit.inventory.json", "tools/segarecomp_genesis_tasbatch5.shadowrun.results.json", "tools/segarecomp_genesis_tasbatch5.nhl94.results.json",
                   "tools/segarecomp_genesis_tasbatch5.shoveit.results.json", "tools/segarecomp_genesis_tasbatch5.doraemon.results.json", "tools/segarecomp_genesis_tasbatch5.terminator.results.json"})


def sha_file(p: pathlib.Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def canonical(obj) -> bytes:
    return json.dumps(obj, indent=1, sort_keys=True).encode() + b"\n"


def build_protocol() -> dict:
    return {
        "schema": SCHEMA + ".plan", "task": "SEG-084-T001",
        "purpose": "reassess the data contribution of the SEG-081 / SEG-083 TAS labels with the TRUE SEG-076 structural model A (a_features + A_CONFIG), not the SEG-072 M1-HGB "
                   "of the SEG-083 diagnostic; decide readiness for a bounded B / C-v2 pilot with code diversity and negative-supervision diversity judged separately",
        "preserved": "ADRs 0181-0187, the SEG-083 frozen plan 228647dd, its decisions (EXPANSION_MIXED, ML_INCONCLUSIVE, MORE_DIVERSITY_NEEDED) and all SEG-076 results are unchanged",
        "model": {"features": "segarecomp_xplat_models.a_features(im, relocs_of(im)) (token aggregates, ordinary-decode chains, PC-relative targets, byte statistics, relocations, "
                              "seed flag, PROVEN reach from seed addresses, +/-1 and +/-4 neighbour means)",
                  "fit": "HistGradientBoostingClassifier(**A_CONFIG) with class_weight balanced, random_state 49, on the pooled labelled 128-byte cells (masked cells excluded)",
                  "score": "log-odds of predict_proba, exactly `score_a`", "unchanged_modules": "imported unchanged: xplat_models, xplat_explore_eval, xplat_transfer, xplat_t005, ml_v3*, gen2_train",
                  "weighted_path": f"{WEIGHTED_VERSION}: optional per-cell sample_weight (1.0 strong-only evidence, 0.25 weak; masked excluded) used ONLY by {list(WEIGHTED_ARMS)}; "
                                   "weights=None is byte-identical to fit_a; weighted arms are NOT called the original model A",
                  "reproduction_gate": "A0 for every fold must equal the stored SEG-076 T001 A.C0.<fold> scores bitwise; otherwise EVIDENCE_INSUFFICIENT"},
        "evaluation": {"folds": list(GENESIS), "scheme": "strict leave-one-lineage-out over the 11 qualified SEG-072 lineages (the SEG-076 C0 folds): a held-out lineage contributes no image to training",
                       "metrics": "SEG-076 definitions unchanged: segarecomp_xplat_explore_eval.image_metrics_ext / lineage_summary / summary (excess retention ER, lineage-max ER, Genesis mean and "
                                  "worst ER, B35/B50 region retention and misses, boundary strata); AP / AUC are secondary context",
                       "negative_errors": "operating point = the A0 fold score at the 10th percentile of positive labelled cells (A0 recall 0.90); every arm is judged at it: false-positive rate on "
                                          "negative cells, on hard-negative cells (genesis_bizhawk.hard_cells) and negative cells admitted at B35/B50",
                       "cost": "per-arm CPU seconds of the fit and inference seconds per held-out lineage",
                       "caution": "development folds are reused and lineage-max ER is outlier-driven: differences inside the noise reference are not evidence; nothing here is confirmatory generalisation"},
        "noise_reference": {"method": f"A0 refitted on a random {NOISE_FRACTION:.0%} of the labelled base cells with seeds {list(NOISE_SEEDS)} (A0_sub1..3); per-fold noise = max |ER(sub) - ER(A0)|",
                            "rule": "an arm's per-fold ER change counts only if it exceeds that fold's noise; the mean change counts only if it exceeds the mean noise"},
        "arms": {"A0": "SEG-072 qualified development corpus only (Genesis, the other 10 lineages per fold)",
                 "A81": "A0 + SEG-081 TAS strong labels (vm2, bnr, ewj2)", "A83": "A0 + SEG-083 TAS strong labels (shadowrun, doraemon, terminator)",
                 "A_all": "A0 + all six qualified TAS strong labels; Sonic 2 (SEG-080) excluded (the same image is the registry's D01 member: no duplicate training, no extra lineage count)",
                 "A_control": "A0 + the matching control strong labels of the same six games (SEG-081: player-1-only primary; SEG-083: all-controller primary)",
                 "A_matched": "A0 + TAS strong labels randomly down-sampled (seed 84) to each game's control positive / negative cell counts",
                 "A_all_W": f"A0 + TAS strong+weak labels with cell weights ({WEIGHTED_VERSION}); not the original model A",
                 "A_control_W": f"A0 + control strong+weak labels with cell weights ({WEIGHTED_VERSION})",
                 "A_term": "A0 + The Terminator TAS strong labels only (low code yield, 2,117 strong hard-negative cells)",
                 "A_noterm": "A0 + the five other TAS strong labels (A_all without The Terminator)",
                 "A0_sub1..3": "noise reference"},
        "seeds": {"problem": "SEG-072 Genesis seeds (`seeds.json`/`d.txt`) are produced by the production CLI `emit-general-startup-bridge-c --reset-entry --direct-control-address-report`, "
                             "a label-free static direct-flow set; the retained TAS samples carry only the reset vector. Feeding model A the retained seeds would give the TAS images a PROVEN-reach feature "
                             "distribution unlike every registry image.",
                  "rule": "`prepare` regenerates d.txt for each TAS ROM with the same pinned CLI and the SEG-072 procedure (seed cell = address // 128); a ROM the CLI rejects gets no seeds, as the "
                          "registry's rejected images do. Seeds never use labels."},
        "isolation": {"image": "a TAS ROM whose SHA-256 equals a registry image is excluded", "lineage": "TAS lineage ids are outside the D## namespace",
                      "code_shingles": "12-byte shingle containment of labelled code runs (TAS strong U control strong) against the held-out lineage's labelled code, either direction, > 0.30 withholds the "
                                       "game from that fold (SEG-081 rule)",
                      "sealed": "no sealed, reserved or blind path is read (refuse_sealed); the SEG-075 cross-platform dev samples are not used (SEG-076 A used the Genesis corpus only)"},
        "negative_supervision": {
            "reported_separately": ["independently qualified code-positive yield", "strong hard-negative yield", "weak weighted hard-negative yield", "mixed-boundary examples",
                                    "novel structural examples", "effective independent lineages supplying each kind"],
            "terminator": "legitimacy of its strong negatives is checked against the tier definitions (strong D = H header + M DMA-source tiers only) and their non-triviality by the fraction "
                          "of their words with an ordinary decode; their value by A_term vs A0 and A_noterm vs A_all on the held-out hard-negative and negative-admitted slices",
            "weak_vs_strong": "A_all_W vs A_all and A_control_W vs A_control; no reference truth repairs any commercial label"},
        "prospective_retention_rule": {"purpose": "decide, prospectively, which labels of the two SEG-083 games that failed a frozen gate may be retained privately; ADR 0187 and its classes are untouched",
                                       "eligible_when": "replay fidelity (identity, played to the end, no perturbation, deterministic, >= 5 distinct states, complete domains) AND label consistency AND sound tracing; yield and the number of "
                                                        "movie fifths with new code are irrelevant; a tracing failure that is solely accepted-executed fraction < 0.99 is waived only when every rejected start is classified "
                                                        "DISASSEMBLER_LIMITATION by the bounded diagnostic",
                                       "no_threshold_lowered": True, "uses": "retention only; the labels are not part of any A arm in this study"},
        "readiness": {"outcomes": ["NEURAL_PILOT_JUSTIFIED", "MORE_DIVERSITY_NEEDED", "MODEL_OR_EVALUATION_LIMITATION", "EVIDENCE_INSUFFICIENT"], "thresholds": READY,
                      "order": "EVIDENCE_INSUFFICIENT (a required arm missing or A0 not reproduced) -> MORE_DIVERSITY_NEEDED (any diversity criterion unmet) -> MODEL_OR_EVALUATION_LIMITATION "
                               "(A does not respond beyond noise, or no residual headroom) -> NEURAL_PILOT_JUSTIFIED",
                      "code_diversity": "independent useful lineages among the eight SEG-081/083 candidates (SEG-083 rule: code yield >= usefulness criterion, shingle containment <= 0.30) >= 5 AND the six "
                                        "qualified games' strong positive cells >= 20 % of the base positive cells",
                      "negative_diversity": "independent qualified lineages with >= 300 strong hard-negative cells, regardless of their code-yield class, >= 3",
                      "response": "A_all improves ER beyond the fold noise on >= 2 folds AND the mean ER change exceeds the mean noise AND the response is dose-dependent (mean ER of A_all below that of A_matched, the same TAS labels at the control's volume). Reported, not gating: A_all vs A_control / A_matched per fold (TAS-specific content versus volume), and the negative-slice responses below",
                      "expected_from_existing_data": "stated before any fit: with the SEG-081/083 results already in hand the code criteria (5 independent useful lineages; strong positive cells of the useful games ~32 % of the base) and the negative criterion (vm2, bnr, ewj2 and The Terminator exceed 300 strong hard-negative cells) are expected to pass, so the decision is expected to hinge on the response and residual rules measured here",
                      "residual": "folds where A_all still has lineage-max ER >= 0.05 and its gain over A0 is within that fold's noise: >= 2",
                      "recommendation_map": {"NEURAL_PILOT_JUSTIFIED": "run a bounded B / C-v2 pilot now (not started automatically)", "MORE_DIVERSITY_NEEDED": "acquire a targeted batch",
                                             "MODEL_OR_EVALUATION_LIMITATION": "improve evaluation / source-backed evidence before more acquisition", "EVIDENCE_INSUFFICIENT": "repair the inputs first"},
                      "independent_of_SEG-083": "this decision does not reinterpret SEG-083's frozen verdict"},
        "limits": {"cpu_hours": 8, "fits": f"{len(ARMS)} arms x {len(GENESIS)} folds, one fit each (A is deterministic)", "no": "hyperparameter search, B / C / C-v2 training, sealed data, retracing, new games"},
        "privacy": "private cell scores and contexts stay in the ignored cache; committed results are aggregate counts, rates and digests",
    }


def write_plan() -> dict:
    if PLAN.exists():
        raise SystemExit("plan already frozen")
    doc = build_protocol()
    doc["bound_code"] = {f: sha_file(HERE.parent / f) for f in bound_files()}
    data = canonical(doc)
    PLAN.write_bytes(data)
    PLAN_DIGEST.write_text(hashlib.sha256(data).hexdigest() + "\n")
    return doc


def verify_plan() -> dict:
    data = PLAN.read_bytes()
    if hashlib.sha256(data).hexdigest() != PLAN_DIGEST.read_text().strip():
        raise SystemExit("plan digest mismatch")
    doc = json.loads(data)
    if {k: v for k, v in doc.items() if k != "bound_code"} != json.loads(json.dumps(build_protocol())):
        raise SystemExit("committed plan differs from the protocol in code")
    if doc["bound_code"] != {f: sha_file(HERE.parent / f) for f in bound_files()}:
        raise SystemExit("bound code changed after the freeze")
    return doc


def plan_sha() -> str:
    return hashlib.sha256(PLAN.read_bytes()).hexdigest()


# ------------------------------------------------------------------------------------------------------------------ private context
def ctx_dir() -> pathlib.Path:
    return pathlib.Path(os.environ.get("SEG084_CTX", str(HARNESS / "segarecomp" / ".cache" / "seg084")))


def sample_dir(root: pathlib.Path, game: str, cond: str, variant: str) -> pathlib.Path:
    return root / game / game / cond / variant


def seg072_data() -> pathlib.Path:
    return pathlib.Path(os.environ.get("SEG072_DATA", str(HARNESS / ".cache" / "seg072" / "data")))


def guard(paths) -> None:
    import segarecomp_xplat_explore_eval as ev6
    import segarecomp_xplat_design as design
    ev6.refuse(paths, design.reserved_ids(), design.SEALED_DIRNAME)


def seeds_from_report(path: pathlib.Path, size: int) -> list:
    """Addresses of a CLI `--direct-control-address-report`, as the SEG-072 registration reads them (`segarecomp_ml_v3_data.cmd_register`)."""
    return [int(t, 16) for t in path.read_text().split() if 0 <= int(t, 16) < size] if path.is_file() else []


def load_registry_images():
    import segarecomp_ml_v3_train as tr
    import segarecomp_xplat_t005 as x5
    import segarecomp_xplat_transfer as xt
    tr.DATA = seg072_data()
    dirs = xt.genesis_dirs(GENESIS)
    guard(dirs)
    return [x5.attach(im, d) for im, d in zip(xt.load(dirs), dirs)], dirs


def check_context(ctx: pathlib.Path) -> dict:
    cj = json.loads((ctx / "context.json").read_text())
    if cj.get("plan_sha256") != plan_sha():
        raise SystemExit("private context was prepared under a different plan: re-run `prepare` under the frozen plan")
    return cj


def load_tas(root: pathlib.Path, ctx: pathlib.Path) -> dict:
    """Per game: ROM-level features (one Image carries tokens / seeds), labels and weights for each (condition, variant)."""
    import numpy as np
    import segarecomp_ml_v3_train as tr
    import segarecomp_xplat_models as xm
    out = {}
    cj = check_context(ctx)
    for g in ALL:
        base = tr.Image(sample_dir(root, g, "tas", "strong"))
        addrs = json.loads((ctx / "seeds" / g / "addresses.json").read_text())
        base.seed_addresses = addrs
        base.seeds = {a // 128 for a in addrs}
        base._dir = sample_dir(root, g, "tas", "strong")
        F = xm.a_features(base, [])
        rec = {"features": F, "labels": {}, "weights": {}}
        for cond in ("tas", "ctl"):
            for variant in ("strong", "weak"):
                d = sample_dir(root, g, cond, variant)
                im = tr.Image(d)
                if im.n_cells != base.n_cells:
                    raise SystemExit(f"{g}/{cond}/{variant}: cell count mismatch")
                rec["labels"][(cond, variant)] = im.labels.astype(np.int64)
                rec["weights"][(cond, variant)] = np.array(json.loads((d / "weights.json").read_text()), dtype=np.float64)
        out[g] = rec
    out["_context"] = cj
    return out


# ------------------------------------------------------------------------------------------------------------------------ prepare
def cmd_prepare(a) -> int:
    """Seeds (pinned CLI), image / lineage isolation and code-shingle containment of every TAS game against every registry lineage."""
    verify_plan()
    import segarecomp_ml_v3_train as tr
    import segarecomp_genesis_tasbatch_ml as ml81
    ctx = ctx_dir()
    root = pathlib.Path(a.samples)
    cli = pathlib.Path(a.cli)
    cli_sha = hashlib.sha256(cli.read_bytes()).hexdigest()
    imgs, dirs = load_registry_images()
    reg_sha = {im.sha256 if hasattr(im, "sha256") else json.loads((d / "meta.json").read_text())["sha256"] for im, d in zip(imgs, dirs)}
    lineage_sh = {}
    for im in imgs:
        lineage_sh.setdefault(im.lineage, set()).update(ml81.code_shingles(im.classes, im.rom))
    out = {"cli_sha256": cli_sha, "plan_sha256": plan_sha(), "games": {}}
    chk = next(im for im in imgs if im.lineage == "D02")                      # seed-procedure self-check on an accepted registry ROM
    chk_dir = pathlib.Path(chk._dir)
    with __import__("tempfile").TemporaryDirectory() as td:
        td = pathlib.Path(td)
        proc = subprocess.run([str(cli), "emit-general-startup-bridge-c", "--rom", str(json.loads((chk_dir / "meta.json").read_text())["image"]),
                               "--reset-entry", "--rom-sha256", json.loads((chk_dir / "meta.json").read_text())["sha256"], "--immutable-rom-aot", "--direct-control-address-report", str(td / "d.txt"),
                               "--immutable-aot-address-report", str(td / "u.txt"), "--generated-c-output", str(td / "p.c")], capture_output=True, text=True, timeout=3600)
        out["seed_procedure_self_check"] = bool(proc.returncode == 0 and (td / "d.txt").read_text() == (chk_dir / "d.txt").read_text())
    if not out["seed_procedure_self_check"]:
        raise SystemExit("seed procedure self-check failed: the CLI does not regenerate the stored registry d.txt")
    for g in ALL:
        meta = json.loads((sample_dir(root, g, "tas", "strong") / "meta.json").read_text())
        rom_path = pathlib.Path(meta["image"])
        rom = rom_path.read_bytes()
        rsha = hashlib.sha256(rom).hexdigest()
        if rsha != meta["sha256"]:
            raise SystemExit(f"{g}: ROM identity mismatch")
        if rsha in reg_sha:
            raise SystemExit(f"{g}: image collision with a registry image")
        sd = ctx / "seeds" / g
        sd.mkdir(parents=True, exist_ok=True)
        rep, probe = sd / "d.txt", sd / "probe.c"
        proc = subprocess.run([str(cli), "emit-general-startup-bridge-c", "--rom", str(rom_path), "--reset-entry", "--rom-sha256", rsha, "--immutable-rom-aot",
                               "--direct-control-address-report", str(rep), "--immutable-aot-address-report", str(sd / "u.txt"), "--generated-c-output", str(probe)],
                              capture_output=True, text=True, timeout=3600)
        probe.unlink(missing_ok=True)
        (sd / "u.txt").unlink(missing_ok=True)
        rejected = proc.returncode != 0
        if rejected:
            rep.unlink(missing_ok=True)
        addrs = seeds_from_report(rep, len(rom)) if not rejected else []
        (sd / "addresses.json").write_text(json.dumps(addrs))
        sh = set()
        for cond in ("tas", "ctl"):
            im = tr.Image(sample_dir(root, g, cond, "strong"))
            sh |= ml81.code_shingles(im.classes, im.rom)
        cont = {lin: round(max(ml81.containment(sh, s), ml81.containment(s, sh)), 5) for lin, s in lineage_sh.items()}
        out["games"][g] = {"rom_sha256": rsha, "seed_addresses": len(addrs), "seed_cells": len({x // 128 for x in addrs}), "cli_rejected": rejected,
                           "containment": cont, "withheld_folds": sorted(l for l, v in cont.items() if v > CONTAINMENT_MAX)}
    (ctx / "context.json").write_text(json.dumps(out, indent=1, sort_keys=True))
    print(json.dumps({g: {"seed_addresses": v["seed_addresses"], "rejected": v["cli_rejected"], "max_containment": max(v["containment"].values()),
                          "withheld": v["withheld_folds"]} for g, v in out["games"].items()}, sort_keys=True))
    return 0


# ---------------------------------------------------------------------------------------------------------------------------- run
def build_arm(np, name, spec, tas, base_X, base_y, rng_sub, match_rng):
    """Assemble (X, y, w) for one arm. w is None for every strong-only arm."""
    X, Y, W, weighted = [base_X], [base_y], [np.ones(len(base_y))], name in WEIGHTED_ARMS
    if spec == "sub":
        sel = rng_sub.rand(len(base_y)) < NOISE_FRACTION
        return base_X[sel], base_y[sel], None
    for games, cond, variant, matched in spec:
        for g in games:
            lab = tas[g]["labels"][(cond, variant)]
            keep = lab >= 0
            if matched:
                ctl = tas[g]["labels"][("ctl", "strong")]
                for v in (1, 0):
                    idx = np.nonzero(lab == v)[0]
                    want = int((ctl == v).sum())
                    if len(idx) > want:
                        drop = np.setdiff1d(idx, match_rng.choice(idx, want, replace=False))
                        keep[drop] = False
            X.append(tas[g]["features"][keep])
            Y.append((lab[keep] == 1).astype(np.int64))
            W.append(tas[g]["weights"][(cond, variant)][keep] if weighted else np.ones(int(keep.sum())))
    return np.concatenate(X), np.concatenate(Y), (np.concatenate(W) if weighted else None)


def score_path(fold: str, arm: str) -> pathlib.Path:
    return ctx_dir() / "scores" / f"{arm}.{fold}.npz"


def cmd_run(a) -> int:
    verify_plan()
    import numpy as np
    import segarecomp_ml_v3_train as tr
    import segarecomp_xplat_models as xm
    ctx = ctx_dir()
    fold = a.fold
    if fold not in GENESIS:
        raise SystemExit("fold must be a SEG-072 Genesis lineage")
    imgs, _dirs = load_registry_images()
    train = [im for im in imgs if im.lineage != fold]
    held = [im for im in imgs if im.lineage == fold]
    if not held or any(im.lineage == fold for im in train):
        raise SystemExit("fold isolation violated")
    feats = {im.id: xm.a_features(im, xm.relocs_of(im)) for im in imgs}
    keep = [(feats[im.id][im.labels >= 0], (im.labels[im.labels >= 0] == 1).astype(np.int64)) for im in train]
    base_X, base_y = np.concatenate([k[0] for k in keep]), np.concatenate([k[1] for k in keep])
    tas = load_tas(pathlib.Path(a.samples), ctx)
    withheld = {g: fold in tas["_context"]["games"][g]["withheld_folds"] for g in ALL}
    for g in ALL:
        if withheld[g]:
            tas[g]["labels"] = {k: np.full_like(v, -1) for k, v in tas[g]["labels"].items()}      # masked everywhere = contributes nothing
    arms = [x for x in ARMS if (not a.arm or x in a.arm)]
    for k, name in enumerate(arms):
        path = score_path(fold, name)
        if path.exists():
            continue
        spec = ARMS[name]
        rng_sub = np.random.RandomState(NOISE_SEEDS[int(name[-1]) - 1]) if spec == "sub" else None
        match_rng = np.random.RandomState(MATCH_SEED)
        X, y, w = build_arm(np, name, spec, tas, base_X, base_y, rng_sub, match_rng)
        t0, c0 = time.time(), time.process_time()
        clf = fit_arrays(X, y, w)
        fit_wall, fit_cpu = time.time() - t0, time.process_time() - c0
        t1 = time.time()
        scores = {im.id: logit(clf, feats[im.id]) for im in held}
        infer = time.time() - t1
        tr.save_scores(path, scores)
        path.with_suffix(".json").write_text(json.dumps({"arm": name, "fold": fold, "plan_sha256": plan_sha(), "train_cells": int(len(y)),
                                                         "positives": int(y.sum()), "weighted": w is not None, "fit_wall_seconds": round(fit_wall, 2),
                                                         "fit_cpu_seconds": round(fit_cpu, 2), "inference_seconds": round(infer, 3),
                                                         "withheld_games": sorted(g for g in ALL if withheld[g])}, sort_keys=True))
        print(json.dumps({"fold": fold, "arm": name, "cells": int(len(y)), "fit_cpu": round(fit_cpu, 1)}), flush=True)
    return 0


# --------------------------------------------------------------------------------------------------------------------------- table
def independent_count(games: list, pair: dict, reg: dict, threshold: float = CONTAINMENT_MAX) -> int:
    """Effective independent lineages among games (pure): registry-redundant games drop out; the rest cluster by pairwise containment > threshold."""
    rest = [g for g in games if reg.get(g, 0.0) <= threshold]
    parent = {g: g for g in rest}

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    for (x, y), v in pair.items():
        if x in parent and y in parent and v > threshold:
            parent[find(x)] = find(y)
    return len({find(g) for g in rest})


def cmd_table(a) -> int:
    plan = verify_plan()
    import numpy as np
    from sklearn.metrics import average_precision_score, roc_auc_score
    import segarecomp_xplat_explore_eval as ev6
    import segarecomp_xplat_transfer as xt
    import segarecomp_genesis_bizhawk as bz
    imgs, _d = load_registry_images()
    ctx = ctx_dir()
    folds = tuple(a.folds) if a.folds else GENESIS         # a subset is a mechanics rehearsal only (no readiness decision)
    by_fold = {f: [im for im in imgs if im.lineage == f] for f in folds}
    lineage_of = {im.id: im.lineage for im in imgs}
    ref = pathlib.Path(a.seg076_scores)
    cj = check_context(ctx)
    for arm in ARMS:
        for f in folds:
            j = score_path(f, arm).with_suffix(".json")
            if j.exists() and json.loads(j.read_text()).get("plan_sha256") != plan_sha():
                raise SystemExit(f"{arm}.{f}: score was produced under a different plan")
    complete, missing = {}, []
    for arm in ARMS:
        ok = all(score_path(f, arm).exists() for f in folds)
        complete[arm] = ok
        if not ok:
            missing.append(arm)
    a0_repro = False
    if complete["A0"] and ref.is_dir():
        a0_repro = True
        for f in folds:
            r = xt.load_npz(ref / f"A.C0.{f}.npz")
            m = xt.load_npz(score_path(f, "A0"))
            a0_repro &= all(k in r and np.array_equal(r[k], m[k]) for k in m) and len(r) == len(m)
    per_arm = {}
    thr = {}
    for f in folds:                                     # A0 operating point of the fold: 10th percentile of positive labelled cells
        if not complete["A0"]:
            break
        sc = xt.load_npz(score_path(f, "A0"))
        pos = np.concatenate([sc[im.id][np.array(_cells(im)) == 1] for im in by_fold[f]])
        thr[f] = float(np.percentile(pos, 10)) if len(pos) else None
    hard = {im.id: _hard(bz, im) for im in imgs}
    for arm in ARMS:
        if not complete[arm]:
            continue
        pi, fr = {}, {}
        ap_s, ap_y, fold_ap = [], [], {}
        for f in folds:
            sc = xt.load_npz(score_path(f, arm))
            neg_fp = neg_n = hard_fp = hard_n = 0
            fs, fy = [], []
            for im in by_fold[f]:
                s = sc[im.id]
                pi[im.id] = ev6.image_metrics_ext(im.classes, im.seed_addresses, s)
                lab = np.array(_cells(im))
                m = lab >= 0
                fs.append(s[m]), fy.append(lab[m])
                if thr.get(f) is not None:
                    neg = lab == 0
                    neg_fp += int(((s >= thr[f]) & neg).sum())
                    neg_n += int(neg.sum())
                    hm = np.array([c in hard[im.id] for c in range(len(lab))])
                    hard_fp += int(((s >= thr[f]) & hm).sum())
                    hard_n += int(hm.sum())
            ys, ss = np.concatenate(fy), np.concatenate(fs)
            ap_s.append(ss), ap_y.append(ys)
            fold_ap[f] = {"ap": round(float(average_precision_score(ys, ss)), 5), "auc": round(float(roc_auc_score(ys, ss)), 5)}
            fr[f] = {"neg_fp_rate": round(neg_fp / neg_n, 6) if neg_n else None, "hard_neg_fp_rate": round(hard_fp / hard_n, 6) if hard_n else None, "hard_cells": hard_n}
        rows = ev6.lineage_summary(pi, lineage_of)
        S = ev6.summary(rows)
        cost = [json.loads(score_path(f, arm).with_suffix(".json").read_text()) for f in folds]
        ys, ss = np.concatenate(ap_y), np.concatenate(ap_s)
        per_arm[arm] = {"summary": S, "lineage": {L: {k: v for k, v in r.items() if k in ("max_er", "mean_image_er", "misses_B35", "misses_B50", "missed_cells_B35", "missed_cells_B50",
                                                                                         "negative_admitted_B35", "negative_admitted_B50", "mean_auroc", "max_frf", "strata")}
                                              for L, r in rows.items()},
                        "negative_errors": {"macro_neg_fp_rate": _mean([v["neg_fp_rate"] for v in fr.values()]), "macro_hard_neg_fp_rate": _mean([v["hard_neg_fp_rate"] for v in fr.values()]),
                                            "by_fold": fr},
                        "secondary": {"pooled_ap": round(float(average_precision_score(ys, ss)), 5), "pooled_auc": round(float(roc_auc_score(ys, ss)), 5), "by_fold": fold_ap},
                        "cost": {"fit_cpu_seconds": round(sum(c["fit_cpu_seconds"] for c in cost), 1), "inference_seconds": round(sum(c["inference_seconds"] for c in cost), 2),
                                 "train_cells_mean": round(sum(c["train_cells"] for c in cost) / len(cost)), "withheld_game_folds": sum(len(c["withheld_games"]) for c in cost)}}
    er = {arm: {f: v["lineage"][f]["max_er"] for f in folds} for arm, v in per_arm.items()}
    out = {"schema": SCHEMA + ".results", "plan_sha256": plan_sha(), "folds": list(folds), "arms": per_arm, "missing_arms": missing, "a0_reproduces_seg076": a0_repro}
    if not missing:
        noise = fold_noise(er["A0"], [er[f"A0_sub{i}"] for i in (1, 2, 3)])
        out["noise_per_fold"] = {f: None if v is None else round(v, 6) for f, v in noise.items()}
        out["response"] = {arm: classify_response(er["A0"], er[arm], noise) for arm in ARMS if arm != "A0" and not arm.startswith("A0_sub")}
        out["noise_reference"] = {"mean_noise": round(_mean([v for v in noise.values() if v is not None]), 6),
                                  "sub_seed_mean_er": {f"A0_sub{i}": per_arm[f"A0_sub{i}"]["summary"]["mean_er"] for i in (1, 2, 3)}}
        pairs = (("A_all", "A_control"), ("A_all", "A_matched"), ("A_all_W", "A_all"), ("A_control_W", "A_control"), ("A_term", "A0"), ("A_all", "A_noterm"), ("A_all", "A81"), ("A_all", "A83"))
        out["paired"] = {f"{x}-{y}": classify_response(er[y], er[x], noise) for x, y in pairs}
        def slice_vals(arm, key):
            if key == "hard_neg_fp_rate":
                return {f: per_arm[arm]["negative_errors"]["by_fold"][f]["hard_neg_fp_rate"] for f in folds}
            return {f: per_arm[arm]["lineage"][f][key] for f in folds}
        out["negative_slices"] = {}
        for key in ("hard_neg_fp_rate", "negative_admitted_B35", "negative_admitted_B50"):
            v0 = slice_vals("A0", key)
            nz = fold_noise(v0, [slice_vals(f"A0_sub{i}", key) for i in (1, 2, 3)])
            out["negative_slices"][key] = {"noise_per_fold": {f: None if x is None else round(x, 6) for f, x in nz.items()},
                                           "paired": {f"{x}-{y}": classify_response(slice_vals(y, key), slice_vals(x, key), nz)
                                                      for x, y in (("A_term", "A0"), ("A_all", "A0"), ("A_all", "A_noterm"), ("A_all", "A_control"), ("A_all", "A_matched"))}}
        out["tas_vs_control"] = {"A_all-A_control": out["paired"]["A_all-A_control"], "A_all-A_matched": out["paired"]["A_all-A_matched"],
                                 "reading": "negative mean_delta_er = TAS better than the control-derived labels; inside the noise = control-equivalent (volume, not TAS content)"}
        out["difficult_folds"] = {f: {arm: {"max_er": er[arm][f], "hard_neg_fp": per_arm[arm]["negative_errors"]["by_fold"][f]["hard_neg_fp_rate"]}
                                      for arm in ("A0", "A_all", "A_control", "A_matched", "A_term")} for f in ("D01", "D03", "D07") if f in folds}
        out["residual"] = {f: {"a0": er["A0"][f], "a_all": er["A_all"][f],
                               "residual": bool(er["A_all"][f] is not None and er["A_all"][f] >= READY["residual_er"] and er["A0"][f] - er["A_all"][f] <= (noise[f] or 0))} for f in folds}
        out["residual_folds"] = sum(1 for v in out["residual"].values() if v["residual"])
        out["negative_audit"] = negative_audit(a, imgs, hard)
        if folds == GENESIS:
            out["readiness_inputs"] = readiness_inputs(a, out, imgs)
            out["readiness"] = readiness(out["readiness_inputs"])
    pathlib.Path(a.out).write_text(json.dumps(out, indent=1, sort_keys=True))
    print(json.dumps({"missing": missing, "a0_reproduces": a0_repro, "mean_er": {k: v["summary"]["mean_er"] for k, v in per_arm.items()},
                      "readiness": out.get("readiness", {}).get("decision")}, sort_keys=True))
    return 0


def _mean(v):
    v = [x for x in v if x is not None]
    return round(sum(v) / len(v), 6) if v else None


def _cells(im) -> list:
    return [int(x) for x in im.labels]


def _hard(bz, im) -> set:
    neg = {int(c) for c, x in enumerate(im.labels) if x == 0}
    return bz.hard_cells(im.classes, im.rom, im.tokens.tobytes(), neg)


def hard_breakdown(cls: bytes, tokens, neg: set) -> dict:
    """Why a negative cell is 'hard' (genesis_bizhawk.hard_cells): adjacent to an instruction cell (near) and/or >= 50 % of its words decode as ordinary instructions (ordinary).
    Reports the split and the mean ordinary-decode fraction, so Terminator can be compared with the registry's own hard negatives (pure given numbers)."""
    n = (len(cls) + 127) // 128
    near_n = ord_only = both = 0
    fr = []
    for c in neg:
        near = any(0 <= c + d < n and b"I" in cls[(c + d) * 128:(c + d + 1) * 128] for d in (-1, 1))
        ws = tokens[c * 64:(c + 1) * 64, 0]
        ordf = float((ws == 2).mean()) if len(ws) else 0.0
        o = ordf >= 0.5
        if not (near or o):
            continue
        fr.append(ordf)
        near_n += int(near and not o)
        ord_only += int(o and not near)
        both += int(near and o)
    h = len(fr)
    return {"hard": h, "near_only": near_n, "ordinary_only": ord_only, "both": both, "mean_ordinary_fraction": round(sum(fr) / h, 4) if h else None,
            "near_share": round((near_n + both) / h, 4) if h else None}


def negative_audit(a, imgs, base_hard) -> dict:
    """Legitimacy and non-triviality of the TAS strong negatives, per game (aggregate only), against the registry's hard negatives."""
    import segarecomp_ml_v3_train as tr
    import segarecomp_genesis_bizhawk as bz
    root = pathlib.Path(a.samples)
    inv = json.loads((HERE / "segarecomp_seg084_audit.inventory.json").read_text())["tas"]["snapshots"]
    tiers = {g: r["conditions"]["tas"] for s in inv.values() for g, r in s["games"].items() if g in ALL}
    out = {}
    reg_neg = reg_hard = 0
    reg_b = {"hard": 0, "near_only": 0, "ordinary_only": 0, "both": 0}
    reg_fr = []
    for im in imgs:
        neg = {int(c) for c, x in enumerate(im.labels) if x == 0}
        reg_neg += len(neg)
        bd = hard_breakdown(im.classes, im.tokens, neg)
        reg_hard += bd["hard"]
        for k in reg_b:
            reg_b[k] += bd[k]
        if bd["mean_ordinary_fraction"] is not None:
            reg_fr.append((bd["mean_ordinary_fraction"], bd["hard"]))
    out["registry"] = {"negative_cells": reg_neg, "hard_negative_cells": reg_hard, "hard_share_of_negatives": round(reg_hard / reg_neg, 4), **{k: v for k, v in reg_b.items() if k != "hard"},
                       "near_share": round((reg_b["near_only"] + reg_b["both"]) / max(1, reg_b["hard"]), 4),
                       "mean_ordinary_fraction": round(sum(f * n for f, n in reg_fr) / max(1, sum(n for _, n in reg_fr)), 4)}
    for g in ALL:
        im = tr.Image(sample_dir(root, g, "tas", "strong"))
        iw = tr.Image(sample_dir(root, g, "tas", "weak"))
        neg = {int(c) for c, x in enumerate(im.labels) if x == 0}
        negw = {int(c) for c, x in enumerate(iw.labels) if x == 0}
        bd, bw = hard_breakdown(im.classes, im.tokens, neg), hard_breakdown(iw.classes, iw.tokens, negw)
        tb_ = tiers[g]["tier_bytes"]
        sb = tiers[g]["strong_bytes"]
        out[g] = {"strong_negative_cells": len(neg), "strong_hard_negative_cells": bd["hard"], "weak_negative_cells": len(negw), "weak_hard_negative_cells": bw["hard"],
                  "strong_positive_cells": int((im.labels == 1).sum()), "mixed_boundary_cells_weak": int(sum(1 for c in range(iw.n_cells) if iw.labels[c] == 1 and b"D" in iw.classes[c * 128:(c + 1) * 128])),
                  "strong_d_bytes": sb["D"], "tier_header_H_bytes": tb_["H"], "tier_dma_M_bytes": tb_["M"],
                  "strong_data_is_only_H_or_M": bool(sb["D"] == tb_["H"] + tb_["M"]), "strong_hard_breakdown": {k: v for k, v in bd.items() if k != "hard"},
                  "hard_share_of_strong_negatives": round(bd["hard"] / len(neg), 4) if neg else None}
    return out


def readiness_inputs(a, res: dict, imgs) -> dict:
    root = HERE
    tr83 = json.loads((root / "segarecomp_genesis_tasbatch5.model_a.results.json").read_text())
    pair = {tuple(k.split("~")): v for k, v in tr83["pairwise_containment"].items()}
    reg = tr83["registry_containment_max"]
    hn = {g: res["negative_audit"][g]["strong_hard_negative_cells"] for g in ALL}
    useful = [g for g, v in GAMES.items() if v[2] == "QUALIFIED_USEFUL"]
    neg_games = [g for g in ALL if hn[g] >= READY["diversity_negative_min_strong_hard_cells"]]
    base_pos = sum(int((im.labels == 1).sum()) for im in imgs)
    new_pos = sum(res["negative_audit"][g]["strong_positive_cells"] for g in useful)            # the code axis counts the useful games' positives only
    resp = res["response"]["A_all"]
    return {"missing_arms": res["missing_arms"], "a0_reproduces": res["a0_reproduces_seg076"], "code_independent_useful": independent_count(useful, pair, reg),
            "code_new_positive_fraction": round(new_pos / base_pos, 4), "negative_lineages": independent_count(neg_games, pair, reg),
            "response_folds": len(resp["folds_better"]), "response_mean_beyond_noise": resp["mean_beyond_noise"],
            "dose_response": bool(res["arms"]["A_all"]["summary"]["mean_er"] < res["arms"]["A_matched"]["summary"]["mean_er"]), "residual_folds": res["residual_folds"]}


# -------------------------------------------------------------------------------------------------------------------------- main
def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("freeze").set_defaults(fn=lambda a: print(json.dumps({"plan_sha256": hashlib.sha256(PLAN.read_bytes()).hexdigest() if write_plan() else ""})) or 0)
    sub.add_parser("verify-plan").set_defaults(fn=lambda a: print(json.dumps({"plan_sha256": plan_sha(), "ok": bool(verify_plan())})) or 0)
    p = sub.add_parser("prepare")
    p.add_argument("--samples", required=True), p.add_argument("--cli", required=True)
    p.set_defaults(fn=cmd_prepare)
    p = sub.add_parser("run")
    p.add_argument("--fold", required=True), p.add_argument("--samples", required=True), p.add_argument("--arm", nargs="*")
    p.set_defaults(fn=cmd_run)
    p = sub.add_parser("table")
    p.add_argument("--samples", required=True), p.add_argument("--seg076-scores", required=True), p.add_argument("--out", required=True), p.add_argument("--folds", nargs="*")
    p.set_defaults(fn=cmd_table)
    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
