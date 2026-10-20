#!/usr/bin/env python3
"""SEG-075-T003/T004 (ADR 0161/0162): tests of the curation, evaluation barrier, pre-registration and transfer decision logic
(standard library only, synthetic data).

Covers instruction-sequence shingles (relocated operands do not break them), deterministic near-duplicate reduction, the feasibility gate,
the evaluation barrier (sealed paths, reserved lineages and sealed sample ids are refused), the committed plan's digest and invariants
(reserved lineages never in a training condition, conditions nested, C0 reuse guarded by bitwise reproduction), the per-image metrics
(FRF, misses, negative admissions, short/mixed stratum, AUROC) and the pre-registered transfer classification.
"""

import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import segarecomp_xplat_design as d  # noqa: E402
import segarecomp_xplat_transfer as x  # noqa: E402

FAILURES = []


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)
        print("FAIL:", msg)


def refused(fn, *args):
    try:
        fn(*args)
    except SystemExit:
        return True
    return False


def prog(words: list[int], ext: dict[int, bytes] = None) -> tuple[bytes, bytes]:
    """Image + classes for a run of one-word instructions; ext adds extension bytes after instruction i."""
    ext = ext or {}
    img, cls = bytearray(), bytearray()
    for i, w in enumerate(words):
        img += w.to_bytes(2, "big") + ext.get(i, b"")
        cls += b"I" + b"c" * (1 + len(ext.get(i, b"")))
    return bytes(img), bytes(cls)


def test_sequences():
    words = [0x2200 + k for k in range(20)]
    a = d.instruction_sequences(*prog(words, {3: b"\x00\x00\x10\x00"}))
    b = d.instruction_sequences(*prog(words, {3: b"\x00\x00\x20\x00"}))
    check(a and a == b, "relocated operand bytes do not change instruction-sequence shingles")
    img, cls = prog(words)
    cut = cls[:10] + b"D" * 2 + cls[12:]
    check(len(d.instruction_sequences(img, cut)) < len(d.instruction_sequences(img, cls)), "a data break splits the instruction run")
    s1, s2, s3 = set(range(100)), set(range(10, 100)), set(range(200, 300))
    kept, dropped = d.representatives([("b", s2, 90), ("a", s1, 100), ("c", s3, 50)])
    check(kept == ["a", "c"] and dropped == {"b": 1.0}, "largest-first greedy keeps independent images, drops contained ones")
    check(d.representatives([("y", s1, 10), ("x", s1, 10)])[0] == ["x"], "ties broken by id (deterministic)")


def table(**over):
    base = {"toslibc": ("atari-st", {"I": 0.28, "c": 0.6, "D": 0.06, "U": 0.05}), "emutos": ("atari-st", {"I": 0.24, "c": 0.55, "D": 0.17, "U": 0.04}),
            "BattleIsleEditor": ("amiga", {"I": 0.2, "c": 0.38, "D": 0.41, "U": 0.0}),
            "amiga-realtime3d": ("amiga", {"I": 0.14, "c": 0.29, "D": 0.57, "U": 0.0}),
            "neogeosdk": ("neo-geo", {"I": 0.23, "c": 0.63, "D": 0.13, "U": 0.0}),
            "space-invaders": ("atari-st", {"I": 0.27, "c": 0.57, "D": 0.14, "U": 0.02}),
            "HelloAmi": ("amiga", {"I": 0.04, "c": 0.08, "D": 0.88, "U": 0.0})}
    rows = {k: {"platform": p, "fractions": f} for k, (p, f) in base.items()}
    for k, v in over.items():
        rows[k]["fractions"] = dict(rows[k]["fractions"], **v)
    return {"lineages": rows, "max_pair_containment": 0.001}


def test_feasibility():
    g = d.feasibility(table(), 50, 0)
    check(g["admit_t004"] and g["admit_t006_one_shot"] and g["evaluation_credibility"] == "LIMITED", "measured corpus admits T004/T006 (LIMITED)")
    check(set(g["reserved_lineages"]) == {"neogeosdk", "space-invaders"}, "reserved lineages")
    check("neogeosdk" not in g["development_lineages"] and "HelloAmi" not in g["development_lineages"], "reserved/diagnostic not development")
    bad = d.feasibility(table(toslibc={"U": 0.4}), 50, 0)
    check(not bad["admit_t004"], "label quality failure refuses T004")
    merged = table()
    merged["max_pair_containment"] = 0.5
    check(not d.feasibility(merged, 50, 0)["admit_t004"], "a merged (non-independent) lineage pair refuses T004")
    check(not d.feasibility(table(), 0, 50)["admit_t004"], "no Genesis folds refuses T004")


def test_plan():
    if not d.PLAN.is_file():
        print("skip plan checks: not frozen yet")
        return
    plan = d.verify_plan()
    reserved = {c for c, r in plan["lineages"].items() if r["role"] == "reserved_evaluation"}
    for cond in plan["conditions"].values():
        check(not reserved & set(cond["xplat_training"]), "no reserved lineage in any training condition")
        check(all(plan["lineages"][c]["role"] == "development" for c in cond["xplat_training"]), "conditions train development lineages only")
    c0, c1, c2 = (set(plan["conditions"][k]["xplat_training"]) for k in ("C0", "C1", "C2"))
    check(c0 == set() and c0 < c1 < c2, "conditions are nested (Genesis-only < +Atari ST < +Atari ST +Amiga)")
    check(plan["feasibility"]["admit_t004"] in (True, False) and plan["barrier"]["sealed_samples"] > 0, "barrier recorded")
    check("bit for bit" in plan["models"]["C0_reuse"] and "reproduce.json" in plan["models"]["C0_reuse"], "C0 reuse guarded by bitwise reproduction")
    check(set(plan["bound_code"]) >= {"segarecomp_xplat_transfer.py", "segarecomp_xplat_design.py"}, "runner and design code bound by digest")
    check("ER" in plan["metrics"]["er"] or "excess" in plan["metrics"]["er"], "ER is the decision metric")
    text = json.dumps(plan)
    check('"offset"' not in text and ".lst" not in text, "plan is aggregate-only")
    for r in reserved:
        check(isinstance(plan["lineages"][r]["fractions"], str), f"{r}: reserved label fractions are not published")


def test_barrier():
    if not d.PLAN.is_file():
        return
    check(refused(d.refuse_sealed, ["/x/snapshot/sealed/xp/neogeosdk/a"]), "sealed directory refused")
    check(refused(d.refuse_sealed, ["neogeosdk"]), "reserved lineage refused")
    check(refused(d.refuse_sealed, ["xp/space-invaders/objects-vbcc/out/game.o#text"]), "reserved sample id refused")
    check(not refused(d.refuse_sealed, ["/x/snapshot/dev/xp/toslibc/a", "emutos"]), "development material accepted")
    for s in ("/x/t002/corpus/xp/neogeosdk", "/x/corpus/xp/space-invaders", "xp/neogeosdk", "neogeosdk/a", "/x/sealed",
              "xp/neogeosdk/demo-compile-link/game#load", "/x/space-invaders@text"):
        check(refused(d.refuse_sealed, [s]), f"barrier refuses {s}")
    import tempfile, os
    with tempfile.TemporaryDirectory() as td:
        target = pathlib.Path(td) / "sealed" / "x"
        target.mkdir(parents=True)
        link = pathlib.Path(td) / "innocent"
        os.symlink(target, link)
        check(refused(d.refuse_sealed, [link]), "a symlink into sealed/ is refused")


def test_metrics():
    cls = b"I" + b"c" * 127 + b"D" * 128 + b"U" * 128 + b"I" + b"c" * 3 + b"D" * 124
    m = x.image_metrics(cls, [], [0.9, 0.1, 0.5, 0.2])
    check(m["positive_cells"] == 2 and m["negative_cells"] == 1 and m["frf"] == 0.75, "FRF counts the cells up to the weakest positive")
    check(m["informative"] and m["er"] == 0.5, "ER = (needed - positive) / (cells - positive) = (3 - 2) / 2")
    check(m["misses_B35"] == 1 and m["misses_B60"] == 0, "misses at fixed budgets (B35 keeps 2 of 4 cells)")
    check(m["negative_admitted_B60"] == 0 and m["stratum_cells"] == 2 and m["auroc"] == 1.0, "negative admissions, stratum, AUROC")
    check(x.auroc([1.0, 0.8], [0.1]) == 1.0 and x.auroc([0.1], [0.9]) == 0.0 and x.auroc([0.5], [0.5]) == 0.5, "AUROC with ties")
    seeded = x.image_metrics(cls, [3 * 128 + 4], [0.9, 0.1, 0.5, 0.2])
    check(seeded["misses_B35"] == 0, "seed cells (from seed addresses) are always retained")
    trivial = x.image_metrics((b"I" + b"c" * 127) * 2, [], [0.1, 0.2])
    check(not trivial["informative"] and trivial["er"] is None and trivial["frf"] == 1.0, "an all-positive image is trivial: no ER")
    m64 = x.image_metrics(cls, [3 * 128 + 70], [0.0] * 8, 64)
    check(m64["cells"] == 8, "64-byte cells")
    try:
        x.image_metrics(cls, [], [0.1, 0.2])
        check(False, "score / cell count mismatch refused")
    except SystemExit:
        pass
    rows = x.lineage_rows({"a": m, "b": trivial}, {"a": "L", "b": "L"})
    check(rows["L"]["max_er"] == 0.5 and rows["L"]["trivial_images"] == 1 and rows["L"]["images"] == 2, "lineage ER ignores trivial images")


def test_transfer_rule():
    ls = x.GENESIS_LINEAGES
    c0 = {l: {"max_er": 0.5, "misses_B50": 0} for l in ls}
    better = {l: {"max_er": 0.45, "misses_B50": 0} for l in ls}
    zero = {l: 0 for l in ls}
    check(x.classify_transfer(c0, better, 0.02, zero, False)["class"] == "TRANSFER_HELPS", "uniform improvement helps")
    one_worse = dict(better, D05={"max_er": 0.56, "misses_B50": 0})
    check(x.classify_transfer(c0, one_worse, 0.02, zero, False)["class"] == "TRANSFER_HARMS", "one lineage worse by > 2 tau harms")
    small = {l: {"max_er": 0.49, "misses_B50": 0} for l in ls}
    check(x.classify_transfer(c0, small, 0.02, zero, False)["class"] == "TRANSFER_NEUTRAL", "effect below tau is neutral")
    miss = {l: {"max_er": 0.45, "misses_B50": 1 if l == "D02" else 0} for l in ls}
    check(x.classify_transfer(c0, miss, 0.02, zero, False)["class"] == "TRANSFER_HARMS", "an extra B50 miss beyond the allowance harms")
    check(x.classify_transfer(c0, miss, 0.02, dict(zero, D02=1), False)["class"] == "TRANSFER_HELPS", "an extra miss within the seed allowance")
    other = {l: {"max_er": 0.45, "misses_B50": 1 if l == "D03" else 0} for l in ls}
    check(x.classify_transfer(c0, other, 0.02, dict(zero, D02=1), False)["class"] == "TRANSFER_HARMS", "allowances are per lineage")
    check(x.classify_transfer(c0, better, 0.02, zero, True)["class"] == "EVIDENCE_INSUFFICIENT", "noise/completeness rule -> insufficient")
    rows = {l: {"misses_B35": 1 if l == "D01" else 0, "misses_B50": 0, "misses_B60": 0} for l in ls}
    check(x.complete_budget(rows, 0) == "B50" and x.complete_budget(rows, 1) == "B35", "complete budget within the allowance sum")


def rec(cfg, misses, er, xer, cond="C2", budget="B50"):
    return {"config": cfg, "condition": cond, "genesis_b50_misses": misses, "genesis_mean_er": er, "xplat_mean_er": xer,
            "complete_budget": budget}


def test_decide():
    import segarecomp_xplat_decide as dc
    # the reviewer's would-be cycle: pools are nested, so the choice is well defined and transitive
    X, Y, Z = rec("M4_DENSE@64", 0, 0.200, 0.30), rec("M4_DENSE@128", 0, 0.209, 0.20), rec("M3_R", 0, 0.218, 0.10)
    check(dc.select([X, Y, Z], 0.01, 0)["config"] == "M4_DENSE@128", "nested pools: ER pool {X,Y} (Z > 0.21), then xplat -> Y")
    check(dc.select([Z, Y, X], 0.01, 0) == dc.select([X, Y, Z], 0.01, 0), "selection independent of input order")
    check(dc.select([rec("M3_R", 0, 0.20, 0.1), rec("M1_HGB", 0, 0.205, 0.1)], 0.01, 0)["config"] == "M1_HGB", "simplest within tau")
    check(dc.select([rec("M3_R", 0, 0.20, None), rec("M4_DENSE@128", 0, 0.20, 0.1)], 0.01, 0)["config"] == "M4_DENSE@128",
          "None ranks after a value")
    check(dc.select([rec("M3_R", 2, 0.10, 0.1), rec("M4_DENSE@128", 0, 0.30, 0.1)], 0.01, 1)["config"] == "M4_DENSE@128",
          "misses beyond the allowance sum decide first")
    check(dc.select([rec("M3_R", 1, 0.10, 0.1), rec("M4_DENSE@128", 0, 0.30, 0.1)], 0.01, 1)["config"] == "M3_R", "misses within allowance tie")
    try:
        dc.select([rec("M3_FROZEN", 0, 0.0, 0.0)], 0.01, 0)
        check(False, "M3_FROZEN is never a candidate")
    except ValueError:
        pass
    ref = rec("M3_R", 0, 0.20, 0.10, "C0")
    check(dc.better_than(rec("M3_R", 0, 0.17, 0.10), ref, 0.02, 0) and not dc.better_than(rec("M3_R", 0, 0.19, 0.10), ref, 0.02, 0),
          "better_than is strict on ER (> tau)")
    check(dc.better_than(rec("A", 0, 0.195, 0.05), ref, 0.02, 0), "within tau on Genesis, better on xplat by > tau")
    check(not dc.better_than(rec("A", 1, 0.0, 0.0), ref, 0.02, 0) and dc.better_than(rec("A", 0, 0.5, 0.5), rec("M3_R", 2, 0.2, 0.1), 0.02, 1),
          "misses beyond the allowance dominate")
    check([dc.budget_rank(b) for b in ("B35", "B50", "B60", None)] == [0, 1, 2, 3], "complete budget None is worst")
    check(dc.best_condition("TRANSFER_NEUTRAL") == "C2" and dc.best_condition("TRANSFER_HARMS") == "C0", "best condition")
    check(dc.b_gate(rec("M3_R", 0, 0.10, 0)) and not dc.b_gate(rec("M3_R", 0, 0.099, 0)), "B gate at ER 0.10")
    check(dc.economics_confirmed("B35", "B50", {"generated_c_reduction": 0.2, "compile_cpu_reduction": 0.15})
          and not dc.economics_confirmed("B50", "B50", {"generated_c_reduction": 0.5, "compile_cpu_reduction": 0.5})
          and not dc.economics_confirmed("B35", "B50", {"generated_c_reduction": 0.05, "compile_cpu_reduction": 0.5}), "economics gate")
    cand = {"reserved_b50_misses": {"neogeosdk": 5, "space-invaders": 1}, "reserved_max_er": {"neogeosdk": 0.2, "space-invaders": 0.3}}
    comp = {"reserved_b50_misses": {"neogeosdk": 5, "space-invaders": 0}, "reserved_max_er": {"neogeosdk": 0.2, "space-invaders": 0.3}}
    check(dc.contradicted(cand, comp, 0.02), "one extra reserved miss contradicts")
    check(dc.reserved_er({"a": 0.2, "b": None, "c": 0.4}) == 0.30000000000000004 or abs(dc.reserved_er({"a": 0.2, "c": 0.4}) - 0.3) < 1e-9,
          "reserved ER: mean of per-lineage max ER")
    good = rec("A", 0, 0.10, 0.05)
    check(dc.outcome("EVIDENCE_INSUFFICIENT", good, ref, 0.02, 0, True, False, None)["class"] == "DATA_INSUFFICIENT", "rule 1")
    check(dc.outcome("TRANSFER_NEUTRAL", good, ref, 0.02, 0, True, False, None)["class"] == "LIGHTWEIGHT_STRUCTURAL_JUSTIFIED", "rule 4")
    check(dc.outcome("TRANSFER_HELPS", rec("M3_R", 0, 0.1, 0.1, "C2"), ref, 0.02, 0, True, False, None)["class"] == "CORPUS_IMPROVES_EXISTING",
          "rule 3")
    check(dc.outcome("TRANSFER_HARMS", rec("M4_DENSE@128", 0, 0.1, 0.1, "C0"), ref, 0.02, 0, True, False, None)["class"]
          == "NO_ECONOMIC_IMPROVEMENT", "C0 existing re-selection is not a SEG-075 result")
    check(dc.outcome("TRANSFER_NEUTRAL", good, ref, 0.02, 0, True, True, None)["class"] == "NO_ECONOMIC_IMPROVEMENT", "contradicted -> rule 7")
    check(dc.outcome("TRANSFER_NEUTRAL", good, ref, 0.02, 0, False, False, None)["class"] == "NO_ECONOMIC_IMPROVEMENT", "no economics -> 7")
    check(dc.outcome("TRANSFER_NEUTRAL", rec("M3_R", 0, 0.2, 0.1), ref, 0.02, 0, False, False, 0.05)["class"] == "CORPUS_EXPANSION_ONLY",
          "rule 8")
    check(dc.outcome("TRANSFER_NEUTRAL", rec("M3_R", 0, 0.2, 0.1), ref, 0.02, 0, False, False, 0.0)["class"] == "NO_ECONOMIC_IMPROVEMENT",
          "rule 9")


def test_jobs():
    if not d.PLAN.is_file():
        return
    plan = d.verify_plan()
    j = x.jobs(plan, True)
    check(not any(s.startswith(("M3R C0 D", "M1 C0 D", "M4 C0 D")) and not s.endswith(" 50") for s in j), "C0 Genesis folds reused, not refit")
    check(sum(1 for s in j if s.endswith(" 50")) == 11, "eleven seed-50 noise replicates")
    check(len(x.jobs(plan, False)) == len(j) + 11 * 5, "without a passing reproduction every C0 Genesis fold is fitted")
    check(not any("neogeosdk" in s or "space-invaders" in s for s in j), "no job touches a reserved lineage")
    check(len(j) == len(set(j)), "no duplicate jobs")


def main():
    for t in (test_sequences, test_feasibility, test_plan, test_barrier, test_metrics, test_transfer_rule, test_decide, test_jobs):
        t()
    if FAILURES:
        print(f"{len(FAILURES)} failure(s)")
        return 1
    print("segarecomp_xplat_design_test: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
