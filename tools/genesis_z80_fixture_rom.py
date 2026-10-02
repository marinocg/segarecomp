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

    def copy_to_z80(self, program_label, length, loop, destination=Z80_RAM):
        self.w(0x41FA)  # LEA d16(PC),A0
        self.fixups.append((len(self.words), program_label + ":pcrel"))
        self.w(0)
        self.w(0x43F9)
        self.l(destination)  # LEA abs.L,A1
        self.w(0x303C, length - 1)  # MOVE.W #n-1,D0
        self.label(loop)
        self.w(0x12D8)  # MOVE.B (A0)+,(A1)+
        self.w(0x51C8)  # DBRA D0
        self.fixups.append((len(self.words), loop + ":dbra"))
        self.w(0)

    def copy_decoded_to_z80(self, program_label, length, loop, key, destination=Z80_RAM):
        """The 'decompressed upload' shape: the 68K computes every byte it stores (here XOR with `key`), it never copies a stored image."""
        self.w(0x41FA)  # LEA d16(PC),A0
        self.fixups.append((len(self.words), program_label + ":pcrel"))
        self.w(0)
        self.w(0x43F9)
        self.l(destination)  # LEA abs.L,A1
        self.w(0x303C, length - 1)  # MOVE.W #n-1,D0
        self.label(loop)
        self.w(0x1418)  # MOVE.B (A0)+,D2
        self.w(0x0A02, key & 0xFF)  # EORI.B #key,D2
        self.w(0x12C2)  # MOVE.B D2,(A1)+
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



# --- fixture: multi-epoch upload with dirty carry-over data (SEG-032-T002, activation-signature falsifier) -----------------

DIRTY_BASE = 0x1F00  # a mailbox/variable area outside every uploaded image


def multi_epoch_programs():
    """Three project-authored 'drivers' (opaque Z80 byte strings; the T002 seam never executes them): X, X' (one code byte
    changed) and Y (different code, same length)."""
    x = bytes((i * 7 + 3) & 0xFF for i in range(96))
    x2 = bytearray(x)
    x2[40] ^= 0x55
    y = bytes((i * 13 + 5) & 0xFF for i in range(96))
    return x, bytes(x2), y


def multi_epoch_ops():
    """The multi-epoch dirty-data program as backend-neutral operations, rendered as 68K code (`fixture_multi_epoch_dirty`)
    and as a raw 68K access script for the runtime-level harness (`access_script`). Epochs (each: BUSREQ, upload, /RESET pulse,
    BUSREQ cancel, /RESET release), with 68K 'command' writes into the mailbox area during separate holds between them:
      1: X over a clean mailbox        2: X again, mailbox dirty (A)     3: Y (different code), mailbox dirty (A)
      4: X again, mailbox dirty (B)    5: X' (one code byte changed), mailbox dirty (B)
      6: a plain Z80 restart: reset pulse with no upload at all (the Z80 runs the code already in RAM)"""
    ops = []

    def epoch(program):
        ops.extend([("busreq", 1), ("reset", 1), ("wait_ack",), ("upload", program), ("reset", 0), ("busreq", 0),
                    ("reset", 1)])  # the last write is the runnable transition: an epoch

    def dirty(value):
        ops.extend([("busreq", 1), ("wait_ack",)])  # a separate hold: the Z80 ran in between
        ops.extend(("write", DIRTY_BASE + i, value + i) for i in range(4))
        ops.append(("busreq", 0))

    epoch("x")
    dirty(0xA0)
    epoch("x")
    epoch("y")
    dirty(0xB0)
    epoch("x")
    epoch("x2")
    ops.extend([("busreq", 1), ("reset", 1), ("wait_ack",), ("reset", 0), ("busreq", 0), ("reset", 1)])  # epoch 6
    return ops


def fixture_multi_epoch_dirty():
    programs = dict(zip(("x", "x2", "y"), multi_epoch_programs()))
    m = M68k()
    waits = 0
    for op in multi_epoch_ops():
        if op[0] == "busreq":
            m.move_w_imm(0x0100 if op[1] else 0x0000, Z80_BUSREQ)
        elif op[0] == "reset":
            m.move_w_imm(0x0100 if op[1] else 0x0000, Z80_RESET)
        elif op[0] == "wait_ack":
            waits += 1
            m.label("ack_%d" % waits)
            m.btst0(Z80_BUSREQ)
            m.branch(0x66, "ack_%d" % waits)
        elif op[0] == "upload":
            m.copy_to_z80("prog_" + op[1], len(programs[op[1]]), "cp_%d" % len(m.words))
        elif op[0] == "write":
            m.move_b_imm(op[2], Z80_RAM + op[1])
    m.move_b_imm(0xA5, DONE)
    m.label("spin")
    m.branch(0x60, "spin")
    for name in sorted(programs):
        m.data("prog_" + name, programs[name])
    return build_rom(m.resolve())


def access_script(ops, programs):
    """Renders backend-neutral operations as the raw 68K access stream the runtime-level harness replays through
    genesis_route_access: `w16 <addr> <value>` / `w8 <addr> <value>` (hex)."""
    lines = []
    for op in ops:
        if op[0] == "busreq":
            lines.append("w16 %06X %04X" % (Z80_BUSREQ, 0x0100 if op[1] else 0))
        elif op[0] == "reset":
            lines.append("w16 %06X %04X" % (Z80_RESET, 0x0100 if op[1] else 0))
        elif op[0] == "upload":
            lines.extend("w8 %06X %02X" % (Z80_RAM + i, b) for i, b in enumerate(programs[op[1]]))
        elif op[0] == "write":
            lines.append("w8 %06X %02X" % (Z80_RAM + op[1], op[2]))
    return "\n".join(lines) + "\n"


# --- fixtures: generated-native Z80 sound programs for the build-time materialization pass (SEG-032-T008) -------------------
# The 68K program touches only the Z80 control registers and Z80 RAM (no work-RAM operands: the bridge's static-fact admission
# takes long-word work-RAM moves only) and ends in a spin. Every Z80 driver is a valid program that drives the PSG and the YM2612
# and then loops; `variant` makes distinct images (a different register value and length).

def sound_driver(variant):
    lines = [".org 0x0000", "        ld sp,0x1F00", "        ld a,%d" % (0x90 | (variant & 0x0F)), "        ld (0x7F11),a",
             "        ld a,%d" % (0x20 + variant), "        ld (0x7F11),a"]
    for n, (port, value) in enumerate(((0, 0x2B), (1, 0x80), (0, 0x2A), (1, 0x40 + variant), (0, 0x2A), (1, 0x80 + variant))):
        lines += ["w%d:     ld a,(0x4000)" % n, "        add a,a", "        jr c,w%d" % n, "        ld a,%d" % value, "        ld (0x%04X),a" % (0x4000 + port)]
    lines += ["        nop"] * variant
    lines += ["loop:   jr loop"]
    return z80("\n".join(lines) + "\n")


def sound_tone_driver():
    """SEG-032-T009: a Z80 driver that plays all three sources and then loops: a PSG tone (two voices), a YM2612 FM tone (algorithm 7,
    left channel only, key on) and DAC samples. Project-authored; every YM2612 write polls the busy flag first."""
    ym = [(0, 0xB0), (1, 0x07), (0, 0xB4), (1, 0x80)]
    for off in (0x00, 0x04, 0x08, 0x0C):
        ym += [(0, 0x30 + off), (1, 1), (0, 0x40 + off), (1, 0x20), (0, 0x50 + off), (1, 0x1F), (0, 0x60 + off), (1, 0),
               (0, 0x70 + off), (1, 0), (0, 0x80 + off), (1, 0x0F)]
    ym += [(0, 0xA4), (1, 0x22), (0, 0xA0), (1, 0x69), (0, 0x28), (1, 0xF0), (0, 0x2B), (1, 0x80)]
    for i in range(40):
        ym += [(0, 0x2A), (1, (i * 53 + 7) & 0xFF)]
    lines = [".org 0x0000", "        ld sp,0x1F00"]
    for value in (0x8A, 0x0F, 0x90, 0xA5, 0x08, 0xB2):
        lines += ["        ld a,%d" % value, "        ld (0x7F11),a"]
    for n, (port, value) in enumerate(ym):
        lines += ["w%d:     ld a,(0x4000)" % n, "        add a,a", "        jr c,w%d" % n, "        ld a,%d" % value, "        ld (0x%04X),a" % (0x4000 + port)]
    lines += ["loop:   jr loop"]
    return z80("\n".join(lines) + "\n")


def sound_smc_driver():
    """A Z80 program that overwrites the byte of its own next instruction: outside every 68K-written extent's identity, so the
    RAM-backed code guard (z80_code_mismatch), not the signature, must catch it."""
    return z80(".org 0x0000\n        ld sp,0x1F00\n        ld a,0x3C\n        ld (patch),a\npatch:  nop\nloop:   jr loop\n")


def sound_smc_psg_driver():
    """One PSG write, then a structural self-modification of the next instruction (z80_code_mismatch), then PSG writes that must never
    be executed: the isolated sound CPU performs no write after the fault."""
    return z80(".org 0x0000\n        ld sp,0x1F00\n        ld a,0x9F\n        ld (0x7F11),a\n        ld a,0x3C\n        ld (patch),a\n"
               "patch:  nop\n        ld a,0x8A\n        ld (0x7F11),a\nloop:   jr loop\n")


def build_sound(plan, programs):
    m = M68k()
    # Prologue: MOVEQ #0,D0 / BEQ.W real / RESET. The 68K translator's startup prefix admits only a handful of operations and
    # takes over the whole flow when no instruction it cannot model is reachable; the (never executed) RESET fallthrough is such an
    # instruction, so everything from `real` on is compiled by the immutable-ROM AOT like any commercial program.
    m.w(0x7000)
    m.branch(0x67, "real")
    m.w(0x4E70)
    m.label("real")
    waits = 0
    uploads = {}
    for kind, arg in plan:
        if kind == "delay":
            m.delay(arg, "delay_%d" % len(m.words))
            continue
        m.move_w_imm(0x0100, Z80_BUSREQ)
        m.move_w_imm(0x0100, Z80_RESET)
        waits += 1
        m.label("ack_%d" % waits)
        m.btst0(Z80_BUSREQ)
        m.branch(0x66, "ack_%d" % waits)
        if kind in ("raw", "decoded"):
            program = programs[arg]
            uploads[arg] = program
            if kind == "raw":
                m.copy_to_z80("prog_" + arg, len(program), "cp_%d" % len(m.words))
            else:
                m.copy_decoded_to_z80("enc_" + arg, len(program), "dec_%d" % len(m.words), 0x5A)
        m.move_w_imm(0x0000, Z80_RESET)
        m.move_w_imm(0x0000, Z80_BUSREQ)
        m.move_w_imm(0x0100, Z80_RESET)
    m.label("spin")
    m.branch(0x60, "spin")
    for name in sorted(uploads):
        kinds = {k for k, a in plan if a == name}
        if "raw" in kinds:
            m.data("prog_" + name, uploads[name])
        if "decoded" in kinds:
            m.data("enc_" + name, bytes(b ^ 0x5A for b in uploads[name]))
    return build_rom(m.resolve())


MAX_IMAGES_FOR_TESTS = 8  # the production image bound (platforms/genesis/machine z80_images.hpp kMaxImages); the bound test uses it and +1

SOUND_DELAY = 20000  # 68K iterations between epochs (about 280k M68K cycles, a few frames)


def fixture_sound_raw():
    return build_sound([("raw", "a")], {"a": sound_driver(1)})


def fixture_sound_multi_epoch():
    """A, B, a plain restart, A again (the same activation signature as the first epoch): 4 epochs, exactly 2 images."""
    return build_sound([("raw", "a"), ("delay", SOUND_DELAY), ("raw", "b"), ("delay", SOUND_DELAY), ("restart", None),
                        ("delay", SOUND_DELAY), ("raw", "a")], {"a": sound_driver(1), "b": sound_driver(2)})


def fixture_sound_tone():
    """SEG-032-T009: one image that plays a PSG tone, an FM tone and DAC samples (the deterministic audio artifact fixture)."""
    return build_sound([("raw", "t")], {"t": sound_tone_driver()})


def fixture_sound_decoded():
    """A raw upload and a computed (XOR-decoded) upload of the same driver bytes: identical activation signature, one image."""
    return build_sound([("raw", "a"), ("delay", SOUND_DELAY), ("decoded", "a")], {"a": sound_driver(3)})


def sound_many(count):
    """`count` distinct drivers uploaded in turn: exactly `count` images (the bound test uses bound and bound + 1)."""
    plan = []
    programs = {}
    for k in range(count):
        plan += [("raw", "d%d" % k), ("delay", 4000)]
        programs["d%d" % k] = sound_driver(k + 1)
    return build_sound(plan[:-1], programs)


def fixture_sound_smc():
    return build_sound([("raw", "s")], {"s": sound_smc_driver()})


def fixture_sound_smc_fault():
    """A structural mutation faults the sound CPU in the first epoch; a later, different (known-shaped) driver is uploaded and must neither
    run nor stop the machine: 1 image, 2 epochs, the 68K keeps running."""
    return build_sound([("raw", "s"), ("delay", SOUND_DELAY), ("raw", "a")], {"s": sound_smc_psg_driver(), "a": sound_driver(1)})


def fixture_sound_late_epoch():
    """A second, different driver is uploaded only after the observation window: the build sees one image, the second epoch
    surfaces at run time as the typed z80_unknown_image."""
    return build_sound([("raw", "a"), ("delay", 0x600000), ("raw", "b")], {"a": sound_driver(1), "b": sound_driver(2)})


FIXTURES = {"multi_epoch_dirty": fixture_multi_epoch_dirty, "bus_reset_probe": fixture_bus_reset_probe, "bus_reset_control": fixture_bus_reset_control,
            "sound_raw": fixture_sound_raw, "sound_multi_epoch": fixture_sound_multi_epoch, "sound_decoded": fixture_sound_decoded,
            "sound_smc": fixture_sound_smc, "sound_smc_fault": fixture_sound_smc_fault, "sound_tone": fixture_sound_tone, "sound_late_epoch": fixture_sound_late_epoch}


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
