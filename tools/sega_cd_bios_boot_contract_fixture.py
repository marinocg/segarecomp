#!/usr/bin/env python3
"""Project-authored BIOS-like image + disc for the SEG-113 static boot-image matrix (ADR 0257). Stdlib only.

No BIOS or commercial byte is read or reproduced. The "BIOS" implements the boot-placement contract of ADR 0257 with the machine's own CDD/CDC path:
the sub reads sectors 0..16 of the disc into Word RAM, reads the disc's SP into Word RAM and copies it to PRG RAM $6000 and calls it; the main
copies the IP span (disc bytes from $200) from Word RAM to main Work RAM $FF0000 and calls it. The BIOS image carries its profile tag at $100 (the
product recognises the fixture contract by that tag; a real BIOS never carries it).

Cases (every case builds its own BIOS and disc):
  contract      tag 1 (IP span $8000), SP at $1000, $400 bytes
  ip_short      tag 2 (IP span $4000)
  sp_long       SP of $2000 bytes at $1800
  sp_moved      SP at $2000, $600 bytes
  sp_source     the disc's SP descriptor names $1000 but the BIOS reads the SP from $1800 (descriptor/source disagreement: the static SP image is wrong)
  ip_patched    the BIOS patches one IP byte in Work RAM after the placement and before the call (the static IP image is not what runs)
  ip_trailing   the BIOS places only the first $2000 bytes of the IP span (a partial placement; the rest of the work RAM is stale)
  no_profile    the same BIOS and disc without the profile tag (an unverified BIOS: nothing is offered)
  form_ok       tag 3: a profile verified only for one header form; the disc has that form (offered)
  form_other    tag 3 with another SP descriptor (the BIOS still places it): the profile does not apply (header_not_verified), execution-observed admission

usage: sega_cd_bios_boot_contract_fixture.py build --case NAME --out DIR   ->   DIR/bios.bin, DIR/disc/{disc.cue,track01.bin}, DIR/expected.json
"""
import argparse
import hashlib
import json
import pathlib
import random
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sega_cd_fixture_disc as F  # noqa: E402
import sega_cd_bios_loader_fixture as L  # noqa: E402

CASES = ("contract", "ip_short", "sp_long", "sp_moved", "sp_source", "ip_patched", "ip_trailing", "no_profile", "form_ok", "form_other")
TAGS = {1: b"SEGA BOOT-CONTRACT FIXTURE 1    ", 2: b"SEGA BOOT-CONTRACT FIXTURE 2    ", 3: b"SEGA BOOT-CONTRACT FIXTURE 3    "}
IP_BASE = 0xFF0000
SP_PRG = 0x006000
IP_MARK = 0xFFC040      # RESULT + 0x40: the IP's marker
SP_MARK = L.MARK        # COMSTAT2: the SP's marker
PATCH_AT = 0x40         # the IP byte the ip_patched BIOS rewrites (inside the first NOP run)
WORD_RAM_MAIN_VIEW = 0x200000
TRACK1 = 160


def ip_code(marker):
    a = F.Asm(IP_BASE)
    a.move("w", F.imm(marker), F.absl(IP_MARK))
    for _ in range(64):
        a.nop()
    a.rts()
    return a.assemble()


def sp_code(marker):
    a = F.Asm(SP_PRG)
    a.move("w", F.imm(marker), F.absl(SP_MARK))
    for _ in range(16):
        a.nop()
    a.rts()
    return a.assemble()


def geometry(case):
    tag = 2 if case == "ip_short" else 3 if case in ("form_ok", "form_other") else 1
    ip_span = 0x4000 if tag == 2 else 0x8000
    sp_addr, sp_len = {"sp_long": (0x1800, 0x2000), "sp_moved": (0x2000, 0x600), "form_other": (0x1800, 0x400)}.get(case, (0x1000, 0x400))
    return tag, ip_span, sp_addr, sp_len


def filler(start, length, seed):
    rng = random.Random(seed)
    return bytes(rng.randrange(1, 256) for _ in range(length))


def build(case, out):
    tag, ip_span, sp_addr, sp_len = geometry(case)
    ip = ip_code(0xA001)
    sp = sp_code(0xB002)
    sp_read = 0x1800 if case == "sp_source" else sp_addr      # what the BIOS actually reads; the descriptor says sp_addr
    placed = 0x2000 if case == "ip_trailing" else ip_span
    area = bytearray(16 * 2048)
    area[0x000:0x010] = b"SEGADISCSYSTEM  "
    area[0x010:0x01B] = b"BOOTCONTRCT"
    area[0x020:0x02B] = b"SEGARECOMP "
    struct.pack_into(">IIII", area, 0x030, 0x200, 0x600, 0, 0x1000)
    struct.pack_into(">IIII", area, 0x040, sp_addr, sp_len, 0, 0x1000)
    area[0x100:0x110] = b"SEGA MEGA DRIVE "
    area[0x180:0x18E] = b"GM SEGARECOMP "
    area[0x1F0:0x1F3] = b"U  "
    area[0x200:0x8000] = filler(0x200, 0x7E00, 0x1001)             # the IP span is non-zero everywhere: a short or shifted placement is visible
    area[0x200:0x200 + len(ip)] = ip
    area[sp_read:sp_read + len(sp)] = sp                       # the SP the BIOS really reads; with sp_source the descriptor names other (filler) bytes
    if sp_addr + sp_len > len(area) or sp_addr < 0x800:
        raise SystemExit("bad geometry")
    rng = random.Random(0x6A55)
    users = []
    for lba in range(TRACK1):
        if lba < 16:
            users.append(bytes(area[lba * 2048:(lba + 1) * 2048]))
        elif lba == 16:
            users.append(filler(0, 2048, 0x1610))                 # sector 16: the span beyond the system area the IP placement reaches
        else:
            users.append(bytes(rng.randrange(256) for _ in range(2048)))
    out.mkdir(parents=True, exist_ok=True)
    disc = out / "disc"
    disc.mkdir(parents=True, exist_ok=True)
    (disc / "track01.bin").write_bytes(b"".join(users))
    (disc / "disc.cue").write_text('FILE "track01.bin" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n', newline="\n")

    # sub program: read the boot area into Word RAM, read the SP, copy it to PRG RAM and call it
    sub = L.Sub()
    sub.load(0, 17, 0)
    first = sp_read // 2048
    count = (sp_read % 2048 + sp_len + 2047) // 2048
    sub.load(first, count, 0x20000)
    sub.copy_blob(L.WORD_RAM_SUB + 0x20000 + sp_read % 2048, SP_PRG, sp_len)
    sub.run(SP_PRG)
    code = sub.finish()
    sub_len = (len(code) + 1) & ~1

    # main program: release the sub, place the IP span from Word RAM at $FF0000, call it
    m = F.Asm(L.MAIN)
    m.move("l", F.imm(L.SUB_SRC), F.a(0))
    m.move("l", F.imm(L.WINDOW), F.a(1))
    m.move("w", F.imm(sub_len // 2 - 1), F.d(0))
    m.label("copy_sub")
    m.move("w", ("inc", 0), ("inc", 1))
    m.dbra(0, "copy_sub")
    m.move("b", F.imm(0x01), F.absl(L.GA + 0x01))
    F.wait_flag(m, L.GA + 0x0F, 0x01, "w_ready")
    m.move("b", F.imm(0x02), F.absl(L.GA + 0x03))
    m.move("b", F.imm(0x01), F.absl(L.GA + 0x0E))
    F.wait_flag(m, L.GA + 0x0F, 0x02, "w_done")
    m.move("l", F.imm(WORD_RAM_MAIN_VIEW + 0x200), F.a(0))
    m.move("l", F.imm(IP_BASE), F.a(1))
    m.move("w", F.imm(placed // 2 - 1), F.d(0))
    m.label("copy_ip")
    m.move("w", ("inc", 0), ("inc", 1))
    m.dbra(0, "copy_ip")
    if case == "ip_patched":
        m.move("b", F.imm(0x5A), F.absl(IP_BASE + PATCH_AT))
    m.jsr(F.absl(IP_BASE))
    for i in range(3):
        m.move("w", F.absl(L.GA + 0x24 + 2 * i), F.absl(L.RESULT + 0x10 + 2 * i))
    m.move("b", F.imm(0xA5), F.absl(L.RESULT + 0xFF))
    m.stop(0x2700)
    main = m.assemble()
    assert L.MAIN + len(main) <= L.FAULT, hex(L.MAIN + len(main))

    rom = bytearray(L.ROM_SIZE)
    struct.pack_into(">II", rom, 0, 0x00FFFE00, L.MAIN)
    for v in range(2, 64):
        struct.pack_into(">I", rom, 4 * v, L.FAULT)
    assert all(len(t) == 32 for t in TAGS.values())
    rom[0x100:0x120] = TAGS[tag] if case != "no_profile" else b"SEGA BOOT-CONTRACT UNVERIFIED   "
    rom[L.MAIN:L.MAIN + len(main)] = main
    fault = F.Asm(L.FAULT)
    fault.move("w", F.imm(0xDEAD), F.absl(L.RESULT + 0xF0))
    fault.stop(0x2700)
    rom[L.FAULT:L.FAULT + len(fault.assemble())] = fault.assemble()
    assert L.SUB_SRC + sub_len <= L.ROM_SIZE
    rom[L.SUB_SRC:L.SUB_SRC + len(code)] = code
    assert len(rom) == L.ROM_SIZE
    (out / "bios.bin").write_bytes(bytes(rom))

    truth = {
        "case": case, "tag": tag, "rom_sha256": hashlib.sha256(bytes(rom)).hexdigest(), "ip_span": ip_span, "ip_placed": placed,
        "sp": {"address": sp_addr, "length": sp_len, "read_from": sp_read},
        "profile_applies": case != "no_profile",
        "markers": {"ip": 0xA001, "sp": 0xB002},
        "ip_patched": case == "ip_patched",
    }
    (out / "expected.json").write_text(json.dumps(truth, indent=1, sort_keys=True) + "\n")
    return truth


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=["build"])
    parser.add_argument("--case", choices=CASES, required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    args = parser.parse_args(argv)
    print(json.dumps(build(args.case, args.out), sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
