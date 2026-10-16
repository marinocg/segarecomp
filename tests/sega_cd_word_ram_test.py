#!/usr/bin/env python3
"""SEG-062-T004: Word RAM 2M/1M ownership, the Gate Array interrupt enables and the sub timer.

  * tests/tools/sega_cd_word_ram_table.c: the table of ownership transitions (2M main-owned, sub-owned, repeated and conflicting
    requests, 1M bank views and swaps, mode switches), the timer and the interrupt-enable rules, driven through the runtime's
    register paths against contract-derived expectations;
  * generated-native fixtures: `wordram2m` (sub fills Word RAM after DMNA, returns it with RET, main verifies), `wordram1m` (both
    processors use their bank at once, the sub swaps them), `timer` (five INT3 interrupts), `wordram_violation` (typed stop);
    hand-derived results, the pinned-Musashi two-processor reference (Word RAM byte for byte) when configured, two runs and eight
    deadline partitions identical.

usage: sega_cd_word_ram_test.py <segarecomp> <c-compiler> <source-root>
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
    with tempfile.TemporaryDirectory(prefix="scd-wordram-") as tmp:
        tmp = pathlib.Path(tmp)
        builder = common.Builder(SEGARECOMP, CC, tmp)
        table = tmp / "word_ram_table"
        subprocess.run([CC, *common.CFLAGS, "-I", str(common.SCD_RUNTIME), "-I", str(common.GENESIS_RUNTIME),
                        str(ROOT / "tests" / "tools" / "sega_cd_word_ram_table.c"), *[str(o) for o in builder.runtime_objects()],
                        "-o", str(table)], check=True)
        out = subprocess.run([str(table)], capture_output=True, text=True, timeout=60)
        rows = [line for line in out.stdout.splitlines() if line.startswith(("ok", "FAIL"))]
        check(out.returncode == 0 and len(rows) >= 40 and not any(r.startswith("FAIL") for r in rows),
              "ownership / bank / timer / interrupt-enable table: %d rows" % len(rows))
        for line in rows:
            if line.startswith("FAIL"):
                print("      " + line)

        checkout = common.musashi_checkout()
        reference = common.build_reference(checkout, CC, tmp / "musashi") if checkout else None
        if reference is None:
            print("skip  Musashi two-processor reference: no pinned checkout configured (%s)" % common.MUSASHI_ENVS[0])
        partitions = [["--slice-ticks", "977"], ["--slice-ticks", "65537"], ["--slice-ticks", "2147727"]]
        partitions += [["--slice-seed", str(s)] for s in (11, 12, 13, 14, 15)]
        for fixture, words in (("wordram2m", 3), ("wordram1m", 3), ("timer", 1)):
            disc, exe, expected, emitted = builder.program(fixture, fixture)
            code, report = common.run(exe, disc, 1000000)
            check(code == 0 and report["result"] == "idle" and common.result_words(report, words) == expected["result_words"],
                  "%s: generated-native result %s equals the hand-derived %s" % (
                      fixture, [hex(w) for w in common.result_words(report, words)], [hex(w) for w in expected["result_words"]]))
            check(all(p["unsupported"] == 0 for p in emitted["processors"]), fixture + ": every reached instruction compiled")
            check(common.run(exe, disc, 1000000)[1] == report, fixture + ": two process runs identical")
            same = [common.run(exe, disc, 1000000, *p)[1] == report for p in partitions]
            check(all(same), "%s: identical under %d deadline partitions (%s)" % (fixture, len(partitions), same))
            if fixture == "timer":
                check(report["sub_interrupts_taken"][3] == 5 and report["ga"]["interrupt_enable"] == 0,
                      "timer: exactly five level-3 interrupts, then INT3 disabled by the handler")
            if fixture == "wordram2m":
                check(report["ga"]["ret"] == 1 and report["ga"]["dmna"] == 0 and report["ga"]["mode_1m"] == 0,
                      "wordram2m: Word RAM returned to main (RET=1, DMNA=0)")
            if fixture == "wordram1m":
                check(report["ga"]["mode_1m"] == 1 and report["ga"]["ret"] == 1 and report["ga"]["comstat"][0] == 0xBEEF,
                      "wordram1m: 1M mode, banks swapped, the main CPU's marker reached the sub through the swap")
            if reference is not None and fixture != "timer":
                code, state = common.run_reference(reference, disc, tmp / ("ref-" + fixture))
                ok = code == 0 and state is not None
                check(ok, fixture + ": Musashi reference ran")
                if ok:
                    for k, name in ((0, "main"), (1, "sub")):
                        mine, ref = report["cpus"][k], state["cpus"][k]
                        check(mine["pc"] == ref["pc"] and mine["sr"] == ref["sr"] and mine["d"] == ref["d"] and mine["a"] == ref["a"],
                              "%s: Musashi reference agrees on %s registers" % (fixture, name))
                    check(report["digests"]["word_ram"] == state["word_ram_sha256"] and
                          report["digests"]["work_ram"] == state["work_ram_sha256"] and
                          report["digests"]["prg_ram"] == state["prg_ram_sha256"],
                          fixture + ": Musashi reference agrees on Word RAM, work RAM and PRG RAM byte for byte")
                    check(report["ga"]["ret"] == state["ret"] and report["ga"]["mode_1m"] == state["mode_1m"] and
                          report["ga"]["comstat"] == state["comstat"], fixture + ": Musashi reference agrees on RET/MODE/COMSTAT")
        disc, exe, expected, _ = builder.program("wordram_violation", "wordram_violation")
        code, report = common.run(exe, disc, 1000000)
        check(code == 10 and report["stop"]["class"] == "wordram_not_owned" and report["stop"]["cpu"] == 0 and
              report["stop"]["address"] == 0x200000, "main access to Word RAM owned by the sub: typed stop wordram_not_owned")
    if failures:
        print("%d failure(s)" % len(failures))
        return 1
    print("sega_cd_word_ram_test: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
