#!/usr/bin/env python3
"""SEG-087-T001: low-tail diagnostic pure functions (synthetic data only; no ROM, no emulator, no model fit)."""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import segarecomp_seg087_diag as dg  # noqa: E402

FAILURES = []


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)


def test_classify_cell():
    P = dg.cell_profile
    check(dg.classify_cell(P(b"I" + b"c" * 127)) == "PURE_CODE", "pure code")
    check(dg.classify_cell(P(b"Icc" + b"D" * 125)) == "SINGLE_START_FRAGMENT", "single start fragment")
    check(dg.classify_cell(P(b"IcIc" + b"D" * 124)) == "DATA_DOMINATED_FRAGMENT", "data dominated fragment")
    check(dg.classify_cell(P(b"Ic" * 40 + b"D" * 48)) == "MIXED_CODE_DATA", "mixed")
    check(dg.classify_cell(P(b"Ic" * 10 + b"U" * 4 + b"D" * 104)) == "AMBIGUOUS_UNKNOWN", "unknown bytes make the cell ambiguous, never clean evidence")
    check(dg.classify_cell(P(b"IcIc" + b"D" * 124)) != "AMBIGUOUS_UNKNOWN", "data only is not ambiguous")


def test_entry_kind():
    classes = b"D" * 4 + b"Ic" + b"D" * 122 + b"Icc" + b"D" * 125     # cell 0: instruction after data; cell 1: instruction at the start
    check(dg.entry_kind(classes, 0, {}) == "AFTER_DATA", "after data")
    check(dg.entry_kind(b"Ic" * 64 + b"Ic" + b"D" * 126, 1, {0: 1}) == "FALLTHROUGH_FROM_CODE", "fall-through")
    check(dg.entry_kind(b"Ic" * 64 + b"Ic" + b"D" * 126, 1, {126: 4}) == "AFTER_TERMINATOR", "after a terminating instruction")
    check(dg.entry_kind(b"U" + b"Ic" + b"D" * 126, 0, {}) == "AFTER_UNKNOWN", "after unknown")
    check(dg.entry_kind(b"D" * 128, 0, {}) == "NO_START", "no start")
    check(dg.entry_kind(b"Ic" + b"D" * 126, 0, {}) == "IMAGE_START", "image start")


def test_subcell_resolution():
    chunk = b"Ic" * 32 + b"D" * 64                      # 64-byte halves: first pure code, second pure data
    r = dg.subcell_resolution(chunk, 64)
    check(r["resolved"] and r["pure_code"] == 1 and r["pure_data"] == 1 and r["mixed"] == 0, f"resolved at 64: {r}")
    chunk = b"Ic" * 16 + b"D" * 96                      # at 32 the first sub-cell is pure code, the rest data; at 64 the first half is mixed
    check(dg.subcell_resolution(chunk, 32)["resolved"] and not dg.subcell_resolution(chunk, 64)["resolved"], "granularity resolves some cells only")
    mixed = (b"Ic" + b"D" * 30) * 4
    r = dg.subcell_resolution(mixed, 32)
    check(r["mixed"] == 4 and not r["resolved"], "ambiguity moves to a finer boundary instead of disappearing")
    check(not dg.subcell_resolution(b"Ic" * 10 + b"U" * 20 + b"D" * 98, 128)["resolved"], "unknown never counts as resolved")


def _structure(n_words=256, reach=(), targets=(), ordinary=None, chain_start=()):
    reach_a = [False] * n_words
    for w in reach:
        reach_a[w] = True
    target = [-1] * n_words
    for w, t in targets:
        target[w] = t
    ordn = [True] * n_words if ordinary is None else ordinary
    chain = [0] * n_words
    for w in chain_start:
        chain[w] = 9
    return reach_a, target, ordn, chain


def test_witness_flags_code_island_in_data():
    # 4 cells of 64 words; cell 2 holds a code island reached from a seed root in cell 0 through a direct edge
    reach, target, ordn, chain = _structure(reach=[10, 140], targets=[(10, 140)])
    f = dg.witness_flags(64, 4, reach, target, ordn, chain, [])
    check(f["W_REACH_ROOTED"] == [True, False, True, False], f"rooted reach {f['W_REACH_ROOTED']}")
    check(f["W_REACH_TARGET"] == [False, False, True, False], "target landing from a reached source")
    check(dg.usable_witness({k: f[k][2] for k in f}) and not dg.usable_witness({k: f[k][1] for k in f}), "usable witness only where rooted")


def test_unrooted_candidates_are_not_independent():
    # a decodable instruction-looking sequence in cell 3 that nothing reaches: its own branch and a long ordinary run produce only heuristic flags
    reach, target, ordn, chain = _structure(reach=[], targets=[(200, 220)], chain_start=[200])
    f = dg.witness_flags(64, 4, reach, target, ordn, chain, [])
    cell = {k: f[k][3] for k in f}
    check(cell["W_UNROOTED_TARGET"] and cell["W_DECODE_CHAIN"], "heuristic flags fire on unreachable instruction-looking bytes")
    check(not dg.usable_witness(cell), "arbitrary decodable sequences are not trusted code roots")


def test_unsupported_indirect_control_is_not_followed():
    # an indirect jump has no target word (-1): nothing past it is witnessed
    reach, target, ordn, chain = _structure(reach=[5], targets=[])
    f = dg.witness_flags(64, 4, reach, target, ordn, chain, [])
    check(f["W_REACH_ROOTED"] == [True, False, False, False] and not any(f["W_REACH_TARGET"]), "no target, no witness")


def test_contradictory_and_out_of_range_candidates_fail_closed():
    reach, target, ordn, chain = _structure(reach=[1], targets=[(1, 9999), (2, -5)])
    f = dg.witness_flags(64, 4, reach, target, ordn, chain, [])
    check(not any(f["W_REACH_TARGET"]) and not any(f["W_UNROOTED_TARGET"]), "out-of-range / negative targets ignored")
    f = dg.witness_flags(64, 4, reach, target, ordn, chain, [(10 ** 9, 2)])
    check(f["W_RELOC"] == [False, False, False, True], "an out-of-range relocation clamps to the last cell, never raises")


def test_missing_seed_universe_has_no_rooted_witness():
    reach, target, ordn, chain = _structure(reach=[], targets=[(3, 70)])
    f = dg.witness_flags(64, 4, reach, target, ordn, chain, [])
    check(not any(f["W_REACH_ROOTED"]) and not any(f["W_REACH_TARGET"]) and f["W_UNROOTED_TARGET"][1], "without roots only heuristics fire")


def test_lineage_minima_driver():
    m = {"L1": -10.0, "L2": -7.0, "L3": 1.0, "L4": None}
    d = dg.lineage_minima_driver(m)
    check(d["t0"] == -10.0 and d["t0_lineage"] == "L1", f"t0 {d}")
    check(d["shortfall"] == 3.0 and d["shortfall_held_out"] == "L1" and d["shortfall_calibration_minimum_lineage"] == "L2", f"shortfall {d}")
    check(dg.lineage_minima_driver({"L1": 1.0}) == {"t0": None}, "needs two lineages")
    check(dg.lineage_minima_driver({"L1": -2.0, "L2": -2.0})["shortfall"] == 0.0, "tie has no shortfall")


def test_budget_ties_and_seed_union():
    s = [1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]
    check(dg.retained_by_budget(s, set(), 35) == {0, 1, 2, 3}, "stable tie order, ceil(35%) cells")
    check(dg.retained_by_budget(s, {9}, 10) == {0, 9}, "seeds are always kept")


def test_stability_and_rank_percentile():
    st = dg.stability([-1.0, 2.0, -0.5], -3.0)
    check(st["fits"] == 4 and st["below_zero"] == 3 and st["min"] == -3.0 and st["max"] == 2.0, f"stability {st}")
    check(dg.rank_percentile([5.0, 4.0, 3.0, 2.0], 0) == 0.0 and dg.rank_percentile([5.0, 4.0, 3.0, 2.0], 3) == 0.75, "rank percentile")


def test_summary_and_determinism():
    rows = [{"w": {k: (k == "W_REACH_ROOTED") for k in dg.INDEPENDENT + dg.HEURISTIC}}, {"w": {k: False for k in dg.INDEPENDENT + dg.HEURISTIC}}]
    s = dg.summarize_flags(rows, dg.INDEPENDENT + dg.HEURISTIC)
    check(s["usable_independent"]["count"] == 1 and s["cells"] == 2 and s["W_RELOC"]["count"] == 0, f"summary {s}")
    check(dg.summarize_flags(rows, dg.INDEPENDENT + dg.HEURISTIC) == s, "deterministic")
    check(dg.group_of("d07") == "sgdk" and dg.group_of("d12") == "sgdk" and dg.group_of("s2") == "historical" and dg.group_of("scd/X") == "historical", "groups")


def test_labels_are_not_a_witness_input():
    import inspect
    sig = inspect.signature(dg.witness_flags)
    check(not any(x in sig.parameters for x in ("labels", "classes", "truth")), "witness extraction takes no label input")


def main():
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
    if FAILURES:
        for f in FAILURES:
            print("FAIL:", f)
        return 1
    print("segarecomp_seg087_diag_test: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
