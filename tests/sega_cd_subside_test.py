#!/usr/bin/env python3
"""SEG-064-T004: the sub-side boot hardware of the Sega CD runtime - stopwatch, PCM write state, graphics ASIC with its level-1
completion interrupt, CDD TOC/seek/pause/resume/jump commands and the CDC host-data transfer.

  * tests/tools/sega_cd_subside_table.c: table rows driven through the runtime's own register paths against expectations derived by
    hand from the documented register behaviour (the graphics rows additionally against an independent geometric oracle), with typed
    stops checked for their processor, pc, address, width and direction and for leaving the state untouched;
  * the generated-native fixture `subside` (tools/sega_cd_fixture_disc.py): the stopwatch read at cycle offsets computed from a
    hand-written MC68000 cycle table (including the 12-bit rollover), and the level-1 request disabled, accepted, masked and taken
    together with level 2 (priority), acknowledged at acceptance; two runs and eight deadline partitions identical.

usage: sega_cd_subside_test.py <segarecomp> <c-compiler> <source-root>
"""
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


def main():
    with tempfile.TemporaryDirectory(prefix="scd-subside-") as tmp:
        tmp = pathlib.Path(tmp)
        builder = common.Builder(SEGARECOMP, CC, tmp)
        table = tmp / ("subside_table" + common.EXE)
        subprocess.run([CC, *common.CFLAGS, "-I", str(common.SCD_RUNTIME), "-I", str(common.GENESIS_RUNTIME),
                        str(ROOT / "tests" / "tools" / "sega_cd_subside_table.c"), *[str(o) for o in builder.runtime_objects()],
                        "-o", str(table)], check=True)
        out = subprocess.run([str(table)], capture_output=True, text=True, timeout=120)
        rows = [line for line in out.stdout.splitlines() if line.startswith(("ok", "FAIL"))]
        check(out.returncode == 0 and len(rows) >= 100 and not any(r.startswith("FAIL") for r in rows),
              "stopwatch / PCM / graphics ASIC / CDD commands / CDC host reads table: %d rows" % len(rows))
        for line in rows:
            if line.startswith("FAIL"):
                print("      " + line)

        disc, exe, expected, emitted = builder.program("subside", "subside")
        check(all(p["unsupported"] == 0 for p in emitted["processors"]), "subside: every reached instruction compiled")
        code, report = common.run(exe, disc, 6000000)
        check(code == 0 and report["result"] == "idle", "subside: the run ends idle (exit %d, %s)" % (code, report.get("result")))
        d = report["cpus"][1]["d"]
        for register, value in expected["stopwatch_d"].items():
            check(d[int(register[1])] == value, "stopwatch on the CPU: %s = %d (hand-derived from the MC68000 cycle table), got %d" % (
                register, value, d[int(register[1])]))
        check(report["ga"]["comstat"] == expected["comstat"],
              "level 1: COMSTAT %s (disabled: 0 interrupts; accepted: 1; masked: still 1, then 2 after the unmask; level 2 handled "
              "before level 1 = order slots 4 and 3; 3 level-1 interrupts) == %s" % (expected["comstat"], report["ga"]["comstat"]))
        check(report["sub_interrupts_taken"][1] == expected["interrupts"]["1"] and report["sub_interrupts_taken"][2] == expected["interrupts"]["2"],
              "level 1 taken 3 times and level 2 once")
        check(report["ga"]["pending"] == 0 and report["ga"]["ifl2"] == 0 and report["cpus"][1]["stopped"] == 1,
              "every request was acknowledged at acceptance (nothing pending, IFL2 clear)")
        # typed stops on the CPU: the faulting instruction is the first not to retire, nothing mutated beyond the earlier instructions
        for case, want in sorted(expected["negatives"].items()):
            ncode, neg = common.run(exe, disc, 6000000, "--set", "sub:a6=" + case)
            stop = neg.get("stop", {})
            check(ncode == 10 and stop.get("class") == want["class"] and stop.get("cpu") == 1 and stop.get("pc") == want["pc"] and
                  stop.get("address") == want["address"] and stop.get("width") == want["width"] and stop.get("write") == want["write"],
                  "%s: typed stop %s on the sub at pc $%X, address $%X, width %d, %s (got %s)" % (
                      want["label"], want["class"], want["pc"], want["address"], want["width"], "write" if want["write"] else "read", stop))
            check(neg["cpus"][1]["retired"] == want["retired"] and neg["cpus"][1]["pc"] == want["pc"],
                  "%s: %d instructions retired and the faulting instruction did not (pc still at it)" % (want["label"], want["retired"]))
            dev = neg["sub_devices"]
            check(dev["gfx"]["busy"] == 0 and dev["gfx"]["operations"] == 0 and dev["stopwatch"]["base"] == 0 and
                  dev["pcm"]["onoff"] == 0 and dev["pcm"]["enabled"] == 0,
                  "%s: the stopwatch, graphics and PCM state are untouched" % want["label"])
            check(common.run(exe, disc, 6000000, "--set", "sub:a6=" + case, "--slice-seed", "5")[1] == neg, "%s: the stop is identical under a sliced run" % want["label"])
        # the STOP wake rule: with the main processor stopped for good, a sub waiting in STOP for the graphics interrupt is not idle
        wcode, wake = common.run(exe, disc, 6000000, "--set", "main:d7=9", "--set", "sub:a6=9")
        check(wcode == 0 and wake["result"] == "idle" and wake["ga"]["comstat"][0] == expected["wake"]["comstat0"] and
              wake["ga"]["sub_flag"] == expected["wake"]["sub_flag"] and wake["sub_interrupts_taken"][1] == expected["wake"]["level1"],
              "level-1 STOP wake: the sub resumes when the graphics operation ends, although the main processor is stopped (and the run then ends idle)")
        check(common.run(exe, disc, 6000000, "--set", "main:d7=9", "--set", "sub:a6=9", "--slice-seed", "9")[1] == wake, "level-1 STOP wake: identical under a sliced run")
        baseline = report
        check(common.run(exe, disc, 6000000)[1] == baseline, "subside: two process runs give identical reports")
        partitions = [["--slice-ticks", str(t)] for t in (100000000, 777777777, 50000000000)]
        partitions += [["--slice-seed", str(s)] for s in range(1, 9)]
        same = [common.run(exe, disc, 6000000, *p)[1] == baseline for p in partitions]
        check(all(same), "subside: identical state for %d deadline partitions (%s)" % (len(partitions), same))
    if failures:
        print("%d failure(s)" % len(failures))
        return 1
    print("sega_cd_subside_test: OK")
    return 0


sys.exit(main())
