#!/usr/bin/env python3
"""SEG-032-T004 (ADR 0072, contract sections 2-3): the Genesis Z80 address space, bank window and 68K Z80-area view.

A project-authored Z80 program is materialized as a RAM-backed image (the T003 registry), compiled to generated-native C and run by
the Genesis Z80 machine (platforms/genesis/runtime/z80_machine.c) on the real Genesis runtime. One image serves every scenario: the
harness stores a scenario id at Z80 RAM $1F00 and the program branches on it. The cartridge is a synthetic 128 KiB region
(byte i = (i * 31 + 7) & $FF); no per-title data of any kind exists. Proved:
  * scenario 0: nine-write serial bank register (banks 0, 1, 3 read the right ROM bytes through $8000-$FFFF, including the last
    byte of the region), the $2000 mirror, a work-RAM write through the window at bank $1C0, YM2612 and PSG ports reaching the
    shared seam;
  * every unsupported access is a typed view stop and nothing else: the unused/$7F00 window, a bank register read, a bank target
    beyond the embedded region, a work-RAM read (open fact U3), a ROM write, a VDP-port write and a PSG read through the window;
    I/O ports are inert;
  * the 68K side: no access without the bus grant (typed), the $A02000 mirror, WORD write stores the high byte, WORD read duplicates,
    LONG fails closed, the bank register shifts in from bytes and words and cannot be read;
  * the program is the same bytes the registry compiled: the guard runs on every instruction of the real run.
usage: genesis_z80_view_test.py <registry_emitter> <cc> <source-root> <c++>
"""
import pathlib
import re
import subprocess
import sys
import tempfile

registry_emitter, cc, root = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3])
cxx = sys.argv[4] if len(sys.argv) > 4 else "c++"
sys.path.insert(0, str(root / "tools"))
import sms_fixture_rom as sms  # noqa: E402
import genesis_ym2612_build as ymbuild  # noqa: E402
import z80_conformance as z  # noqa: E402

DIAG_VIEW, DIAG_BANK, DIAG_NO_BUS, DIAG_Z80_RAM = 53, 54, 55, 40


def rom_byte(i):
    return (i * 31 + 7) & 0xFF


def set_bank(value):
    return "".join("        ld a,%d\n        ld (0x6000),a\n" % ((value >> b) & 1) for b in range(9))


def program():
    sc = ["sc%d" % i for i in range(9)]
    src = ".org 0x0000\n        ld a,(0x1F00)\n"
    for i, label in enumerate(sc):
        src += "        cp %d\n        jp z,%s\n" % (i, label)
    src += "        halt\n"
    src += "sc0:\n" + set_bank(0) + "        ld a,(0x8123)\n        ld (0x1000),a\n"
    src += set_bank(1) + "        ld a,(0x8000)\n        ld (0x1001),a\n"
    src += set_bank(3) + "        ld a,(0xFFFF)\n        ld (0x1002),a\n"
    src += "        ld a,0x5A\n        ld (0x2F05),a\n        ld a,(0x0F05)\n        ld (0x1003),a\n"
    src += "        ld a,0x2B\n        ld (0x4000),a\n        ld a,0x80\n        ld (0x4001),a\n        ld a,(0x4000)\n        ld (0x1004),a\n"
    src += "        ld a,0x8F\n        ld (0x7F11),a\n"
    src += set_bank(0x1C0) + "        ld a,0xA7\n        ld (0x8010),a\n        halt\n"
    src += "sc1:   ld a,(0x7000)\n        halt\n"
    src += "sc2:\n" + set_bank(0x10) + "        ld a,(0x8000)\n        halt\n"
    src += "sc3:\n" + set_bank(0x1C0) + "        ld a,(0x8010)\n        halt\n"
    src += "sc4:\n" + set_bank(0) + "        ld (0x8000),a\n        halt\n"
    src += "sc5:   ld a,(0x6000)\n        halt\n"
    src += "sc6:   ld (0x7F10),a\n        halt\n"
    src += "sc7:   ld a,(0x7F11)\n        halt\n"
    src += "sc8:   in a,(0x7F)\n        out (0x7F),a\n        ld a,0x66\n        ld (0x1005),a\n        halt\n"
    image, _ = sms.Assembler(src).assemble()
    ram = bytearray(8192)
    for address, byte in image.items():
        ram[address] = byte
    return bytes(ram), max(image) + 1


failures = []


def check(ok, label):
    print("%-5s %s" % ("ok" if ok else "FAIL", label))
    if not ok:
        failures.append(label)


def main():
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        ram, length = program()
        written = bytearray(1024)
        for offset in range(length):
            written[offset >> 3] |= 1 << (offset & 7)
        spec = tmp / "image.spec"
        spec.write_text("epoch %s %s\n" % (ram.hex(), bytes(written).hex()))
        emitted = subprocess.run([registry_emitter, str(spec), str(tmp / "gen"), "genesis_z80"], text=True, capture_output=True)
        assert emitted.returncode == 0, emitted.stdout + emitted.stderr
        harness = root / "tests" / "tools" / "genesis_z80_view_harness.c"
        tc = z.Toolchain(cc, pathlib.Path("unused-emitter"), opt="-O0", cache=False)
        exe, message = z.compile_units(
            tc, tmp / "gen", "genesis_z80",
            extra_sources=[harness, root / "platforms/genesis/runtime/runtime.c", root / "platforms/genesis/runtime/z80_machine.c",
                           root / "platforms/genesis/runtime/genesis_audio.c", root / "platforms/genesis/runtime/genesis_mixer.c", root / "libs/device/sega/psg/src/sn76489.c"],
            extra_flags=["-I", str(root / "platforms/genesis/runtime"), "-I", str(root / "libs/device/sega/psg/include"), "-I", str(root / "libs/device/sega/ym2612/include")],
            extra_objects=ymbuild.build_objects(cc, cxx, root, tmp / "ymobj"))
        assert exe is not None, message
        hexfile = tmp / "ram.hex"
        hexfile.write_text(ram.hex())

        def run(scenario):
            out = subprocess.run([str(exe), str(hexfile), str(scenario), "200000"], text=True, capture_output=True, check=True).stdout
            result = dict(kv.split("=") for kv in re.search(r"RESULT (.*)", out).group(1).split())
            return result, out

        r, out = run(0)
        z80ram = bytes.fromhex(r["z80ram"])
        check(r["outcome"] == "halted" and r["view_stop"] == "0", "scenario 0 runs to HALT with no view stop")
        check(z80ram[0] == rom_byte(0x123) and z80ram[1] == rom_byte(0x8000) and z80ram[2] == rom_byte(0x1FFFF),
              "banks 0, 1 and 3 read the right cartridge bytes (including the last byte of the region)")
        check(z80ram[3] == 0x5A, "a write at $2F05 is visible at $0F05 (the $2000 mirror)")
        check(int(r["work10"], 16) == 0xA7 and r["bank"] == "1c0", "a write through the window at bank $1C0 lands in work RAM (and the register holds $1C0)")
        check(z80ram[4] == 0x80 and int(r["psg_tone0"]) == 0x0F,
              "the YM2612 write/status read (busy right after a data write) and the PSG write reach the shared devices")
        for scenario, diag, label in ((1, DIAG_VIEW, "an access in the unused $6100-$7EFF space"),
                                      (2, DIAG_BANK, "a bank target beyond the embedded cartridge region"),
                                      (3, DIAG_BANK, "a work-RAM read through the window (open fact U3)"),
                                      (4, DIAG_BANK, "a ROM write through the window"),
                                      (5, DIAG_VIEW, "a read of the write-only bank register"),
                                      (6, DIAG_VIEW, "a VDP-port write through the $7F00 window"),
                                      (7, DIAG_VIEW, "a PSG read through the $7F00 window")):
            r, _ = run(scenario)
            check(int(r["view_stop"]) == diag and r["outcome"] == "deadline", "typed view stop %d for %s" % (diag, label))
        r, _ = run(8)
        check(r["outcome"] == "halted" and r["view_stop"] == "0" and bytes.fromhex(r["z80ram"])[5] == 0x66, "I/O ports are inert (IN returns $FF, OUT is ignored)")
        # the 68K side
        _, out = run(0)
        lines = [l for l in out.splitlines() if l.startswith("M68K ")]
        first = dict(kv.split("=") for kv in lines[0][5:].split())
        m = dict(kv.split("=") for kv in lines[1][5:].split())
        check(first["without_bus_write"] == "0" and first["diag"] == str(DIAG_NO_BUS), "no 68K access to the Z80 area without the bus grant (typed)")
        check(m["mirror_read"] == "12", "the 68K reads its own write back through the $A02000 mirror")
        check(m["word_write_byte"] == "ab" and m["word_read"] == "abab", "a WORD write stores the high byte; a WORD read returns the byte in both halves")
        check(m["long_read"] == "0" and m["long_diag"] == str(DIAG_Z80_RAM), "a LONG access fails closed")
        check(m["bank68"] == "101", "nine byte writes to $A06000 shift the bank register in LSB first")
        check(m["bank68w"] == "180", "a WORD write contributes D8 of the word and shifts the register right")
        check(m["bankreg_read"] == "0" and m["bankreg_diag"] == str(DIAG_VIEW), "the bank register cannot be read from the 68K (typed)")
    print("genesis z80 view: %s" % ("FAILED (%d)" % len(failures) if failures else "ok"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
