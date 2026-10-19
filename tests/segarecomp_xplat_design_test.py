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
    check("bitwise" in plan["models"]["C0_reuse"], "C0 reuse guarded by bitwise reproduction")
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


def test_metrics():
    cls = b"I" + b"c" * 127 + b"D" * 128 + b"U" * 128 + b"I" + b"c" * 3 + b"D" * 124
    m = x.image_metrics(cls, set(), [0.9, 0.1, 0.5, 0.2])
    check(m["positive_cells"] == 2 and m["negative_cells"] == 1 and m["frf"] == 0.75, "FRF counts the cells up to the weakest positive")
    check(m["misses_B35"] == 1 and m["misses_B60"] == 0, "misses at fixed budgets (B35 keeps 2 of 4 cells)")
    check(m["negative_admitted_B60"] == 0 and m["stratum_cells"] == 2 and m["auroc"] == 1.0, "negative admissions, stratum, AUROC")
    check(x.auroc([1.0, 0.8], [0.1]) == 1.0 and x.auroc([0.1], [0.9]) == 0.0 and x.auroc([0.5], [0.5]) == 0.5, "AUROC with ties")
    seeds = x.image_metrics(cls, {3}, [0.9, 0.1, 0.5, 0.2])
    check(seeds["misses_B35"] == 0 and seeds["residual_frf"] == 0.25, "seed cells always retained; residual FRF excludes them")


def test_transfer_rule():
    c0 = {l: {"max_frf": 0.5, "misses_B50": 0} for l in ("A", "B", "C")}
    better = {l: {"max_frf": 0.45, "misses_B50": 0} for l in ("A", "B", "C")}
    check(x.classify_transfer(c0, better, 0.02, False)["class"] == "TRANSFER_HELPS", "uniform improvement helps")
    one_worse = dict(better, C={"max_frf": 0.56, "misses_B50": 0})
    check(x.classify_transfer(c0, one_worse, 0.02, False)["class"] == "TRANSFER_HARMS", "one lineage worse by > 2 tau harms")
    small = {l: {"max_frf": 0.49, "misses_B50": 0} for l in ("A", "B", "C")}
    check(x.classify_transfer(c0, small, 0.02, False)["class"] == "TRANSFER_NEUTRAL", "effect below tau is neutral")
    miss = {l: {"max_frf": 0.45, "misses_B50": 1 if l == "A" else 0} for l in ("A", "B", "C")}
    check(x.classify_transfer(c0, miss, 0.02, False)["class"] == "TRANSFER_HARMS", "an added B50 miss harms")
    check(x.classify_transfer(c0, better, 0.02, True)["class"] == "EVIDENCE_INSUFFICIENT", "noise rule -> insufficient")


def test_jobs():
    if not d.PLAN.is_file():
        return
    plan = d.verify_plan()
    j = x.jobs(plan, c0_reuse=True)
    check(not any(s.startswith(("M3R C0 D", "M1 C0 D", "M4 C0 D")) and not s.endswith(" 50") for s in j), "C0 Genesis folds reused, not refit")
    check(sum(1 for s in j if s.endswith(" 50")) == 11, "eleven seed-50 noise replicates")
    check(not any("neogeosdk" in s or "space-invaders" in s for s in j), "no job touches a reserved lineage")
    check(len(j) == len(set(j)), "no duplicate jobs")


def main():
    for t in (test_sequences, test_feasibility, test_plan, test_barrier, test_metrics, test_transfer_rule, test_jobs):
        t()
    if FAILURES:
        print(f"{len(FAILURES)} failure(s)")
        return 1
    print("segarecomp_xplat_design_test: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
