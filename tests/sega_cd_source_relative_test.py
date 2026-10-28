#!/usr/bin/env python3
"""SEG-096-T002 (ADR 0207): the source-relative (position-independent) Sega CD emitter against the per-mapping emitter, its verified reference.

For every program below both emitters are run on the same discovered module list and compiled as strict C11; the program is then run on the
same routes and the COMPLETE run reports must be equal apart from the program identity: both CPUs' registers, retired instructions, cycles
and instruction-trace hash, every memory digest, the device log, the overlay selection history and the typed stop. Hand-derived expectations
(computed from the fixture's own layout, not from either emitter) guard against both emitters being wrong in the same way.

  * `unvisited/pi` module C at two bases: PC-relative lea / d16(PC) / (d8,PC,Xn), a PC-relative JSR, a computed JMP, short/long/backward
    branches, DBRA, BSR, an ABSOLUTE call and absolute stores (never relocated), TRAP and divide-by-zero exceptions (stacked PCs);
  * odd-address fault, a transfer outside every admitted image, a word overwritten after loading (second mapping): identical typed stops;
  * the base disc (A and B alternatives at one address, C at two bases) on all three routes;
  * the SEG-063 `modules` fixture (A and B at two bases each) on every loader case: checkpoint, unknown, mismatch, partial, missing_entry, stale;
  * the cost counters: with several mappings the source-relative bodies are shared (fewer bodies and C bytes), with one mapping nothing changes;
  * the default emission is unchanged by the option's existence, and the option changes the program identity.

usage: sega_cd_source_relative_test.py <segarecomp> <c-compiler> <source-root>
"""
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

SEGARECOMP, CC, ROOT = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3]).resolve()
sys.path.insert(0, str(ROOT / "tests"))
import sega_cd_common as common  # noqa: E402

failures = []
checks = [0]
BUDGET = 3_000_000


def check(ok, label):
    checks[0] += 1
    print(("ok    " if ok else "FAIL  ") + label)
    if not ok:
        failures.append(label)


def discover(cue, out):
    proc = subprocess.run([SEGARECOMP, "sega-cd", "discover-modules", "--cue", str(cue), "--out", str(out)], capture_output=True, text=True, timeout=120)
    return json.loads(proc.stdout)


class Pair:
    """The same disc and module list emitted and compiled by both emitters."""

    def __init__(self, builder, directory, disc, manifest):
        self.disc, self.reports, self.exes, self.sources = disc, {}, {}, {}
        for tag, flags in (("compat", []), ("pi", ["--position-independent"])):
            c_path = directory / (tag + ".c")
            code, report = builder.emit(disc / "disc.cue", c_path, "--modules", str(manifest), *flags)
            assert code == 0, report
            exe = directory / (tag + common.EXE)
            builder.compile(c_path, exe)
            self.reports[tag], self.exes[tag], self.sources[tag] = report, exe, c_path

    def run(self, choice=None, *extra):
        out = {}
        for tag, exe in self.exes.items():
            args = (["--set", "main:d7=%d" % choice] if choice else []) + list(extra)
            code, report = common.run(exe, self.disc, BUDGET, *args)
            report.pop("program_identity", None)
            out[tag] = (code, report)
        return out

    def equal(self, choice=None):
        runs = self.run(choice)
        return runs["compat"] == runs["pi"], runs["compat"]


def main():
    with tempfile.TemporaryDirectory(prefix="scd-sr-") as tmp:
        tmp = pathlib.Path(tmp)
        builder = common.Builder(SEGARECOMP, CC, tmp)
        dumps = tmp / "dumps"

        # ---- the address-sensitive module, at two bases ------------------------------------------------------------------------
        disc = tmp / "pi" / "disc"
        oracle = common.build_disc(disc, "unvisited", "cooked", "--unvisited-case", "pi")
        found = discover(disc / "disc.cue", tmp / "pi" / "modules.txt")
        pair = Pair(builder, tmp / "pi", disc, tmp / "pi" / "modules.txt")
        sub = {t: pair.reports[t]["processors"][1] for t in pair.reports}
        same, (code, report) = pair.equal(3)
        check(found["ok"] and code == 0 and same, "pi: the complete run reports are equal (registers, cycles, trace hash, memory digests, device log, overlay history)")
        pi = oracle["pi"]
        words = common.result_words(report, 8)
        check(words[2] == 2 and words[5] == 0xCCCC and words[6] == pi["sum_low16"] and words[7] == pi["w2"],
              "pi: hand-derived results hold - C ran twice, the LEA d(PC) addresses summed over both bases ($%04X), the (d8,PC,Xn) read ($%04X)" % (words[6], words[7]))
        for tag, exe in pair.exes.items():
            shutil.rmtree(dumps, ignore_errors=True)
            dumps.mkdir()
            c, r = common.run(exe, disc, BUDGET, "--set", "main:d7=3", "--dump-dir", str(dumps))
            ram = (dumps / "prg_ram.bin").read_bytes()
            word = lambda name: int.from_bytes(ram[pi["scratch"][name]:pi["scratch"][name] + 2], "big")
            total = int.from_bytes(ram[pi["sum_address"]:pi["sum_address"] + 4], "big")
            expect_sum = sum(b + pi["data_offset"] for b in (0x090000, 0x0A0000))
            check(c == 0 and word("W1") == pi["w1"] and word("CNT") == 8 and word("EXC") == 2 and word("EXC2") == 2 and word("HELP") == 2 and
                  word("W3") == 2 and word("W4") == 2 and total == expect_sum,
                  "pi/%s: memory holds the hand-derived values (PC-relative read, 2x4 DBRA iterations, TRAP and divide-by-zero handlers each ran twice and returned, "
                  "absolute helper called twice, PC-relative JSR and computed JMP each reached twice, address sum $%06X)" % (tag, total))
        check(sub["pi"]["shared_bodies"] > 0 and sub["pi"]["fallback_instances"] == 0 and sub["compat"]["shared_bodies"] == 0,
              "pi: %d shared bodies serve %d mapped instructions, none fell back (per-mapping reference: %d bodies, source-relative: %d)" %
              (sub["pi"]["shared_bodies"], sub["pi"]["shared_instances"], sub["compat"]["bodies"], sub["pi"]["bodies"]))
        check(sub["pi"]["bodies"] < sub["compat"]["bodies"] and len(pair.sources["pi"].read_bytes()) < len(pair.sources["compat"].read_bytes()),
              "pi: fewer native bodies (%d < %d) and fewer C bytes (%d < %d)" % (sub["pi"]["bodies"], sub["compat"]["bodies"],
                                                                              len(pair.sources["pi"].read_bytes()), len(pair.sources["compat"].read_bytes())))
        check(pair.reports["pi"]["program_identity"] != pair.reports["compat"]["program_identity"] and pair.reports["pi"]["position_independent"] and
              not pair.reports["compat"]["position_independent"], "the emitter policy enters the program identity (generated-code identity) and is reported")
        rc, again = builder.emit(disc / "disc.cue", tmp / "pi" / "again.c", "--modules", str(tmp / "pi" / "modules.txt"), "--position-independent")
        check(again["program_identity"] == pair.reports["pi"]["program_identity"] and (tmp / "pi" / "again.c").read_bytes() == pair.sources["pi"].read_bytes(),
              "the source-relative emission is deterministic (identical identity and C)")
        text = pair.sources["pi"].read_text()
        check("scd_pc0" in text and re.search(r"static int scd_p1_m4_s[0-9A-F]{6}\(ScdMachine \*m\)", text) is not None and
              "scd_p1_m4_i_" not in text, "the shared bodies are named by image and source offset; no per-mapping body of the shared image remains")
        absolute = re.findall(r"UINT32_C\(0x00FF8005\)|UINT32_C\(0x%08X\)" % pi["scratch"]["W1"], text)
        check(len(absolute) >= 1, "absolute addresses appear unchanged in the source-relative text (never turned into pc0 + k)")

        # ---- typed stops -------------------------------------------------------------------------------------------------------
        for case, stop in (("pi_odd", "address_error"), ("pi_escape", "entry_not_compiled"), ("pi_overwrite", "image_bytes_mismatch")):
            d = tmp / case / "disc"
            common.build_disc(d, "unvisited", "cooked", "--unvisited-case", case)
            discover(d / "disc.cue", tmp / case / "modules.txt")
            p = Pair(builder, tmp / case, d, tmp / case / "modules.txt")
            same, (code, report) = p.equal(3)
            check(same and report["stop"]["class"] == stop and code != 0, "%s: identical complete reports and the typed stop %s at the actual guest pc $%X" %
                  (case, stop, report["stop"]["pc"]))

        # ---- the base disc: alternatives at one address, every route ------------------------------------------------------------
        d = tmp / "base" / "disc"
        common.build_disc(d, "unvisited", "cooked", "--unvisited-case", "base")
        discover(d / "disc.cue", tmp / "base" / "modules.txt")
        p = Pair(builder, tmp / "base", d, tmp / "base" / "modules.txt")
        for choice in (None, 2, 3):
            same, (code, report) = p.equal(choice)
            check(same and code == 0, "base route %s: identical complete reports (A and B alternatives at $080000, C at two bases)" % (choice or "default"))

        # ---- the SEG-063 modules fixture: A and B at two bases each, every loader case --------------------------------------------
        for case in ("checkpoint", "unknown", "mismatch", "partial", "missing_entry", "stale"):
            d = tmp / ("mod-" + case) / "disc"
            common.build_disc(d, "modules", "cooked", "--module-case", case)
            (tmp / ("mod-" + case) / "modules.txt").write_text(common.standard_manifest(d), newline="\n")
            p = Pair(builder, tmp / ("mod-" + case), d, tmp / ("mod-" + case) / "modules.txt")
            same, (code, report) = p.equal()
            shared = p.reports["pi"]["processors"][1]["shared_bodies"]
            check(same and (case != "checkpoint" or (code == 0 and shared > 0)),
                  "modules/%s: identical complete reports%s (stop %s)" % (case, ", %d shared bodies" % shared if shared else "", report["stop"]["class"]))

        # ---- a single mapping shares nothing and is byte-identical to the reference --------------------------------------------------
        d = tmp / "single" / "disc"
        common.build_disc(d, "modules", "cooked")
        (tmp / "single").mkdir(exist_ok=True)
        (tmp / "single" / "modules.txt").write_text(common.module_list(*(lambda i, f, _: (i, common.standard_modules(f, bases=(0x080000,))))(*common.disc_facts(d))),
                                                    newline="\n")
        rc1, r1 = builder.emit(d / "disc.cue", tmp / "single" / "a.c", "--modules", str(tmp / "single" / "modules.txt"))
        rc2, r2 = builder.emit(d / "disc.cue", tmp / "single" / "b.c", "--modules", str(tmp / "single" / "modules.txt"), "--position-independent")
        body = lambda path: re.sub(r"program_identity.*|Disc identity.*", "", path.read_text())
        check(r2["processors"][1]["shared_bodies"] == 0 and body(tmp / "single" / "a.c").replace(r1["program_identity"], "") ==
              body(tmp / "single" / "b.c").replace(r2["program_identity"], ""), "one mapping: nothing is shared and the C is identical apart from the identity")

        # ---- the default emission is the reference: no source-relative symbol --------------------------------------------------------
        check("scd_pc0" not in pair.sources["compat"].read_text(), "the default (reference) emission contains no source-relative construct")
        rc, out = builder.emit(disc / "disc.cue", tmp / "x.c", "--position-independent")
        check(rc == 0 and out["processors"][1]["shared_bodies"] == 0, "without modules (boot images only) the option shares nothing and still emits")
    print("%d checks, %d failures" % (checks[0], len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
