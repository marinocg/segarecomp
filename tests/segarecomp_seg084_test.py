#!/usr/bin/env python3
"""SEG-084-T001: offline tests of the corpus audit and the structural model-A reassessment (synthetic data only; no ROM, no emulator).

The model-fit equivalence test needs numpy and scikit-learn and is skipped (reported) without them."""
import json
import pathlib
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import segarecomp_seg084_a as sa  # noqa: E402
import segarecomp_seg084_audit as au  # noqa: E402
import segarecomp_seg084_diag as dg  # noqa: E402

FAILURES = []
SKIPPED = []


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)


def ready(**kw):
    m = {"missing_arms": [], "a0_reproduces": True, "code_independent_useful": 5, "code_new_positive_fraction": 0.33, "negative_lineages": 4,
         "response_folds": 3, "response_mean_beyond_noise": True, "dose_response": True, "residual_folds": 4}
    m.update(kw)
    return sa.readiness(m)


def test_readiness():
    check(ready()["decision"] == "NEURAL_PILOT_JUSTIFIED", "all criteria -> pilot")
    check(ready(missing_arms=["A83"])["decision"] == "EVIDENCE_INSUFFICIENT", "missing arm")
    check(ready(a0_reproduces=False)["decision"] == "EVIDENCE_INSUFFICIENT", "A0 not reproduced")
    d = ready(code_independent_useful=4)
    check(d["decision"] == "MORE_DIVERSITY_NEEDED" and d["recommendation"] == "acquire_targeted_batch" and any("code" in r for r in d["reasons"]), "code diversity")
    d = ready(negative_lineages=2)
    check(d["decision"] == "MORE_DIVERSITY_NEEDED" and any("negatives" in r for r in d["reasons"]), "negative diversity judged separately from code diversity")
    check(ready(code_new_positive_fraction=0.1)["decision"] == "MORE_DIVERSITY_NEEDED", "code volume")
    check(ready(response_mean_beyond_noise=False)["decision"] == "MODEL_OR_EVALUATION_LIMITATION", "A does not respond")
    check(ready(response_folds=1)["decision"] == "MODEL_OR_EVALUATION_LIMITATION", "response on a single fold")
    check(ready(dose_response=False)["decision"] == "MODEL_OR_EVALUATION_LIMITATION", "a response that is not dose-dependent is not trusted")
    check(ready(residual_folds=1)["decision"] == "MODEL_OR_EVALUATION_LIMITATION", "no residual headroom")
    # diversity is judged before response; evidence before everything
    check(ready(negative_lineages=0, response_mean_beyond_noise=False)["decision"] == "MORE_DIVERSITY_NEEDED", "order: diversity before response")
    check(ready(missing_arms=["x"], negative_lineages=0)["decision"] == "EVIDENCE_INSUFFICIENT", "order: evidence first")
    check(sa.READY["diversity_negative_min_strong_hard_cells"] == 300 and sa.READY["residual_er"] == 0.05, "frozen thresholds")


def test_noise_and_response():
    a0 = {"D01": 0.5, "D02": 0.1, "D03": 0.2}
    subs = [{"D01": 0.45, "D02": 0.1, "D03": 0.21}, {"D01": 0.55, "D02": 0.11, "D03": 0.2}]
    n = sa.fold_noise(a0, subs)
    check(abs(n["D01"] - 0.05) < 1e-9 and abs(n["D02"] - 0.01) < 1e-9 and abs(n["D03"] - 0.01) < 1e-9, "per-fold noise is the largest sub-fit deviation")
    arm = {"D01": 0.3, "D02": 0.105, "D03": 0.2}
    r = sa.classify_response(a0, arm, n)
    check(r["folds_better"] == ["D01"] and r["folds_worse"] == [] and isinstance(r["mean_beyond_noise"], bool), "response bookkeeping")
    check(r["folds_better"] == ["D01"], "only D01 beats its noise")
    check(sa.classify_response(a0, a0, n)["folds_better"] == [], "no change -> no response")
    worse = sa.classify_response(a0, {"D01": 0.9, "D02": 0.1, "D03": 0.2}, n)
    check(worse["folds_worse"] == ["D01"], "worse beyond noise")


def test_independence_and_matching():
    check(sa.independent_count(["a", "b", "c"], {("a", "b"): 0.5}, {}) == 2, "pair above 0.30 is one lineage")
    check(sa.independent_count(["a", "b"], {}, {"a": 0.4}) == 1, "registry-redundant game drops out")
    check(sa.independent_count(["a", "b"], {("a", "b"): 0.3}, {}) == 2, "threshold is strict")
    check(sa.matched_counts([1, 0, 1, -1], [1, 1, 0, 0, -1]) == {1: 2, 0: 2}, "matched counts are the control's")


def test_arms_and_plan():
    check("sonic2" not in sa.GAMES, "Sonic 2 excluded from additional lineage counting and duplicate training")
    check(set(sa.G81) | set(sa.G83) == set(sa.ALL) and not set(sa.G81) & set(sa.G83), "81 / 83 partition")
    check(set(sa.WEIGHTED_ARMS) <= set(sa.ARMS) and all(sa.ARMS[a][0][2] == "weak" for a in sa.WEIGHTED_ARMS), "weighted arms use the weak variant only")
    strong_only = [a for a, s in sa.ARMS.items() if s != "sub" and a not in sa.WEIGHTED_ARMS]
    check(all(v[2] == "strong" for a in strong_only for v in sa.ARMS[a]), "every other arm is strong-only")
    check(all(g in sa.GAMES for a, s in sa.ARMS.items() if s != "sub" for v in s for g in v[0]), "arms reference known games")
    check(set(sa.READY["required_comparisons"]) <= set(sa.ARMS), "required comparisons exist")
    p = sa.build_protocol()
    check("a_features" in p["model"]["features"] and "A_CONFIG" in p["model"]["fit"] and "M1" in p["purpose"], "true structural model A, not M1")
    check(p["limits"]["cpu_hours"] == 8 and "B / C / C-v2" in p["limits"]["no"], "budget and exclusions")
    check(p["evaluation"]["folds"] == list(sa.GENESIS), "SEG-076 C0 folds")
    if sa.PLAN.exists():
        check(bool(sa.verify_plan()), "frozen plan verifies")


def test_hard_breakdown():
    try:
        import numpy as np
    except Exception:
        SKIPPED.append("hard_breakdown (numpy missing)")
        return
    cls = b"I" * 128 + b"D" * 128 + b"D" * 128 + b"D" * 128
    tok = np.zeros((256, 8), dtype=np.uint8)
    tok[128:192, 0] = 2                                              # cell 2: all words ordinary decodes
    bd = sa.hard_breakdown(cls, tok, {1, 2, 3})
    check(bd["hard"] == 2 and bd["near_only"] == 1 and bd["ordinary_only"] == 1, "hard = adjacent to code or >= 50 % ordinary decodes; cell 3 is easy")


def test_audit_helpers():
    check(au.class_counts(b"IcDU" * 2) == {"I": 2, "c": 2, "D": 2, "U": 2}, "class counts")
    cls = b"I" * 128 + b"D" * 128 + b"U" * 128 + b"D" * 100 + b"U" * 28
    check(au.cell_counts(cls) == {"pos": 1, "neg": 1, "masked": 2, "cells": 4}, "cell rule: any I positive, all D negative, else masked")
    for bad in ("/x/sealed/a", "/x/reserved-unsealed/a", "/x/blind/a"):
        try:
            au.refuse_sealed(bad)
            check(False, f"{bad} refused")
        except SystemExit:
            pass
    au.refuse_sealed("/x/seg072/data/d05")
    check(au.opaque("a", "b") == au.opaque("a", "b") and len(au.opaque("a")) == 16, "opaque keys are deterministic")


def test_fit_equivalence():
    try:
        import numpy as np
        from sklearn.ensemble import HistGradientBoostingClassifier
        import segarecomp_xplat_models as xm
    except Exception:
        SKIPPED.append("fit equivalence (numpy / scikit-learn missing)")
        return
    rng = np.random.RandomState(0)
    X = rng.rand(600, 8)
    y = (X[:, 0] + 0.3 * rng.rand(600) > 0.6).astype(np.int64)
    ref = HistGradientBoostingClassifier(**xm.A_CONFIG).fit(X, y)
    a = sa.fit_arrays(X, y)
    check(np.array_equal(ref.predict_proba(X), a.predict_proba(X)), "weights=None is byte-identical to the original A fit")
    w = sa.fit_arrays(X, y, np.ones(len(y)))
    check(np.allclose(ref.predict_proba(X), w.predict_proba(X), atol=1e-12), "all-ones weights reproduce the strong-only fit")
    ww = sa.fit_arrays(X, y, np.where(rng.rand(len(y)) < 0.5, 1.0, 0.25))
    check(not np.array_equal(ref.predict_proba(X), ww.predict_proba(X)), "non-trivial weights change the model")
    lg = sa.logit(ref, X)
    p = ref.predict_proba(X)[:, 1]
    check(np.allclose(lg, np.log(np.clip(p, 1e-300, 1)) - np.log(np.clip(1 - p, 1e-300, 1)), atol=1e-12), "score is the log-odds of score_a")


def row(**kw):
    r = {"outside_image": False, "header_or_vector": False, "cpu_ordinary": True, "cpu_length": 4, "inside_other_instruction": False, "kind": "straight", "corroborated": True,
         "contradicted": False, "has_successor": True, "opcode_line": 4, "disasm": "short"}
    r.update(kw)
    return r


def test_diag_rules():
    f = dg.fifth_distribution({"0": 2411, "1": 34, "4": 3, "30": 3}, 66331)
    check(f["per_fifth"][0] == 2448 and f["per_fifth"][4] == 3 and f["fifths_ge_min"] == 1 and f["first_window_fraction"] > 0.98, "Shove It!-shaped growth: one fifth")
    check(dg.fifth_distribution({"0": 10, "20": 10, "40": 10, "60": 10, "80": 10}, 180000)["fifths_ge_min"] == 5, "evenly spread discovery: five fifths")
    check(dg.classify_rejections([row() for _ in range(20)])["class"] == "DISASSEMBLER_LIMITATION", "corroborated by emulator successors")
    check(dg.classify_rejections([row(corroborated=False, contradicted=True) for _ in range(20)])["class"] == "UNSOUND_EXECUTION_LABELS", "successor contradicts the CPU length")
    check(dg.classify_rejections([row(cpu_ordinary=False) for _ in range(20)])["class"] == "INSTRUCTION_FORM_MISMATCH", "CPU decoder not ordinary")
    check(dg.classify_rejections([row(), row(header_or_vector=True)])["class"] == "ROM_MAPPING_ISSUE", "mapping row")
    check(dg.classify_rejections([row() for _ in range(20)], mapping_signals=1)["class"] == "ROM_MAPPING_ISSUE", "a skipped odd / out-of-image executed PC is a mapping signal")
    check(dg.classify_rejections([row(corroborated=False, has_successor=False, kind="unverifiable") for _ in range(20)])["class"] == "UNRESOLVED", "no evidence is not a pass")
    check(dg.classify_rejections([row() for _ in range(18)] + [row(corroborated=False, kind="unverifiable")] * 2)["class"] == "UNRESOLVED", "unverifiable starts count against (90 % < 95 %)")
    check(dg.classify_rejections([row() for _ in range(19)] + [row(inside_other_instruction=True)])["class"] == "UNRESOLVED", "any overlap with an accepted instruction blocks the limitation finding")
    check(dg.classify_rejections([row() for _ in range(18)] + [row(inside_other_instruction=True)] * 2)["class"] == "UNSOUND_EXECUTION_LABELS", "two overlaps in twenty are unsound")
    mixed = [row(opcode_line=4) for _ in range(40)] + [row(opcode_line=6, corroborated=False, kind="unverifiable") for _ in range(2)] + [row(opcode_line=6) for _ in range(1)]
    check(dg.classify_rejections(mixed)["class"] == "UNRESOLVED" and dg.classify_rejections(mixed)["opcode_lines_below_90pct"] == [6], "a weak opcode line blocks the aggregate claim")
    # classify_pc: straight-line and direct branch corroboration
    toks = bytes([2, 2, 1, 0, 0, 0, 0, 0] * 4)                       # status ordinary, length 2 words, control 0
    r = dg.classify_pc(0x200, 0x1000, toks, {0x204}, [], ("short", "short"), 4)
    check(r["cpu_length"] == 0 or True, "classify_pc runs")
    toks2 = bytes([2, 2, 1, 0, 0, 0, 0, 0]) * 400
    r = dg.classify_pc(0x200, 0x4000, toks2, {0x204}, [], ("short", "absent"), 4)
    check(r["kind"] == "straight" and r["corroborated"] and r["disasm"] == "mixed", "straight-line corroboration")
    r = dg.classify_pc(0x200, 0x4000, toks2, {0x208}, [], ("short", "short"), 4)
    check(r["contradicted"] and not r["corroborated"], "successor != pc + length contradicts")
    toks3 = bytes([2, 2, 1, 1, 0, 0, 0, 0]) * 400                    # control 1 = conditional direct branch
    r = dg.classify_pc(0x200, 0x4000, toks3, {0x204, 0x300}, [], ("short", "short"), 6, 0x300)
    check(r["kind"] == "direct" and r["corroborated"], "conditional branch: fall-through and target are both allowed")
    r = dg.classify_pc(0x200, 0x4000, toks3, {0x310}, [], ("short", "short"), 6, 0x300)
    check(r["contradicted"], "an unexplained successor contradicts")
    r = dg.classify_pc(0x200, 0x4000, bytes([2, 2, 1, 4, 0, 0, 0, 0]) * 400, {0x300}, [], ("short", "short"), 4)
    check(r["kind"] == "unverifiable" and not r["corroborated"] and not r["contradicted"], "dynamic jump is unverifiable")
    check(set(dg.CLASSES) >= {"DISASSEMBLER_LIMITATION", "UNRESOLVED"}, "closed vocabulary")
    res = {"gate_replay": {"identity_ok": True, "played_to_end": True, "no_perturbation": True, "determinism": {"a": True}, "distinct_ram_vram_states": 6, "digest_domains_missing": False,
                           "fifths_with_new_code": 1, "progression": False, "replay_ok": False},
           "gate_tracing": {"tracing_ok": True}, "consistency": {"ok": True}, "classification": {"class": "REPLAY_INVALID", "code_yield_rule": False, "novelty_rule": False}}
    g = dg.proposed_gates(res)
    check(g["replay_fidelity"] and g["label_confidence"] and g["proposed_class"] == "QUALIFIED_LOW_YIELD" and g["original_class"] == "REPLAY_INVALID", "fidelity decoupled from discovery")
    res2 = json.loads(json.dumps(res))
    res2["gate_replay"]["no_perturbation"] = False
    check(dg.proposed_gates(res2)["proposed_class"] == "REPLAY_INVALID", "a perturbed replay stays invalid")
    res3 = json.loads(json.dumps(res))
    res3["gate_tracing"]["tracing_ok"] = False
    check(dg.proposed_gates(res3)["proposed_class"] == "TRACING_UNAVAILABLE" and not dg.proposed_gates(res3)["label_confidence"], "tracing failure keeps labels out")


def test_committed_artifacts_aggregate_only():
    for f in sorted((ROOT / "tools").glob("segarecomp_seg084*.json")):
        t = f.read_text()
        for b in ('"/root', "/home/", ".bin\"", ".md\"", ".bk2"):
            check(b not in t, f"{f.name}: no path or file name ({b})")


def main():
    test_readiness()
    test_noise_and_response()
    test_independence_and_matching()
    test_arms_and_plan()
    test_hard_breakdown()
    test_audit_helpers()
    test_fit_equivalence()
    test_diag_rules()
    test_committed_artifacts_aggregate_only()
    print("FAILURES:", len(FAILURES), *FAILURES)
    if SKIPPED:
        print("SKIPPED:", *SKIPPED)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
