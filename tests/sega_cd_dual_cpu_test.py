#!/usr/bin/env python3
"""SEG-062-T003 (Checkpoint B, ADR 0128): two MC68000 processors executing generated-native C11 under one scheduler.

The `comm` fixture disc: the main CPU writes the sub's reset vectors and a parameter through the PRG-RAM window (banks 0 and 1),
releases the sub, posts a command through COMCMD and IFL2; the sub's INT2 handler computes a response into COMSTAT; the main CPU
publishes it at $FFC000. Checked here:
  * the result equals the hand-derived expectation, and the final state of both processors and all shared memory equals the pinned
    Musashi two-processor reference (when a pinned checkout is configured; otherwise reported as skipped, never passed);
  * identical reports across two process runs and across 13 different deadline partitions (fixed and pseudo-random slices);
  * independence: perturbing one processor's private registers changes only that processor's execution trace;
  * the generated program is strict C11, defines nothing but `main` and static generated functions, and links no interpreter or
    decoder symbol;
  * every negative fixture ends in its typed stop (or its defined non-faulting hardware result), and a finite budget ends a run.

usage: sega_cd_dual_cpu_test.py <segarecomp> <c-compiler> <source-root>
"""
import os
import pathlib
import re
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tests"))
import sega_cd_common as common  # noqa: E402

failures = []
FORBIDDEN_SYMBOLS = re.compile(r"(?i)m68k_execute|m68ki_|decode_m68k|disassembl|interpret|m68k_pulse_reset|jit")


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def main():
    with tempfile.TemporaryDirectory(prefix="scd-dual-") as tmp:
        tmp = pathlib.Path(tmp)
        builder = common.Builder(SEGARECOMP, CC, tmp)
        reports = {}
        for variant in ("cooked", "raw"):
            disc, exe, expected, emitted = builder.program("comm_" + variant, "comm", variant)
            check(all(p["unsupported"] == 0 for p in emitted["processors"]) and emitted["processors"][1]["vector_roots"] == 1,
                  variant + ": every reached instruction compiled; the INT2 handler is a statically proven root")
            code, report = common.run(exe, disc, 100000)
            reports[variant] = report
            check(code == 0 and report["result"] == "idle", variant + ": run ends idle (exit %d, %s)" % (code, report.get("result")))
            check(common.result_words(report, 4) == expected["result_words"],
                  variant + ": main published the hand-derived response %s" % [hex(w) for w in expected["result_words"]])
            check(report["ga"]["comstat"][:3] == expected["result_words"][:3] and report["ga"]["comcmd"][:2] == [0x1234, 3],
                  variant + ": COMCMD/COMSTAT hold the exchanged words")
            check(all(c["stopped"] == 1 for c in report["cpus"]) and report["sub_interrupts_taken"][2] == 1 and report["ga"]["ifl2"] == 0,
                  variant + ": both processors stopped; one level-2 interrupt taken and acknowledged")
            check(report["cpus"][1]["d"][:4] == [0x159E1, 3, 0x12345, 0x5A5A],
                  variant + ": sub registers: product + PRG-RAM parameter, bank-1 word read through the sub's linear view")
            check(report["instructions"] < 100000, variant + ": finite run well inside the budget")
        check(reports["cooked"]["cpus"] == reports["raw"]["cpus"] and reports["cooked"]["digests"] == reports["raw"]["digests"],
              "cooked and raw discs give the identical machine state")

        disc, exe = tmp / "comm_cooked" / "disc", tmp / "comm_cooked" / ("program" + common.EXE)
        baseline = common.run(exe, disc, 100000)[1]
        check(common.run(exe, disc, 100000)[1] == baseline, "two process runs give identical reports")
        partitions = [["--slice-ticks", str(t)] for t in (1, 977, 2147727, 3500000, 1000000000)]
        partitions += [["--slice-seed", str(s)] for s in range(1, 9)]
        same = [common.run(exe, disc, 100000, *p)[1] == baseline for p in partitions]
        check(all(same), "identical state for %d deadline partitions (%s)" % (len(partitions), same))

        main_perturbed = common.run(exe, disc, 100000, "--set", "main:d5=0xDEADBEEF", "--set", "main:a4=0x00123456")[1]
        sub_perturbed = common.run(exe, disc, 100000, "--set", "sub:d6=0xCAFEF00D")[1]
        check(main_perturbed["cpus"][1]["trace"] == baseline["cpus"][1]["trace"] and
              main_perturbed["cpus"][0]["trace"] != baseline["cpus"][0]["trace"],
              "perturbing main private registers changes only the main trace")
        check(sub_perturbed["cpus"][0]["trace"] == baseline["cpus"][0]["trace"] and
              sub_perturbed["cpus"][1]["trace"] != baseline["cpus"][1]["trace"],
              "perturbing sub private registers changes only the sub trace")
        check(main_perturbed["digests"] == baseline["digests"] and sub_perturbed["ga"] == baseline["ga"],
              "shared memory and Gate Array state are unaffected by private-register perturbation")

        # Symbol hygiene of the generated program and the linked executable (POSIX object formats; Apple prefixes '_').
        if os.name == "nt":
            print("skip  symbol hygiene: COFF symbol tables are not inspected on Windows hosts")
        else:
            obj = pathlib.Path(str(exe) + ".o")
            listing = subprocess.run(["nm", "-g", str(obj)], capture_output=True, text=True, check=True).stdout
            defined = sorted(line.split()[-1].lstrip("_") for line in listing.splitlines()
                             if len(line.split()) == 3 and line.split()[1] not in ("U", "w", "v"))
            check(defined == ["main"], "the generated translation unit exports only main (%s)" % defined[:5])
            everything = subprocess.run(["nm", str(exe)], capture_output=True, text=True, check=True).stdout
            names = {line.split()[-1] for line in everything.splitlines() if line.split()}
            bad = sorted(name for name in names if FORBIDDEN_SYMBOLS.search(name))
            check(not bad, "no interpreter/decoder/JIT symbol in the linked program (%s)" % bad[:5])
        source = (tmp / "comm_cooked" / "program.c").read_text()
        check("scd_no_linear_memory" not in source and "switch (m->cpu[0].pc)" in source,
              "generated C uses only routed accesses and selects compiled entries by PC")

        # Independent pinned-Musashi reference.
        checkout = common.musashi_checkout()
        if checkout is None:
            print("skip  Musashi two-processor reference: no pinned checkout configured (%s)" % common.MUSASHI_ENVS[0])
        else:
            ref_exe = common.build_reference(checkout, CC, tmp / "musashi")
            code, state = common.run_reference(ref_exe, disc, tmp / "musashi-comm")
            check(code == 0, "Musashi reference ran the comm fixture")
            if state:
                for k, name in ((0, "main"), (1, "sub")):
                    mine, ref = baseline["cpus"][k], state["cpus"][k]
                    check(mine["pc"] == ref["pc"] and mine["sr"] == ref["sr"] and mine["d"] == ref["d"] and mine["a"] == ref["a"] and
                          mine["stopped"] == ref["stopped"], "Musashi reference: %s registers, SR, PC and STOP state agree" % name)
                check(baseline["ga"]["comstat"] == state["comstat"] and baseline["ga"]["sub_flag"] == state["sub_flag"],
                      "Musashi reference: COMSTAT and flags agree")
                check(baseline["digests"]["prg_ram"] == state["prg_ram_sha256"] and baseline["digests"]["work_ram"] == state["work_ram_sha256"],
                      "Musashi reference: PRG RAM and work RAM agree byte for byte")

        # Negative fixtures.
        negatives = {
            "comm_unmapped": ("unmapped", 1), "neg_ga_register": ("gate_array_register_unsupported", 0),
            "neg_prg_window_nobus": ("prg_ram_window_without_bus", 0), "neg_handler_not_compiled": ("exception_vector_not_compiled", 1),
            "neg_entry_not_compiled": ("entry_not_compiled", 1), "neg_code_overwrite": ("code_bytes_mismatch", 1),
            "neg_busreq_stopped": ("busreq_while_sub_stopped_unsupported", 0),
        }
        for fixture, (stop, cpu) in negatives.items():
            disc, exe, _, _ = builder.program(fixture, fixture)
            code, report = common.run(exe, disc, 100000)
            check(code == 10 and report["result"] == "stopped" and report["stop"]["class"] == stop and report["stop"]["cpu"] == cpu,
                  "%s: typed stop %s on cpu %d (got %s cpu %s)" % (fixture, stop, cpu, report.get("stop", {}).get("class"),
                                                                    report.get("stop", {}).get("cpu")))
            again = common.run(exe, disc, 100000, "--slice-seed", "3")[1]
            check(again == report, fixture + ": the stop is identical under a sliced run")
        disc, exe, expected, _ = builder.program("neg_ifl2_masked", "neg_ifl2_masked")
        code, report = common.run(exe, disc, 100000)
        block = bytes.fromhex(report["result_block"])
        check(code == 0 and list(block[:2]) == expected["result_bytes"] and report["sub_interrupts_taken"][2] == 0 and
              report["cpus"][1]["stopped"] == 1, "IFL2 with IEN2 = 0 is dropped (defined result, no stop, no interrupt)")
        disc, exe, expected, _ = builder.program("neg_write_protect", "neg_write_protect")
        code, report = common.run(exe, disc, 100000)
        check(code == 0 and common.result_words(report, 2) == expected["result_words"] and report["prg_write_protect_drops"] == 1,
              "a sub write below the write-protect boundary is ignored (defined result)")
        disc, exe = tmp / "comm_cooked" / "disc", tmp / "comm_cooked" / ("program" + common.EXE)
        code, report = common.run(exe, disc, 7)
        check(code == 11 and report["result"] == "budget_exhausted" and report["instructions"] == 7,
              "a finite instruction budget ends the run with a typed result")
        bad = subprocess.run([str(exe), "--disc-dir", str(disc)], capture_output=True, text=True, timeout=30)
        check(bad.returncode == 2, "a run without --instruction-budget is refused")

    if failures:
        print("%d failure(s)" % len(failures))
        return 1
    print("sega_cd_dual_cpu_test: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
