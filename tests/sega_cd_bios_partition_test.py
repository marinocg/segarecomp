"""SEG-064-T003: deterministic translation-unit partitioning and incremental object reuse of the Sega CD BIOS-image profile.

On the project-authored synthetic BIOS-like image (tools/sega_cd_bios_fixture.py) and its bounded materialization fixed point
(tools/sega_cd_bios_build.py) this checks, against expectations computed here rather than by the code under test:
  - partitioned emission is stable (two emissions byte-identical), units come in (processor, window) order, every instruction function
    is defined exactly once across units and the set equals the single translation unit's; the program identity does not depend on
    partitioning; invalid windows are refused;
  - the single-unit fixed point (window 0, the SEG-064-T002 output) and partitioned fixed points (two windows) admit the same chain and
    their final programs give identical reports on the positive run, an epoch variant, negative cases and deadline partitions;
  - object reuse: a rebuild over a warm cache rebuilds nothing; a template round rebuilds only the template's unit and the shared
    dispatch unit; a changed compiler flag rebuilds every unit; a cold build in reverse compile order with one job links a byte-identical
    program.
usage: sega_cd_bios_partition_test.py <segarecomp> <cc> <project root>"""
import argparse
import importlib.util
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3])
sys.path.insert(0, str(ROOT / "tests"))
import sega_cd_common as common  # noqa: E402

FIXTURE = ROOT / "tools" / "sega_cd_bios_fixture.py"
DRIVER = ROOT / "tools" / "sega_cd_bios_build.py"
BUDGET = 5_000_000
failures = []


def check(condition, message):
    if not condition:
        failures.append(message)
        print("FAIL:", message)


def emit(cue, bios, out, *extra):
    r = subprocess.run([SEGARECOMP, "sega-cd", "emit", "--cue", str(cue), "--bios", str(bios), "--out", str(out), *map(str, extra)],
                       capture_output=True, text=True, timeout=300)
    return r.returncode, (json.loads(r.stdout) if r.stdout.strip() else {})


def drive(work, cue, bios, copy_spec, *extra):
    r = subprocess.run([sys.executable, str(DRIVER), "--segarecomp", SEGARECOMP, "--cc", CC, "--cue", str(cue), "--bios", str(bios),
                        "--work", str(work), "--copy-image", copy_spec, *map(str, extra)], capture_output=True, text=True, timeout=1800)
    summary = json.loads(r.stdout) if r.stdout.strip() else {}
    check(r.returncode == 0 and summary.get("ok"), "fixed point %s %s: %s" % (work.name, extra, r.stdout[-600:]))
    return summary


def run_program(exe, disc, bios, *extra):
    out = subprocess.run([str(exe), "--disc-dir", str(disc), "--bios", str(bios), "--instruction-budget", str(BUDGET), *map(str, extra)],
                         capture_output=True, text=True, timeout=common.RUN_TIMEOUT)
    return out.returncode, (json.loads(out.stdout) if out.stdout.strip() else {})


# A Windows PE link stamps a time into the executable: there the build-order property is checked on the link inputs (object bytes in
# link order) and on the program's behaviour; elsewhere the executable must also be byte-identical.
REPRODUCIBLE_LINK = os.name != "nt"
DEFINITION = re.compile(r"^static int (scd_p[01]_(?:i\d+|t|v)_[0-9A-F_]+)\(ScdMachine \*m\) \{$", re.M)

with tempfile.TemporaryDirectory() as tmp:
    tmp = pathlib.Path(tmp)
    disc = tmp / "disc"
    common.build_disc(disc, "comm")
    fx = tmp / "fx"
    expected = json.loads(subprocess.run([sys.executable, str(FIXTURE), "build", "--out", str(fx)], check=True, capture_output=True,
                                         text=True).stdout)
    bios = fx / "bios.bin"
    cue = disc / "disc.cue"
    copy = expected["copy_image"]
    copy_spec = "%s:0x%x:0x%x:0x%x" % (copy["processor"], copy["base"], copy["rom_offset"], copy["length"])

    # ---- partitioned emission: stability, order, unique ownership, identity ----
    single_dir, part_a, part_b = tmp / "single", tmp / "part-a", tmp / "part-b"
    for d in (single_dir, part_a, part_b):
        d.mkdir()
    code, single = emit(cue, bios, single_dir / "program.c", "--copy-image", copy_spec)
    check(code == 0 and single.get("units") == [] and single.get("partition_window") == 0, "single translation unit: no units")
    window = 0x400
    code_a, ra = emit(cue, bios, part_a / "program.c", "--copy-image", copy_spec, "--partition-window", window)
    code_b, rb = emit(cue, bios, part_b / "program.c", "--copy-image", copy_spec, "--partition-window", window)
    check(code_a == 0 and code_b == 0 and ra == rb, "two partitioned emissions report identically")
    names = [u["name"] for u in ra["units"]]
    check(len(names) >= 3, "the fixture spans several windows (%s)" % names)
    keys = [(u["cpu"], u["first_pc"]) for u in ra["units"]]
    check(keys == sorted(keys) and len(set(keys)) == len(keys), "units in (processor, window) order, one per window")
    check(all(u["first_pc"] % window == 0 and u["name"] == "p%d_%06X.c" % (u["cpu"], u["first_pc"]) for u in ra["units"]),
          "unit names and first PCs are the windows")
    for name in ["program.c"] + names:
        check((part_a / name).read_bytes() == (part_b / name).read_bytes(), "unit %s byte-identical across emissions" % name)
    check(ra["program_identity"] == single["program_identity"], "partitioning is not part of the program identity")
    check(sum(u["functions"] for u in ra["units"]) == ra["functions"] == single["functions"], "every function in exactly one unit")
    single_defs = DEFINITION.findall((single_dir / "program.c").read_text())
    unit_defs = []
    for u in ra["units"]:
        text = (part_a / u["name"]).read_text()
        defs = DEFINITION.findall(text)
        unit_defs += defs
        lo, hi = u["first_pc"], u["first_pc"] + window
        pcs = [int(d.rsplit("_", 1)[1], 16) for d in defs]
        check(all(lo <= pc < hi for pc in pcs), "unit %s holds only its window's PCs" % u["name"])
        check(u["functions"] == sum(1 for d in defs if "_v_" not in d), "unit %s function count" % u["name"])
        check("Program identity" not in text and ra["program_identity"] not in text, "unit %s does not embed the identity" % u["name"])
    check(len(unit_defs) == len(set(unit_defs)) and sorted(unit_defs) == sorted(single_defs),
          "unit definitions are the single unit's, each exactly once")
    shared = (part_a / "program.c").read_text()
    check(not DEFINITION.findall(shared) and "scd_p0_dispatch" in shared and "scd_p1_dispatch" in shared,
          "the shared unit holds only tables, lookups and the window dispatch")
    for bad in ("0x300", "0x80", "0x2000000", "0"):
        code, _ = emit(cue, bios, tmp / "bad.c", "--partition-window", bad)
        check(code == 2, "partition window %s refused" % bad)

    # ---- fixed points: single unit vs two partitionings ----
    s0 = drive(tmp / "w0", cue, bios, copy_spec, "--partition-window", 0)
    s1 = drive(tmp / "w1", cue, bios, copy_spec, "--partition-window", window)
    s2 = drive(tmp / "w2", cue, bios, copy_spec)  # the default window
    if not (s0.get("ok") and s1.get("ok") and s2.get("ok")):
        sys.exit(1)
    def chain(summary):  # an epoch admission names its snapshot file inside the work directory: compare up to the path
        return [a.rsplit(":", 1)[0] if a.startswith("epoch:") else a for a in summary["admissions"]]
    for s in (s1, s2):
        check(chain(s) == chain(s0), "same admission chain")
        check([h.get("admitted") for h in s["history"]] == [h.get("admitted") for h in s0["history"]], "same admitted observations")
        check(s["outcome"] == s0["outcome"] and s["program_identity"] == s0["program_identity"], "same candidate outcome")
    exes = [pathlib.Path(s["program"]) for s in (s0, s1, s2)]
    variants = [()] + [("--set", spec) for spec in expected["positive_variants"]] + [("--set", spec) for spec in expected["negatives"]]
    variants += [("--slice-seed", seed) for seed in (3, 11)]
    for extra in variants:
        reports = [run_program(exe, disc, bios, *extra) for exe in exes]
        check(reports[0] == reports[1] == reports[2], "single and partitioned programs agree on %s" % (extra,))

    # ---- object reuse and invalidation ----
    rounds = s1["round_metrics"]
    check(rounds[0]["rebuilt"] == rounds[0]["units"], "the first round builds every unit")
    for r in rounds[1:]:
        prior = s1["history"][r["round"] - 1].get("admitted", {}).get("kind")
        if prior == "operand_template":
            check(r["rebuilt"] <= 2 and r["reused"] >= r["units"] - 2,
                  "after a template admission only its unit and the shared unit rebuild (%s)" % r)
    check(any(s1["history"][r["round"] - 1].get("admitted", {}).get("kind") == "operand_template" for r in rounds[1:]),
          "a template round was observed")
    warm = drive(tmp / "w1", cue, bios, copy_spec, "--partition-window", window)
    check(warm.get("ok") and all(r["rebuilt"] == 0 for r in warm["round_metrics"]), "a warm rebuild compiles nothing")
    check(warm.get("link_inputs") == s1["link_inputs"], "warm rebuild links the same objects in the same order")
    if REPRODUCIBLE_LINK:
        check(pathlib.Path(warm["program"]).read_bytes() == exes[1].read_bytes(), "warm rebuild links a byte-identical program")
    flag = drive(tmp / "w1", cue, bios, copy_spec, "--partition-window", window, "--cflags=-DSEGARECOMP_T003_CACHE_PROBE=1")
    check(flag.get("ok") and flag["round_metrics"][0]["rebuilt"] == flag["round_metrics"][0]["units"],
          "a changed compiler flag rebuilds every unit")
    cold = drive(tmp / "w1-reverse", cue, bios, copy_spec, "--partition-window", window, "--jobs", 1, "--reverse-compile-order")
    check(cold.get("ok") and cold.get("link_inputs") == s1["link_inputs"],
          "a cold build in reverse order with one job links byte-identical objects in the same order")
    if cold.get("ok"):
        check(run_program(pathlib.Path(cold["program"]), disc, bios) == run_program(exes[1], disc, bios),
              "the reverse-order cold build runs identically")
        if REPRODUCIBLE_LINK:
            check(pathlib.Path(cold["program"]).read_bytes() == exes[1].read_bytes(),
                  "a cold build in reverse order with one job links a byte-identical program")

    # ---- the object key covers headers reached through relative includes outside the runtime directories ----
    tree = tmp / "tree"
    for part in ("tools", "platforms", "libs"):
        shutil.copytree(ROOT / part, tree / part, ignore=shutil.ignore_patterns("__pycache__"))
    spec = importlib.util.spec_from_file_location("copied_driver", tree / "tools" / "sega_cd_bios_build.py")
    copied = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(copied)

    def key():
        args = argparse.Namespace(work=str(tmp / "keywork"), cflags="", partition_window=0, jobs=1, bios=str(bios), cc=CC)
        return copied.Driver(args).key_base
    base_key = key()
    with open(tree / "libs" / "cpu" / "m68k" / "include" / "segarecomp" / "cpu" / "m68k" / "timing_core.h", "a") as f:
        f.write("\n/* cache probe */\n")
    core_key = key()
    with open(tree / "libs" / "device" / "sega" / "genesis" / "include" / "segarecomp" / "device" / "sega" / "genesis" /
              "controller_io_contract.h", "a") as f:
        f.write("\n/* cache probe */\n")
    device_key = key()
    (tree / "platforms" / "sega-cd" / "README.cache-probe").write_text("not a dependency\n")
    check(base_key != core_key != device_key and key() == device_key,
          "the object key changes with a CPU-core or device-contract header outside the runtime directories, not with an unrelated file")

if failures:
    print("%d failure(s)" % len(failures))
    sys.exit(1)
print("sega_cd_bios_partition_test: OK")
