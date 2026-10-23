#!/usr/bin/env python3
"""SEG-080-T003: tests of the training-label confidence policy (standard library, synthetic data only; no truth, no model)."""
import json
import pathlib
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import segarecomp_genesis_label_policy as lp  # noqa: E402

FAILURES = []


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)


def make(d, cls, estarts, rstarts, extents):
    d.mkdir()
    (d / "classes.bin").write_bytes(cls)
    (d / "spans.json").write_text(json.dumps({"tiers": {"E": estarts, "R": rstarts}, "data_extents": extents, "typed": {}}))


def test_tiers_and_variants():
    c = lp.CELL
    cls = bytearray(b"U" * (4 * c))
    cls[0:4] = b"Iccc"                        # E instruction
    cls[4:c] = b"D" * (c - 4)                 # weak data (DR) sharing the cell with E code
    cls[c:c + 2] = b"Ic"                      # R instruction
    cls[2 * c:3 * c] = b"D" * c               # DMA extent: strong negative cell
    cls[3 * c:3 * c + c] = b"D" * c           # DR-only negative cell (weak)
    ext = [{"kind": "DR", "start": 4, "end": c, "masked": False}, {"kind": "DMA", "start": 2 * c, "end": 3 * c, "masked": False},
           {"kind": "DR", "start": 3 * c, "end": 4 * c, "masked": False}]
    with tempfile.TemporaryDirectory() as t:
        d = pathlib.Path(t) / "l"
        make(d, bytes(cls), [0], [c], ext)
        tiers, k = lp.tiers_of(d)
        check(tiers[0] == lp.E and tiers[3] == lp.E and tiers[c] == lp.R and tiers[2 * c] == lp.M and tiers[3 * c] == lp.W and tiers[10] == lp.W,
              "per-byte tiers")
        s, w = lp.build_variant(tiers, k, "strong"), lp.build_variant(tiers, k, "weak")
        check(s[0:4] == b"Iccc" and s[c:c + 2] == b"UU" and s[10] == ord("U") and s[2 * c] == ord("D") and s[3 * c] == ord("U"), "strong variant")
        check(w[c:c + 2] == b"Ic" and w[10] == ord("D") and w[3 * c] == ord("D"), "weak variant adds R and DR")
        ls, lw = lp.cell_labels(s), lp.cell_labels(w)
        check(ls == [1, -1, 0, -1] and lw == [1, 1, 0, 0], "cell labels")
        check(lp.cell_weights(tiers, w, lw) == [1.0, lp.WEAK_WEIGHT, 1.0, lp.WEAK_WEIGHT], "cell weights: strong 1.0, weak-only 0.25")
        check(lp.cell_weights(tiers, s, ls) == [1.0, 0.0, 1.0, 0.0], "strong variant weights and masked 0")
        # nonexclusive: a weak data read never removes a byte from a code cell (cell 0 stays positive although it holds DR data)
        check(lw[0] == 1, "weak data never vetoes a code cell")


def test_errors():
    c = lp.CELL
    tiers = bytes([lp.E, lp.E, lp.W, lp.W, lp.M, lp.R])
    cls = b"IcDDDI"
    truth = b"IcIcDD"
    e = lp.tier_errors(tiers, cls, truth)
    check(e["E"]["wrong"] == 0 and e["W"]["code_as_data"] == 2 and e["W"]["wrong"] == 2 and e["M"]["wrong"] == 0, "per-tier code-as-data")
    check(e["R"]["data_as_code"] == 1, "data-as-code in the weak code tier")
    n = lp.noisy_negative_cells(b"D" * c, [0], b"I" + b"D" * (c - 1), [lp.WEAK_WEIGHT])
    check(n["negative_cells_containing_truth_code"]["weak"] == 1 and n["negative_cells_containing_truth_code"]["strong"] == 0, "noisy weak negative")


def test_plan():
    if lp.PLAN.exists():
        check(bool(lp.verify_plan()), "frozen plan verifies")
    check(lp.PROTOCOL["policy"] == "seg080.label_policy.v1" and lp.WEAK_WEIGHT == 0.25, "policy id and weight")
    check("never used to form, repair or filter" in lp.PROTOCOL["truth_use"], "truth isolation stated")


def main():
    test_tiers_and_variants()
    test_errors()
    test_plan()
    print("FAILURES:", len(FAILURES), *FAILURES)
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
