#!/usr/bin/env python3
"""SEG-086: leakage-safe calibration, selection rule, decision ladder and benchmark accounting (synthetic data only)."""
import pathlib
import random
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import segarecomp_seg086_cal as cal  # noqa: E402
import segarecomp_seg086_norm as nm  # noqa: E402

FAILURES = []


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)


def raises(fn, exc=Exception):
    try:
        fn()
    except exc:
        return True
    return False


def rec(id_, lineage, n=200, code_fraction=0.2, weak=None, seed=1, seeds=(0,), excludes=None):
    """Synthetic record: code cells first (label 1), plateau after; `weak` lowers one code cell to plateau + weak."""
    rng = random.Random(seed)
    k = int(n * code_fraction)
    sc = [rng.gauss(10.0, 0.8) if c < k else rng.gauss(-11.5, 0.6) for c in range(n)]
    labels = [1 if c < k else 0 for c in range(n)]
    if weak is not None:
        sc[k - 1] = -11.5 + weak
    return {"id": id_, "lineage": lineage, "scores": sc, "labels": labels, "seeds": set(seeds), "classes": b"I" * 128 * n, "has_universe": bool(seeds),
            "model_excludes": frozenset(excludes if excludes is not None else {lineage})}


def test_calibrate_tau_rule():
    r = cal.calibrate_tau({"a": 5.0, "b": 8.0, "c": 7.0})
    check(r["T0"] == 5.0 and r["shortfall"] == 2.0 and r["tau"] == 3.0, f"tau {r}")       # for a: min(others)=7 - 5 = 2
    check(cal.calibrate_tau({"a": 4.0, "b": 4.0})["tau"] == 4.0, "equal minima need no margin")
    check(raises(lambda: cal.calibrate_tau({"a": 1.0}), ValueError), "single lineage must fail closed")
    check(raises(lambda: cal.calibrate_tau({"a": 1.0, "b": float("inf")}), ValueError), "non-finite minimum")
    ref = cal.dyn.calibrate_threshold([("a", 5.0), ("b", 8.0), ("c", 7.0)])
    check(abs(ref["T"] - r["tau"]) < 1e-12, "identical to the SEG-085 DYN_SCORE rule")


def test_leakage_guard():
    held = "H"
    ok = [rec("c1", "C1", excludes={"H", "C1"}), rec("c2", "C2", excludes={"H", "C2"}, seed=2)]
    check(cal.calibrate_fold(held, ok, "N2")["lineages"] == 2, "clean calibration refused")
    check(raises(lambda: cal.calibrate_fold(held, ok + [rec("h", "H", excludes={"H"})], "N2"), cal.LeakageError), "held-out lineage inside its own calibration set")
    leaky = [rec("c1", "C1", excludes={"C1"}), rec("c2", "C2", excludes={"H", "C2"})]
    check(raises(lambda: cal.calibrate_fold(held, leaky, "N2"), cal.LeakageError), "model that trained on the held-out lineage accepted")
    leaky2 = [rec("c1", "C1", excludes={"H"}), rec("c2", "C2", excludes={"H", "C2"})]
    check(raises(lambda: cal.calibrate_fold(held, leaky2, "N2"), cal.LeakageError), "model that trained on the calibration lineage's own data (out-of-fold missing) accepted")
    check(raises(lambda: cal.calibrate_fold(held, [dict(r, model_excludes=frozenset()) for r in ok], "N2"), cal.LeakageError), "missing isolation record accepted")
    # the threshold must not depend on the held-out lineage's data at all
    a = cal.calibrate_fold(held, ok, "N2")["tau"]
    b = cal.calibrate_fold(held, [dict(r) for r in ok], "N2")["tau"]
    check(a == b, "deterministic")


def test_margin_vs_plain_and_low_tail_not_trimmed():
    a = rec("a", "A", weak=1.0, seed=1, excludes={"H", "A"})
    b = rec("b", "B", seed=2, excludes={"H", "B"})
    c = rec("c", "C", seed=3, excludes={"H", "C"})
    with_margin = cal.calibrate_fold("H", [a, b, c], "N2")
    plain = cal.calibrate_fold("H", [a, b, c], "N2", margin=False)
    check(plain["tau"] == plain["T0"] and with_margin["tau"] < plain["tau"], "margin must lower the threshold below the plain minimum")
    check(abs(plain["T0"] - cal.lineage_minima([a, b, c], "N2")["A"]) < 1e-12, "the extreme low-tail positive of lineage A defines T0 (not trimmed)")
    check(plain["T0"] < min(cal.lineage_minima([b, c], "N2").values()) - 3.0, "the weak positive is far below the other lineages' minima")


def test_evaluate_counts_and_exclusions():
    good = rec("g", "G", n=200, code_fraction=0.2, seeds=(0,))
    dense = rec("d", "G", n=200, code_fraction=0.5, seeds=(0,))
    tiny = rec("t", "G", n=10, code_fraction=0.5, seeds=(0,))
    noseed = rec("n", "G", n=200, seeds=())
    flat = dict(rec("f", "G", n=200), scores=[-11.0] * 200)
    ev = cal.evaluate([good, tiny, noseed, flat], "N2", 5.0)
    ev["rows"].update(cal.evaluate([dense], "N2", -50.0)["rows"])                  # a threshold below the plateau selects every cell: cap abstention
    st = {k: r["status"] for k, r in ev["rows"].items()}
    check(st["g"] == "selected" and st["d"] == "abstain_cap" and st["t"] == "normalisation_failed" and st["n"] == "unsupported", f"statuses {st}")
    sm = ev["summary"]
    check(sm["status_counts"]["unsupported"] == 1 and sm["below_minimum_cells"] == 1, "counts")
    check(ev["rows"]["d"]["all_code"] is False and ev["rows"]["d"]["retained"] == 1.0, "cap abstention is retained 1.0")
    allcode = rec("a", "G", n=100, code_fraction=0.7, seeds=(0,))
    check(cal.evaluate([allcode], "N2", 5.0)["rows"]["a"]["all_code"] is True, "all-code image flagged")
    check(cal.evaluate([allcode], "N2", 5.0)["summary"]["images_evaluated"] == 0, "all-code image is not in the mean")


def test_gate_inputs_and_decision_ladder():
    def row(L, status, ret, cells=200, missed=0, all_code=False, reason=None):
        return {"lineage": L, "status": status, "retained": ret, "all_code": all_code, "cells": cells, "reason": reason, "missed_starts": missed}
    L8 = [f"L{i}" for i in range(8)]
    sel = {f"s{i}": row(L8[i], "selected", r) for i, r in enumerate((0.12, 0.2, 0.28, 0.33, 0.38, 0.45))}
    sel.update({"s6": row("L6", "selected", 0.3), "s7": row("L7", "selected", 0.25)})
    gi = cal.gate_inputs(sel)
    check(gi["selected_images"] == 8 and abs(gi["retained_spread"] - 0.33) < 1e-9 and gi["evaluable_lineages"] == 8, f"gate inputs {gi}")
    good = cal.decide(gi, 0.0, True, True)
    check(good["decision"] == "GATES_PASSED_ECONOMICS_REQUIRED", f"{good}")
    check(cal.decide(gi, 0.5, True, True)["decision"] == "RANKING_LIMITATION", "oracle over cap")
    check(cal.decide(gi, 0.0, False, True)["decision"] == "EVIDENCE_INSUFFICIENT", "non-deterministic")
    check(cal.decide(gi, 0.0, True, False)["decision"] == "EVIDENCE_INSUFFICIENT", "isolation not established")
    miss = dict(sel, m=row("L0", "selected", 0.3, missed=2))
    check(cal.decide(cal.gate_inputs(miss), 0.0, True, True)["decision"] == "RECALL_FAILED", "a missed start fails recall")
    abst = {f"a{i}": row(L8[i % 8], "abstain_cap", 1.0) for i in range(8)}
    abst.update({"s": row("L0", "selected", 0.3)})
    d = cal.decide(cal.gate_inputs(abst), 0.0, True, True)
    check(d["decision"] == "CALIBRATION_INEFFECTIVE" and not d["gates"]["A_abstention"], f"excess abstention {d}")
    allabs = {f"a{i}": row(L8[i % 8], "abstain_cap", 1.0) for i in range(10)}
    check(cal.decide(cal.gate_inputs(allabs), 0.0, True, True)["decision"] == "CALIBRATION_INEFFECTIVE", "universal abstention is ineffective, not a recall pass")
    one = {f"b{i}": row(L8[i % 8], "selected", 0.35) for i in range(10)}
    check(cal.decide(cal.gate_inputs(one), 0.0, True, True)["decision"] == "CALIBRATION_INEFFECTIVE", "a disguised fixed budget has no dynamics")
    check(cal.decide(cal.gate_inputs(sel), 0.0, True, True)["decision"] == "GATES_PASSED_ECONOMICS_REQUIRED" and
          cal.decide({**gi, "evaluable_lineages": 3}, 0.0, True, True)["decision"] == "EVIDENCE_INSUFFICIENT", "too few lineages")
    dev = {"decision": "GATES_PASSED_ECONOMICS_REQUIRED"}
    check(cal.decide_with_economics(dev, 0.2, 0.0, True)["decision"] == "SCORE_CALIBRATION_FEASIBLE", "feasible")
    check(cal.decide_with_economics(dev, 0.05, 0.0, True)["decision"] == "CALIBRATION_INEFFECTIVE", "no meaningful saving")
    check(cal.decide_with_economics(dev, 0.2, 0.2, True)["decision"] == "CALIBRATION_INEFFECTIVE", "regression against the deployed policy")
    check(cal.decide_with_economics(dev, 0.2, 0.0, False)["decision"] == "RECALL_FAILED", "runtime equivalence failure")
    check(cal.decide_with_economics({"decision": "CALIBRATION_INEFFECTIVE"}, 0.9, 0, True)["decision"] == "CALIBRATION_INEFFECTIVE", "economics cannot rescue a failed gate")


def test_choose_variant_rule():
    s = lambda m: {"lineage_balanced_mean_retained": m}
    check(cal.choose_variant({"N1": s(0.30), "N2": s(0.35)}, {"N1": 0, "N2": 0})["chosen"] == "N1", "clearly better N1")
    check(cal.choose_variant({"N1": s(0.345), "N2": s(0.35)}, {"N1": 0, "N2": 0})["chosen"] == "N2", "difference below 0.01 prefers N2")
    check(cal.choose_variant({"N1": s(0.20), "N2": s(0.35)}, {"N1": 1, "N2": 0})["chosen"] == "N2", "a failing candidate is disqualified")
    check(cal.choose_variant({"N1": s(0.2), "N2": s(0.3)}, {"N1": 1, "N2": 1})["chosen"] is None, "no candidate")


def test_benchmark_accounting_after_failed_admission_and_fallback():
    res = {"strategies": {"BROAD": {"total_cpu": 100.0}, "R1": {"accepted": True, "total_cpu": 40.0}, "R2": {"accepted": False, "plan_cpu": 5.0}},
           "policy_to_region": {"SEL": "R1", "REJ": "R2"}}
    check(cal.policy_cost(res, "SEL")[:2] == (40.0, "selective"), "selective")
    check(cal.policy_cost(res, "REJ")[:2] == (105.0, "rejected_then_broad"), "failed admission is charged in full: rejected plan CPU plus the broad fallback")
    check(cal.policy_cost(res, "ABS")[:2] == (100.0, "broad_abstain"), "abstention costs the broad build")
    check(cal.policy_cost(res, "SEL", overhead_cpu=1.5)[0] == 41.5 and cal.policy_cost(res, "ABS", overhead_cpu=1.5)[0] == 101.5, "inference/normalisation overhead is charged even on fallback")


def test_oracle_over_cap_share():
    ok = rec("o", "A", code_fraction=0.2)
    heavy = dict(rec("h", "A", code_fraction=0.3, weak=-21.0), )                 # weak code cell scores below almost everything: the oracle threshold retains nearly all cells
    check(cal.oracle_over_cap_share([ok], "N2") == 0.0, "adequate ranking")
    check(cal.oracle_over_cap_share([ok, heavy], "N2") == 0.5, "one of two images needs more than the cap even with the oracle threshold")


def main() -> int:
    for fn in [v for k, v in sorted(globals().items()) if k.startswith("test_")]:
        fn()
    if FAILURES:
        print("\n".join(FAILURES))
        return 1
    print("segarecomp_seg086_cal_test: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
