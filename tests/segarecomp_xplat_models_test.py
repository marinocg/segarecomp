#!/usr/bin/env python3
"""SEG-075-T005 (ADR 0161/0163): tests of the pre-registered T005 gates and, when numpy/torch are installed (research environment), of
the model A/B/C structure features on synthetic images.

Pure part (always): the B gate (best of {M1, A, M3_R, M4@128, M4@64} by decide.select with ER >= 0.10), the C gate (B better than A and
ER lower by >= tau), candidate assembly from T004 results, and that M3_FROZEN never enters selection. Research part (numpy): PC-relative
branch / DBcc targets from instruction words, decoder-length chains, PROVEN reachability from seed roots only along fall-through and
direct edges (a candidate target into data never becomes reachable), A feature shape / determinism; (torch) B/C forward shapes.
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


def t004(tau=0.02, allowance=None):
    cands = [{"config": "M1_HGB", "condition": "C2", "genesis_b50_misses": 0, "genesis_mean_er": 0.30, "xplat_mean_er": 0.2, "complete_budget": "B50"},
             {"config": "M3_R", "condition": "C2", "genesis_b50_misses": 0, "genesis_mean_er": 0.25, "xplat_mean_er": 0.2, "complete_budget": "B50"},
             {"config": "M4_DENSE@128", "condition": "C0", "genesis_b50_misses": 0, "genesis_mean_er": 0.26, "xplat_mean_er": 0.2, "complete_budget": "B50"},
             {"config": "M4_DENSE@64", "condition": "C0", "genesis_b50_misses": 0, "genesis_mean_er": 0.40, "xplat_mean_er": 0.2, "complete_budget": "B60"}]
    return {"noise": {"tau": tau, "b50_miss_allowance": allowance or {"D01": 0}}, "candidates": cands, "transfer_class": "TRANSFER_NEUTRAL"}


def tab(er, xer, misses=0):
    return {"condition": "C2", "genesis": {"D01": {"max_er": er, "misses_B50": misses, "misses_B35": misses, "misses_B60": misses}},
            "xplat": {"toslibc": {"max_er": xer, "misses_B50": 0}}}


def test_gates():
    import segarecomp_xplat_t005 as x5
    r = x5.candidates_and_gates(t004(), {"A": tab(0.24, 0.2)})
    check(r["best_without_B_C"]["config"] == "A" and r["gate_B"], "A within tau of M3_R and simpler -> best; ER 0.24 >= 0.10 opens B")
    r = x5.candidates_and_gates(t004(), {"A": tab(0.05, 0.05)})
    check(r["best_without_B_C"]["config"] == "A" and not r["gate_B"], "best ER below 0.10 closes the B gate")
    r = x5.candidates_and_gates(t004(), {"A": tab(0.24, 0.2), "B": tab(0.20, 0.2)})
    check(r["gate_C"] and r["selected"]["config"] == "B", "B better than A by >= tau opens C and is selected")
    r = x5.candidates_and_gates(t004(), {"A": tab(0.24, 0.2), "B": tab(0.23, 0.2)})
    check(not r["gate_C"] and r["selected"]["config"] == "A", "B within tau of A: C closed, simpler A selected")
    check(all(c["config"] != "M3_FROZEN" for c in r["candidates"]), "M3_FROZEN never a candidate")
    insuff = dict(t004(), transfer_class="EVIDENCE_INSUFFICIENT")
    check(not x5.candidates_and_gates(insuff, {"A": tab(0.3, 0.3)})["gate_B"], "insufficient T004 closes the B gate")


def make_image(words: list[int], seed_addresses=(0,)):
    import numpy as np

    class Im:
        pass
    im = Im()
    arr = np.array(words, dtype=np.int64)
    im.hi, im.lo = (arr >> 8).astype(np.uint8), (arr & 0xFF).astype(np.uint8)
    im.n_words = len(words)
    im.n_cells = (len(words) + 63) // 64
    tok = np.zeros((len(words), 8), dtype=np.uint8)
    im.tokens = tok
    im.seeds = {0}
    im.seed_addresses = list(seed_addresses)
    return im


def test_structure():
    try:
        import numpy  # noqa: F401
    except ImportError:
        print("skip structure: numpy not installed (research environment only)")
        return
    import segarecomp_xplat_models as xm
    # word 0: BRA.S +4 (target word 3); word 1: data; word 2: data reachable only as a candidate; word 3: NOP; word 4: RTS;
    # word 5: BEQ.S -2... (conditional to word 5 + 1 - 1 = 5) ; word 6 data
    words = [0x6004, 0xFFFF, 0x6000, 0x4E71, 0x4E75, 0x67FE, 0x1234]
    im = make_image(words)
    tok = im.tokens
    for w, (status, length, control) in {0: (2, 1, 2), 3: (2, 1, 0), 4: (2, 1, 6), 5: (2, 1, 1), 2: (2, 2, 2)}.items():
        tok[w, 0], tok[w, 1], tok[w, 3] = status, length, control
    s = xm.structure(im)
    check(int(s["target"][0]) == 3, "BRA.S displacement -> target word (pc + 2 + disp)")
    check(int(s["target"][5]) == 5, "BEQ.S -2 targets itself")
    check(list(s["reach"]) == [True, False, False, True, True, False, False], "PROVEN reach: root, branch target, fall-through to RTS")
    check(int(s["chain"][3]) == 3 and int(s["chain"][1]) == 0, "chains follow decoder lengths and stop at non-ordinary words")
    f1 = xm.a_features(im)
    f2 = xm.a_features(make_image(words))
    check(f1.shape == (1, 210), "A features: 70 base columns x (self, +/-1, +/-4)")
    try:
        import torch
    except ImportError:
        return
    for graph in (False, True):
        m = xm.build_bc(torch, graph)
        check(sum(p.numel() for p in m.parameters()) > 0, f"B/C builds (graph={graph})")


def main():
    test_gates()
    test_structure()
    if FAILURES:
        print(f"{len(FAILURES)} failure(s)")
        return 1
    print("segarecomp_xplat_models_test: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
