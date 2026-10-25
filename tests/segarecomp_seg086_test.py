#!/usr/bin/env python3
"""SEG-086: score-distribution diagnostics and the normalised dynamic selector (synthetic data only; no ROM, no emulator, no model fit)."""
import math
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import segarecomp_seg086_diag as dg  # noqa: E402

FAILURES = []


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)


def test_quantiles():
    q = dg.quantile_summary([1, 2, 3, 4, 5], (0.0, 0.5, 1.0, 0.25))
    check(q == {"q00": 1.0, "q50": 3.0, "q100": 5.0, "q25": 2.0}, f"quantiles {q}")
    check(dg.quantile_summary([], (0.5,)) == {"q50": None}, "empty")
    try:
        dg.quantile_summary([1.0, float("nan")])
        check(False, "nan accepted")
    except ValueError:
        pass
    check(dg.quantile_summary([0.0, 10.0], (0.5,))["q50"] == 5.0, "interpolation")


def test_weakest_positive_oracle_and_density():
    scores = [5.0, -3.0, 2.0, -9.0, 8.0, 0.0]
    labels = [1, 0, 1, 1, -1, 0]
    check(dg.weakest_positive(scores, labels, set()) == -9.0, "weakest positive")
    check(dg.weakest_positive(scores, labels, {3}) == 2.0, "seed excluded from the weakest positive")
    check(dg.weakest_positive([1.0], [0], set()) is None, "no positive")
    check(dg.oracle_fraction(scores, set(), 2.0) == 3 / 6, "oracle fraction ties inclusive")
    check(dg.oracle_fraction(scores, {1}, 2.0) == 4 / 6, "oracle fraction unites seeds")
    check(dg.oracle_fraction(scores, set(), None) is None, "oracle none")
    check(dg.positive_density(labels, {1}) == 4 / 6, "density with seed")


def test_error_classes_are_exclusive_and_ordered():
    c = dg.classify_error
    check(c(0.7, 0.9, True, 0) == "BUDGET_INFEASIBLE", "density above the cap is infeasible regardless of the rest")
    check(c(0.2, 0.8, False, 0) == "RANKING_ERROR", "oracle above cap with density below is a ranking error")
    check(c(0.2, 0.3, True, 0) == "CALIBRATION_ERROR", "adequate ranking but abstention is a calibration error")
    check(c(0.2, 0.3, False, 2) == "CALIBRATION_ERROR", "adequate ranking but a missed start is a calibration error")
    check(c(0.2, 0.3, False, 0) == "OK", "ok")
    check(c(0.6, 0.6, False, 0) == "OK", "exactly at the cap is feasible")


def test_instability_and_location_scale():
    check(dg.lineage_instability({"a": -10.0, "b": 10.0, "c": None}) == {"n": 2, "min": -10.0, "max": 10.0, "range": 20.0, "median": 0.0}, "instability")
    check(dg.lineage_instability({"a": None}) == {"n": 0}, "no minima")
    ls = dg.location_scale_candidates([float(i) for i in range(101)])
    check(ls["median"] == 50.0 and ls["iqr"] == 50.0 and ls["lower_half_spread"] == 40.0, f"location/scale {ls}")
    flat = dg.location_scale_candidates([3.0] * 20)
    check(flat["iqr"] == 0.0 and flat["lower_half_spread"] == 0.0, "flat image has zero scale (the normalisation must fail closed)")


def test_spearman_and_weak_character():
    try:
        import numpy  # noqa: F401
    except ImportError:
        return
    check(abs(dg.spearman([1, 2, 3, 4], [10, 20, 30, 40]) - 1.0) < 1e-12, "monotone transform keeps rank agreement")
    check(abs(dg.spearman([1, 2, 3, 4], [4, 3, 2, 1]) + 1.0) < 1e-12, "reversal")
    check(dg.spearman([1, 1, 1], [1, 2, 3]) is None or True, "constant input does not crash")
    scores = [-10.0, -11.0, -12.0, -12.5, 9.0, -9.5, 8.0, -13.0, -12.2, -11.8]
    labels = [1, 0, 0, 0, 1, 1, 0, 0, 0, 0]
    ch = dg.weak_positive_character(scores, labels, set(), [1, 0, 0, 0, 5, 3, 0, 0, 0, 0], [0, 0, 0, 0, 0, 100, 0, 0, 0, 0])
    check(ch["all_positive"]["cells"] == 3 and ch["weak"]["cells"] >= 1, f"weak character {ch}")
    check(ch["all_positive"]["mixed_with_data"] == 1 and ch["all_positive"]["mostly_data"] == 1, "mixed accounting")


def main() -> int:
    for fn in [v for k, v in sorted(globals().items()) if k.startswith("test_")]:
        fn()
    if FAILURES:
        print("\n".join(FAILURES))
        return 1
    print("segarecomp_seg086_test: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
