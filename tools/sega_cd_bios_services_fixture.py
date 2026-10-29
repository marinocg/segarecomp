#!/usr/bin/env python3
"""Project-authored BIOS-image + disc for the SEG-099 work-variable loader matrix (ADR 0216). Stdlib only.

No BIOS or commercial byte is read or reproduced. The "BIOS" (this file) boots like the one of tools/sega_cd_bios_unvisited_fixture.py and additionally implements, in
sub PRG RAM at the public vector `_CDBIOS = $5F22`, the documented CD services the disc SP calls (ADR 0215): ROMREADN, CDCSTOP, CDCSTAT, CDCREAD, CDCTRN (sub-read
device: the data is read from the host-data port into (A0)) and CDCACK, over the machine's CDD/CDC. Any other function number returns carry set. The disc SP is
tools/sega_cd_services_sp_fixture.py: a file engine whose arguments live in a RAM work area; it loads module A (default) or B by NAME through the ISO9660 directory.

usage: sega_cd_bios_services_fixture.py build --out DIR [--case CASE]  ->  DIR/bios.bin, DIR/disc/{disc.cue,track01.bin}, DIR/expected.json
"""
import argparse
import hashlib
import json
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sega_cd_fixture_disc as F  # noqa: E402
import sega_cd_bios_loader_fixture as L  # noqa: E402
import sega_cd_bios_unvisited_fixture as BU  # noqa: E402
import sega_cd_services_sp_fixture as S  # noqa: E402

ROM_SIZE = 0x8000
GA_SUB = L.GA_SUB
BV_LBA, BV_COUNT, BV_WANT, BV_HDR, BV_DAC, BV_READY = 0x5E00, 0x5E04, 0x5E08, 0x5E0C, 0x5E10, 0x5E12
TABLE = 0x1000
TABLE_LBAS = 96
ENTRY = 14   # five play words + two header words


def table_bytes():
    out = bytearray()
    for lba in range(TABLE_LBAS):
        play, _ = F.cdd_words(3, lba)
        msf = lba + 150
        want = [(F.bcd(msf // 75 // 60) << 8) | F.bcd(msf // 75 % 60), (F.bcd(msf % 75) << 8) | 0x01]
        out += b"".join(struct.pack(">H", w) for w in list(play) + want)
    return bytes(out)


def sub_program(sp_disc_offset, sp_len, init_entry):
    sectors_to_read = (sp_disc_offset + sp_len + 2047) // 2048
    sub = L.Sub()
    sub.load(0, sectors_to_read, 0)
    sub.copy_blob(L.WORD_RAM_SUB + sp_disc_offset, F.SP_BASE, sp_len)
    s = sub.s
    s.move("b", F.imm(0x01), F.absl(GA_SUB + 0x03))
    s.move("b", F.imm(0x10), F.absl(GA_SUB + 0x0F))
    F.wait_flag(s, GA_SUB + 0x0E, 0x81, "w_enter")
    s.move("b", F.imm(0x20), F.absl(GA_SUB + 0x0F))
    s.label("w_clear")
    s.tst("b", F.absl(GA_SUB + 0x0E))
    s.bcc("ne", "w_clear")
    s.jsr(F.absl(init_entry))
    s.stop(0x2700)

    def ccr(value):
        s.w(0x44FC, value)                                    # MOVE #value,CCR

    def cdc_write(reg, value):
        s.move("b", F.imm(reg), F.absl(GA_SUB + 0x05))
        s.move("b", F.imm(value), F.absl(GA_SUB + 0x07))

    def want_for(reg_lba_label):
        pass

    # ---- the service dispatcher (D0.W = function) -----------------------------------------------------------------------------------
    s.label("svc")
    for fn, label in ((0x20, "romreadn"), (0x89, "cdcstop"), (0x8A, "cdcstat"), (0x8B, "cdcread"), (0x8C, "cdctrn"), (0x8D, "cdcack")):
        s.immop("cmp", "w", fn, F.d(0))
        s.bcc("eq", label)
    ccr(1)                                                    # any other function: carry set, nothing done
    s.rts()

    s.label("romreadn")                                       # A0 -> {first sector, count}
    s.move("l", ("ind", 0), F.absl(BV_LBA))
    s.move("l", ("d16", 4, 0), F.absl(BV_COUNT))
    s.move("b", F.imm(0), F.absl(BV_READY))
    s.move("l", F.absl(BV_LBA), F.d(0))
    s.mulu(F.imm(ENTRY), 0)
    s.lea(F.absl(TABLE), 1)
    s.w(0xD3C0)                                               # ADDA.L D0,A1
    s.move("w", ("d16", 10, 1), F.absl(BV_WANT))
    s.move("w", ("d16", 12, 1), F.absl(BV_WANT + 2))
    cdc_write(0x0F, 0x00)
    cdc_write(0x01, 0x02)
    cdc_write(0x0A, 0x84)
    s.move("b", F.imm(0x00), F.absl(GA_SUB + 0x07))
    for i in range(5):
        s.move("w", ("d16", 2 * i, 1), F.absl(GA_SUB + 0x42 + 2 * i))   # the play command (the last word starts it)
    ccr(0)
    s.rts()

    s.label("cdcstop")
    s.move("b", F.imm(0), F.absl(BV_READY))
    ccr(0)
    s.rts()

    s.label("cdcstat")
    s.tst("b", F.absl(BV_READY))
    s.bcc("ne", "ok")
    s.move("b", F.imm(0x0F), F.absl(GA_SUB + 0x05))
    s.move("b", F.absl(GA_SUB + 0x07), F.d(0))
    s.btst(7, F.d(0))
    s.bcc("ne", "cs")
    s.move("b", F.imm(0x04), F.absl(GA_SUB + 0x05))
    for i in range(4):
        s.move("b", F.absl(GA_SUB + 0x07), F.absl(BV_HDR + i))
    s.move("b", F.absl(GA_SUB + 0x07), F.absl(BV_DAC + 1))
    s.move("b", F.absl(GA_SUB + 0x07), F.absl(BV_DAC))
    s.move("w", F.absl(BV_HDR), F.d(0))
    s.alu("cmp", "w", F.absl(BV_WANT), 0)
    s.bcc("ne", "cs")
    s.move("w", F.absl(BV_HDR + 2), F.d(0))
    s.alu("cmp", "w", F.absl(BV_WANT + 2), 0)
    s.bcc("ne", "cs")
    s.move("w", F.absl(BV_DAC), F.d(0))
    s.quick("add", "w", 4, F.d(0))                            # DAC = PT + 4
    s.move("w", F.d(0), F.absl(BV_DAC))
    s.move("b", F.imm(1), F.absl(BV_READY))
    s.label("ok")
    ccr(0)
    s.rts()
    s.label("cs")
    ccr(1)
    s.rts()

    s.label("cdcread")
    s.tst("b", F.absl(BV_READY))
    s.bcc("eq", "cs")
    s.move("l", F.absl(BV_HDR), F.d(0))
    ccr(0)
    s.rts()

    s.label("cdctrn")                                         # A0 = data buffer, A1 = header buffer; device 3
    s.tst("b", F.absl(BV_READY))
    s.bcc("eq", "cs")
    s.move("b", F.imm(3), F.absl(GA_SUB + 0x04))
    s.move("b", F.imm(0x02), F.absl(GA_SUB + 0x05))
    s.move("b", F.imm(0xFF), F.absl(GA_SUB + 0x07))
    s.move("b", F.imm(0x07), F.absl(GA_SUB + 0x07))
    s.move("b", F.absl(BV_DAC + 1), F.absl(GA_SUB + 0x07))
    s.move("b", F.absl(BV_DAC), F.absl(GA_SUB + 0x07))
    s.move("b", F.imm(0x00), F.absl(GA_SUB + 0x07))           # DTRG
    s.move("l", F.absl(BV_HDR), ("inc", 1))
    s.move("w", F.imm(1023), F.d(0))
    s.label("ct_read")
    s.move("w", F.absl(GA_SUB + 0x08), ("inc", 0))
    s.dbra(0, "ct_read")
    s.label("ct_edt")
    s.btst(7, F.absl(GA_SUB + 0x04))
    s.bcc("eq", "ct_edt")
    ccr(0)
    s.rts()

    s.label("cdcack")
    s.move("b", F.imm(0), F.absl(BV_READY))
    s.quick("add", "l", 1, F.absl(BV_LBA))
    s.quick("sub", "l", 1, F.absl(BV_COUNT))
    s.move("l", F.absl(BV_LBA), F.d(0))
    s.mulu(F.imm(ENTRY), 0)
    s.lea(F.absl(TABLE), 1)
    s.w(0xD3C0)                                               # ADDA.L D0,A1
    s.move("w", ("d16", 10, 1), F.absl(BV_WANT))
    s.move("w", ("d16", 12, 1), F.absl(BV_WANT + 2))
    ccr(0)
    s.rts()

    while s.pc < TABLE:
        s.w(0)
    assert s.pc == TABLE, hex(s.pc)
    for word in struct.unpack(">%dH" % (len(table_bytes()) // 2), table_bytes()):
        s.w(word)
    assert s.pc < 0x5E00, hex(s.pc)
    while s.pc < 0x5F22:
        s.w(0)
    s.w(0x4EF9)                                               # JMP svc : the public _CDBIOS vector
    s.lw("svc")
    return s.assemble()


def build(out, case="base"):
    ip, sp, sectors, oracle = S.build({"services_case": case})
    area, extents = F.system_area(ip, sp, "BIOSSVC")
    ip_off, ip_len = extents["ip"][0], extents["ip"][1]
    sp_off, sp_len = extents["sp"][0], extents["sp"][1]
    assert ip_off + ip_len <= 2048 <= sp_off and sp_off + sp_len <= 16 * 2048, extents
    sub = sub_program(sp_off, sp_len, F.SP_BASE + S.SP_TABLE_PREFIX)
    sub_len = (len(sub) + 1) & ~1
    rom = bytearray(ROM_SIZE)
    struct.pack_into(">II", rom, 0, 0x00FFFE00, L.MAIN)
    for v in range(2, 64):
        struct.pack_into(">I", rom, 4 * v, L.FAULT)
    rom[0x100:0x120] = b"SEGA BIOS-IMAGE SERVICES FX9"[:32]
    main = BU.main_program(sub_len, ip_off, (ip_len + 1) & ~1)
    rom[L.MAIN:L.MAIN + len(main)] = main
    fault = F.Asm(L.FAULT)
    fault.move("w", F.imm(0xDEAD), F.absl(L.RESULT + 0xF0))
    fault.stop(0x2700)
    rom[L.FAULT:L.FAULT + len(fault.assemble())] = fault.assemble()
    assert L.SUB_SRC + sub_len <= ROM_SIZE, hex(L.SUB_SRC + sub_len)
    rom[L.SUB_SRC:L.SUB_SRC + len(sub)] = sub
    users = []
    for lba in range(S.LBA_PVD + 80):
        users.append(area[lba * 2048:(lba + 1) * 2048] if lba < 16 else sectors[lba])
    out.mkdir(parents=True, exist_ok=True)
    disc = out / "disc"
    disc.mkdir(parents=True, exist_ok=True)
    (disc / "track01.bin").write_bytes(b"".join(users))
    (disc / "disc.cue").write_text('FILE "track01.bin" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n', newline="\n")
    (out / "bios.bin").write_bytes(bytes(rom))
    oracle = dict(oracle, rom_sha256=hashlib.sha256(bytes(rom)).hexdigest(), boot_extents=extents)
    (out / "expected.json").write_text(json.dumps(oracle, indent=1, sort_keys=True) + "\n")
    return oracle


def main(argv=None):
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build")
    b.add_argument("--out", required=True)
    b.add_argument("--case", default="base")
    args = ap.parse_args(argv)
    oracle = build(pathlib.Path(args.out), args.case)
    print(json.dumps(oracle, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
