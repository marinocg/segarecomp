#!/usr/bin/env python3
"""SEG-076-T003 (exploratory research tooling): synthetic tests of the pre-registered T003 statistics and classes.

- exact sign-flip permutation p (all-same-sign, symmetric, zero cases); 95% t interval over 4 paired seeds (t_0.975,3 = 3.182)
- transfer classes: MEASURABLE (both directions), EVIDENCE_INSUFFICIENT (wide interval), NO_MEANINGFUL_ADVANTAGE (narrow, null), the
  0.01 magnitude floor and the lineage-test requirement
- seed-stability class: the 0.6 spread ratio in BOTH conditions and for BOTH statistics, and the 4-seed-ensemble mean condition
- loss-log parsing of the preserved SEG-072 / SEG-075 epoch lines; the job list (55 new M3-R fits, 11 A C2 fits, 1 determinism refit)
"""
import math
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
FAILURES = []


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)
        print("FAIL:", msg)


def main() -> int:
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
    check(tiny["class"] == "NO_MEANINGFUL_ADVANTAGE", f"below the 0.01 floor {tiny['class']}")
    wide = s.transfer_class([-0.2, 0.1, -0.15, 0.12], [-0.02] * 11)
    check(wide["class"] == "EVIDENCE_INSUFFICIENT", f"wide interval {wide['class']}")
    one_lineage = s.transfer_class([-0.03, -0.031, -0.029, -0.03], [-0.3] + [0.001, -0.001] * 5)
    check(one_lineage["class"] != "TRANSFER_EFFECT_MEASURABLE", "a one-lineage effect fails the lineage test")
    single = {"C0": {"mean_er": [0.10, 0.20, 0.10, 0.20], "d01": [0.2, 0.7, 0.2, 0.7]},
              "C2": {"mean_er": [0.10, 0.20, 0.10, 0.20], "d01": [0.2, 0.7, 0.2, 0.7]}}
    pairs = {c: {"mean_er": [0.15] * 6, "d01": [0.45] * 6} for c in ("C0", "C2")}
    r = s.stability_class(single, pairs, {"C0": {"mean_er": 0.14}, "C2": {"mean_er": 0.14}})
    check(r["class"] == "SEED_STABILITY_IMPROVED", f"zero pair spread improves {r}")
    pairs_bad = {"C0": {"mean_er": [0.10, 0.20] * 3, "d01": [0.45] * 6}, "C2": pairs["C2"]}
    check(s.stability_class(single, pairs_bad, {"C0": {"mean_er": 0.14}, "C2": {"mean_er": 0.14}})["class"] == "NO_MEANINGFUL_ADVANTAGE",
          "one condition / statistic not reduced -> no improvement")
    check(s.stability_class(single, pairs, {"C0": {"mean_er": 0.20}, "C2": {"mean_er": 0.14}})["class"] == "NO_MEANINGFUL_ADVANTAGE",
          "a 4-seed ensemble worse than the single-seed mean -> no improvement")
    log = "[M3R/D01] epoch 1/30 loss 0.37547 t=19s\nnoise\n[M3R/D01] epoch 2/30 loss 0.15352 t=45s\n[M3R/D02] epoch 1/30 loss 0.5 t=1s\n"
    check(s.parse_losses(log, "M3R/D01") == [0.37547, 0.15352], "SEG-072 log parsing")
    check(s.parse_losses("[M3R/C0/D01] epoch 3/30 loss 0.09156 t=64s", "M3R/C0/D01") == [0.09156], "SEG-075 log parsing")
    ls = s.loss_summary([0.4, 0.1, 0.13, 0.05])
    check(ls["spikes_gt20pct"] == 1 and ls["final"] == 0.05, f"loss summary {ls}")
    jobs = s.jobs()
    check(sum(1 for j in jobs if j["model"] == "M3R" and j["dir"] == ".") == 55, "55 new M3-R fits")
    check(sum(1 for j in jobs if j["model"] == "A") == 11 and sum(1 for j in jobs if j["dir"] == "repro") == 1, "11 A C2 fits, 1 refit")
    check(len({j["id"] for j in jobs}) == len(jobs), "job ids unique")
    if FAILURES:
        print(f"{len(FAILURES)} failure(s)")
        return 1
    print("segarecomp_xplat_stability_test: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
