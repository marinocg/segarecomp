#!/usr/bin/env python3
"""SEG-076-T005 (exploratory research tooling): synthetic tests of the pre-registered C-v2 vs A rule and the job list; when torch is
installed (research environment), C-v2's padded in-edge table on a hand-built edge set.

- cv2_class: C advantage (interval below 0, |mean| >= 0.01, no extra B50 misses), A advantage, bounded no-difference, insufficient (wide
  interval, failed fits, non-deterministic refit), and that a C advantage with extra B50 misses is not an advantage
- jobs: 33 fold fits (3 seeds x 11 folds), the seed-1 'all' fit and one determinism refit
"""
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
    import segarecomp_xplat_cv2 as c
    adv = c.cv2_class([-0.03, -0.031, -0.029], [0, 0, 0], 0, True, True)
    check(adv["class"] == "C_ADVANTAGE_OVER_A", f"clear C advantage {adv}")
    extra = c.cv2_class([-0.03, -0.031, -0.029], [0, 2, 0], 0, True, True)
    check(extra["class"] != "C_ADVANTAGE_OVER_A", "extra B50 misses block a C advantage")
    aadv = c.cv2_class([0.03, 0.031, 0.029], [0, 0, 0], 0, True, True)
    check(aadv["class"] == "A_ADVANTAGE_OVER_C", "clear A advantage")
    tie = c.cv2_class([0.001, -0.001, 0.0005], [0, 0, 0], 0, True, True)
    check(tie["class"] == "NO_MEANINGFUL_DIFFERENCE_BOUNDED", f"bounded tie {tie}")
    wide = c.cv2_class([-0.05, 0.04, 0.01], [0, 0, 0], 0, True, True)
    check(wide["class"] == "EVIDENCE_INSUFFICIENT", "wide interval")
    check(c.cv2_class([-0.03] * 3, [0] * 3, 0, False, True)["class"] == "EVIDENCE_INSUFFICIENT", "failed fits")
    check(c.cv2_class([-0.03] * 3, [0] * 3, 0, True, False)["class"] == "EVIDENCE_INSUFFICIENT", "non-deterministic refit")
    check(c.cv2_class([-0.03] * 3, [0] * 3, 0, True, None)["class"] == "EVIDENCE_INSUFFICIENT", "missing determinism refit")
    ci = c.t_interval([1.0, 2.0, 3.0])
    check(abs(ci["half_width"] - round(4.303 * 1.0 / 3 ** 0.5, 6)) < 1e-6, f"t interval df=2 {ci}")
    jobs = c.jobs()
    check(sum(1 for j in jobs if j["dir"] == "." and j["fold"] != "all") == 33, "33 fold fits")
    check(sum(1 for j in jobs if j["fold"] == "all") == 1 and sum(1 for j in jobs if j["dir"] == "repro") == 1, "all fit and refit")
    try:
        import numpy as np
        import torch
        import segarecomp_xplat_models_v2 as v2
    except Exception:
        print("skip: torch/numpy not installed (padded-table check)")
    else:
        e = lambda s, d: (torch.tensor(s, dtype=torch.int64), torch.tensor(d, dtype=torch.int64))  # noqa: E731
        edges = [[e([600, 601], [602, 602]), e([600], [602]), e([], [])]]
        dst, pad, mask = v2.padded_table(np, torch, edges, [512], 1, torch.device("cpu"))
        N = 1 * 1 * (256 + 128)
        lo = 512 - 64
        check(dst.tolist() == [602 - lo], f"one destination {dst.tolist()}")
        check(sorted(pad[0][mask[0] > 0].tolist()) == sorted([600 - lo, 601 - lo, N + 600 - lo]), f"typed sources {pad.tolist()}")
        check(v2.padded_table(np, torch, [[e([], []), e([], []), e([], [])]], [512], 1, torch.device("cpu")) is None, "no edges -> None")
    if FAILURES:
        print(f"{len(FAILURES)} failure(s)")
        return 1
    print("segarecomp_xplat_cv2_test: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
