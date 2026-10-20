#!/usr/bin/env python3
"""SEG-075 (ADR 0161): pre-registered, pure decision procedures for T004-T006 (standard library only; bound into the frozen plan by digest).

Everything that turns development / one-shot records into a choice lives here and is fixed before any fit:

  select        candidate selection by nested pools (SEG-072 `select_pair` style; transitive): keep the configurations whose Genesis B50
                misses are within the seed-replicate allowance sum of the minimum; of those, keep the ones whose Genesis mean ER is
                within tau (<=) of the minimum; of those, the ones whose xplat_lolo mean ER is within tau (<=) of the minimum; then the
                simplest (COMPLEXITY). A record with a None value on a key is ranked after every record with a value on that key (it
                survives the pool only if no record has a value)
  better_than   the STRICT relation used by the outcome rules: candidate c is better than reference r iff c has fewer Genesis B50 misses
                than r by more than the allowance sum, or (not more by more than the allowance sum AND (r.er - c.er > tau OR
                (|r.er - c.er| <= tau AND r.xer - c.xer > tau)))
  budget_rank   complete-budget order B35 < B50 < B60 < None (None = no fixed budget reaches complete recall: worst)
  outcome       the T006 outcome classes, rules in order (plan decisions.t006.outcome_rule), exactly one class
  reserved_er   reserved aggregation: per reserved lineage max ER over informative samples, then the mean over the reserved lineages

M3_FROZEN is a reference and comparator only (frozen weights cannot be refit; lineage-correlated on D01/D03, ADR 0153); it never enters
selection. ER values of M4_DENSE@64 are measured in 64-byte cells (finer than the 128-byte cells of the others): they are ranked on the same
keys as pre-registered, and this incommensurability is disclosed in ADR 0161.
"""
from __future__ import annotations

COMPLEXITY = ["M1_HGB", "A", "M3_R", "M4_DENSE@128", "M4_DENSE@64", "B", "C"]
REFERENCE_ONLY = ("M3_FROZEN",)
BUDGETS = ("B35", "B50", "B60")
B_GATE_ER = 0.10
ECON_GATE = {"generated_c_reduction": 0.10, "compile_cpu_reduction": 0.10}


def _pool(records: list[dict], key: str, slack: float) -> list[dict]:
    vals = [r[key] for r in records if r[key] is not None]
    if not vals:
        return records
    best = min(vals)
    return [r for r in records if r[key] is not None and r[key] <= best + slack + 1e-12]


def select(records: list[dict], tau: float, allowance_sum: int) -> dict:
    """records: [{config, condition, genesis_b50_misses, genesis_mean_er, xplat_mean_er}]; returns the selected record (pure; tested)."""
    pool = [r for r in records if r["config"] not in REFERENCE_ONLY]
    if not pool:
        raise ValueError("no candidate")
    pool = _pool(pool, "genesis_b50_misses", allowance_sum)
    pool = _pool(pool, "genesis_mean_er", tau)
    pool = _pool(pool, "xplat_mean_er", tau)
    return min(pool, key=lambda r: (COMPLEXITY.index(r["config"]), r["condition"]))


def better_than(c: dict, r: dict, tau: float, allowance_sum: int) -> bool:
    """Strict pre-registered improvement of candidate c over reference r (pure; tested)."""
    dm = c["genesis_b50_misses"] - r["genesis_b50_misses"]
    if dm < -allowance_sum:
        return True
    if dm > allowance_sum or c["genesis_mean_er"] is None or r["genesis_mean_er"] is None:
        return False
    der = r["genesis_mean_er"] - c["genesis_mean_er"]
    if der > tau:
        return True
    if abs(der) <= tau and c.get("xplat_mean_er") is not None and r.get("xplat_mean_er") is not None:
        return r["xplat_mean_er"] - c["xplat_mean_er"] > tau
    return False


def budget_rank(b) -> int:
    return BUDGETS.index(b) if b in BUDGETS else len(BUDGETS)


def best_condition(transfer_class: str) -> str:
    """A configuration enters selection in C2 if its own transfer class is HELPS or NEUTRAL, else in C0."""
    return "C2" if transfer_class in ("TRANSFER_HELPS", "TRANSFER_NEUTRAL") else "C0"


def b_gate(selected: dict) -> bool:
    """Model B is built iff the selected best of {M1_HGB, A, M3_R, M4_DENSE@128, M4_DENSE@64} has Genesis mean ER >= 0.10."""
    return selected["genesis_mean_er"] is not None and selected["genesis_mean_er"] >= B_GATE_ER


def reserved_er(per_lineage_max_er: dict):
    v = [x for x in per_lineage_max_er.values() if x is not None]
    return sum(v) / len(v) if v else None


def contradicted(cand: dict, comp: dict, tau: float) -> bool:
    """One-shot contradiction: on any reserved lineage more B50 misses than the comparator, or reserved ER worse by > tau."""
    for lin, m in cand["reserved_b50_misses"].items():
        if m > comp["reserved_b50_misses"].get(lin, 0):
            return True
    ce, re_ = reserved_er(cand["reserved_max_er"]), reserved_er(comp["reserved_max_er"])
    return ce is not None and re_ is not None and ce - re_ > tau


def economics_confirmed(cand_budget, comp_budget, bench: dict | None) -> bool:
    """Economically better: strictly smaller complete budget AND the SEG-074 benchmark confirms both >= 10% reductions."""
    if budget_rank(cand_budget) >= budget_rank(comp_budget) or bench is None:
        return False
    return (bench.get("generated_c_reduction", 0.0) >= ECON_GATE["generated_c_reduction"]
            and bench.get("compile_cpu_reduction", 0.0) >= ECON_GATE["compile_cpu_reduction"])


def outcome(t004_overall: str, cand: dict, comp: dict, tau: float, allowance_sum: int, econ: bool, contra: bool,
            xplat_c2_vs_c0_gain: float | None) -> dict:
    """T006 outcome, rules in order (pure; tested). cand/comp: development records (+ condition); comp = C0 M3_R."""
    if t004_overall == "EVIDENCE_INSUFFICIENT":
        return {"class": "DATA_INSUFFICIENT", "rule": 1}
    better = better_than(cand, comp, tau, allowance_sum) and cand["config"] not in REFERENCE_ONLY
    if better and econ and not contra:
        by_type = {"A": ("LIGHTWEIGHT_STRUCTURAL_JUSTIFIED", 4), "B": ("LOCAL_TRANSFORMER_JUSTIFIED", 5),
                   "C": ("GRAPH_TRANSFORMER_JUSTIFIED", 6)}
        if cand["config"] in by_type:
            cls, rule = by_type[cand["config"]]
            return {"class": cls, "rule": rule}
        if cand["condition"] in ("C1", "C2"):
            return {"class": "CORPUS_IMPROVES_EXISTING", "rule": 3}
        # an existing architecture trained on Genesis only (C0) is a re-selection outside SEG-075's question
        return {"class": "NO_ECONOMIC_IMPROVEMENT", "rule": 7, "note": "C0-trained existing model; not a corpus or new-model result"}
    if better:
        return {"class": "NO_ECONOMIC_IMPROVEMENT", "rule": 7, "note": "contradicted by the one-shot" if contra else "not economically better"}
    if xplat_c2_vs_c0_gain is not None and xplat_c2_vs_c0_gain >= tau:
        return {"class": "CORPUS_EXPANSION_ONLY", "rule": 8}
    return {"class": "NO_ECONOMIC_IMPROVEMENT", "rule": 9}
