#!/usr/bin/env python3
"""SEG-085: offline tests of the historical-evaluation diagnostics and the dynamic-budget policies (synthetic data only; no ROM, no emulator, no model fit)."""
import json
import math
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import segarecomp_seg085_diag as dg  # noqa: E402
import segarecomp_seg085_dyn as dy  # noqa: E402

FAILURES = []


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)


def synthetic(n_cells=8, cell=128):
    """Cells 0-1 code (cell 1 mixed with data), 2-3 data, 4 masked (U), 5 code, 6-7 data."""
    cls = bytearray()
    for c in range(n_cells):
        if c == 0:
            cls += (b"Ic" * (cell // 2))
        elif c == 1:
            cls += (b"Ic" * (cell // 4)) + b"D" * (cell // 2)
        elif c == 4:
            cls += b"U" * cell
        elif c == 5:
            cls += (b"Ic" * (cell // 2))
        else:
            cls += b"D" * cell
    return bytes(cls)


def test_quantiles_and_cell_stats():
    q = dg.quantiles([3, 1, 2, 4, 5], (0.0, 0.5, 1.0))
    check(q == {"p00": 1.0, "p50": 3.0, "p100": 5.0}, f"quantiles {q}")
    check(dg.quantiles([], (0.5,)) == {"p50": None}, "empty quantiles")
    cls = synthetic()
    labels = dg.v3.cell_labels(cls)
    check(labels == [1, 1, 0, 0, -1, 1, 0, 0], f"labels {labels}")
    starts, data, code = dg.cell_stats(cls, labels)
    check(starts[0] == 64 and data[1] == 64 and code[1] == 64, "cell stats")


def test_image_taxonomy_budgets_and_oracle():
    cls = synthetic()
    n_words = len(cls) // 2
    struct = {"reach": [False] * n_words, "target": [-1] * n_words}
    tokens = [[2, 1, 1, 0, 0, 0, 0, 0] for _ in range(n_words)]
    tokens[0][3] = 4                                    # a dynamic jump in cell 0
    scores = [5.0, 4.0, -3.0, -4.0, 2.0, -6.0, -5.0, -7.0]      # cell 5 (code) is ranked below masked cell 4
    t = dg.image_taxonomy(cls, [], scores, struct, tokens)
    check(t["positive_cells"] == 3 and t["negative_cells"] == 4 and t["masked_cells"] == 1, "cell accounting")
    check(t["oracle_fraction"] == 3 / 8 and t["oracle_fraction_if_masked_is_code"] == 4 / 8, "oracle bounds")
    # B35 keeps ceil(8*0.35)=3 ranked cells {0,1,4}: cell 5 is a missed code cell; the masked cell 4 consumed budget
    check(t["B35"]["missed_cells"] == 1 and t["B35"]["missed_starts"] == 64 and t["B35"]["masked_admitted"] == 1, f"B35 {t['B35']}")
    check(t["B35"]["oracle_feasible"] is True, "feasible budget")
    check(t["B50"]["missed_cells"] == 1 and t["B50"]["retained"] == 0.5, "B50 keeps four cells and still misses the lowest-ranked code cell")
    check(t["B35"]["mechanism"]["unreached_by_direct_flow"] == 1, "unreached mechanism")
    check(t["dynamic_control_cells"] == 1 and t["positive_cells_near_dynamic_control"] >= 2, "dynamic control proximity")
    check(t["positive_cells_mixed_with_data"] == 1, "mixed cell")


def test_target_audit():
    cls = b"IcIc" + b"DDDD" + b"UUUU" + b"IcIc"
    n = len(cls) // 2
    tgt = [-1] * n
    tgt[0] = 4          # byte 8 is U: not evidence
    tgt[1] = 2          # byte 4 is D: contradiction
    tgt[6] = 0          # byte 0 is I: consistent
    tgt[7] = 3          # byte 6 is D: contradiction
    a = dg.target_audit(cls, {"target": tgt}, n)
    check(a["direct_targets"] == 4 and a["on_start"] == 1 and a["on_unknown"] == 1 and a["on_labelled_data"] == 2, f"audit {a}")
    check(a["contradiction_rate"] == 0.5, "contradiction rate")
    check(dg.target_audit(b"DD", {"target": [-1]}, 1)["contradiction_rate"] is None, "no targets -> no rate")


def test_mask_cells_never_truncates_an_instruction():
    import segarecomp_seg085_cohort as co
    cls = b"D" * 126 + b"Ic" + b"c" * 0 + b"IcIc" + b"D" * 122     # instruction at bytes 126-127 ends exactly at the cell edge; next cell holds code
    out, n = co.mask_cells(cls, [130])
    check(n == 1 and out[:128] == cls[:128], "cell 0 untouched")
    cls2 = b"D" * 126 + b"Icc" + b"D" * 125                          # 3-byte extent straddles the boundary
    out2, _ = co.mask_cells(cls2, [200])
    check(out2[126:129] == b"UUU", f"straddling instruction masked whole {out2[126:129]!r}")


def test_dyn_select_guardrails():
    s = [5.0, 4.0, 4.0, -1.0, -2.0, -9.0, 0.0, -3.0, -4.0, -5.0]
    r = dy.dyn_select(s, {9}, 0.0)
    check(r["status"] == "selected" and r["region"] == {0, 1, 2, 6, 9} and r["retained"] == 0.5, f"selection with ties and a seed {r}")
    check(dy.dyn_select(list(reversed(s)), {0}, 0.0)["region"] == {9, 8, 7, 3, 0}, "order-independent threshold selection with the seed union")
    check(dy.dyn_select(s, set(), -4.5)["status"] == "abstain_cap", "retained fraction above the cap abstains, never truncates")
    check(dy.dyn_select(s, set(), -4.5)["region"] == set(range(10)), "abstention is the broad region")
    check(dy.dyn_select([1.0, float("nan")], set(), 0.0)["status"] == "rejected", "NaN rejected")
    check(dy.dyn_select([1.0, float("inf")], set(), 0.0)["status"] == "rejected", "inf rejected")
    check(dy.dyn_select([True, 1.0], set(), 0.0)["status"] == "rejected", "bool rejected")
    check(dy.dyn_select(s, {0}, 0.0, have_universe=False)["status"] == "unsupported", "no seed universe is unsupported, not abstained")
    check(dy.dyn_select([], set(), 0.0)["status"] == "rejected", "empty")
    check(dy.dyn_select(s, {99}, 0.0)["region"] == {0, 1, 2, 6}, "out-of-range seed ignored")


def test_calibration_rule():
    c = dy.calibrate_threshold([("A", -2.0), ("A", 1.0), ("B", -5.0), ("C", 3.0), ("D", None)])
    check(c["T0"] == -5.0 and c["shortfall"] == 3.0 and c["T"] == -8.0 and c["lineages"] == 3, f"calibration {c}")
    try:
        dy.calibrate_threshold([("A", 1.0)])
        check(False, "single lineage must fail closed")
    except ValueError:
        pass
    check(dy.image_record([5.0, 1.0, 2.0, 3.0], [1, 1, 0, 1], {0}) == 1.0, "record skips seed cells")
    check(dy.image_record([5.0], [0], set()) is None, "no positive -> no record")


def test_policy_outcome_counts_misses():
    cls = b"Ic" * 64 + b"D" * 128 + b"Ic" * 64
    labels = dg.v3.cell_labels(cls)
    out = dy.policy_outcome("DYN_SCORE", [3.0, -5.0, 2.0], [0.9, 0.1, 0.8], set(), labels, cls, threshold=0.0)
    check(out["status"] == "abstain_cap" and out["abstained"] and out["missed_starts"] == 0, f"2 of 3 cells exceeds the cap -> broad {out}")
    out2 = dy.policy_outcome("DYN_SCORE", [3.0, -5.0, -2.0], [0.9, 0.1, 0.1], set(), labels, cls, threshold=0.0)
    check(out2["missed_starts"] == 64 and out2["positive_cells_missed"] == 1, f"missed starts counted {out2}")


def test_categories_cover_every_lineage():
    check(set(dg.GENESIS) | set(dg.XPLAT) == set(dg.LINEAGES), "lineage table covers the corpus")
    check(all(v[0] in dg.CATEGORIES for v in dg.LINEAGES.values()), "category defined")
    check(sum(1 for L in dg.GENESIS if dg.LINEAGES[L][0] == "modern_sgdk") == 7 and sum(1 for L in dg.GENESIS if dg.LINEAGES[L][0] == "historical_genesis") == 4,
          "disclosed 7 of 11 SGDK imbalance")


def main():
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
    if FAILURES:
        for f in FAILURES:
            print("FAIL:", f)
        return 1
    print("segarecomp_seg085_test: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
