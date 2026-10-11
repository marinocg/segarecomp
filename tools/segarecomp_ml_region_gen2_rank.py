#!/usr/bin/env python3
"""SEG-049-T002 (ADR 0101): rank-budget executable-region selection for the frozen gen2 M3 model -- stdlib-only core.

OFFLINE RESEARCH TOOLING. M3 is used as a *ranking* model: T001 showed its cross-title ordering transfers while its absolute probability
scale does not. The selection policy is therefore a fixed fraction of the highest-ranked 128-byte decision cells of the immutable ROM.

This module is deliberately generic: `rank_select` receives only (scores, fraction) and knows nothing about titles, ROM digests, source
projects, instruction addresses, truth labels or runtime coverage. The cell ordinal is used ONLY as a deterministic tie-break after scoring
(never a model input; the model-input leakage guard of `segarecomp_ml_region_gen2.assert_input_channels` is unchanged and rejects it).

Everything that grades a policy (FRF, headroom, R/ROM, C outside R / K, post-ML loss attribution) reuses the single T001 definitions in
`segarecomp_ml_region_gen2`.
"""
from __future__ import annotations

import inspect
import json
import math
import pathlib

import segarecomp_ml_region_gen2 as g

SCHEMA = "segarecomp.ml_region_gen2_rank_plan.v1"
EXPERIMENT = "SEG-049-T002"

# Pre-registered, exactly five, integer percents (integer arithmetic keeps the cell count exact and host independent).
BUDGET_PERCENT = {"B30": 30, "B35": 35, "B40": 40, "B45": 45, "B50": 50}
HEADROOM_MIN = 1.25
R_OVER_ROM_HARD = g.R_OVER_ROM_GATE
R_OVER_ROM_PREFERRED = g.R_OVER_ROM_PREFERRED

CLASSES = ("GEN2_CALIBRATION_READY_FOR_NATIVE_PORT", "GEN2_CALIBRATION_VALID_RUNTIME_BLOCKED",
           "GEN2_CALIBRATION_VALID_ECONOMICS_BLOCKED", "GEN2_CALIBRATION_UNSOLVED")

# The only information a rank policy may consume.
POLICY_INPUTS = ("scores", "decision_cell_count", "budget_percent")
FORBIDDEN_POLICY_INPUTS = ("title", "rom", "sha", "truth", "label", "coverage", "runtime", "address", "path", "source", "oracle", "pc")


# ---------------------------------------------------------------------------------------------------------------------- the policy
def budget_cell_count(cell_count: int, percent: int) -> int:
    """N = ceil(cell_count * percent / 100) in exact integer arithmetic (never floating point)."""
    if not isinstance(cell_count, int) or isinstance(cell_count, bool) or cell_count <= 0:
        raise SystemExit("invalid decision cell count")
    if not isinstance(percent, int) or isinstance(percent, bool) or not 0 < percent <= 100:
        raise SystemExit("invalid budget percent")
    return (cell_count * percent + 99) // 100


def rank_order(scores) -> list[int]:
    """Cell indices ordered by score descending, then cell ordinal ascending (deterministic, architecture independent).

    NaN is rejected: a NaN score has no rank. The ordinal is a tie-break only; it is not an input to the model that produced `scores`.
    """
    scores = list(scores)
    if not scores:
        raise SystemExit("rank: no scores")
    for s in scores:
        if isinstance(s, bool) or not isinstance(s, (int, float)) or math.isnan(s):
            raise SystemExit("rank: score is not a number")
    return sorted(range(len(scores)), key=lambda c: (-scores[c], c))


def rank_select(scores, percent: int) -> set[int]:
    """The ML proposal of budget `percent`: the N highest-ranked cells, N = ceil(cells * percent / 100)."""
    order = rank_order(scores)
    return set(order[:budget_cell_count(len(order), percent)])


def policy_signature() -> list[str]:
    """Parameter names of the selection function (checked by tests: nothing but scores and the budget)."""
    return list(inspect.signature(rank_select).parameters)


# --------------------------------------------------------------------------------------------------------------------- arithmetic
def rank_headroom(percent: int, frf_value: float) -> float:
    """nominal budget fraction / FRF. >= 1 means the budget alone reaches the weakest positive cell; the gate demands >= 1.25."""
    if not frf_value > 0.0:
        raise SystemExit("invalid FRF")
    return (percent / 100.0) / frf_value


def r_stage(scores, labels, truth, seeds, rom_size: int, percent: int) -> dict:
    """R-stage record of one outer title: ML proposal, seeds union, R/ROM, C outside ML-only / R, FRF and headroom."""
    cells = g.cell_count(rom_size)
    if len(scores) != cells or len(labels) != cells:
        raise SystemExit("r_stage: score/label length does not match the ROM")
    selected = rank_select(scores, percent)
    region = selected | set(seeds)
    f = g.frf(scores, labels)
    out_ml = g.c_outside_cells(truth, selected)
    out_r = g.c_outside_cells(truth, region)
    return {"percent": percent, "cells": cells, "selected_cells": len(selected), "ml_selected_fraction": len(selected) / cells,
            "seed_cells": len(set(seeds)), "r_cells": len(region), "r_bytes": g.region_bytes(region, rom_size), "rom_bytes": rom_size,
            "r_over_rom": g.region_bytes(region, rom_size) / rom_size, "c_outside_ml_only": len(out_ml), "c_outside_r": len(out_r),
            "frf": f["frf"], "frf_cells_needed": f["cells_needed"], "rank_headroom": rank_headroom(percent, f["frf"]),
            "truth_starts": len(truth), "_region": region}


def r_gate(record: dict) -> bool:
    return record["c_outside_r"] == 0 and record["r_over_rom"] <= R_OVER_ROM_HARD and record["rank_headroom"] >= HEADROOM_MIN


def k_gate(record: dict) -> bool:
    return record.get("validator") == "accepted" and record.get("attribution", {}).get("c_outside_k") == 0


def policy_r_passes(folds: dict) -> dict:
    """folds[title][budget] = r_stage record. Per budget: passes the R gate on every title."""
    return {b: all(r_gate(folds[t][b]) for t in folds) for b in BUDGET_PERCENT}


def policy_k_passes(folds: dict) -> dict:
    return {b: all(k_gate(folds[t][b]) for t in folds if "validator" in folds[t][b]) and all("validator" in folds[t][b] for t in folds)
            for b in BUDGET_PERCENT}


def choose_budget(folds: dict):
    """The SMALLEST pre-registered budget passing every R gate and every K gate on every outer title; None if none does."""
    r_ok, k_ok = policy_r_passes(folds), policy_k_passes(folds)
    for b in sorted(BUDGET_PERCENT, key=BUDGET_PERCENT.get):
        if r_ok[b] and k_ok[b]:
            return b
    return None


def classify(*, selected: str | None, runtime_ok: bool | None, economics_ok: bool | None, frozen_ok: bool) -> str:
    if selected is None:
        return "GEN2_CALIBRATION_UNSOLVED"
    if runtime_ok is False:
        return "GEN2_CALIBRATION_VALID_RUNTIME_BLOCKED"
    if economics_ok is False:
        return "GEN2_CALIBRATION_VALID_ECONOMICS_BLOCKED"
    if runtime_ok and economics_ok and frozen_ok:
        return "GEN2_CALIBRATION_READY_FOR_NATIVE_PORT"
    return "GEN2_CALIBRATION_UNSOLVED"


def public_record(record: dict) -> dict:
    """Aggregate-only view (drops the runtime-only cell set)."""
    return {k: v for k, v in record.items() if not k.startswith("_")}


# ----------------------------------------------------------------------------------------------------------------------------- plan
def build_plan(t001_plan_sha256: str, outer_score_digests: dict) -> dict:
    """The T002 pre-registration. Written (and committed) BEFORE any budget is evaluated against labels."""
    return {
        "schema": SCHEMA, "experiment": EXPERIMENT, "adr": "0101",
        "status": "FROZEN BEFORE ANY B30-B50 EVALUATION",
        "scope": "OFFLINE research; production Optimized AOT and SEG-047 v1 artifacts are unchanged; M3 is frozen exactly as in T001",
        "frozen_from": {"t001_plan_sha256": t001_plan_sha256, "representation_sha256": g.representation_sha256(),
                        "token_schema_sha256": g.token_schema_sha256(), "model": "M3", "seed": g.SEED,
                        "outer_score_digests": outer_score_digests,
                        "score_reuse": "private T001 outer M3 score arrays are reused only if their prediction digests equal outer_score_digests"},
        "evidence_status": "the five titles are NOT blind (T001 exposed their FRF); this is calibration engineering on a known corpus, "
                           "not fresh blind generalization proof",
        "policy": {
            "kind": "rank budget (no absolute probability threshold)",
            "budgets_percent": BUDGET_PERCENT,
            "cell_count_rule": "N = ceil(cells * percent / 100) in integer arithmetic; cells = ceil(rom_bytes / 128)",
            "ranking": "scores descending, then cell ordinal ascending; ordinal is a deterministic tie-break only, never a model input",
            "inputs": list(POLICY_INPUTS),
            "forbidden_inputs": list(FORBIDDEN_POLICY_INPUTS),
            "region": "R = rank-selected cells UNION certain-code seed cells (cells holding a precise direct-control-discovery identity)",
            "uniform": "one budget for every ROM; no per-ROM budget; no additional budgets may be evaluated",
            "score": "the float64 sigmoid probability of the M3 logit as produced in T001 (identical ranking to the logit; saturated ties, if "
                     "any, sit at the top of the ranking and are resolved by the ordinal tie-break)"},
        "gates": {
            "r_stage": {"c_outside_r": 0, "r_over_rom_max": R_OVER_ROM_HARD, "r_over_rom_preferred": R_OVER_ROM_PREFERRED,
                        "rank_headroom_min": HEADROOM_MIN,
                        "rank_headroom": "nominal budget fraction (percent/100) / FRF of the held-out fold"},
            "k_stage": {"validator": "accepted on every outer title", "c_outside_k": 0,
                        "stage": "existing unchanged structural prune + validator (tools/segarecomp_ml_region_v2.PruneRunner)",
                        "attribution": "C outside R separated from post-ML pruning losses (POST_ML_PRUNING_BLOCKER); M3 is never retrained to mask them"},
            "choice": "the SMALLEST budget among B30..B50 passing every R and K gate on all five outer titles; none -> GEN2_CALIBRATION_UNSOLVED and stop"},
        "after_static_pass": [
            "freeze one all-five-title M3 research model with the exact T001 recipe, fit twice independently, require bitwise equal predictions",
            "runtime differential broad vs production v1 vs gen2 held-out rank-budget explicit plan on the five source-backed titles",
            "historical runtime-only falsifier on authorized SEG-046 titles (no static completeness claim)",
            "economics: R/ROM, K/U, generated C, compile CPU, wall, RSS, inference time"],
        "economics_gate": {"generated_c_reduction_min": 0.30, "compile_cpu_reduction_min": 0.20, "build_wall": "must not regress materially",
                           "research_inference_note": "RESEARCH_INFERENCE_COST_NOT_REPRESENTATIVE if PyTorch inference dominates wall"},
        "not_done": ["no architecture / recipe change", "no probability calibration family unless every rank policy fails",
                     "no native inference, no launcher/default-policy change", "no v1 artifact change"],
        "classes": list(CLASSES),
        "v1_artifact_sha256": g.V1_ARTIFACT_SHA256,
    }


def plan_digest(plan: dict) -> str:
    return g.sha256_bytes(g.canonical_json(plan).encode())


def t001_plan_digest(path) -> str:
    """Canonical-JSON digest of the T001 plan (the `plan_sha256` of the T001 results): independent of line endings."""
    return g.sha256_bytes(g.canonical_json(json.loads(pathlib.Path(path).read_text())).encode())


def verify_plan(path, t001_plan_path, outer_score_digests=None) -> dict:
    """Fail closed if the committed T002 plan differs from this code's constants or from the T001 plan it is derived from."""
    committed = json.loads(pathlib.Path(path).read_text())
    t001 = t001_plan_digest(t001_plan_path)
    digests = committed["frozen_from"]["outer_score_digests"] if outer_score_digests is None else outer_score_digests
    if build_plan(t001, digests) != committed:
        raise SystemExit("rank plan drift: constants differ from the frozen pre-registered T002 plan")
    if set(committed["policy"]["budgets_percent"]) != set(BUDGET_PERCENT) or committed["policy"]["budgets_percent"] != BUDGET_PERCENT:
        raise SystemExit("rank plan drift: the budget set is not exactly B30..B50")
    return committed


# ------------------------------------------------------------------------------------------------------------ frozen research candidate
FROZEN_NAMES = {"model": "segarecomp_ml_region_gen2.model.json", "frozen": "segarecomp_ml_region_gen2.frozen.json",
                "parity": "segarecomp_ml_region_gen2.parity.json", "plan": "segarecomp_ml_region_gen2_rank.plan.json"}
PARITY_PROBES = 3


def parity_probes(count: int = PARITY_PROBES) -> list[dict]:
    """Deterministic SYNTHETIC 512-word contexts (hi/lo bytes + token rows) from a private 64-bit LCG: no ROM data, no random-module dependence."""
    state = g.SEED * 6364136223846793005 + 1442695040888963407
    probes = []

    def nxt(bound):
        nonlocal state
        state = (state * 6364136223846793005 + 1442695040888963407) & 0xFFFFFFFFFFFFFFFF
        return (state >> 33) % bound

    for _ in range(count):
        hi = [nxt(256) for _ in range(g.CONTEXT_WORDS)]
        lo = [nxt(256) for _ in range(g.CONTEXT_WORDS)]
        tok = [[nxt(g.TOKEN_CARDINALITY[f]) for f in g.TOKEN_FIELDS] for _ in range(g.CONTEXT_WORDS)]
        probes.append({"hi": hi, "lo": lo, "tok": tok})
    return probes


def logits_digest(logits) -> str:
    return g.sha256_bytes("".join(f"{x:.6e}\n" for x in logits).encode())


def verify_frozen(directory) -> dict:
    """Fail-closed integrity check of the frozen research candidate (record <-> model bytes <-> plan bytes <-> policy <-> parity)."""
    base = pathlib.Path(directory)
    frozen = g.verify_frozen_artifact(base / FROZEN_NAMES["frozen"])
    plan = json.loads((base / FROZEN_NAMES["plan"]).read_text())
    if frozen["policy"] != {"id": frozen["policy"]["id"], "percent": BUDGET_PERCENT[frozen["policy"]["id"]], "kind": "rank budget"}:
        raise SystemExit("frozen policy does not match the pre-registered budget table")
    if frozen["representation_sha256"] != g.representation_sha256() or frozen["token_schema_sha256"] != g.token_schema_sha256():
        raise SystemExit("frozen representation identity drift")
    if frozen["rank_plan_sha256"] != plan_digest(plan):
        raise SystemExit("frozen record names a different rank plan")
    parity = json.loads((base / FROZEN_NAMES["parity"]).read_text())
    if frozen["parity_artifact_sha256"] != g.sha256_bytes((base / FROZEN_NAMES["parity"]).read_bytes().replace(b"\r\n", b"\n")):
        raise SystemExit("parity artifact digest mismatch")
    if parity["policy"] != frozen["policy"] or parity["model_sha256"] != frozen["model_artifact"]["sha256"]:
        raise SystemExit("parity artifact does not describe the frozen model/policy")
    return frozen
