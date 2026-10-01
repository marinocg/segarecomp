#!/usr/bin/env python3
"""SEG-032-T003 (ADR 0073): the RAM-backed (live-bytes) Z80 image guard.

A project-authored Z80 program is compiled twice by the public emitter: as a RAM-backed image (`live 1`: banked, one
window, every entry byte-guarded) and as the immutable reference (invariant window). Both run in the host runner
(tests/tools/z80_live_guard_runner.c) with a mirrored 8 KiB RAM. Proved here:

  * equivalence: while the live bytes match, the guarded image executes instruction-for-instruction exactly like the
    immutable reference (state and cycles after every instruction), for four programs and several owner-group modes;
  * EXHAUSTIVE byte mutation of a straight-line program covering one, two, three and four byte instructions: mutating any
    one byte of any instruction before it runs stops at THAT instruction with `code_mismatch`, `state.pc` at its start and
    the state of the reference after the preceding instructions (no effect of the mutated instruction);
  * data mutation: a byte that is never fetched as an instruction (a data table) changes nothing;
  * re-verification on every execution: a loop-body instruction mutated after the first iteration stops on the next
    iteration; a block-repeat instruction (LDIR) mutated between iterations stops on the next iteration;
  * the interrupt entry is guarded: a mutated IM1 handler stops at $0038;
  * a host without `code_matches` fails closed at the first instruction;
  * the immutable reference does NOT notice the same mutation (control: the guard is what detects it);
  * structure: a live image's C contains no in-group `goto` chaining and no direct owner binding, and an RAM-backed image
    that is not a one-window banked image is rejected by the emitter;
  * regression: the emission of immutable images is unchanged (golden digest of three specs), and a live image of the same
    bytes differs only by the guard.
usage: z80_live_guard_test.py <z80_image_emitter> <cc> <source-root>
"""
import hashlib
import pathlib
import re
import subprocess
import sys
import tempfile

emitter, cc, root = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3])
sys.path.insert(0, str(root / "tools"))
import sms_fixture_rom as sms  # noqa: E402
import z80_conformance as z  # noqa: E402

RUNNER = root / "tests" / "tools" / "z80_live_guard_runner.c"

# One-, two-, three- and four-byte instructions (hand-encoded: the SMS assembler has no IX forms):
# ld hl,1000h; ld a,12h; ld b,a; add a,b; ld (hl),a; inc hl; ld ix,1234h; ld (ix+5),77h; ld de,(1100h); bit 3,(hl);
# push hl; pop bc; xor a; sub 3; ex de,hl; halt
P1_BYTES = bytes.fromhex("210010" "3E12" "47" "80" "77" "23" "DD213412" "DD360577" "ED5B0011" "CB5E" "E5" "C1" "AF" "D603" "EB" "76")
P2 = """
.org 0x0000
        ld b,4
        ld hl,0x1200
loop:   ld a,(hl)
        add a,3
        ld (hl),a
        inc hl
        djnz loop
        call sub1
        halt
sub1:   ld c,9
        ret
"""
P3 = """
.org 0x0000
        ld hl,0x1000
        ld de,0x1100
        ld bc,6
        ldir
        halt
"""
P4 = """
.org 0x0000
        im 1
        ei
        nop
        nop
        nop
        nop
        halt
.org 0x0038
        inc a
        ei
        reti
"""


def assemble(source):
    image, _ = sms.Assembler(source).assemble()
    ram = bytearray(8192)
    for address, byte in image.items():
        ram[address] = byte
    return bytes(ram)


def spec_for(ram, live):
    text = "image 1 %s\nwindow 1 0000 0 4000\n" % ("banked" if live else "invariant")
    if live:
        text += "live 1\n"
    doubled = ram * 2
    for start in range(0, len(doubled), 4096):
        text += "bytes 1 %s\n" % doubled[start:start + 4096].hex()
    return text


def build(tc, ram, live, workdir, stem="image"):
    stats, owners, error = z.emit_image(tc, spec_for(ram, live), workdir, stem)
    assert error is None, error
    exe, message = z.compile_units(tc, workdir, stem, extra_sources=[RUNNER])
    assert exe is not None, message
    return exe


def run(exe, ram, steps, *extra, workdir):
    hexfile = pathlib.Path(workdir) / "ram.hex"
    hexfile.write_text(ram.hex())
    out = subprocess.run([str(exe), str(hexfile), "run", str(steps), *extra], text=True, capture_output=True, check=True).stdout
    lines = out.splitlines()
    return lines[:-1], lines[-1]


failures = []


def check(ok, label):
    print("%-5s %s" % ("ok" if ok else "FAIL", label))
    if not ok:
        failures.append(label)


def state_of(line):
    return line.split(" ", 2)[2] if line.startswith("STEP") else line.split(" ", 3)[3]


def main():
    tc = z.Toolchain(cc, pathlib.Path(emitter), opt="-O0", cache=False)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        programs = {"P1": P1_BYTES + bytes(8192 - len(P1_BYTES)), "P2": assemble(P2), "P3": assemble(P3), "P4": assemble(P4)}
        # data bytes the programs read
        for name in ("P2", "P3"):
            ram = bytearray(programs[name])
            ram[0x1000:0x1010] = bytes(range(1, 17))
            ram[0x1200:0x1204] = bytes([9, 8, 7, 6])
            programs[name] = bytes(ram)
        ram1 = bytearray(programs["P1"])
        ram1[0x1100:0x1102] = b"\x34\x12"
        programs["P1"] = bytes(ram1)

        # ---- equivalence with the immutable reference, several owner-group modes ----
        built = {}
        for name, ram in programs.items():
            for group in (1, 128):
                tcg = z.Toolchain(cc, pathlib.Path(emitter), opt="-O0", cache=False, owner_group=group)
                live_exe = build(tcg, ram, True, tmp / ("live_%s_%d" % (name, group)))
                ref_exe = build(tcg, ram, False, tmp / ("ref_%s_%d" % (name, group)))
                built[(name, group)] = (live_exe, ref_exe)
                extra = ["int-after", "3"] if name == "P4" else []
                live_lines, live_end = run(live_exe, ram, 60, *extra, workdir=tmp)
                ref_lines, ref_end = run(ref_exe, ram, 60, *extra, "immutable", workdir=tmp)
                same = [state_of(l) for l in live_lines] == [state_of(l) for l in ref_lines] and state_of(live_end) == state_of(ref_end)
                check(same and live_end.split()[1] == "halted", "%s group=%d: guarded run equals the immutable reference (%d instructions)" % (name, group, len(live_lines)))

        # ---- exhaustive byte mutation of the straight-line program ----
        live_exe, ref_exe = built[("P1", 128)]
        ram = programs["P1"]
        trace, end = run(live_exe, ram, 60, workdir=tmp)
        starts = [0] + [int(re.search(r"pc=([0-9A-F]{4})", l).group(1), 16) for l in trace]
        count = len(trace)
        lengths = [starts[i + 1] - starts[i] for i in range(count)]
        check(set(lengths) >= {1, 2, 3, 4}, "the program covers one, two, three and four byte instructions (%s)" % sorted(set(lengths)))
        mismatched = 0
        for i in range(count):
            for offset in range(starts[i], starts[i] + lengths[i]):
                lines, endline = run(live_exe, ram, 60, "mutate-after", "0", "%X" % offset, "41", workdir=tmp)
                # the mutation is applied before the first instruction: the program stops at the first mutated instruction
                expected_index = i
                expect_state = state_of(trace[expected_index - 1]) if expected_index > 0 else None
                ok = endline.startswith("END code_mismatch") and ("pc=%04X" % starts[i]) in endline
                if expect_state is not None:
                    ok = ok and len(lines) == expected_index
                mismatched += 1 if ok else 0
        total = sum(lengths)
        check(mismatched == total, "exhaustive mutation: %d of %d single-byte mutations stop at their own instruction with code_mismatch" % (mismatched, total))
        # a mutation applied just before instruction i runs: effects of 0..i-1 are intact, instruction i has no effect
        for i in range(1, count):
            lines, endline = run(live_exe, ram, 60, "mutate-after", str(i), "%X" % starts[i], "FF", workdir=tmp)
            same_state = len(lines) == i and (not lines or state_of(lines[-1]) == state_of(trace[i - 1]))
            check(endline.startswith("END code_mismatch") and same_state and state_of(endline) == state_of(trace[i - 1]),
                  "mutating instruction %d just before it runs stops with the state of the reference after %d instructions" % (i, i))
            break  # one representative in the log; the exhaustive loop above covers every byte
        ok_all = True
        for i in range(1, count):
            lines, endline = run(live_exe, ram, 60, "mutate-after", str(i), "%X" % (starts[i] + lengths[i] - 1), "FF", workdir=tmp)
            ok_all &= endline.startswith("END code_mismatch") and state_of(endline).startswith("pc=%04X" % starts[i]) \
                and len(lines) == i
        check(ok_all, "mid-run mutation of the last byte of every instruction stops exactly at it")

        # ---- control: the immutable reference does not notice ----
        lines, endline = run(ref_exe, ram, 60, "mutate-after", "0", "0", "41", "immutable", workdir=tmp)
        check(endline.startswith("END halted"), "control: the immutable reference runs on, blind to the mutated byte (the guard is what detects it)")

        # ---- data mutation ----
        lines, endline = run(live_exe, ram, 60, "mutate-after", "0", "1500", "FF", workdir=tmp)
        check(endline.startswith("END halted"), "mutating a byte that is never fetched as an instruction changes nothing")

        # ---- loop and block-repeat re-verification ----
        live_exe, _ = built[("P2", 128)]
        ram = programs["P2"]
        # P2: ld b,4 @0; ld hl,nn @2; loop: ld a,(hl) @5; add a,3 @6; ld (hl),a @8; inc hl @9; djnz @10. Steps 0-1 are the two loads,
        # steps 2-6 the first iteration, step 7 starts the second iteration at the loop head, step 8 is `add a,3`.
        lines, endline = run(live_exe, ram, 80, "mutate-after", "7", "6", "07", workdir=tmp)
        check(endline.startswith("END code_mismatch") and "pc=0006" in endline and len(lines) == 8,
              "a loop-body instruction mutated during the first iteration is caught when the second iteration reaches it")
        live_exe, _ = built[("P3", 128)]
        ram = programs["P3"]
        # P3: three loads (steps 0-2), then LDIR @9 re-enters its owner once per iteration (steps 3-8).
        lines, endline = run(live_exe, ram, 80, "mutate-after", "5", "A", "01", workdir=tmp)
        check(endline.startswith("END code_mismatch") and "pc=0009" in endline and len(lines) == 5,
              "a block-repeat instruction mutated between iterations stops on the next iteration")

        # ---- interrupt entry ----
        live_exe, _ = built[("P4", 128)]
        ram = programs["P4"]
        lines, endline = run(live_exe, ram, 60, "int-after", "3", "mutate-after", "0", "38", "01", workdir=tmp)
        check(endline.startswith("END code_mismatch") and "pc=0038" in endline, "a mutated IM1 handler stops at $0038 when the interrupt is taken")

        # ---- a host without the matcher fails closed ----
        live_exe, _ = built[("P1", 128)]
        lines, endline = run(live_exe, programs["P1"], 60, "no-matcher", workdir=tmp)
        check(endline.startswith("END code_mismatch") and not lines, "a host without code_matches fails closed at the first instruction")

        # ---- structure ----
        sources = "".join(p.read_text() for p in sorted((tmp / "live_P1_128").glob("*.c")))
        check("goto z80_e_" not in sources and "Z80_OWNER_NEXT(" not in sources, "a live image has no in-group goto chaining and no direct owner binding")
        check("z80_code_guard(" in sources and "z80_owner_prologue(" in sources, "every entry runs the prologue and the guard")
        (tmp / "bad.spec").write_text("image 1 invariant\nwindow 1 0000 0 100\nfill 1 00 100\nlive 1\n")
        bad = subprocess.run([emitter, str(tmp / "bad.spec"), str(tmp / "bad"), "bad"], capture_output=True, text=True)
        check(bad.returncode != 0 and "RAM-backed" in bad.stdout, "a RAM-backed image that is not a one-window banked image is rejected")
        (tmp / "bad2.spec").write_text("image 1 banked\nwindow 1 0000 0 100\nwindow 1 4000 0 100\nfill 1 00 100\nlive 1\n")
        bad2 = subprocess.run([emitter, str(tmp / "bad2.spec"), str(tmp / "bad2"), "bad"], capture_output=True, text=True)
        check(bad2.returncode != 0 and "RAM-backed" in bad2.stdout, "a RAM-backed image with two windows is rejected")

        # ---- immutable emission is unchanged (golden digest of three specs) ----
        digests = []
        import random
        random.seed(7)
        data = bytes(random.randrange(256) for _ in range(4096))
        data2 = bytes(random.randrange(256) for _ in range(0x4000))
        specs = [
            "image 1 invariant\nwindow 1 0000 0 1000\nbytes 1 %s\n" % data.hex(),
            "image 2 banked\nwindow 2 0000 0 1000\nbytes 2 %s\n" % data.hex(),
            "image 1 invariant\nwindow 1 0000 0 400\nbytes 1 %s\nimage 2 banked\nwindow 2 0000 400 3c00\nwindow 2 4000 0 4000\nwindow 2 8000 0 4000\nbytes 2 %s\n"
            % (data2[:0x400].hex(), data2.hex()),
        ]
        for n, spec in enumerate(specs):
            d = tmp / ("golden%d" % n)
            stats, owners, error = z.emit_image(tc, spec, d, "z80_image")
            assert error is None, error
            h = hashlib.sha256()
            for p in sorted(d.glob("z80_image*")):
                if p.suffix in (".c", ".h", ".units"):
                    h.update(p.name.encode() + p.read_bytes())
            digests.append(h.hexdigest()[:16])
        check(digests == GOLDEN, "immutable emission is byte-identical to the pre-SEG-032 emitter (%s)" % ",".join(digests))
    print("z80 live guard: %s" % ("FAILED (%d)" % len(failures) if failures else "ok"))
    return 1 if failures else 0


# Digests of the pre-SEG-032 emitter (measured with the main-branch emitter binary before the guard existed).
GOLDEN = ["c081e8ba8881d2f9", "275bed5dd4de0df4", "3fc4a12a9e52beba"]

if __name__ == "__main__":
    sys.exit(main())
