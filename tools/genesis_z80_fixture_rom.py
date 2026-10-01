#!/usr/bin/env python3
"""Deterministic synthetic Genesis fixture ROM builder for the Z80/audio milestone (SEG-032-T001, ADR 0072).

Stdlib only. Every byte emitted is project-authored: hand-encoded MC68000 code, Z80 programs assembled with the
Z80 assembler of tools/sms_fixture_rom.py, a fixed header and fill. No commercial byte is read or reproduced.

A fixture is (name, 68K program, Z80 program bytes). The ROM is regenerated at test time and never committed; the
committed manifest tests/fixtures/genesis-z80-fixture-roms.json records each fixture's size and SHA-256, so
`--check` proves byte-for-byte reproducibility on every host.

Result-block convention: the 68K program reports into work RAM at $FF0000 (offset r), and writes the completion
byte $A5 at $FF00FF last.

Usage:
  genesis_z80_fixture_rom.py --out DIR [--fixture NAME]...   write <name>.md
  genesis_z80_fixture_rom.py --check                         verify every fixture against the manifest
  genesis_z80_fixture_rom.py --write-manifest                regenerate the committed manifest
"""
import argparse
import hashlib
import json
import pathlib
import struct
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import sms_fixture_rom as sms  # noqa: E402  (the Z80 assembler only)

SCHEMA = 1
MANIFEST = HERE.parent / "tests" / "fixtures" / "genesis-z80-fixture-roms.json"
ROM_SIZE = 0x4000
PROGRAM_BASE = 0x200
RESULT = 0xFF0000  # work RAM result block
DONE = RESULT + 0xFF

Z80_BUSREQ = 0xA11100
Z80_RESET = 0xA11200
Z80_RAM = 0xA00000


class M68k:
    """A tiny encoder for the handful of MC68000 forms the fixtures need (absolute long addressing only)."""

    def __init__(self, base=PROGRAM_BASE):
        self.base = base
        self.words = []
        self.labels = {}
        self.fixups = []  # (word index, label, kind)

    @property
    def pc(self):
        return self.base + 2 * len(self.words)

    def w(self, *values):
        for v in values:
            self.words.append(v & 0xFFFF)

    def l(self, value):
        self.w(value >> 16, value)

    def label(self, name):
        self.labels[name] = self.pc

    # MOVE.W #imm,(abs).L
    def move_w_imm(self, imm, address):
        self.w(0x33FC, imm)
        self.l(address)

    # MOVE.B #imm,(abs).L
    def move_b_imm(self, imm, address):
        self.w(0x13FC, imm & 0xFF)
        self.l(address)

    # MOVE.B (abs).L,(abs).L
    def move_b(self, src, dst):
        self.w(0x13F9)
        self.l(src)
        self.l(dst)

    # BTST #0,(abs).L
    def btst0(self, address):
        self.w(0x0839, 0x0000)
        self.l(address)

    def branch(self, opcode, label):  # opcode: 0x66 BNE, 0x67 BEQ, 0x60 BRA (word displacement)
        self.w(opcode << 8)  # 16-bit displacement follows
        self.fixups.append((len(self.words), label))
        self.w(0)

    def delay(self, count, name):
        self.w(0x223C)
        self.l(count)  # MOVE.L #count,D1
        self.label(name)
        self.w(0x5381)  # SUBQ.L #1,D1
        self.branch(0x66, name)  # BNE

    def acquire_bus(self, name):
        self.move_w_imm(0x0100, Z80_BUSREQ)
        self.label(name)
        self.btst0(Z80_BUSREQ)
        self.branch(0x66, name)

    def release_bus(self):
        self.move_w_imm(0x0000, Z80_BUSREQ)

    def copy_to_z80(self, program_label, length, loop):
        self.w(0x41FA)  # LEA d16(PC),A0
        self.fixups.append((len(self.words), program_label + ":pcrel"))
        self.w(0)
        self.w(0x43F9)
        self.l(Z80_RAM)  # LEA abs.L,A1
        self.w(0x303C, length - 1)  # MOVE.W #n-1,D0
        self.label(loop)
        self.w(0x12D8)  # MOVE.B (A0)+,(A1)+
        self.w(0x51C8)  # DBRA D0
        self.fixups.append((len(self.words), loop + ":dbra"))
        self.w(0)

    def data(self, program_label, payload):
        self.label(program_label)
        padded = bytes(payload) + (b"\x00" if len(payload) % 2 else b"")
        for i in range(0, len(padded), 2):
            self.w((padded[i] << 8) | padded[i + 1])

    def resolve(self):
        for index, target in self.fixups:
            word_address = self.base + 2 * index
            if target.endswith(":pcrel"):
                value = self.labels[target[:-6]] - word_address
            elif target.endswith(":dbra"):
                value = self.labels[target[:-5]] - word_address
            else:
                value = self.labels[target] - word_address
            self.words[index] = value & 0xFFFF
        return b"".join(struct.pack(">H", w) for w in self.words)


def z80(source):
    image, _ = sms.Assembler(source).assemble()
    end = max(image) + 1
    return bytes(image.get(i, 0) for i in range(end))


def header(size):
    text = bytearray(b" " * 0x100)
    text[0x00:0x10] = b"SEGA MEGA DRIVE "
    text[0x10:0x20] = b"(C)SEGA 2026.OCT "[:16]
    text[0x20:0x30] = b"GENESIS Z80 FIXT"[:16]
    text[0x50:0x60] = b"GENESIS Z80 FIXT"[:16]
    text[0x80:0x82] = b"GM"
    text[0x82:0x8E] = b"00000000-00  "[:12]
    text[0x90:0x9F] = b"J" + b" " * 14
    text[0xA0:0xA4] = struct.pack(">I", 0)
    text[0xA4:0xA8] = struct.pack(">I", size - 1)
    text[0xA8:0xAC] = struct.pack(">I", 0xFF0000)
    text[0xAC:0xB0] = struct.pack(">I", 0xFFFFFF)
    text[0xF0:0xF3] = b"U  "
    return bytes(text)


def build_rom(code, size=ROM_SIZE):
    rom = bytearray([0xFF]) * size
    vectors = bytearray()
    vectors += struct.pack(">I", 0x00FF0000)  # initial SSP
    vectors += struct.pack(">I", PROGRAM_BASE)  # initial PC
    for _ in range(62):
        vectors += struct.pack(">I", PROGRAM_BASE)  # every other vector: the (spinning) default
    rom[0:0x100] = vectors
    rom[0x100:0x200] = header(size)
    rom[PROGRAM_BASE:PROGRAM_BASE + len(code)] = code
    checksum = 0
    for i in range(0x200, size, 2):
        checksum = (checksum + (rom[i] << 8) + rom[i + 1]) & 0xFFFF
    rom[0x18E:0x190] = struct.pack(">H", checksum)
    return bytes(rom)


# --- fixture: BUSREQ / RESET / Z80 RAM / bank / YM status black-box probe -------------------------------

PROBE_Z80 = """
.org 0x0000
        ld b,9
        ld hl,0x6000
        xor a
bankloop:
        ld (hl),a
        djnz bankloop
        ld a,(0x8100)
        ld (0x1FF1),a
        ld a,(0x4000)
        ld (0x1FF2),a
        ld hl,0x1FF0
        ld (hl),0
count:
        inc (hl)
        jr count
"""


def probe(start_z80):
    program = z80(PROBE_Z80)
    m = M68k()
    r = RESULT
    m.move_b(Z80_BUSREQ, r + 0)  # R0: BUSREQ high byte before any request (bit 0: bus NOT granted)
    m.move_w_imm(0x0100, Z80_BUSREQ)  # BUSREQ ...
    m.move_w_imm(0x0100, Z80_RESET)  # ... and RESET released (the Z80 is held in reset at power-on; the grant needs /RESET high)
    m.label("ack1")
    m.btst0(Z80_BUSREQ)
    m.branch(0x66, "ack1")
    m.move_b(Z80_BUSREQ, r + 1)  # R1: after the grant (bit 0 clear)
    m.copy_to_z80("zprog", len(program), "cp1")
    m.move_b(Z80_RAM + 0, r + 2)  # R2: first uploaded byte read back through the 68K window
    if start_z80:
        m.move_w_imm(0x0000, Z80_RESET)  # reset asserted while BUSREQ is still requested
        m.move_b(Z80_BUSREQ, r + 10)  # R10: bit 0 set again - BUSREQ is not acknowledged while /RESET is low
        m.release_bus()  # BUSREQ cancelled while the Z80 is held in reset ...
        m.move_w_imm(0x0100, Z80_RESET)  # ... then /RESET released: the Z80 becomes runnable at PC 0
        m.delay(4000, "d1")
        m.acquire_bus("ack2")
    m.move_b(Z80_RAM + 0x1FF0, r + 3)  # R3: counter sample 1
    m.move_b(Z80_RAM + 0x1FF1, r + 4)  # R4: ROM byte read by the Z80 through the bank window (bank 0, $8100)
    m.move_b(Z80_RAM + 0x1FF2, r + 5)  # R5: YM2612 status read by the Z80 ($4000)
    m.move_b(Z80_RAM + 0x1FF0, r + 6)  # R6: counter sample 2 while the bus is held (equals R3)
    m.move_b_imm(0x00, Z80_RAM + 0x1FF1)  # clear the marker the program writes only at its start
    m.move_w_imm(0x0000, Z80_RESET)  # reset asserted while the 68K holds the bus
    m.move_w_imm(0x0100, Z80_RESET)  # reset released while the 68K holds the bus
    m.move_b(Z80_RAM + 0x1FF1, r + 7)  # R7: marker still clear (the Z80 has not run yet)
    m.release_bus()
    m.delay(4000, "d2")
    m.acquire_bus("ack3")
    m.move_b(Z80_RAM + 0x1FF1, r + 8)  # R8: marker rewritten (the program restarted from PC 0)
    m.move_b(Z80_RAM + 0x1FF0, r + 9)  # R9: counter after the restart
    m.move_b_imm(0xA5, DONE)
    m.label("spin")
    m.branch(0x60, "spin")
    m.data("zprog", program)
    return build_rom(m.resolve())


def fixture_bus_reset_probe():
    return probe(True)


def fixture_bus_reset_control():
    """Negative control: the first run sequence (reset assert, BUSREQ cancel, reset release) is omitted, so the Z80 does not run before the first sample."""
    return probe(False)


FIXTURES = {"bus_reset_probe": fixture_bus_reset_probe, "bus_reset_control": fixture_bus_reset_control}


def build(name):
    return FIXTURES[name]()


def manifest_entry(name):
    rom = build(name)
    return {"size": len(rom), "sha256": hashlib.sha256(rom).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out")
    parser.add_argument("--fixture", action="append")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--write-manifest", action="store_true")
    args = parser.parse_args()
    names = args.fixture or sorted(FIXTURES)
    if args.write_manifest:
        data = {"schema": SCHEMA, "fixtures": {n: manifest_entry(n) for n in sorted(FIXTURES)}}
        MANIFEST.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")
        return 0
    if args.check:
        data = json.loads(MANIFEST.read_text())
        bad = [n for n in sorted(FIXTURES) if data["fixtures"].get(n) != manifest_entry(n)]
        bad += [n for n in data["fixtures"] if n not in FIXTURES]
        if bad:
            sys.stderr.write("fixture manifest mismatch: %s\n" % ", ".join(bad))
            return 1
        return 0
    if args.out:
        out = pathlib.Path(args.out)
        out.mkdir(parents=True, exist_ok=True)
        for n in names:
            (out / (n + ".md")).write_bytes(build(n))
        return 0
    parser.error("one of --out, --check, --write-manifest is required")


if __name__ == "__main__":
    sys.exit(main())
