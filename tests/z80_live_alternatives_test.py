#!/usr/bin/env python3
"""SEG-115 (ADR 0259): alternative compiled forms of a RAM-backed Z80 entry, selected by the live structural bytes before any effect.

Every program is project-authored (hand-encoded Z80). A RAM-backed image is compiled with a finite set of ALTERNATIVE byte values (`alt` spec lines;
in production the static mutation analysis supplies them) and run in the host runner (tests/tools/z80_live_guard_runner.c) while the live RAM is
mutated to one of those values. The differential reference is the IMMUTABLE image compiled from the mutated RAM: the alternative form must execute
instruction-for-instruction (state, cycles, memory digest) exactly like the plain image of the bytes that are live. Proved here:

  * one statically justified replacement opcode (JR -> RET) mutated before the first execution, and several finite alternatives at one byte (RET,
    EXX, NOP), each equal to its reference; a replacement that is NOT a compiled alternative is the same typed `code_mismatch` as without alternatives,
    stopping at that instruction before any effect (fail closed);
  * different instruction lengths (a 3-byte CALL replaced by a 1-byte RET and by a 2-byte LD A,n) with the correct successor PC;
  * overlapping entries: the alternative byte is the operand of the instruction before it and the opcode of its own entry (execution started at both);
  * a conditional branch (JR NZ -> JR Z) with the flag state that makes the two differ;
  * the interrupt path: a handler byte replaced by NOP and a RETI replaced by RETN, taken through an IM 1 interrupt;
  * a mutation of an operand (payload) byte is live and needs no alternative form; no entry carries one for it;
  * a mutation after the original form executed, and a toggle back (alternating forms), against hand-derived register values;
  * the bound: an entry that would need more than the pre-registered number of forms drops ALL alternatives of the image, which then behaves as the
    plain guard (typed mismatch), reported in the stats;
  * regression: an image with no alternatives is emitted byte for byte as before (z80_live_guard_test golden digests).
usage: z80_live_alternatives_test.py <z80_image_emitter> <cc> <source-root>
"""
import concurrent.futures
import pathlib
import re
import subprocess
import sys
import tempfile

emitter, cc, root = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3])
sys.path.insert(0, str(root / "tools"))
import z80_conformance as z  # noqa: E402

RUNNER = root / "tests" / "tools" / "z80_live_guard_runner.c"
failures = []


def check(ok, label):
    print("%-5s %s" % ("ok" if ok else "FAIL", label))
    if not ok:
        failures.append(label)


def ram_of(sections):
    ram = bytearray(8192)
    for address, data in sections.items():
        ram[address:address + len(data)] = bytes(data)
    return bytes(ram)


def spec_for(ram, live, alts=()):
    text = "image 1 %s\nwindow 1 0000 0 4000\n" % ("banked" if live else "invariant")
    if live:
        text += "live 1\n"
        for offset, value in alts:
            text += "alt 1 %X %02X\n" % (offset, value)
    doubled = ram * 2
    for start in range(0, len(doubled), 4096):
        text += "bytes 1 %s\n" % doubled[start:start + 4096].hex()
    return text


def build(tc, ram, live, workdir, alts=()):
    stats, _, error = z.emit_image(tc, spec_for(ram, live, alts), workdir, "image")
    assert error is None, error
    exe, message = z.compile_units(tc, workdir, "image", extra_sources=[RUNNER])
    assert exe is not None, message
    return exe, stats


def run(exe, ram, steps, *extra, workdir):
    hexfile = pathlib.Path(workdir) / "ram.hex"
    hexfile.write_text(ram.hex())
    out = subprocess.run([str(exe), str(hexfile), "run", str(steps), *extra], text=True, capture_output=True, check=True).stdout
    lines = out.splitlines()
    return lines[:-1], lines[-1]


def mutated(ram, offset, value):
    copy = bytearray(ram)
    copy[offset] = value
    return bytes(copy)


def xor_to(ram, offset, value):
    return "%X" % (ram[offset] ^ value)


# --- the programs ----------------------------------------------------------------------------------------------------------------------------
# P_RET: CALL R / LD A,5 / HALT; R at $20 = JR $30 (the patch site); $30: LD B,7 / RET
P_RET = ram_of({0x00: [0xCD, 0x20, 0x00, 0x3E, 0x05, 0x76], 0x20: [0x18, 0x0E], 0x30: [0x06, 0x07, 0xC9]})
RET_VALUES = {"RET": 0xC9, "EXX": 0xD9, "NOP": 0x00}
# P_LEN: JP $10 / $10: CALL $40 (3 bytes; the site is its opcode) / HALT; $40: LD A,$99 / RET
P_LEN = ram_of({0x00: [0xC3, 0x10, 0x00], 0x10: [0xCD, 0x40, 0x00, 0x76], 0x40: [0x3E, 0x99, 0xC9]})
# P_OVL: LD HL,$073E at 0 (its low operand byte, offset 1, is also the opcode of the entry at 1) / HALT at 3
P_OVL = ram_of({0x00: [0x21, 0x3E, 0x07, 0x76]})
# P_COND: XOR A (Z set) / JR NZ,+2 (the site: JR NZ -> JR Z) / LD A,$11 / HALT
P_COND = ram_of({0x00: [0xAF, 0x20, 0x02, 0x3E, 0x11, 0x76]})
# P_INT: IM 1 / EI / NOPs / HALT; handler at $38: INC A / EI / RETI
P_INT = ram_of({0x00: [0xED, 0x56, 0xFB, 0x00, 0x00, 0x00, 0x00, 0x76], 0x38: [0x3C, 0xFB, 0xED, 0x4D]})
# P_PAY: LD A,$12 / LD B,A / HALT
P_PAY = ram_of({0x00: [0x3E, 0x12, 0x47, 0x76]})
# P_LOOP: LD B,3 / L: NOP (the site: NOP <-> INC A) / DJNZ L / HALT
P_LOOP = ram_of({0x00: [0x06, 0x03, 0x00, 0x10, 0xFD, 0x76]})
# P_BOUND: INC A / HALT with nine distinct one-byte alternatives at offset 0 (more than the per-entry bound)
P_BOUND = ram_of({0x00: [0x3C, 0x76]})
BOUND_VALUES = [0x3D, 0x04, 0x05, 0x0C, 0x0D, 0x14, 0x15, 0x1C, 0x1D]


def states(lines, end):
    return lines + [end]


def a_register(end):
    return int(re.search(r" af=([0-9A-F]{2})", end).group(1), 16)


def b_register(end):
    return int(re.search(r" bc=([0-9A-F]{2})", end).group(1), 16)


def main():
    tc = z.Toolchain(cc, pathlib.Path(emitter), opt="-O0", cache=False)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        jobs = {}   # name -> (ram, live, alts)
        jobs["ret_live"] = (P_RET, True, [(0x20, v) for v in RET_VALUES.values()])
        for name, value in RET_VALUES.items():
            jobs["ret_ref_" + name] = (mutated(P_RET, 0x20, value), False, ())
        jobs["len_live"] = (P_LEN, True, [(0x10, 0xC9), (0x10, 0x3E)])
        jobs["len_ref_C9"] = (mutated(P_LEN, 0x10, 0xC9), False, ())
        jobs["len_ref_3E"] = (mutated(P_LEN, 0x10, 0x3E), False, ())
        jobs["ovl_live"] = (P_OVL, True, [(0x01, 0x06)])
        jobs["ovl_ref"] = (mutated(P_OVL, 0x01, 0x06), False, ())
        jobs["cond_live"] = (P_COND, True, [(0x01, 0x28)])
        jobs["cond_ref"] = (mutated(P_COND, 0x01, 0x28), False, ())
        jobs["int_live"] = (P_INT, True, [(0x38, 0x00), (0x3B, 0x45)])
        int_mut = mutated(mutated(P_INT, 0x38, 0x00), 0x3B, 0x45)
        jobs["int_ref"] = (int_mut, False, ())
        jobs["pay_live"] = (P_PAY, True, [(0x01, 0x55)])
        jobs["pay_ref"] = (mutated(P_PAY, 0x01, 0x55), False, ())
        jobs["loop_live"] = (P_LOOP, True, [(0x02, 0x3C)])
        jobs["bound_live"] = (P_BOUND, True, [(0x00, v) for v in BOUND_VALUES])

        def build_job(item):
            name, (ram, live, alts) = item
            return name, build(tc, ram, live, tmp / name, alts)

        with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
            built = dict(pool.map(build_job, jobs.items()))
        exe = {name: pair[0] for name, pair in built.items()}
        stats = {name: pair[1] for name, pair in built.items()}

        def same(live_name, ref_name, ram, mutations, steps=40, *extra):
            args = list(extra)
            for offset, value in mutations:
                args += ["mutate-after", "0", "%X" % offset, "%X" % (ram[offset] ^ value)]
            live_lines, live_end = run(exe[live_name], ram, steps, *args, workdir=tmp)
            ref_ram = ram
            for offset, value in mutations:
                ref_ram = mutated(ref_ram, offset, value)
            ref_lines, ref_end = run(exe[ref_name], ref_ram, steps, *extra, "immutable", workdir=tmp)
            return states(live_lines, live_end) == states(ref_lines, ref_end), live_end, len(live_lines)

        # ---- one replacement opcode before first execution, and several alternatives at one byte ----
        for name, value in RET_VALUES.items():
            ok, end, count = same("ret_live", "ret_ref_" + name, P_RET, [(0x20, value)])
            check(ok and end.startswith("END halted"), "JR -> %s before its first execution: equal to the plain image of the live bytes (%d instructions)" % (name, count))
        _, end = run(exe["ret_live"], P_RET, 40, workdir=tmp)
        check(end.startswith("END halted") and a_register(end) == 5 and b_register(end) == 7, "the unmodified bytes still run the snapshot form (A=5, B=7)")
        lines, end = run(exe["ret_live"], P_RET, 40, "mutate-after", "0", "20", xor_to(P_RET, 0x20, 0x76), workdir=tmp)
        check(end.startswith("END code_mismatch") and "pc=0020" in end and len(lines) == 1,
              "a replacement that is not a compiled alternative is the typed code_mismatch at that instruction, before any effect")
        check(stats["ret_live"]["alt_entries"] >= 1 and stats["ret_live"]["alt_forms"] >= 3 and stats["ret_live"]["alt_dropped"] == 0,
              "the stats report the alternative entries and forms (%s)" % {k: v for k, v in stats["ret_live"].items() if k.startswith("alt_")})

        # ---- different lengths ----
        for value, label in ((0xC9, "3-byte CALL -> 1-byte RET"), (0x3E, "3-byte CALL -> 2-byte LD A,n")):
            ok, end, count = same("len_live", "len_ref_%02X" % value, P_LEN, [(0x10, value)])
            check(ok and count >= 3, "%s: equal to the plain image, correct successor (%d instructions)" % (label, count))

        # ---- overlapping entries: the operand of one instruction is the opcode of another entry ----
        ok, end, count = same("ovl_live", "ovl_ref", P_OVL, [(0x01, 0x06)], 10)
        check(ok and end.startswith("END halted"), "alternative byte as operand: the entry before it still runs its form with the live operand")
        ok, end, count = same("ovl_live", "ovl_ref", P_OVL, [(0x01, 0x06)], 10, "start", "1")
        check(ok and end.startswith("END halted"), "alternative byte as opcode: execution started at the overlapping entry runs the alternative form")
        ok, end, _ = same("ovl_live", "ovl_ref", P_OVL, [], 10, "start", "1")
        check(end.startswith("END halted"), "the same overlapping entry runs its snapshot form when the byte is unchanged")

        # ---- conditional branch ----
        ok, end, _ = same("cond_live", "cond_ref", P_COND, [(0x01, 0x28)], 10)
        check(ok and a_register(end) == 0x00, "JR NZ -> JR Z with Z set: the taken branch of the live form (A=%02X)" % a_register(end))
        lines, end = run(exe["cond_live"], P_COND, 10, workdir=tmp)
        check(a_register(end) == 0x11, "the unmodified JR NZ is not taken (A=%02X)" % a_register(end))

        # ---- interrupt and return paths ----
        ok, end, _ = same("int_live", "int_ref", P_INT, [(0x38, 0x00), (0x3B, 0x45)], 40, "int-after", "3")
        check(ok and end.startswith("END halted"), "IM 1 interrupt through a handler with a replaced opcode and RETI -> RETN equals the plain image")

        # ---- operand (payload) mutation is live and needs no form ----
        check(stats["pay_live"]["alt_forms"] == 2 and stats["pay_live"]["alt_entries"] == 2,
              "an alternative on an operand byte adds a form only to the entry that starts at that byte (both mirrors), none to the instruction it belongs to (%s)" % {k: v for k, v in stats["pay_live"].items() if k.startswith("alt_")})
        ok, end, _ = same("pay_live", "pay_ref", P_PAY, [(0x01, 0x55)], 10)
        check(ok and a_register(end) == 0x55, "the operand mutation runs the compiled form with the live operand (A=%02X)" % a_register(end))

        # ---- after the original form executed, and alternating forms ----
        steps = 40
        init = ("init", "a=0")
        _, end = run(exe["loop_live"], P_LOOP, steps, *init, workdir=tmp)
        check(end.startswith("END halted") and a_register(end) == 0 and b_register(end) == 0, "loop without mutation: NOP three times (A=0)")
        _, end = run(exe["loop_live"], P_LOOP, steps, "mutate-after", "0", "2", "3C", *init, workdir=tmp)
        check(end.startswith("END halted") and a_register(end) == 3, "NOP -> INC A before the first execution: three increments (A=%d)" % a_register(end))
        _, end = run(exe["loop_live"], P_LOOP, steps, "mutate-after", "3", "2", "3C", *init, workdir=tmp)
        check(end.startswith("END halted") and a_register(end) == 2, "NOP -> INC A after the NOP form executed once: two increments (A=%d)" % a_register(end))
        _, end = run(exe["loop_live"], P_LOOP, steps, "mutate-after", "3", "2", "3C", "mutate-after", "5", "2", "3C", *init, workdir=tmp)
        check(end.startswith("END halted") and a_register(end) == 1, "NOP -> INC A -> NOP (alternating forms): one increment (A=%d)" % a_register(end))
        _, end = run(exe["loop_live"], P_LOOP, steps, "mutate-after", "3", "2", "3C", "mutate-after", "5", "2", "3D", *init, workdir=tmp)
        check(end.startswith("END code_mismatch"), "a replacement outside the alternatives on the third pass stops with code_mismatch")

        # ---- the per-entry bound drops all alternatives of the image ----
        check(stats["bound_live"]["alt_dropped"] == 1 and stats["bound_live"]["alt_forms"] == 0 and stats["bound_live"]["alt_entries"] == 0,
              "more forms than the per-entry bound: all alternatives of the image are dropped (%s)" % {k: v for k, v in stats["bound_live"].items() if k.startswith("alt_")})
        _, end = run(exe["bound_live"], P_BOUND, 10, "mutate-after", "0", "0", xor_to(P_BOUND, 0x00, 0x3D), workdir=tmp)
        check(end.startswith("END code_mismatch"), "a dropped image behaves as the plain guard: the replacement is the typed mismatch")

    if failures:
        print("z80_live_alternatives_test: %d FAILED" % len(failures))
        return 1
    print("z80_live_alternatives_test: OK")
    return 0


sys.exit(main())
