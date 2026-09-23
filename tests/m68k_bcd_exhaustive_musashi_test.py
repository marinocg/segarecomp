#!/usr/bin/env python3
"""SEG-021-T015: exhaustive byte sweeps of ABCD, SBCD and NBCD against the pinned Musashi oracle.

usage: m68k_bcd_exhaustive_musashi_test.py <m68k_conformance_emitter> <cc>

Every (source byte, destination byte) pair x every X/Z seed (X in {0,1}, Z in {0,1}) runs through the T003 harness
(decode -> lift -> emitted strict-C11 -> native versus the pinned Musashi core) for the register (`Dy,Dx`) and
`-(Ay),-(Ax)` forms of ABCD and SBCD; every operand byte x every X/Z seed runs for NBCD on Dn, `(An)`, `(An)+` and
`-(An)`. The compared state includes the full CCR: X/C/Z are the documented result; N and V are UNDEFINED on the
base MC68000 and are compared as the deliberate matched-to-Musashi policy (see
`M68kDecimalArithmeticSpecification`). Destination upper register bits are non-zero to prove they are preserved.

Tier: `full`, oracle-available (about 1.05 million vectors, a few minutes). Without the pinned Musashi checkout the
whole sweep is SKIPPED with exit 0 (never failed); the always-on BCD checks live in m68k_conformance_harness_test
(committed rows), m68k_routed_lowering_test and m68k_pipeline_tests.
"""
import copy
import pathlib
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import m68k_conformance as mc  # noqa: E402

SR_SEEDS = ["2700", "2704", "2710", "2714"]  # X/Z combinations


def main():
    emitter, cc = pathlib.Path(sys.argv[1]), sys.argv[2]
    table, forms = mc.load_table(), mc.load_forms()
    checkout = mc.musashi_checkout(None)
    if checkout is None:
        print("m68k_bcd_exhaustive_musashi_test: SKIPPED (no pinned Musashi checkout)")
        return 0
    table = copy.deepcopy(table)
    table["profiles"]["bcd_exhaustive_pairs"] = {
        "kind": "pair_list", "sr": SR_SEEDS,
        "pairs": [["%08X" % s, "%08X" % (0x5A5A5A00 | d)] for s in range(256) for d in range(256)]}
    table["profiles"]["bcd_exhaustive_unary"] = {
        "kind": "pair_list", "sr": SR_SEEDS, "pairs": [["%08X" % (0x5A5A5A00 | b), "00000000"] for b in range(256)]}
    selected = {  # row id -> the one exercised primary word (distinct registers, so no aliasing)
        "abcd.dn_dn.b.dn.dn": 0xC501, "abcd.predec_predec.b.predec.predec": 0xC509,
        "sbcd.dn_dn.b.dn.dn": 0x8501, "sbcd.predec_predec.b.predec.predec": 0x8509,
        "nbcd.unary.b.none.dn": 0x4802, "nbcd.unary.b.none.ind": 0x4812,
        "nbcd.unary.b.none.postinc": 0x481A, "nbcd.unary.b.none.predec": 0x4822}
    forms = copy.deepcopy(forms)
    rows = []
    for row in table["rows"]:
        if row["id"] not in selected:
            continue
        row = copy.deepcopy(row)
        row["profile"] = "bcd_exhaustive_unary" if row["id"].startswith("nbcd") else "bcd_exhaustive_pairs"
        row.pop("full_profile", None)
        row.pop("full_words", None)
        forms[row["form"]]["word_ranges"] = [[selected[row["id"]], selected[row["id"]]]]
        rows.append(row)
    assert len(rows) == len(selected)
    table["rows"] = rows
    expected = 4 * 65536 * len(SR_SEEDS) + 4 * 256 * len(SR_SEEDS)
    with tempfile.TemporaryDirectory() as scratch:
        report = mc.run(table, forms, emitter, cc, checkout, pathlib.Path(scratch))
    assert report["vectors"] == expected, (report["vectors"], expected)
    assert report["deterministic"], "generated-native execution must be deterministic"
    for row in report["rows"]:
        assert row["status"] in ("validated", "self_consistent"), (row["row"], row["status"], row["first_divergence"])
        assert row["counts"]["unsupported"] == 0 and row["counts"]["diverged"] == 0, row
    assert report["musashi"] == "pinned"
    print("m68k_bcd_exhaustive_musashi_test: OK (%d vectors matched the pinned Musashi core)" % report["vectors"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
