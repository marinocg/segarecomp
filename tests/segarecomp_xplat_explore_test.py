#!/usr/bin/env python3
"""SEG-076 (exploratory research tooling): synthetic tests of the SEG-076 evaluation module and runner.

- Barrier: the fail-closed sealed-subtree resolver refuses existing and NONEXISTENT paths under both slash conventions, drive and UNC
  prefixes, case variants, '..' traversal and symlinks into the sealed subtree, and reserved lineage / sample ids; development paths pass.
- Boundary strata: D-byte, adjacent-to-D-only and short-run positive cells on a synthetic class string; missed cells at B35/B50.
- Expansion rule: each of the four classes, the best-comparator choice, the lineage tolerance, the boundary / B50 protections, and that
  a failed model never qualifies.
- Runner: the plan refuses to verify when a bound file or the digest is tampered with (synthetic temporary copy).
"""

import json
import os
import pathlib
import shutil
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

FAILURES = []


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)
        print("FAIL:", msg)


def test_barrier():
    import segarecomp_xplat_explore_eval as ev
    res = {"neogeosdk", "space-invaders"}
    refused = ["/x/seg075/t003/snapshot/sealed/a", "/x/sealed", "C:\\cache\\seg075\\t003\\snapshot\\sealed\\new\\file.bin",
               "C:\\cache\\SEALED\\x", "\\\\server\\share\\sealed\\x", "D:/cache/Sealed/x", "relative\\sealed", "sealed",
               "/x/dev/../sealed/y", "C:\\x\\dev\\..\\sealed\\y", "/x/snapshot/dev/neogeosdk@1", "C:\\s\\dev\\space-invaders#0\\meta.json",
               "neogeosdk", "/x/Space-Invaders/y", "//srv/sealed/x", "\\\\?\\UNC\\srv\\sealed", "\\\\srv\\sealed\\x",
               "C:\\x\\sealed.\\y", "C:\\x\\sealed \\y", "C:\\x\\sealed::$DATA", "C:\\x\\SEALED~1\\y", "C:\\x\\NEOGEO~1",
               "C:\\x\\sealed:alt\\y", "/x/a/b/../../sealed"]
    for p in refused:
        check(ev.sealed_target(p, res), f"refused: {p!r}")
        try:
            ev.refuse([p], res)
            check(False, f"refuse raises for {p!r}")
        except SystemExit:
            pass
    allowed = ["/x/seg075/t003/snapshot/dev/toslibc@0", "C:\\cache\\seg076\\t001\\scores", "/x/sealedish/y", "/x/unsealed",
               "/x/sealed/../dev/z", "C:\\x\\sealed\\..\\dev\\z", "neogeosdk2"]
    for p in allowed:
        check(not ev.sealed_target(p, res), f"allowed: {p!r}")
    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        (td / "sealed" / "inner").mkdir(parents=True)
        (td / "dev").mkdir()
        link = td / "dev" / "innocent"
        try:
            os.symlink(td / "sealed" / "inner", link, target_is_directory=True)
        except (OSError, NotImplementedError):
            link = None  # unprivileged Windows: no symlink; the lexical checks above still apply
        if link is not None:
            check(ev.sealed_target(link, res), "symlink into sealed/ refused (existing target resolved)")
        check(ev.sealed_target(td / "sealed" / "not-yet-created" / "f", res), "nonexistent POSIX path under sealed/ refused")
        check(not ev.sealed_target(td / "dev" / "plain", res), "nonexistent development path allowed")
        if link is not None:
            check(ev.sealed_target(link / "newdir" / "f", res), "nonexistent child under a symlink into sealed/ refused")


def test_strata():
    import segarecomp_xplat_explore_eval as ev
    cell = 16
    # cells: 0 dense code, 1 code with a D byte, 2 D-only, 3 short code (2 starts, rest 'c'), 4 dense code, 5 'U' (masked)
    classes = (b"I" * 16) + (b"I" * 8 + b"D" * 8) + (b"D" * 16) + (b"Ic" * 2 + b"c" * 12) + (b"I" * 16) + (b"U" * 16)
    import segarecomp_ml_v3 as v3
    labels = v3.cell_labels(classes, cell)
    check(labels == [1, 1, 0, 1, 1, -1], f"labels {labels}")
    st = ev.boundary_strata(classes, labels, cell)
    check(st["d_byte"] == {1}, f"d_byte {st['d_byte']}")
    check(st["adjacent_d_only"] == {1, 3}, f"adjacent {st['adjacent_d_only']}")
    check(st["short_run"] == {1, 3}, f"short {st['short_run']}")
    check(st["boundary"] == {1, 3}, "boundary is the union")
    # scores rank cell 3 last: at B50 (3 of 6 cells) cells 0, 4, 1 retained -> boundary cell 3 missed
    scores = [5.0, 3.0, 1.0, -9.0, 4.0, 0.0]
    m = ev.image_metrics_ext(classes, [], scores, cell)
    check(m["strata"]["boundary"]["missed_B50"] == 1 and m["strata"]["d_byte"]["missed_B50"] == 0, f"strata misses {m['strata']}")
    check(m["strata"]["short_run"]["ranks"] == sorted([2 / 6, 5 / 6]), f"normalized ranks {m['strata']['short_run']['ranks']}")
    check(m["misses_B50"] == 2, f"two starts in cell 3 missed at B50 ({m['misses_B50']})")
    rows = ev.lineage_summary({"g/a": m}, {"g/a": "D01"})
    check(rows["D01"]["strata"]["boundary"]["missed_B50"] == 1 and rows["D01"]["informative_images"] == 1, "lineage summary")
    check(ev.quantiles([0.1, 0.2, 0.9]) == {"q50": 0.2, "q90": 0.9, "q100": 0.9}, f"quantiles {ev.quantiles([0.1, 0.2, 0.9])}")


def cfg(ers, b50=0, boundary=0):
    rows = {L: {"max_er": e} for L, e in ers.items()}
    vals = [e for e in ers.values() if e is not None]
    return {"rows": rows, "summary": {"mean_er": sum(vals) / len(vals), "misses_B50": b50, "boundary_missed_B50": boundary}}


def test_expansion():
    import segarecomp_xplat_explore_eval as ev
    rule = {"candidates": ["B", "C"], "comparators": ["M4_DENSE_s1@128", "M3_R_s49", "A"], "mean_er_margin": 0.02, "lineage_tolerance": 0.05}
    base = {"M4_DENSE_s1@128": cfg({"D01": 0.20, "D02": 0.10}), "M3_R_s49": cfg({"D01": 0.25, "D02": 0.12}), "A": cfg({"D01": 0.30, "D02": 0.2})}
    r = ev.expansion_class(dict(base, B=cfg({"D01": 0.17, "D02": 0.09}), C=cfg({"D01": 0.15, "D02": 0.10})), rule)
    check(r["best_comparator"] == "M4_DENSE_s1@128", "lowest mean ER comparator chosen")
    check(r["class"] == "EXPAND_B_AND_C", f"both qualify ({r['class']})")
    r = ev.expansion_class(dict(base, B=cfg({"D01": 0.19, "D02": 0.09}), C=cfg({"D01": 0.15, "D02": 0.10})), rule)
    check(r["class"] == "EXPAND_C", "B advantage 0.01 < 0.02")
    r = ev.expansion_class(dict(base, B=cfg({"D01": 0.10, "D02": 0.16}), C=None), rule)
    check(r["class"] == "NEGATIVE_STOP" and not r["candidates"]["B"]["lineage_ok"], "lineage worse by 0.06 > 0.05 fails")
    check(r["candidates"]["C"]["qualifies"] is False, "failed C never qualifies")
    r = ev.expansion_class(dict(base, B=cfg({"D01": 0.10, "D02": 0.05}, b50=1)), rule)
    check(r["class"] == "NEGATIVE_STOP" and not r["candidates"]["B"]["misses_ok"], "more B50 misses fails")
    r = ev.expansion_class(dict(base, B=cfg({"D01": 0.10, "D02": 0.05}, boundary=3)), rule)
    check(not r["candidates"]["B"]["boundary_ok"], "more boundary missed cells fails")
    r = ev.expansion_class(dict(base, B=cfg({"D01": 0.13, "D02": 0.13})), rule)
    check(r["class"] == "EXPAND_B", "exact 0.02 margin and 0.03 lineage regression qualify")
    a_best = dict(base, A=cfg({"D01": 0.05, "D02": 0.05}), B=cfg({"D01": 0.10, "D02": 0.05}))
    r = ev.expansion_class(a_best, rule)
    check(r["best_comparator"] == "A" and r["class"] == "NEGATIVE_STOP", "A as best comparator blocks B")
    tie = {"M4_DENSE_s1@128": cfg({"D01": 0.2}, b50=2), "M3_R_s49": cfg({"D01": 0.2}, b50=1), "A": None}
    check(ev.expansion_class(tie, rule)["best_comparator"] == "M3_R_s49", "tie broken by fewer B50 misses")


def test_plan_tamper():
    try:
        import segarecomp_xplat_explore as rx
    except Exception as e:  # pragma: no cover
        check(False, f"runner imports: {e}")
        return
    if not rx.PLAN.is_file():
        print("skip: plan not frozen in this tree")
        return
    with tempfile.TemporaryDirectory() as td:
        td = pathlib.Path(td)
        for n in list(rx.BOUND_CODE) + [rx.PLAN.name, rx.DIGEST.name]:
            shutil.copy(rx.HERE / n, td / n)
        doc = json.loads((td / rx.PLAN.name).read_text())
        check(set(doc["bound_code"]) == set(rx.BOUND_CODE), "plan binds every listed file")
        check(doc["expansion_rule"]["mean_er_margin"] == 0.02 and doc["expansion_rule"]["lineage_tolerance"] == 0.05, "frozen thresholds")
        check(doc["resources"]["attempts"] == 1, "one attempt per fit")
        doc["expansion_rule"]["mean_er_margin"] = 0.01
        (td / rx.PLAN.name).write_text(json.dumps(doc))
        old = (rx.PLAN, rx.DIGEST)
        rx.PLAN, rx.DIGEST = td / rx.PLAN.name, td / rx.DIGEST.name
        try:
            rx.verify_plan()
            check(False, "a tampered plan must not verify")
        except SystemExit as e:
            check("digest" in str(e), f"tamper detected by digest ({e})")
        finally:
            rx.PLAN, rx.DIGEST = old


def test_rocm_amendment():
    import segarecomp_xplat_explore as rx
    p = rx.HERE / "segarecomp_xplat_explore.t001.rocm.plan.json"
    if not p.is_file():
        print("skip: no ROCm amendment in this tree")
        return
    import hashlib
    doc = json.loads(p.read_text())
    digest = (rx.HERE / "segarecomp_xplat_explore.t001.rocm.plan.digest").read_text().strip()
    check(hashlib.sha256(rx.canonical(doc)).hexdigest() == digest, "amendment digest matches")
    check(doc["t001_plan_sha256"] == rx.plan_sha(), "amendment bound to the frozen T001 plan")
    check(doc["resources"]["parallel_fits"] == 1 and doc["resources"]["attempts"] == 1, "serial ROCm, one attempt")
    check(doc["seed"] == rx.PILOT_SEED, "same pilot seed")
    want = [f"{m}.{f}" for m in ("B", "C") for f in rx.GENESIS + ("all",)] + ["B.D05.repro", "C.D05.repro"]
    check(doc["jobs"] == want, "amendment jobs: B and C on every fold plus determinism refits")
    check("never replacing" in doc["reporting"], "amendment never replaces the pre-registered class")


def main() -> int:
    test_barrier()
    test_strata()
    test_expansion()
    test_plan_tamper()
    test_rocm_amendment()
    if FAILURES:
        print(f"{len(FAILURES)} failure(s)")
        return 1
    print("segarecomp_xplat_explore_test: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
