#!/usr/bin/env python3
"""SEG-086: label-free normalisation and the normalised dynamic selector (synthetic data only)."""
import math
import pathlib
import random
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import segarecomp_seg086_norm as nm  # noqa: E402

FAILURES = []


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)


def image(n=400, code_fraction=0.2, plateau=-11.5, code=10.5, noise=0.7, seed=1, low_tail=()):
    """Synthetic bimodal logits: a data plateau and a code mode; `low_tail` indices are code cells that score in the plateau."""
    rng = random.Random(seed)
    k = int(n * code_fraction)
    s = [rng.gauss(code, 1.0) if c < k else rng.gauss(plateau, noise) for c in range(n)]
    for c in low_tail:
        s[c] = plateau + 1.0
    return s


def test_affine_covariance():
    s = image()
    z1 = nm.normalise(s, "N1")
    z2 = nm.normalise([3.0 * x - 7.0 for x in s], "N1")
    check(z1["status"] == z2["status"] == "ok" and max(abs(a - b) for a, b in zip(z1["z"], z2["z"])) < 1e-9, "N1 is invariant to positive affine changes of the score scale and location")
    a = nm.normalise(s, "N2")
    b = nm.normalise([x + 5.0 for x in s], "N2")
    check(max(abs(p - q) for p, q in zip(a["z"], b["z"])) < 1e-9, "N2 is shift invariant")
    c = nm.normalise([2.0 * x for x in s], "N2")
    check(abs(c["z"][0] - 2.0 * a["z"][0]) < 1e-9, "N2 keeps the shared logit scale (not scale invariant, by design)")
    t = 0.3
    r1 = nm.norm_select(s, set(), t, "N1")
    r2 = nm.norm_select([3.0 * x - 7.0 for x in s], set(), t, "N1")
    check(r1["region"] == r2["region"], "same selection under an affine score change")


def test_ranking_preserved():
    s = image(seed=3)
    for m in nm.METHODS:
        z = nm.normalise(s, m)["z"]
        order = sorted(range(len(s)), key=lambda i: s[i])
        check(all(z[order[i]] <= z[order[i + 1]] + 1e-12 for i in range(len(order) - 1)), f"{m} not monotone")


def test_flat_and_degenerate_inputs_fail_closed():
    flat = [-11.0] * 200
    r = nm.norm_select(flat, {3}, 0.0, "N1")
    check(r["status"] == "normalisation_failed" and r["reason"] == "scale_below_floor" and r["retained"] == 1.0, f"flat N1 {r['status']}")
    nearly = [-11.0 + 1e-9 * i for i in range(200)]
    check(nm.norm_select(nearly, set(), 0.0, "N1")["status"] == "normalisation_failed", "nearly constant")
    check(nm.norm_select([1.0] * 10, set(), 0.0, "N2")["reason"] == "too_few_cells", "too few cells")
    r2 = nm.norm_select(flat, {3}, 0.5, "N2")
    check(r2["status"] == "selected" and r2["region"] == {3}, "N2 on a flat image selects only seeds above the threshold (everything equals the location)")
    huge = [1e308, -1e308] * 64
    check(nm.norm_select(huge, set(), 0.0, "N2")["status"] in ("normalisation_failed", "abstain_cap", "selected"), "overflow handled")
    check(nm.normalise(huge, "N1")["status"] == "failed" or nm.normalise(huge, "N1")["status"] == "ok", "overflow returns a status")


def test_outliers_do_not_move_the_normalisation():
    s = image(seed=4)
    base = nm.normalise(s, "N1")
    s2 = list(s)
    s2[10], s2[11] = 600.0, -600.0
    out = nm.normalise(s2, "N1")
    check(abs(out["location"] - base["location"]) < 0.5 and abs(out["scale"] - base["scale"]) < 0.5, "robust location/scale moved by two extreme outliers")
    q = nm.normalise(s2, "N2")
    check(abs(q["location"] - nm.normalise(s, "N2")["location"]) < 0.5, "N2 location moved by two outliers")


def test_non_finite_and_bad_inputs():
    for bad in (float("nan"), float("inf"), -float("inf")):
        s = image()
        s[5] = bad
        check(nm.norm_select(s, set(), 0.0, "N1")["status"] == "rejected", f"non-finite {bad} not rejected")
    check(nm.norm_select([], set(), 0.0, "N1")["status"] == "rejected", "empty")
    check(nm.norm_select(image(), set(), float("nan"), "N1")["status"] == "rejected", "bad threshold")
    check(nm.norm_select(image(), set(), 0.0, "N1", have_universe=False)["status"] == "unsupported", "no universe")
    check(nm.norm_select([True] * 100, set(), 0.0, "N1")["status"] == "rejected", "booleans are not scores")
    try:
        nm.normalise(image(), "N3")
        check(False, "unknown method")
    except ValueError:
        pass


def test_seed_only_ties_and_cap():
    s = image(code_fraction=0.0)
    r = nm.norm_select(s, {1, 2, 99999}, 1e6, "N2")
    check(r["status"] == "selected" and r["region"] == {1, 2} and r["seed_only_cells"] == 2, f"seed-only region {r['region']}")
    # ties are inclusive and order independent
    t = [0.0] * 40 + [5.0] * 24
    z = nm.normalise(t, "N2")["z"]
    tau = z[-1]
    sel = nm.norm_select(t, set(), tau, "N2")["region"]
    check(sel == set(range(40, 64)), "threshold ties inclusive")
    rev = list(reversed(t))
    check(nm.norm_select(rev, set(), tau, "N2")["region"] == set(range(24)), "order independent")
    # cap abstention: never a truncated region
    dense = image(code_fraction=0.8)
    r = nm.norm_select(dense, set(), 0.0, "N2")
    check(r["status"] == "abstain_cap" and r["region"] == set(range(len(dense))) and r["retained"] == 1.0, "cap abstention")
    check(r["selected_fraction"] > nm.CAP, "reports the pre-cap fraction")
    check(nm.norm_select(dense, set(), 0.0, "N2", cap=0.95)["status"] == "selected", "cap parameter")


def test_dense_and_sparse_islands():
    n = 1000
    sparse = [-11.6 + 0.001 * (i % 7) for i in range(n)]
    for c in range(100, 110):
        sparse[c] = 10.0                                                  # a sparse code island embedded in data
    r = nm.norm_select(sparse, set(), 5.0, "N2")
    check(r["status"] == "selected" and r["region"] == set(range(100, 110)) and abs(r["retained"] - 0.01) < 1e-12, "sparse island selected, nothing else")
    check(nm.norm_select(sparse, set(), 0.0, "N1")["status"] == "normalisation_failed", "plateau-dominated image has no identifiable IQR: fail closed")
    dense = image(n=500, code_fraction=0.9)
    r = nm.norm_select(dense, set(), -3.0, "N1")
    check(r["status"] in ("abstain_cap", "normalisation_failed") and r["retained"] == 1.0, f"dense image with a protective threshold abstains: {r['status']}")
    # the median of a dense image sits in the code mode (ADR 0195 finding 3): z >= 0 keeps only about half of the code. That is a property of N1, not hidden by the selector
    r0 = nm.norm_select(dense, set(), 0.0, "N1")
    check(r0["status"] == "selected" and 0.4 < r0["retained"] < 0.6, f"N1 median location on a dense image {r0['status']} {r0.get('retained')}")


def test_selection_responds_to_distribution():
    """Not a disguised fixed budget: the retained fraction follows the code fraction of the image at one common threshold."""
    fr = {}
    for cf in (0.05, 0.15, 0.30, 0.50):
        r = nm.norm_select(image(n=1000, code_fraction=cf, seed=7), set(), 5.0, "N2")
        fr[cf] = r["retained"] if r["status"] == "selected" else None
    check(all(fr[c] is not None for c in (0.05, 0.15, 0.30, 0.50)), f"unexpected abstention {fr}")
    check(abs(fr[0.05] - 0.05) < 0.01 and abs(fr[0.15] - 0.15) < 0.01 and abs(fr[0.30] - 0.30) < 0.01 and abs(fr[0.50] - 0.50) < 0.01, f"retained follows density {fr}")
    check(len(set(round(v, 2) for v in fr.values())) == 4, "distinct budgets")


def test_weakest_positive_z():
    z = [0.0, 3.0, -2.0, 7.0]
    check(nm.weakest_positive_z(z, [0, 1, 1, 1], {2}) == 3.0, "seed excluded")
    check(nm.weakest_positive_z(z, [0, 0, 0, 0], set()) is None, "none")


def main() -> int:
    for fn in [v for k, v in sorted(globals().items()) if k.startswith("test_")]:
        fn()
    if FAILURES:
        print("\n".join(FAILURES))
        return 1
    print("segarecomp_seg086_norm_test: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
