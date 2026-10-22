#!/usr/bin/env python3
"""SEG-077-T005: tests of the SEG-077 retraining / evaluation runner (synthetic data only).

Pure (standard library): the frozen development class rule, the neural-advantage rule, the final class precedence, labelled-cell blind
metrics (masked cells never count, misses use SEG-072's budget regions), the pre-registered job matrix, and the committed plan's frozen
rules. With numpy (research environment): label-free seeds and leakage tests - A's features, the seed set and (with torch) C's candidate
graph are bit-identical when classes.bin is replaced, so no label, ground-truth edge or reconstructed topology reaches A, B or C.
Exits 77 (CTest skip) for the numpy-dependent part when numpy is absent.
"""

import json
import pathlib
import random
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import segarecomp_seg077 as s7  # noqa: E402

FAILURES = []


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)
        print("FAIL:", msg)


def test_rules():
    d_imp = {f"L{i}": -0.02 for i in range(11)}
    check(s7.dev_class(d_imp) == "IMPROVED", "uniform improvement")
    check(s7.dev_class({f"L{i}": 0.02 for i in range(11)}) == "WORSENED", "uniform worsening")
    check(s7.dev_class({f"L{i}": (0.02 if i % 2 else -0.02) for i in range(11)}) == "NO_CHANGE", "mixed")
    check(s7.dev_class({f"L{i}": -0.001 for i in range(11)}) == "NO_CHANGE", "too small")
    ba = {f"T{i}": 0.30 for i in range(8)}
    bx = {f"T{i}": 0.25 for i in range(8)}
    check(s7.neural_advantage(bx, ba, 0.09, 0.10, d_imp, [0.095, 0.094, 0.093], 0, 0), "clear advantage")
    check(not s7.neural_advantage(bx, ba, 0.09, 0.10, d_imp, [0.095, 0.094, 0.11], 0, 0), "one seed above A blocks")
    check(not s7.neural_advantage(bx, ba, 0.09, 0.10, d_imp, [0.095, 0.094, 0.093], 3, 0), "more B50 misses blocks")
    check(not s7.neural_advantage({f"T{i}": 0.295 for i in range(8)}, ba, 0.09, 0.10, d_imp, [0.09] * 3, 0, 0), "blind gap < 0.01")
    check(not s7.neural_advantage(bx, ba, 0.098, 0.10, d_imp, [0.09] * 3, 0, 0), "development gap < 0.005")
    f = s7.final_class
    check(f(3, 16, {"CV2": True}, {"CV2": True}, False, {}) == "LABEL_TRUTH_INSUFFICIENT", "label truth first")
    check(f(8, 1, {"CV2": True}, {"CV2": True}, False, {}) == "DATA_INSUFFICIENT", "data second")
    check(f(8, 16, {"CV2": True}, {"CV2": False}, False, {}) == "ECONOMICS_NOT_JUSTIFIED", "economics")
    check(f(8, 16, {"CV2": True}, {"CV2": None}, False, {}) == "ECONOMICS_NOT_JUSTIFIED", "unmeasured economics never credits")
    check(f(8, 16, {"CV2": True}, {"CV2": True}, False, {}) == "C_ADVANTAGE", "C advantage")
    check(f(8, 16, {"B": True, "CV2": True}, {"B": True, "CV2": True}, False, {"B": 0.2, "CV2": 0.198}) == "EQUIVALENT_WITH_COST_DIFFERENCE",
          "two winners within 0.005")
    check(f(8, 16, {}, {}, True, {}) == "EQUIVALENT_WITH_COST_DIFFERENCE", "equivalent")
    check(f(8, 16, {}, {}, False, {}) == "A_PREFERRED", "A preferred")


def test_jobs_and_plan():
    js = s7.jobs()
    ids = [j["id"] for j in js]
    check(len(ids) == len(set(ids)), "unique job ids")
    check(sum(1 for j in js if j["model"] == "A" and j["cond"] == "K1") == 12, "A K1: 11 folds + all")
    check(sum(1 for j in js if j["model"] in ("B", "CV2") and j["cond"] == "K1") == 72, "B and C-v2 K1: 3 seeds x 12")
    check(sum(1 for j in js if j["repro"]) == 3, "three reuse checks")
    check(not any(j["cond"] == "K0" and j["fold"] != "all" and j["model"] == "CV2" and not j["repro"] for j in js),
          "C-v2 K0 folds are SEG-076's (reused only after the check)")
    if s7.PLAN.exists():
        doc = json.loads(s7.PLAN.read_text())
        check(doc["decision"] == json.loads(json.dumps(s7.DECISION)), "committed plan carries the frozen rules")
        check(doc["jobs"] == ids, "committed plan carries the job matrix")
        check(doc["conditions"]["K0"]["new_lineages"] == [] and doc["conditions"]["K1"]["new_lineages"] == sorted(s7.NEW_LINEAGES),
              "conditions")


def test_labelled_metrics():
    try:
        import segarecomp_ml_v3  # noqa: F401
    except Exception:
        return
    cell = 128
    classes = bytearray(b"U" * cell * 6)
    classes[0] = ord("I")                      # cell 0 positive
    classes[cell * 1:cell * 2] = b"D" * cell    # cell 1 negative
    classes[cell * 2:cell * 3] = b"D" * cell    # cell 2 negative
    classes[cell * 3] = ord("I")               # cell 3 positive
    # cells 4, 5 masked (U)
    scores = [5.0, 4.0, 1.0, 3.0, 9.0, 8.0]     # masked cells rank highest: they must not count as excess
    m = s7.labelled_metrics(bytes(classes), scores)
    check(m["labelled_positive"] == 2 and m["labelled_negative"] == 2, "labelled counts")
    check(abs(m["labelled_er"] - 0.5) < 1e-12, f"labelled ER counts only labelled negatives at or above the weakest positive {m}")
    check(m["informative"] is False, "informative needs >= 50 of each")
    check(m["misses_B35"] >= 1, "B35 region (2 of 6 cells) misses a labelled positive")


def plain(x):
    """Recursively turn tensors / arrays / containers into comparable Python values."""
    if hasattr(x, "tolist"):
        return x.tolist()
    if isinstance(x, dict):
        return {k: plain(v) for k, v in sorted(x.items())}
    if isinstance(x, (list, tuple)):
        return [plain(v) for v in x]
    return x


def test_numpy_leakage():
    try:
        import numpy as np  # noqa: F401
        import segarecomp_ml_v3_train as t
        import segarecomp_xplat_models as xm
    except Exception:
        return 77
    rnd = random.Random(7)
    size = 0x8000
    img = bytes(rnd.randrange(256) for _ in range(size))
    with tempfile.TemporaryDirectory() as td:
        rom = pathlib.Path(td) / "rom.bin"
        rom.write_bytes(img)
        tokens = bytes(rnd.randrange(3) if i % 8 == 0 else rnd.randrange(2) for i in range(size // 2 * 8))
        feats, graphs, seeds = [], [], []
        for variant in (b"U" * size, bytes(rnd.choice(b"IcDU") for _ in range(size))):
            d = pathlib.Path(td) / f"s{len(feats)}"
            d.mkdir()
            (d / "classes.bin").write_bytes(variant)
            (d / "tokens.bin").write_bytes(tokens)
            (d / "seeds.json").write_text("[]")
            (d / "relocs.json").write_text("[]")
            meta = {"id": "G077-99/cart", "lineage": "G077-99", "kind": "genesis_commercial", "image": str(rom),
                    "sha256": __import__("hashlib").sha256(img).hexdigest(), "size": size,
                    "identity": {"reset": 0x200, "reset_in_image": True}}
            (d / "meta.json").write_text(json.dumps(meta))
            im = s7.attach_new(t.Image(d), d)
            seeds.append((tuple(im.seed_addresses), tuple(sorted(im.seeds))))
            feats.append(xm.a_features(im, xm.relocs_of(im)))
            try:
                import torch
                graphs.append(plain(xm.window_graph(np, torch, im, 0, 2048)))
            except Exception:
                graphs.append(None)
        check(seeds[0] == seeds[1] == ((0x200,), (4,)), f"seeds come from the header only {seeds}")
        check(np.array_equal(feats[0], feats[1]), "A features are invariant to classes.bin")
        if graphs[0] is not None:
            check(graphs[0] == graphs[1], "C candidate graph is invariant to classes.bin")
    return 0


def main():
    test_rules()
    test_jobs_and_plan()
    test_labelled_metrics()
    rc = test_numpy_leakage()
    print("FAILURES:", len(FAILURES))
    if FAILURES:
        return 1
    return 77 if rc == 77 else 0


if __name__ == "__main__":
    sys.exit(main())
