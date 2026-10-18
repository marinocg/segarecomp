#!/usr/bin/env python3
"""Project-authored BIOS-like image for the SEG-064-T004 disc-extent images of the BIOS-image profile (ADR 0142 decision 5). Stdlib only.

No BIOS or commercial byte is read or reproduced. The "BIOS" releases a tiny sub program and then places a disc's boot area the way a
real BIOS does - the disc IP into main work RAM at $FF0000 and the disc SP into sub PRG RAM at $6000, over stale transient code the
sub's epoch image holds there - and enters the IP. The IP and SP bytes are supplied by the caller from the (independently generated)
fixture disc; this tool only copies them into the ROM image it builds.

Cases (main d7 selects, the sub's a6 mirrors it):
  0   the epoch only: the sub writes COMSTAT0 = $C0DE and stops
  12  the boot: the SP replaces the stale code at $6000 and the sub enters it (COMSTAT1 = the SP's marker), then main places the IP and
      calls it: the IP writes its marker and branches to the application entry
  13  as 12, but one byte of the IP is corrupted after it was placed: no compiled variant matches, the typed stop code_bytes_mismatch
  14  as 12, but the SP is never placed: the sub enters the stale epoch code (COMSTAT1 = $1111) - the epoch variant still runs - and the
      IP is entered through a sign-extended register ($FFFF0000), which the 24-bit program counter must select as $FF0000
usage: sega_cd_bios_boot_fixture.py build --out DIR --boot-ip FILE --boot-sp FILE  ->  DIR/bios.bin, DIR/expected.json (stdout)
"""
import argparse
import hashlib
import json
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sega_cd_fixture_disc as F  # noqa: E402

ROM_SIZE = 0x2000
MAIN = 0x0200
FAULT = 0x0C00
SUB_SRC = 0x0D00
STALE_SRC = 0x0E40
IP_SRC = 0x1000
SP_SRC = 0x1800
RESULT = 0xFFC000
GA = 0xA12000
WINDOW = 0x020000
SUB_SSP = 0x2000
SUB_ENTRY = 0x0100
GA_SUB = 0xFF8000
STALE_MARKER = 0x1111


def stale_code():
    s = F.Asm(0x6000)
    s.move("w", F.imm(STALE_MARKER), F.absl(GA_SUB + 0x22))   # COMSTAT1: the transient code the SP replaces
    s.move("b", F.imm(0x02), F.absl(GA_SUB + 0x0F))
    s.stop(0x2700)
    return s.assemble()


def sub_program():
    s = F.Asm(0)
    s.lw(SUB_SSP)
    s.lw(SUB_ENTRY)
    while s.pc < SUB_ENTRY:
        s.w(0)
    s.move("w", F.imm(0xC0DE), F.absl(GA_SUB + 0x20))
    s.move("b", F.imm(0x01), F.absl(GA_SUB + 0x0F))
    s.move("l", F.a(6), F.d(0))
    s.immop("cmp", "w", 12, F.d(0))
    s.bcc("lt", "done")
    s.label("wait_go")                                  # the boot cases: wait for main's go (the SP is placed), then enter $6000
    s.tst("b", F.absl(GA_SUB + 0x0E))
    s.bcc("eq", "wait_go")
    s.jsr(F.absl(0x6000))
    s.label("done")
    s.stop(0x2700)
    return s.assemble()


def copy_words(m, source, destination, byte_length, label):
    m.move("l", F.imm(source), F.a(0))
    m.move("l", F.imm(destination), F.a(1))
    m.move("w", F.imm(byte_length // 2 - 1), F.d(0))
    m.label(label)
    m.move("w", ("inc", 0), ("inc", 1))
    m.dbra(0, label)


def main_program(sub_len, stale_len, ip_len, sp_len):
    m = F.Asm(MAIN)
    copy_words(m, SUB_SRC, WINDOW, sub_len, "copy_sub")
    copy_words(m, STALE_SRC, WINDOW + 0x6000, stale_len, "copy_stale")
    m.move("b", F.imm(0x01), F.absl(GA + 0x01))         # SRES = 1, SBRQ = 0: the sub's runnable epoch
    F.wait_flag(m, GA + 0x0F, 0x01, "wait_sub")
    for case in (12, 13, 14):
        m.immop("cmp", "w", case, F.d(7))
        m.bcc("ne", "not%d" % case)
        if case != 14:                                  # place the disc SP over the stale code (the bus is requested around the copy)
            m.move("b", F.imm(0x03), F.absl(GA + 0x01))
            copy_words(m, SP_SRC, WINDOW + 0x6000, sp_len, "copy_sp%d" % case)
            m.move("b", F.imm(0x01), F.absl(GA + 0x01))
        m.move("b", F.imm(0x00), F.absl(GA + 0x0F))     # (the sub flag is rewritten by the SP / stale code with 2)
        m.move("b", F.imm(0x01), F.absl(GA + 0x0E))     # the go: the sub enters $6000
        F.wait_flag(m, GA + 0x0F, 0x02, "wait_sp%d" % case)
        copy_words(m, IP_SRC, 0xFF0000, ip_len, "copy_ip%d" % case)
        if case == 13:
            m.move("b", F.imm(0xFF), F.absl(0xFF0003))  # corrupt the low byte of the first instruction's immediate
        if case == 14:                                  # through a sign-extended register: the 24-bit program counter selects the same entry
            m.move("l", F.imm(0xFFFF0000), F.a(2))
            m.jsr(("ind", 2))
        else:
            m.jsr(F.absl(0xFF0000))
        m.label("not%d" % case)
    m.stop(0x2700)
    code = m.assemble()
    assert MAIN + len(code) <= FAULT, hex(MAIN + len(code))
    return code


def build(ip, sp):
    sub = sub_program()
    stale = stale_code()
    assert len(sp) % 2 == 0 and len(ip) % 2 == 0 and len(sub) % 2 == 0 and len(stale) % 2 == 0
    rom = bytearray(ROM_SIZE)
    struct.pack_into(">II", rom, 0, 0x00FFFE00, MAIN)
    for v in range(2, 64):
        struct.pack_into(">I", rom, 4 * v, FAULT)
    rom[0x100:0x120] = b"SEGA BIOS-IMAGE BOOT FIXTURE T04"
    main = main_program(len(sub), len(stale), len(ip), len(sp))
    rom[MAIN:MAIN + len(main)] = main
    fault = F.Asm(FAULT)
    fault.move("w", F.imm(0xDEAD), F.absl(RESULT + 0xF0))
    fault.stop(0x2700)
    rom[FAULT:FAULT + len(fault.assemble())] = fault.assemble()
    assert SUB_SRC + len(sub) <= STALE_SRC and STALE_SRC + len(stale) <= IP_SRC and IP_SRC + len(ip) <= SP_SRC and SP_SRC + len(sp) <= ROM_SIZE
    rom[SUB_SRC:SUB_SRC + len(sub)] = sub
    rom[STALE_SRC:STALE_SRC + len(stale)] = stale
    rom[IP_SRC:IP_SRC + len(ip)] = ip
    rom[SP_SRC:SP_SRC + len(sp)] = sp
    return bytes(rom), sub, stale


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=["build"])
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--boot-ip", type=pathlib.Path, required=True)
    parser.add_argument("--boot-sp", type=pathlib.Path, required=True)
    args = parser.parse_args(argv)
    rom, sub, stale = build(args.boot_ip.read_bytes(), args.boot_sp.read_bytes())
    padded = bytearray(sub)
    image = bytearray(0x6000 + len(stale))
    image[0:len(sub)] = sub
    image[0x6000:0x6000 + len(stale)] = stale
    image = bytes(image).ljust((len(image) + 0xFF) & ~0xFF, b"\0")
    expected = {"rom_sha256": hashlib.sha256(rom).hexdigest(), "stale_marker": STALE_MARKER, "epoch_length": len(image),
                "epoch_sha256": hashlib.sha256(image).hexdigest(), "result_ip_marker_offset": 0x40, "result_app_offset": 0x42}
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "bios.bin").write_bytes(rom)
    (args.out / "expected.json").write_text(json.dumps(expected, indent=1, sort_keys=True) + "\n")
    print(json.dumps(expected, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
