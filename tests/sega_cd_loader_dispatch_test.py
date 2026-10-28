#!/usr/bin/env python3
"""SEG-098-T003 (ADR 0212): constant-argument dispatch through immutable PC-relative offset tables in the static loader scan - the pre-registered matrix.

`dispatch`: the disc SP reaches each module through one dispatcher (`add.w d0,d0 / move.w tab(pc,d0.w),d0 / jsr (0,pc,d0.w)`) called with a constant selector per arm.
`dispatch_unknown`: module C's selector is a Gate Array register read. usage: sega_cd_loader_dispatch_test.py <segarecomp> <c-compiler> <source-root>
"""
import json
import pathlib
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tests"))
import sega_cd_common as common  # noqa: E402

failures = []


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def discover(cue, *flags):
    proc = subprocess.run([SEGARECOMP, "sega-cd", "discover-modules", "--cue", str(cue), *flags], capture_output=True, text=True, timeout=120)
    return proc.returncode, json.loads(proc.stdout)


def mods(f):
    return sorted((m["block"], tuple(m["bases"]), tuple(m["entries"]), (m["range"]["offset"], m["range"]["length"]), m["sha256"]) for m in f["modules"])


def main():
    with tempfile.TemporaryDirectory(prefix="scd-dispatch-") as tmp:
        tmp = pathlib.Path(tmp)
        base = tmp / "base"
        common.build_disc(base, "unvisited", "cooked", "--unvisited-case", "table_header")
        dsp = tmp / "dsp"
        oracle = common.build_disc(dsp, "unvisited", "cooked", "--unvisited-case", "dispatch")
        unk = tmp / "unk"
        common.build_disc(unk, "unvisited", "cooked", "--unvisited-case", "dispatch_unknown")
        flags = ("--module-table-roots",)
        full = flags + ("--constant-dispatch",)

        _, ref = discover(base / "disc.cue", *flags)
        code0, d0 = discover(dsp / "disc.cue", *flags)
        check(code0 != 0 and not d0["modules"] and d0["survey"]["path_ends"].get("computed_call_pc_index", 0) >= 1 and not d0["refusals"],
              "without the rule the walk ends at the computed call: nothing is discovered (the M1 behaviour, unchanged: no refusal is added)")
        code1, d1 = discover(dsp / "disc.cue", *full)
        check(code1 == 0 and d1["complete"] and len(d1["modules"]) == 3, "with the rule: modules A, B and C are discovered through the dispatcher")
        strip = lambda m: [(a[0], a[1], a[2], a[3], a[4]) for a in m]  # noqa: E731
        check(strip(mods(d1)) == strip(mods(ref)), "...and equal (source, bases, entries, executable subrange, digest) to those found by the direct loaders of the entry-at-start/table fixture")
        sv = d1["survey"]
        check(sv["dispatch_resolved"] == 1 and sv["table_reads"] >= 3, "survey: one dispatch site resolved from three constant selectors (three immutable table reads)")
        check(oracle["modules"]["C"]["bases"] == [m for m in d1["modules"] if m["block"] == oracle["lba"]["C"]][0]["bases"], "C keeps both mappings through the dispatcher")

        code2, d2 = discover(unk / "disc.cue", *full)
        found = {m["block"] for m in d2["modules"]}
        check(code2 == 0 and found == {oracle["lba"]["A"], oracle["lba"]["B"]}, "unknown selector: the independently proven modules A and B are discovered, C is not")
        check([r["reason"] for r in d2["refusals"]] == ["computed_control_unresolved"], "unknown selector: the unresolved computed call is a typed refusal (the inventory knows it is incomplete behind it)")

        _, b0 = discover(base / "disc.cue", *flags)
        _, b1 = discover(base / "disc.cue", *full)
        check((b0["modules"], b0["operations"], b0["refusals"]) == (b1["modules"], b1["operations"], b1["refusals"]),
              "a disc with no computed control: the rule changes nothing (modules, operations, refusals)")
        _, again = discover(dsp / "disc.cue", *full)
        check(again == d1, "two discoveries are byte-identical")
        usage = subprocess.run([SEGARECOMP, "sega-cd", "discover-modules", "--cue", str(dsp / "disc.cue"), "--constant-dispatch", "--modules", "x"], capture_output=True, text=True)
        check(usage.returncode == 2, "the discovery accepts no module manifest")
    print("%d failures" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
