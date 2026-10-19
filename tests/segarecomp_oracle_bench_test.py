#!/usr/bin/env python3
"""SEG-074-T001 (ADR 0156/0157): tests of tools/segarecomp_oracle_bench.py.

Without arguments: pure helpers on synthetic data (standard library only) - proposal construction (unknown bytes never excluded, adjacent
ranges merged, pure-data cells the only region exclusion), deployed-fallback accounting, per-lineage gate evaluation (every G1 condition, the
runtime blocker, G2/G3), generated-C counters and the rho normalization, the privacy filter of public records.

With `<segarecomp CLI> <C compiler>`: the synthetic exact-map fixture end to end through the real CLI, the report-only `close` producer, the
unchanged validator, strict-C11 compilation and a bounded generated-native run: the exact oracle is runtime-equivalent to broad, and
dropping the label of a dynamically reached instruction fails closed (typed stop) instead of being silently admitted.
"""

import json
import os
import pathlib
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import segarecomp_oracle_bench as b  # noqa: E402

FAILURES = []


def check(cond, msg):
    if not cond:
        FAILURES.append(msg)
        print("FAIL:", msg)


def proposals():
    check(b.merge_ranges([(4, 6), (0, 2), (2, 4), (10, 12)]) == [(0, 6), (10, 12)], "adjacent ranges are merged (the format rejects touching ranges)")
    for bad in ([(1, 3)], [(2, 2)]):
        try:
            b.merge_ranges(bad)
            check(False, f"odd/empty range must be rejected: {bad}")
        except ValueError:
            pass
    classes = b"IcDDUUIcIcccDDDD"
    check(b.oracle_instr_ranges(classes) == [(0, 2), (4, 10)], "instruction oracle ranges are merged")
    check(b.labelled_starts(classes) == [0, 6, 8], "labelled starts are the I positions")
    cells = b"D" * 128 + b"D" * 127 + b"U" + b"c" + b"D" * 127 + b"D" * 128
    check(b.oracle_region_cells(cells, []) == {1, 2}, "region oracle keeps every cell holding any I/c/U byte; only pure-data cells are excluded")
    check(b.oracle_region_cells(cells, [3, 99]) == {1, 2, 3}, "seed cells are added; out-of-range seeds are ignored")
    check(b.cell_ranges([1, 2], 300) == [(128, 256), (256, 300)], "cell ranges clamp to the image")
    text = b.regions_text("a" * 64, [(128, 256), (256, 300)])
    check(text.splitlines()[2] == "range 00000080 0000012c" and text.endswith("end\n"), "regions text merges and formats ranges")
    plan = "segarecomp.m68k_hybrid_admission_plan.v1\nrange 00000000 00000004\nrange 00000010 00000012\nend\n"
    check(b.admitted_from_plan(plan, [0, 2, 4, 16, 18]) == {0, 2, 16}, "admitted set = universe inside plan ranges")
    check(b.admitted_from_plan("strategy broad\nend\n", [0, 2]) == {0, 2}, "a broad plan admits the whole universe")


def strategy(cpu, c, accepted=True, plan_cpu=0.0, rho=0.0, runtime_equal=True):
    return {"accepted": accepted, "total_cpu": cpu, "generated_bytes": c, "plan_cpu": plan_cpu, "counts": {"rho": rho}, "runtime_equal": runtime_equal}


def image(lineage, broad, inc, region, instr, **kw):
    return {"lineage": lineage, "status": "measured", "strategies": {"BROAD": strategy(*broad), "INC": strategy(*inc), "ORACLE_REGION": strategy(*region),
                                                                       "ORACLE_INSTR": strategy(*instr, **kw)}}


def gate():
    dep = b.deployed({"accepted": False, "plan_cpu": 3.0}, {"total_cpu": 100.0, "generated_bytes": 1000})
    check(dep == {"cpu": 103.0, "generated_bytes": 1000, "fallback": True}, "a rejected plan is deployed as broad plus the attempt")
    # Eight lineages, strong instruction-level gain everywhere -> G1.
    imgs = {iid: image(L, (100, 1000), (60, 600), (50, 500), (20, 200)) for iid, L in b.GATE_IMAGES.items()}
    g = b.evaluate_gate(imgs)
    check(g["decision"] == "G1" and abs(g["g"] - 0.6) < 1e-9 and abs(g["h"] - (1 - 20 / 60)) < 1e-9 and len(g["lineages"]) == 8, f"G1: {g}")
    # Lineage weighting: D01 has three images; one bad D01 image cannot outweigh seven lineages, and the mean is per lineage.
    imgs2 = dict(imgs)
    imgs2["s1"] = image("D01", (100, 1000), (60, 600), (50, 500), (50, 500))
    g2 = b.evaluate_gate(imgs2)
    check(abs(g2["per_lineage_instr_vs_region_gain"]["D01"] - (1 - (1.0 + 0.4 + 0.4) / 3)) < 1e-9, "per-lineage mean over the lineage's images")
    # Region already close to instruction level, high representation headroom -> G2.
    close = {iid: image(L, (100, 1000), (60, 600), (50, 500), (47, 470)) for iid, L in b.GATE_IMAGES.items()}
    for v in close.values():
        v["strategies"]["ORACLE_REGION"]["counts"]["rho"] = 0.3
    check(b.evaluate_gate(close)["decision"] == "G2", "G2 when region ~ instruction and rho >= 0.20")
    for v in close.values():
        v["strategies"]["ORACLE_REGION"]["counts"]["rho"] = 0.05
    check(b.evaluate_gate(close)["decision"] == "G3", "G3 when neither holds")
    # A single favourable lineage cannot carry G1 (per-lineage condition).
    one = {iid: image(L, (100, 1000), (60, 600), (50, 500), (48, 480)) for iid, L in b.GATE_IMAGES.items()}
    one["d07"] = image("D07", (100, 1000), (60, 600), (50, 500), (1, 10))
    r = b.evaluate_gate(one)
    check(not r["g1_conditions"]["per_lineage"] and r["decision"] != "G1", "one favourable lineage does not select G1")
    # Validator acceptance on too few lineages blocks G1; the deployed accounting then uses broad for that lineage.
    rej = {iid: image(L, (100, 1000), (60, 600), (50, 500), (20, 200)) for iid, L in b.GATE_IMAGES.items()}
    for iid in ("s1", "flicky", "sf1"):
        rej[iid]["strategies"]["ORACLE_INSTR"]["accepted"] = False
    r = b.evaluate_gate(rej)
    check(r["oracle_instr_accepted_lineages"] == 5 and not r["g1_conditions"]["accepted"] and r["decision"] != "G1", f"acceptance condition: {r}")
    # A runtime difference of an accepted instruction oracle blocks G1.
    bad = {iid: image(L, (100, 1000), (60, 600), (50, 500), (20, 200)) for iid, L in b.GATE_IMAGES.items()}
    bad["d05"]["strategies"]["ORACLE_INSTR"]["runtime_equal"] = False
    r = b.evaluate_gate(bad)
    check(r["runtime_blocker"] and r["decision"] != "G1", "runtime non-equivalence blocks G1")
    # An image whose broad build is absent contributes nothing.
    partial = {"s1": {"lineage": "D01", "status": "measured", "strategies": {}}}
    check(b.evaluate_gate(partial)["g"] is None, "no ratios without a broad build")


def counters():
    seg = ("genesis_aot_entry_00001600: {\n  x = UINT32_C(0x00001600); y = UINT64_C(5632);\n  pc += UINT32_C(2);\n}\n")
    seg2 = seg.replace("00001600", "00001608").replace("5632", "5640")
    seg3 = seg.replace("pc += UINT32_C(2)", "pc += UINT32_C(4)")
    owner = "GenesisControlTransfer genesis_aot_owner_0004(GenesisRuntime *runtime) {\n  switch (runtime->pc) {\n  case UINT32_C(0x00001600): goto genesis_aot_entry_00001600;\n  }\n" + seg + seg2 + seg3 + "}\n"
    entries = "const uint32_t genesis_compiled_entry_addresses[] = {\n  0x00001600u,\n  0x00001608u,\n};\n"
    shared = "GenesisControlTransfer genesis_aot_shared_00000(GenesisRuntime *runtime, const void *s, uint32_t pc) {\n  return 0;\n}\n"
    with tempfile.TemporaryDirectory() as tmp:
        d = pathlib.Path(tmp)
        (d / "a.c").write_text(owner)
        (d / "e.c").write_text(entries + shared)
        c = b.generated_counts(d)
    check(c["entry_segments"] == 3 and c["aot_owner_functions"] == 1 and c["dispatch_cases"] == 1 and c["compiled_entry_rows"] == 2
          and c["aot_shared_helpers"] == 1, f"counters: {c}")
    check(c["duplicate_segment_bytes"] == len(seg2.encode()), "a segment differing only in address literals is a duplicate; a semantic difference is not")
    check(abs(c["rho"] - c["duplicate_segment_bytes"] / c["generated_bytes"]) < 1e-12, "rho = duplicate segment bytes / total generated bytes")
    single = "static const uint32_t genesis_compiled_entry_addresses[] = {\n  UINT32_C(0x00000200),\n  UINT32_C(0x00000206),\n};\n"
    check(b.compiled_entry_rows(single) == 2, "single-file dispatch table rows are counted")
    pub = b.public({"a": 1.23456789, "_pcs": [1], "p": pathlib.Path("/x"), "n": [{"_x": 1, "y": 2}]})
    check(pub == {"a": 1.2346, "n": [{"y": 2}]}, f"public() strips private keys and paths: {pub}")


def end_to_end(cli, cc):
    if not hasattr(os, "wait4"):
        print("SKIP: end-to-end measurement needs POSIX wait4")
        return 77
    build = ROOT / "build"  # the startup bridge requires an ignored output directory inside the product root (repository convention)
    build.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(dir=build, prefix="oracle-bench-") as tmp:
        work = pathlib.Path(tmp) / "work"
        out = pathlib.Path(tmp) / "out.json"
        argv = ["fixture", "--cli", cli, "--cc", cc, "--opt", "O0", "--frames", "2", "--timeout", "600", "--work", str(work), "--jobs", "2"]
        old = sys.stdout
        with open(out, "w") as f:
            sys.stdout = f
            try:
                rc = b.main(argv)
            finally:
                sys.stdout = old
        check(rc == 0, "fixture command succeeds")
        res = json.loads(out.read_text())
    exact, dropped = res["exact"]["strategies"], res["dropped_dynamic_target"]["strategies"]
    for name in ("ORACLE_REGION", "ORACLE_INSTR"):
        check(exact[name]["accepted"], f"{name}: the unchanged validator accepts the exact oracle")
        check(exact[name]["runtime_equal"] is True and exact[name]["executed_pcs_outside_broad"] == 0, f"{name}: runtime-equivalent to broad")
        check(exact[name]["c_outside_k"] == 0, f"{name}: every labelled start admitted")
    check(exact["ORACLE_INSTR"]["k"] == 12 and exact["ORACLE_INSTR"]["stats"]["k0"] == "12", "instruction oracle admits exactly the 12 labelled starts")
    check(exact["ORACLE_INSTR"]["generated_bytes"] < exact["ORACLE_REGION"]["generated_bytes"] < exact["BROAD"]["generated_bytes"],
          "instruction < region < broad generated C on the fixture")
    for name in ("BROAD", "ORACLE_REGION", "ORACLE_INSTR"):
        s = exact[name]
        check(s["compile_cpu"] >= 0 and s["object_bytes"] > 0 and s["executable_bytes"] > 0 and s["peak_compiler_rss_kib"] > 0, f"{name}: measured")
        check(s["cache"]["hits"] == s["cache"]["keyed_units"] and s["cache"]["misses"] == 0, f"{name}: warm rebuild hits every unit")
    check(exact["BROAD"]["runtime"]["outcome"] == "frames_reached", "the fixture reaches its frames under broad")
    d = dropped["ORACLE_INSTR"]
    check(d["accepted"] and d["k"] == 11, "dropping the dynamic-target label removes it from the exact-start admission")
    check(d["runtime_equal"] is False and d["runtime"]["outcome"] != "frames_reached",
          "an unadmitted dynamically reached instruction fails closed (typed stop), it is never silently executed")
    check(dropped["ORACLE_REGION"]["runtime_equal"] is True, "the region oracle still covers it (whole cell)")
    return 0


def main():
    proposals()
    gate()
    counters()
    rc = 0
    if len(sys.argv) == 3:
        rc = end_to_end(sys.argv[1], sys.argv[2])
    if FAILURES:
        print(f"{len(FAILURES)} failure(s)")
        return 1
    if rc == 77:
        return 77
    print("segarecomp_oracle_bench_test: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
