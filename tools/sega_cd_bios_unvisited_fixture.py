#!/usr/bin/env python3
"""Project-authored BIOS-image + disc for the SEG-098-T002 matrix (ADR 0211): static admission of never-visited modules in the BIOS-image build. Stdlib only.

No BIOS or commercial byte is read or reproduced. The "BIOS" (this file) boots like the real one at the level that matters here: its sub program reads the
disc's system area through the CDD/CDC (so the delivered-block ledger covers it), places the disc SP at sub PRG RAM $6000, returns Word RAM to main, which
copies the disc IP to main work RAM $FF0000 and enters it; the sub then enters the SP through its module jump table (init entry). The disc SP is the loader of
tools/sega_cd_unvisited_fixture.py (a `MAIN` module header + jump table, descriptor entry 0): it loads module A (default), B or C - chosen by the IP from the
machine input `main:d7` - through the ordinary CDD/CDC. The build-pass route (d7 = 0) loads A only; B and C are never loaded or executed by it.

usage: sega_cd_bios_unvisited_fixture.py build --out DIR [--case CASE]  ->  DIR/bios.bin, DIR/disc/{disc.cue,track01.bin}, DIR/expected.json
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
import sega_cd_unvisited_fixture as U  # noqa: E402

ROM_SIZE = L.ROM_SIZE
MAIN, FAULT, SUB_SRC = L.MAIN, L.FAULT, L.SUB_SRC
GA, GA_SUB, WINDOW, RESULT = L.GA, L.GA_SUB, L.WINDOW, L.RESULT
CASES = ("base", "overwritten", "header_patched", "unknown_lba", "unknown_dest", "prg_dest", "dispatch", "dispatch_unknown", "computed_entry")


def sub_program(sp_disc_offset, sp_len, init_entry):
    sectors_to_read = (sp_disc_offset + sp_len + 2047) // 2048
    sub = L.Sub()
    sub.load(0, sectors_to_read, 0)                                           # the system area (IP and SP) through the CDD/CDC into Word RAM
    sub.copy_blob(L.WORD_RAM_SUB + sp_disc_offset, F.SP_BASE, sp_len)   # the SP into PRG RAM at $6000
    s = sub.s
    s.move("b", F.imm(0x01), F.absl(GA_SUB + 0x03))             # RET = 1: Word RAM back to main (the IP is copied from it)
    s.move("b", F.imm(0x10), F.absl(GA_SUB + 0x0F))             # the SP is placed
    F.wait_flag(s, GA_SUB + 0x0E, 0x81, "w_enter")              # main: the IP is placed, enter the SP
    s.move("b", F.imm(0x20), F.absl(GA_SUB + 0x0F))
    s.label("w_clear")                                          # main clears the comm word before the SP's own handshake starts
    s.tst("b", F.absl(GA_SUB + 0x0E))
    s.bcc("ne", "w_clear")
    s.jsr(F.absl(init_entry))                                   # the module jump table's init entry (the BIOS performs the placement and the entry)
    s.stop(0x2700)
    return s.assemble()


def main_program(sub_len, ip_disc_offset, ip_len):
    m = F.Asm(MAIN)
    m.move("l", F.imm(SUB_SRC), F.a(0))
    m.move("l", F.imm(WINDOW), F.a(1))
    m.move("w", F.imm(sub_len // 2 - 1), F.d(0))
    m.label("copy_sub")
    m.move("w", ("inc", 0), ("inc", 1))
    m.dbra(0, "copy_sub")
    m.move("b", F.imm(0x01), F.absl(GA + 0x01))                 # SRES = 1, SBRQ = 0: the sub's runnable epoch
    F.wait_flag(m, GA + 0x0F, 0x01, "w_ready")
    m.move("b", F.imm(0x02), F.absl(GA + 0x03))                 # DMNA: Word RAM to the sub
    m.move("b", F.imm(0x01), F.absl(GA + 0x0E))                 # go: the sub reads the system area
    F.wait_flag(m, GA + 0x0F, 0x10, "w_placed")
    m.move("l", F.imm(F.WORD_RAM_MAIN + ip_disc_offset), F.a(0))   # the IP, from Word RAM (the sub returned it) to main work RAM
    m.move("l", F.imm(F.IP_BASE), F.a(1))
    m.move("w", F.imm(ip_len // 2 - 1), F.d(0))
    m.label("copy_ip")
    m.move("w", ("inc", 0), ("inc", 1))
    m.dbra(0, "copy_ip")
    m.move("b", F.imm(0x81), F.absl(GA + 0x0E))
    F.wait_flag(m, GA + 0x0F, 0x20, "w_entered")
    m.move("b", F.imm(0x00), F.absl(GA + 0x0E))
    m.jsr(F.absl(F.IP_BASE))
    m.stop(0x2700)
    code = m.assemble()
    assert MAIN + len(code) <= FAULT, hex(MAIN + len(code))
    return code


def iso_sectors(files):
    """SEG-100 (ADR 0221): a minimal ISO9660 volume (PVD at LBA 16, terminator at 17, a flat root at 18) naming `files` ({identifier: (lba, size)}).
    The game never reads it; the build's file-first enumeration does. No name carries meaning for the build."""
    pvd = bytearray(2048)
    pvd[0], pvd[1:6], pvd[6] = 1, b"CD001", 1
    pvd[8:40] = b"SEGARECOMP".ljust(32)
    pvd[40:72] = b"SEGARECOMP_FILEFIRST".ljust(32)
    pvd[80:88] = F.iso_both32(F.TRACK1_SECTORS)
    pvd[120:124] = F.iso_both16(1)
    pvd[124:128] = F.iso_both16(1)
    pvd[128:132] = F.iso_both16(2048)
    pvd[156:190] = F.iso_record(b"\x00", 18, 2048, directory=True)
    pvd[190:318] = b" " * 128
    pvd[881] = 1
    term = bytearray(2048)
    term[0], term[1:6], term[6] = 255, b"CD001", 1
    root = bytearray()
    root += F.iso_record(b"\x00", 18, 2048, directory=True)
    root += F.iso_record(b"\x01", 18, 2048, directory=True)
    for name, (lba, size) in files.items():
        root += F.iso_record(name.encode(), lba, size)
    assert len(root) <= 2048
    return {16: bytes(pvd), 17: bytes(term), 18: bytes(root.ljust(2048, b"\x00"))}


def build(out, case="base", many=0, many_groups=4, many_nops=0, iso=False):
    assert case in CASES, case
    ip, sp, sectors, oracle = U.build({"unvisited_case": case, "sp_table": True, "bios": True, "many": many, "many_groups": many_groups, "many_nops": many_nops})
    area, extents = F.system_area(ip, sp, "BIOSUNVIS")
    ip_off, ip_len = extents["ip"][0], extents["ip"][1]
    sp_off, sp_len = extents["sp"][0], extents["sp"][1]
    assert ip_off + ip_len <= 2048 <= sp_off and sp_off + sp_len <= 16 * 2048, extents       # IP in LBA 0, the SP from LBA 1 on: the sectors it spans are read
    sub = sub_program(sp_off, sp_len, F.SP_BASE + U.SP_TABLE_PREFIX)
    sub_len = (len(sub) + 1) & ~1
    rom = bytearray(ROM_SIZE)
    struct.pack_into(">II", rom, 0, 0x00FFFE00, MAIN)
    for v in range(2, 64):
        struct.pack_into(">I", rom, 4 * v, FAULT)
    rom[0x100:0x120] = b"SEGA BIOS-IMAGE UNVISITED FIX T02"[:32]
    main = main_program(sub_len, ip_off, (ip_len + 1) & ~1)
    rom[MAIN:MAIN + len(main)] = main
    fault = F.Asm(FAULT)
    fault.move("w", F.imm(0xDEAD), F.absl(RESULT + 0xF0))
    fault.stop(0x2700)
    rom[FAULT:FAULT + len(fault.assemble())] = fault.assemble()
    assert SUB_SRC + sub_len <= ROM_SIZE, hex(SUB_SRC + sub_len)
    rom[SUB_SRC:SUB_SRC + len(sub)] = sub
    import random
    rng = random.Random(0x7004)
    users = []
    if iso:   # SEG-100: the volume names the module sectors the loader reads (A..D, X) plus the many-module sectors
        listing = {"MOD%s.BIN;1" % k: (U.LBA[k], 2048) for k in ("A", "B", "C", "D", "X")}
        for k in range(many):
            listing["MODN%03d.BIN;1" % k] = (U.MANY_LBA + k, 2048)
        sectors = {**sectors, **iso_sectors(listing)}
    for lba in range(max(F.TRACK1_SECTORS, (max(sectors) + 4) if sectors else 0)):
        if lba < 16:
            users.append(area[lba * 2048:(lba + 1) * 2048])
        elif lba in sectors:
            users.append(sectors[lba])
        else:
            users.append(bytes(rng.randrange(256) for _ in range(2048)))
    out.mkdir(parents=True, exist_ok=True)
    disc = out / "disc"
    disc.mkdir(parents=True, exist_ok=True)
    (disc / "track01.bin").write_bytes(b"".join(users))
    (disc / "disc.cue").write_text('FILE "track01.bin" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n', newline="\n")
    (out / "bios.bin").write_bytes(bytes(rom))
    oracle = dict(oracle, case=case, rom_sha256=hashlib.sha256(bytes(rom)).hexdigest(), boot_extents=extents)
    (out / "expected.json").write_text(json.dumps(oracle, indent=1, sort_keys=True) + "\n")
    return oracle


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=["build"])
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--case", choices=CASES, default="base")
    parser.add_argument("--many-groups", type=int, default=4)
    parser.add_argument("--many-nops", type=int, default=0)
    parser.add_argument("--iso", action="store_true", help="SEG-100: add an ISO9660 volume naming the module sectors (the game never reads it)")
    parser.add_argument("--many", type=int, default=0, help="N distinct modules behind the dispatcher over 4 bases (SEG-098-T004)")
    args = parser.parse_args(argv)
    print(json.dumps(build(args.out, args.case, args.many, args.many_groups, args.many_nops, args.iso), sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
