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
    # the reviewer's reproducer: B fitted although its gate is closed must never be selected
    r = x5.candidates_and_gates(t004(), {"A": tab(0.06, 0.2), "B": tab(0.01, 0.2)})
    check(not r["gate_B"] and r["selected"]["config"] != "B", "closed-gate B never enters selection")
    r = x5.candidates_and_gates(t004(), {"A": tab(0.24, 0.2), "B": tab(0.23, 0.2), "C": tab(0.01, 0.0)})
    check(not r["gate_C"] and r["selected"]["config"] != "C", "closed-gate C never enters selection")
    try:
        x5.candidates_and_gates(t004(), {"B": tab(0.2, 0.2)})
        check(False, "gate evaluation without A refused")
    except SystemExit:
        pass


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
    im2 = make_image(words)
    im2.tokens[:] = im.tokens
    f2 = xm.a_features(im2)
    check(f1.shape == (1, 210), "A features: 70 base columns x (self, +/-1, +/-4)")
    import numpy as np
    check(np.array_equal(f1, f2), "A features are deterministic")
    # DBcc -4 at word 7 (+ disp word) -> word 7 + 1 - 2 = 6; JSR (d16,PC) +6 at word 9 -> 9 + 1 + 3 = 13; JMP abs.L 0x00000004 -> word 2;
    # JMP abs.L outside the image -> no target
    w2 = [0x4E71] * 7 + [0x51C8, 0xFFFC, 0x4EBA, 0x0006, 0x4EF9, 0x0000, 0x0004, 0x4EF9, 0x0010, 0x0000, 0x4E75]
    im3 = make_image(w2)
    for w, (status, length, control) in {7: (2, 2, 1), 9: (2, 2, 3), 11: (2, 3, 2), 14: (2, 3, 2)}.items():
        im3.tokens[w, 0], im3.tokens[w, 1], im3.tokens[w, 3] = status, length, control
    s3 = xm.structure(im3)
    check(int(s3["target"][7]) == 6, "DBcc displacement target")
    check(int(s3["target"][9]) == 13, "JSR (d16,PC) target")
    check(int(s3["target"][11]) == 2 and int(s3["target"][14]) == -1, "JMP abs.L as an in-image offset; outside the image: none")
    try:
        import torch
    except ImportError:
        return
    for graph in (False, True):
        m = xm.build_bc(torch, graph)
        check(sum(p.numel() for p in m.parameters()) > 0, f"B/C builds (graph={graph})")
    import segarecomp_ml_v3_train as tr
    big = make_image([0x4E71] * 3000)
    big.tokens[:, 0], big.tokens[:, 1] = 2, 1
    big.wlabels = np.zeros(3000, dtype=np.int8)
    reach, edges = xm.window_graph(np, torch, big, 0, 2048)
    L = 2048 + 2 * tr.M4["margin"]
    check(len(reach) == L and all(int(s.max()) < L and int(d.min()) >= 0 for s, d in edges if s.numel()), "window graph in window coordinates")
    ft_s, ft_d = edges[0]
    check(bool(((ft_d - ft_s) == 1).all()), "fall-through edges follow decoder length (1 word NOPs)")


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
