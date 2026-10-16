#!/usr/bin/env python3
"""SEG-063-T001 Checkpoints C and D (ADR 0127, ADR 0132): runtime activation of overlapping precompiled module images.

  * the runtime unit harness (tests/tools/sega_cd_overlay_runtime_harness.c): selection by whole-image comparison, write marking
    through every processor view (CPU stores, exception frames, PRG-RAM window, Word RAM owner), view changes, revalidation cost;
  * Checkpoint D on the `modules` disc (cooked and raw): the sub loads module A from the disc by CDC DMA into Word RAM and runs it
    at $080000 as compiled C, copies it into PRG RAM $020000 with CPU stores and runs it there, then loads module B over A and runs
    B at both addresses through B's own compiled entries - selected only by live-byte comparison against the registry;
  * deterministic and slice-independent results; the program refuses another disc; without a module list nothing runs;
  * every negative case ends in its own typed stop with no instruction retired by the wrong image: an unlisted module
    (image_unknown), a modified A' (image_bytes_mismatch), B copied over half of A (image_partially_overwritten), two listed images
    both live (ambiguous_image), a jump to an address only A compiled while B is loaded (image_entry_not_compiled), and a return
    into A after the loader replaced it with B (stale_image_selection).

usage: sega_cd_overlay_test.py <segarecomp> <c-compiler> <source-root>
"""
import pathlib
import re
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tests"))
import sega_cd_common as common  # noqa: E402

failures = []
FORBIDDEN = re.compile(r"(?i)m68k_execute|m68ki_|decode_m68k|disassembl|interpret|jit|opcode")
BUDGET = 3000000


def check(ok, label):
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def retired(report):
    """{(group start, candidate name): retired instructions}"""
    out = {}
    for group in report.get("overlays", {}).get("groups", []):
        for c in group["candidates"]:
            out[(group["start"], c["name"])] = c["retired"]
    return out


def main():
    with tempfile.TemporaryDirectory(prefix="scd-overlay-") as tmp:
        tmp = pathlib.Path(tmp)
        builder = common.Builder(SEGARECOMP, CC, tmp)

        # --- runtime unit harness ---------------------------------------------------------------------------------------------
        harness = tmp / ("overlay_harness" + common.EXE)
        obj = tmp / "overlay_harness.o"
        subprocess.run([CC, *common.CFLAGS, "-I", str(common.SCD_RUNTIME), "-I", str(common.GENESIS_RUNTIME), "-c",
                        str(ROOT / "tests" / "tools" / "sega_cd_overlay_runtime_harness.c"), "-o", str(obj)], check=True)
        runtime = [str(o) for o in builder.runtime_objects() if not o.name.endswith("scd_main.o")]
        subprocess.run([CC, str(obj), *runtime, "-o", str(harness)], check=True)
        out = subprocess.run([str(harness)], capture_output=True, text=True, timeout=common.RUN_TIMEOUT)
        for line in out.stdout.splitlines():
            if line.startswith("FAIL"):
                check(False, "harness: " + line[5:].strip())
        check(out.returncode == 0 and out.stdout.strip().endswith("PASS (0 failures)"),
              "runtime harness: %d checks" % sum(1 for l in out.stdout.splitlines() if l.startswith(("ok", "FAIL"))))

        # --- Checkpoint D -----------------------------------------------------------------------------------------------------
        reports = {}
        for variant in ("cooked", "raw"):
            disc, exe, expected, emitted = builder.program("checkpoint_" + variant, "modules", variant, manifest=common.standard_manifest)
            source = (tmp / ("checkpoint_" + variant) / "program.c").read_text()
            modules = [i for i in emitted["images"] if i["producer"] == "sega_cd.disc_module"]
            check(len(modules) == 4 and all(i["unsupported"] == 0 for i in modules) and emitted["overlay_groups"] == 2,
                  variant + ": A and B compiled at $080000 and $020000 (4 candidates, 2 groups), nothing unsupported")
            check(not FORBIDDEN.search(source), variant + ": the generated program contains no decoder, interpreter or JIT")
            code, report = common.run(exe, disc, BUDGET)
            reports[variant] = report
            check(code == 0 and report["result"] == "idle", variant + ": run ends idle (exit %d, %s, %s)" %
                  (code, report.get("result"), report.get("stop", {}).get("class")))
            check(common.result_words(report, 5) == expected["result_words"],
                  variant + ": A ran twice ($AAAA, and $A1A1 after its service check), B ran twice ($BBBB): %s" %
                  [hex(w) for w in common.result_words(report, 5)])
            counts = retired(report)
            check(all(counts.get((base, name), 0) > 0 for base in common.MODULE_BASES for name in ("moda", "modb")),
                  variant + ": every candidate retired instructions through its own compiled entries: %s" % counts)
            events = [(e["group"], e["candidate"], e["pc"], e["kind"]) for e in report["overlays"]["events"]]
            groups = {g["start"]: i for i, g in enumerate(report["overlays"]["groups"])}
            w, p = groups[0x080000], groups[0x020000]
            check(events == [(w, "moda", 0x080000, "selected"), (p, "moda", 0x020000, "selected"),
                             (w, "moda", 0x080000, "invalidated"), (w, "modb", 0x080000, "selected"),
                             (p, "moda", 0x020000, "invalidated"), (p, "modb", 0x020000, "selected")],
                  variant + ": A selected at both addresses, then invalidated by B's DMA / copy and B selected at the same PCs")
            check(all(g["revalidations"] <= 2 and g["selections"] == 2 for g in report["overlays"]["groups"]),
                  variant + ": selection work is bounded (2 selections, <= 2 revalidations per group)")
        a, b = reports["cooked"], reports["raw"]
        check(a["cpus"] == b["cpus"] and a["digests"] == b["digests"] and retired(a) == retired(b),
              "cooked and raw discs give the identical machine and overlay state")

        disc, exe = tmp / "checkpoint_cooked" / "disc", tmp / "checkpoint_cooked" / ("program" + common.EXE)
        baseline = common.run(exe, disc, BUDGET)[1]
        check(common.run(exe, disc, BUDGET)[1] == baseline, "two process runs give identical reports")
        partitions = [["--slice-ticks", str(t)] for t in (999983, 2147727, 3500000, 357954500000)] + [["--slice-seed", str(s)] for s in (1, 2, 3)]
        same = [common.run(exe, disc, BUDGET, *part)[1] == baseline for part in partitions]
        check(all(same), "identical report (overlay events included) for %d deadline partitions: %s" % (len(partitions), same))
        code, short = common.run(exe, disc, 1000)
        check(code == 11 and short["result"] == "budget_exhausted", "a finite budget ends the run")

        # the program is bound to its disc; without a module list, the modules never execute
        other = tmp / "other_disc"
        common.build_disc(other, "modules", "cooked", "--module-case", "unknown")
        code, refused = common.run(exe, other, BUDGET)
        check(code == 20 and refused.get("result") == "disc_refused", "the program refuses a disc with other bytes")
        plain_disc, plain_exe, _, plain_emit = builder.program("plain", "modules", "cooked")
        code, plain = common.run(plain_exe, plain_disc, BUDGET)
        check(plain_emit["overlay_groups"] == 0 and code == 10 and plain["stop"]["class"] == "entry_not_compiled" and
              plain["stop"]["pc"] == 0x080000 and "overlays" not in plain,
              "without a module list the jump to the loaded module stops entry_not_compiled (SEG-062 behaviour)")

        # --- negative cases -------------------------------------------------------------------------------------------------
        def ambiguous_manifest(disc_dir):
            identity, files, _ = common.disc_facts(disc_dir)
            return common.module_list(identity, common.standard_modules(files, ("MODA.BIN;1", "MODB.BIN;1", "APFX.BIN;1")))

        negatives = [("unknown", "image_unknown", common.standard_manifest, {}),
                     ("mismatch", "image_bytes_mismatch", common.standard_manifest, {}),
                     ("partial", "image_partially_overwritten", common.standard_manifest, {}),
                     ("checkpoint", "ambiguous_image", ambiguous_manifest, {}),
                     ("missing_entry", "image_entry_not_compiled", common.standard_manifest, {}),
                     ("stale", "stale_image_selection", common.standard_manifest, {(0x080000, "moda")})]
        seen = set()
        for case, stop, manifest, may_retire in negatives:
            name = "neg_" + case + ("_ambiguous" if stop == "ambiguous_image" else "")
            disc, exe, expected, _ = builder.program(name, "modules", "cooked", "--module-case", case, manifest=manifest)
            code, report = common.run(exe, disc, BUDGET)
            got = report.get("stop", {}).get("class")
            counts = retired(report)
            wrong = {k: v for k, v in counts.items() if v and k not in may_retire}
            check(code == 10 and got == stop and report["stop"]["cpu"] == 1 and not wrong,
                  "%s: typed stop %s (got %s), no instruction retired by another image (%s)" % (case, stop, got, counts))
            seen.add(got)
            if case == "stale":
                check(counts.get((0x080000, "moda"), 0) > 0 and counts.get((0x080000, "modb"), 0) == 0 and
                      report["stop"]["address"] == 2 and common.result_words(report, 5) == [0, 0, 0, 0, 0],
                      "stale: A ran up to its service call; after B replaced it, nothing of B ran and A's continuation did not run")
        check(len(seen) == len(negatives), "the six negative cases end in six distinct typed stops")

    print("PASS" if not failures else "FAIL (%d)" % len(failures))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
