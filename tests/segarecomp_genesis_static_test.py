#!/usr/bin/env python3
"""SEG-077-T009: tests of the static-first labeller's corrected measurement (v2; synthetic data only, standard library).

The v1 score (tools/segarecomp_genesis_static.py, frozen) counted predictions over reference-unknown bytes in the denominator but never
in the numerator; v2 reports them separately. Covered: known/unknown separation, false starts vs continuation vs data errors, error
cells, recalled starts, marginal positive / mixed / hard-negative / trivial-negative credit, the error taxonomy and the v1 bias itself.
"""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import segarecomp_genesis_static as v1  # noqa: E402
import segarecomp_genesis_static_v2 as v2  # noqa: E402

FAILURES = []


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)
        print("FAIL:", msg)


def test_v1_bias_and_v2_separation():
    names = ["V", "D_FILL"]
    cls = b"IccIcc" + b"DDDD"
    truth = b"IccUUU" + b"UUDD"
    tier = bytes([1] * 6 + [2] * 4)
    s1 = v1.score(cls, truth, tier, names)
    check(s1["V"]["labelled"] == 6 and s1["V"]["wrong"] == 0, "v1 counts unknown-truth predictions in the denominator")
    s2 = v2.score_v2(cls, truth, tier, names)
    check(s2["V"]["pred_known"] == 3 and s2["V"]["pred_unknown"] == 3 and s2["V"]["rate_known"] == 0.0, f"v2 separates unknown {s2['V']}")
    check(s2["D_FILL"]["pred_known"] == 2 and s2["D_FILL"]["pred_unknown"] == 2, "v2 data known/unknown")
    # a tier whose only errors sit next to many unknown-truth predictions: v1 rate diluted, v2 not
    cls2 = b"I" + b"c" * 9
    truth2 = b"D" + b"U" * 9
    s1b = v1.score(cls2, truth2, bytes([1] * 10), ["V"])
    s2b = v2.score_v2(cls2, truth2, bytes([1] * 10), ["V"])
    check(s1b["V"]["wrong"] / s1b["V"]["labelled"] == 0.1, "v1 rate diluted to 10%")
    check(s2b["V"]["rate_known"] == 1.0, "v2 rate over known truth is 100%")


def test_error_kinds_cells_recall():
    names = ["V", "J", "D_PT"]
    cell = 128
    cls = bytearray(b"U" * cell * 3)
    truth = bytearray(b"D" * cell * 3)
    tier = bytearray(cell * 3)
    cls[0:4] = b"Iccc"
    truth[0:4] = b"Iccc"
    tier[0:4] = b"\x01" * 4                 # correct V instruction
    cls[4:6] = b"Ic"
    truth[4:6] = b"cI"
    tier[4:6] = b"\x02\x02"                 # J: one false start, one continuation error
    cls[cell:cell + 4] = b"DDDD"
    truth[cell:cell + 4] = b"IcDD"
    tier[cell:cell + 4] = b"\x03" * 4       # PT: two data errors in cell 1
    s = v2.score_v2(bytes(cls), bytes(truth), bytes(tier), names, cell=cell)
    check(s["V"]["recalled_starts"] == 1 and s["V"]["wrong_known"] == 0, "correct start recalled")
    check(s["J"]["false_start"] == 1 and s["J"]["cont_wrong"] == 1 and s["J"]["error_cells"] == 1, f"J error kinds {s['J']}")
    check(s["D_PT"]["data_wrong"] == 2 and s["D_PT"]["error_cells"] == 1, f"data errors {s['D_PT']}")


def test_marginal_cells():
    cell = 128
    names = ["V", "D_FILL", "D_PT"]
    cls = bytearray(b"D" * cell * 4)
    tier = bytearray([3] * cell * 4)
    cls[0] = ord("I")
    tier[0] = 1                              # cell 0: mixed (V code + PT data)
    tier[cell * 2:cell * 3] = bytes([2] * cell)   # cell 2: FILL-only negative = trivial
    s = v2.score_v2(bytes(cls), bytes(cls), bytes(tier), names, cell=cell)
    check(s["V"]["marginal_positive"] == 1 and s["V"]["marginal_mixed"] == 1 and s["D_PT"]["marginal_mixed"] == 1, f"mixed credit {s}")
    check(s["D_PT"]["marginal_hard_negative"] == 1, "cell 1 is a hard negative (adjacent to positive cell 0)")
    check(s["D_FILL"]["marginal_trivial_negative"] == 1, "FILL-only negative is trivial")


def test_taxonomy():
    cls = b"IcIcIc" + b"DD"
    truth = b"IcDDDD" + b"Ic"
    names = ["V", "J", "D_PT"]
    tier = bytes([1, 1, 2, 2, 2, 2, 3, 3])
    res = {"code": {0: (2, "V"), 2: (2, "J"), 4: (2, "J")}, "prov": {0: ("root", None), 2: ("table", 0), 4: ("fall", 2)},
           "typed": {"DECODER_DISAGREE_REMOVED": 1, "DECODERS": {"cpu_m68k_cap": 3}}}
    tx = v2.taxonomy(cls, truth, res, tier, names)
    check(tx.get("indirect_dispatch_table", {}).get("instructions") == 1, f"table error {tx}")
    check(tx.get("inherited_from_wrong_parent", {}).get("instructions") == 1, "fall-through after a wrong parent is inherited")
    check(tx.get("pointer_table_boundary", {}).get("bytes") == 2, "data boundary error")
    check(tx["decoder_disagreement_removed"]["instructions"] == 1, "decoder removals reported")


def main():
    for t in (test_v1_bias_and_v2_separation, test_error_kinds_cells_recall, test_marginal_cells, test_taxonomy):
        t()
    print("FAILURES:", len(FAILURES))
    return 1 if FAILURES else 0


if __name__ == "__main__":
    sys.exit(main())
