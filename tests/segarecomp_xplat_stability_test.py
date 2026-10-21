#!/usr/bin/env python3
"""SEG-076-T003 (exploratory research tooling): synthetic tests of the pre-registered T003 statistics and classes.

- exact sign-flip permutation p (all-same-sign, symmetric, zero cases); 95% t interval over 4 paired seeds (t_0.975,3 = 3.182)
- transfer classes: MEASURABLE (both directions), EFFECT_LINEAGE_CONCENTRATED, NO_EFFECT_DETECTED_BOUNDED (half-width <= 0.02),
  EVIDENCE_INSUFFICIENT, the 0.01 magnitude floor
- seed-stability class: ensemble gain over the member mean (5/6 pairs for D01, mean Genesis gain <= -0.005, 4-seed gain <= 0, both
  conditions); the overlapping-pair SD ratio's arithmetic null of exactly 1/sqrt(3) is pinned
- loss-log parsing of the preserved SEG-072 / SEG-075 epoch lines; the job list (55 new M3-R fits, 11 A C2 fits, 2 determinism refits)
"""
import math
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
# The SEG-076 research runners are Linux research tooling and import the Unix-only `resource` module (peak-RSS records). They are
# digest-bound by their frozen plans, so they stay unchanged; on platforms without `resource` (Windows CI) a stand-in lets these tests
# exercise the pure parts (rules, barrier, job lists), which never call it.
try:
    import resource  # noqa: F401
except ImportError:
    import types
    sys.modules["resource"] = types.ModuleType("resource")
FAILURES = []


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)
        print("FAIL:", msg)


def main() -> int:
    import itertools
    import random
    import segarecomp_xplat_stability as s
    check(abs(s.sign_flip_p([0.1] * 11) - 2 / 2048) < 1e-12, "11 same-sign lineages: p = 2/2048")
    check(s.sign_flip_p([0.1, -0.1]) == 1.0, "symmetric differences: p = 1")
    check(s.sign_flip_p([0.0, 0.0, 0.0]) == 1.0, "all zero: p = 1")
    ci = s.t_interval([1.0, 2.0, 3.0, 4.0])
    sd = math.sqrt(sum((x - 2.5) ** 2 for x in (1, 2, 3, 4)) / 3)
    check(abs(ci["half_width"] - round(3.182 * sd / 2, 6)) < 1e-6 and ci["mean"] == 2.5, f"t interval {ci}")
    helps = s.transfer_class([-0.03, -0.031, -0.029, -0.03], [-0.02] * 11)
    check(helps["class"] == "TRANSFER_EFFECT_MEASURABLE" and helps["direction"] == "helps", f"clear negative effect {helps}")
    harms = s.transfer_class([0.03, 0.031, 0.029, 0.03], [0.02] * 11)
    check(harms["class"] == "TRANSFER_EFFECT_MEASURABLE" and harms["direction"] == "harms", "clear positive effect")
    tiny = s.transfer_class([-0.005, -0.0051, -0.0049, -0.005], [-0.005] * 11)
    check(tiny["class"] == "NO_EFFECT_DETECTED_BOUNDED", f"below the 0.01 floor, narrow interval {tiny['class']}")
    mid = s.transfer_class([-0.03, 0.0, -0.01, 0.02], [-0.002] * 11)
    check(mid["class"] == "EVIDENCE_INSUFFICIENT", f"interval beyond +/-0.02 {mid['class']}")
    edge = s.transfer_class([0.008, 0.009, 0.0085, 0.0095], [0.001] * 11)
    check(edge["class"] == "NO_EFFECT_DETECTED_BOUNDED", f"small effect below the floor, interval inside +/-0.02 {edge['class']}")
    reach = s.transfer_class([0.0, 0.01, 0.03, 0.04], [0.001] * 11)
    check(reach["t_interval"]["lo"] < 0 < 0.02 < reach["t_interval"]["hi"] and reach["class"] == "EVIDENCE_INSUFFICIENT",
          f"an interval including 0 but reaching past +0.02 is not bounded {reach['class']} {reach['t_interval']}")
    wide = s.transfer_class([-0.2, 0.1, -0.15, 0.12], [-0.02] * 11)
    check(wide["class"] == "EVIDENCE_INSUFFICIENT", f"wide interval {wide['class']}")
    conc = s.transfer_class([-0.03, -0.031, -0.029, -0.03], [-0.3] + [0.001, -0.001] * 5)
    check(conc["class"] == "EFFECT_LINEAGE_CONCENTRATED", f"a one-lineage effect {conc['class']}")
    # the arithmetic null found by the C4 review: overlapping-pair means of 4 values have SD ratio exactly 1/sqrt(3) for ANY data
    rnd = random.Random(7)
    for _ in range(5):
        x = [rnd.random() for _ in range(4)]
        pr = [(a + b) / 2 for a, b in itertools.combinations(x, 2)]
        check(abs(s.pop_sd(pr) / s.pop_sd(x) - 1 / math.sqrt(3)) < 1e-9, "overlapping-pair SD ratio is exactly 1/sqrt(3)")
    good = {c: {"pair_d01": [-0.1, -0.2, -0.05, -0.3, -0.01, 0.02], "pair_mean_er": [-0.01] * 6, "ens4_mean_er": -0.02} for c in ("C0", "C2")}
    check(s.stability_class(good)["class"] == "SEED_STABILITY_IMPROVED", "5/6 negative D01 gains, mean gain <= -0.005, ens4 <= 0")
    linear = {c: {"pair_d01": [0.0] * 6, "pair_mean_er": [0.0] * 6, "ens4_mean_er": 0.0} for c in ("C0", "C2")}
    check(s.stability_class(linear)["class"] == "NO_MEANINGFUL_ADVANTAGE", "pure averaging (zero gain) never passes")
    three = dict(good, C2={"pair_d01": [-0.1, -0.2, -0.05, 0.3, 0.01, 0.02], "pair_mean_er": [-0.01] * 6, "ens4_mean_er": -0.02})
    check(s.stability_class(three)["class"] == "NO_MEANINGFUL_ADVANTAGE", "a condition with only 3/6 negative gains fails")
    small = dict(good, C0=dict(good["C0"], pair_mean_er=[-0.004] * 6))
    check(s.stability_class(small)["class"] == "NO_MEANINGFUL_ADVANTAGE", "mean gain above -0.005 fails")
    worse4 = dict(good, C0=dict(good["C0"], ens4_mean_er=0.001))
    check(s.stability_class(worse4)["class"] == "NO_MEANINGFUL_ADVANTAGE", "4-seed ensemble gain > 0 fails")
    check(s.stability_class({})["class"] == "NO_MEANINGFUL_ADVANTAGE", "no conditions never passes")
    # the convexity trap found by the C4 re-review: with seed-varying worst images, averaging the members' image ERs lowers the
    # max-aggregate by itself (Jensen); g_score is measured against that pure-averaging reference, so it is 0 there and fails
    a_er, b_er = (0.3, 0.1), (0.1, 0.3)
    member_mean_of_max = (max(a_er) + max(b_er)) / 2
    m_lin = max((x + y) / 2 for x, y in zip(a_er, b_er))
    check(m_lin - member_mean_of_max < 0, "Jensen term is negative when the argmax varies (the trap)")
    pure = {c: {"pair_d01": [m_lin - m_lin] * 6, "pair_mean_er": [0.0] * 6, "ens4_mean_er": 0.0} for c in ("C0", "C2")}
    check(s.stability_class(pure)["class"] == "NO_MEANINGFUL_ADVANTAGE", "pure image-wise averaging (g_score = 0) fails")
    lin = {"a": "D01", "b": "D01", "c": "D02"}
    ma, mb = {"a": 0.3, "b": 0.1, "c": 0.05}, {"a": 0.1, "b": 0.3, "c": 0.07}
    avg = {k: (ma[k] + mb[k]) / 2 for k in ma}
    gt = s.gain_terms([ma, mb], avg, lin)
    check(abs(gt["g_score_d01"]) < 1e-12 and abs(gt["g_score_mean_er"]) < 1e-12, f"pure averaging gives g_score 0 {gt}")
    check(abs(gt["g_jensen_d01"] - (-0.1)) < 1e-12, f"Jensen term -0.1 in the trap example {gt}")
    better = dict(avg, a=0.15, b=0.15)
    gt2 = s.gain_terms([ma, mb], better, lin)
    check(abs(gt2["g_score_d01"] - (-0.05)) < 1e-12, f"a genuine tail improvement is negative g_score {gt2}")
    gt3 = s.gain_terms([dict(ma, c=None), dict(mb, c=None)], dict(avg, c=None), lin)
    check(gt3["g_score_mean_er"] == 0 and s.aggregate_er({"c": None, "a": 0.2}, lin)["mean_er"] == 0.2, "None ERs are skipped")
    try:
        s.gain_terms([ma, {"a": 0.1}], avg, lin)
        check(False, "mismatched image sets refused")
    except SystemExit:
        pass
    log = "[M3R/D01] epoch 1/30 loss 0.37547 t=19s\nnoise\n[M3R/D01] epoch 2/30 loss 0.15352 t=45s\n[M3R/D02] epoch 1/30 loss 0.5 t=1s\n"
    check(s.parse_losses(log, "M3R/D01") == [0.37547, 0.15352], "SEG-072 log parsing")
    check(s.parse_losses("[M3R/C0/D01] epoch 3/30 loss 0.09156 t=64s", "M3R/C0/D01") == [0.09156], "SEG-075 log parsing")
    ls = s.loss_summary([0.4, 0.1, 0.13, 0.05])
    check(ls["spikes_gt20pct"] == 1 and ls["final"] == 0.05, f"loss summary {ls}")
    jobs = s.jobs()
    check(sum(1 for j in jobs if j["model"] == "M3R" and j["dir"] == ".") == 55, "55 new M3-R fits")
    check(sum(1 for j in jobs if j["model"] == "A") == 11 and sum(1 for j in jobs if j["dir"] == "repro") == 2, "11 A C2 fits, 2 refits")
    check(len({j["id"] for j in jobs}) == len(jobs), "job ids unique")
    if FAILURES:
        print(f"{len(FAILURES)} failure(s)")
        return 1
    print("segarecomp_xplat_stability_test: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
