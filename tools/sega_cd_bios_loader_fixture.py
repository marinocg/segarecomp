#!/usr/bin/env python3
"""Project-authored BIOS-like image + disc for the SEG-078-T003 extent-discovery matrix (ADR 0205). Stdlib only.

No BIOS or commercial byte is read or reproduced. The "BIOS" releases a small sub program; the sub program reads modules from the fixture
disc through the CDD/CDC (decoder, DMA into the 2M Word RAM, exactly the path the cdframe fixture exercises), then runs, corrupts, decodes or
generates code according to the case. The disc is project-authored: a data track whose blocks 40.. carry the modules and filler.

Cases (each builds its own image and disc):
  direct       one module (2 blocks) is DMA'd to sub Word RAM $080000 and run
  siblings     modules P and Q (adjacent on the disc, Q shares P's first 1,500 bytes) are loaded to $080000 / $081000; Q is run
  duplicates   two identical copies of one module at disjoint disc spans are both loaded ($080000 and $0A0000); the first is run
  overlay      module M1 is loaded to $080000 and run, then module M2 is loaded over it and run (two images at one address)
  late_flip    a module is loaded, one byte far inside its code is flipped, then it is run (the prefix is a copy, the flipped instruction is not)
  generated    the sub writes instructions into PRG RAM with immediate moves and runs them (no disc span)
  transformed  an XOR-encoded module is loaded and decoded in place before it runs (the live bytes are not a disc span)
  undelivered  the sub image carries bytes identical to a disc module that is never read; they are copied to PRG RAM and run

usage: sega_cd_bios_loader_fixture.py build --case NAME --out DIR   ->   DIR/bios.bin, DIR/disc/{disc.cue,track01.bin}, DIR/expected.json
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

ROM_SIZE = 0x4000
MAIN = 0x0200
FAULT = 0x0C00
SUB_SRC = 0x1000
RESULT = 0xFFC000
GA = 0xA12000
GA_SUB = 0xFF8000
WINDOW = 0x020000
SUB_SSP = 0x2000
SUB_ENTRY = 0x0100
WORD_RAM_SUB = 0x080000
TRACK1_SECTORS = 160
FIRST_MODULE_LBA = 40
BLOB_AT = 0x1000
CASES = ("direct", "siblings", "duplicates", "overlay", "late_flip", "generated", "transformed", "undelivered")
MARK = GA_SUB + 0x24        # COMSTAT2: written by the modules
MARK2 = GA_SUB + 0x26       # COMSTAT3


def module_bytes(base, mark_addr, mark, nops, size, seed, flip_after=None):
    """Code at `base`: write a marker word, `nops` NOPs, RTS; then seeded filler words up to `size` bytes (data the program never runs)."""
    m = F.Asm(base)
    m.move("w", F.imm(mark), F.absl(mark_addr))
    for _ in range(nops):
        m.nop()
    m.rts()
    code = m.assemble()
    rng = random.Random(seed)
    return code + bytes(rng.randrange(256) for _ in range(size - len(code)))


class Sub:
    """The sub program: vectors at 0, the entry at $100, then the case's steps."""

    def __init__(self):
        self.s = F.Asm(0)
        self.s.lw(SUB_SSP)
        self.s.lw(SUB_ENTRY)
        while self.s.pc < SUB_ENTRY:
            self.s.w(0)
        s = self.s
        s.move("w", F.imm(0xC0DE), F.absl(GA_SUB + 0x20))
        s.move("b", F.imm(0x01), F.absl(GA_SUB + 0x0F))
        F.wait_flag(s, GA_SUB + 0x0E, 1, "w_go")
        s.label("w_own")
        s.btst(0, F.absl(GA_SUB + 0x03))
        s.bcc("ne", "w_own")
        self.n = 0
        self.cdc_ready = False

    def cdc_write(self, reg, value):
        self.s.move("b", F.imm(reg), F.absl(GA_SUB + 0x05))
        self.s.move("b", F.imm(value), F.absl(GA_SUB + 0x07))

    def load(self, first_lba, count, dest):
        """Read `count` consecutive blocks from `first_lba` through the CDD/CDC, DMA'd to sub Word RAM offset `dest` (bytes, a multiple of 8)."""
        s = self.s
        p = "L%d_" % self.n
        self.n += 1
        if not self.cdc_ready:
            self.cdc_write(0x0F, 0x00)
            self.cdc_write(0x01, 0x02)
            self.cdc_write(0x0A, 0x84)
            s.move("b", F.imm(0x00), F.absl(GA_SUB + 0x07))
            self.cdc_ready = True
        play, _ = F.cdd_words(3, first_lba)
        for i, word in enumerate(play):
            s.move("w", F.imm(word), F.absl(GA_SUB + 0x42 + 2 * i))
        s.lea(F.absl(GA_SUB + 0x20), 2)
        for k in range(count):
            msf = first_lba + k + 150
            want = [(F.bcd(msf // 75 // 60) << 8) | F.bcd(msf // 75 % 60), (F.bcd(msf % 75) << 8) | 0x01]
            s.label(p + "valid%d" % k)
            s.move("b", F.imm(0x0F), F.absl(GA_SUB + 0x05))
            s.move("b", F.absl(GA_SUB + 0x07), F.d(0))
            s.btst(7, F.d(0))
            s.bcc("ne", p + "valid%d" % k)
            s.move("b", F.imm(0x04), F.absl(GA_SUB + 0x05))
            for reg in (0, 5):
                s.move("b", F.absl(GA_SUB + 0x07), F.d(reg))
                s.lsl_w(8, reg)
                s.move("b", F.absl(GA_SUB + 0x07), F.d(reg))
            s.immop("cmp", "w", want[0], F.d(0))
            s.bcc("ne", p + "valid%d" % k)
            s.immop("cmp", "w", want[1], F.d(5))
            s.bcc("ne", p + "valid%d" % k)
            s.moveq(0, 1)
            s.move("b", F.absl(GA_SUB + 0x07), F.d(1))
            s.moveq(0, 2)
            s.move("b", F.absl(GA_SUB + 0x07), F.d(2))
            s.lsl_w(8, 2)
            s.alu("or", "w", F.d(1), 2)
            s.quick("add", "w", 4, F.d(2))
            s.move("b", F.imm(7), F.absl(GA_SUB + 0x04))
            s.move("w", F.imm((dest + k * 2048) >> 3), F.absl(GA_SUB + 0x0A))
            s.move("b", F.imm(0x02), F.absl(GA_SUB + 0x05))
            s.move("b", F.imm(0xFF), F.absl(GA_SUB + 0x07))
            s.move("b", F.imm(0x07), F.absl(GA_SUB + 0x07))
            s.move("b", F.d(2), F.absl(GA_SUB + 0x07))
            s.move("w", F.d(2), F.d(3))
            s.lsr_w(8, 3)
            s.move("b", F.d(3), F.absl(GA_SUB + 0x07))
            s.move("b", F.imm(0x00), F.absl(GA_SUB + 0x07))
            s.label(p + "edt%d" % k)
            s.btst(7, F.absl(GA_SUB + 0x04))
            s.bcc("eq", p + "edt%d" % k)

    def run(self, address):
        self.s.jsr(F.absl(address))

    def flip(self, address, value):
        self.s.move("b", F.imm(value), F.absl(address))

    def decode(self, address, length, key):
        s = self.s
        p = "D%d_" % self.n
        self.n += 1
        s.move("l", F.imm(address), F.a(0))
        s.move("w", F.imm(length - 1), F.d(1))
        s.label(p + "x")
        s.move("b", ("ind", 0), F.d(2))
        s.immop("eor", "b", key, F.d(2))
        s.move("b", F.d(2), ("inc", 0))
        s.dbra(1, p + "x")

    def generate(self, address, words):
        for i, word in enumerate(words):
            self.s.move("w", F.imm(word), F.absl(address + 2 * i))

    def copy_blob(self, source, destination, length):
        s = self.s
        p = "C%d_" % self.n
        self.n += 1
        s.move("l", F.imm(source), F.a(0))
        s.move("l", F.imm(destination), F.a(1))
        s.move("w", F.imm(length // 2 - 1), F.d(1))
        s.label(p + "c")
        s.move("w", ("inc", 0), ("inc", 1))
        s.dbra(1, p + "c")

    def finish(self):
        s = self.s
        s.move("b", F.imm(0x01), F.absl(GA_SUB + 0x03))   # RET = 1: Word RAM back to main
        s.move("b", F.imm(0x02), F.absl(GA_SUB + 0x0F))
        s.stop(0x2700)
        return s.assemble()


def main_program(sub_len):
    m = F.Asm(MAIN)
    m.move("l", F.imm(SUB_SRC), F.a(0))
    m.move("l", F.imm(WINDOW), F.a(1))
    m.move("w", F.imm(sub_len // 2 - 1), F.d(0))
    m.label("copy_sub")
    m.move("w", ("inc", 0), ("inc", 1))
    m.dbra(0, "copy_sub")
    m.move("b", F.imm(0x01), F.absl(GA + 0x01))          # SRES = 1, SBRQ = 0: the sub's runnable epoch
    F.wait_flag(m, GA + 0x0F, 0x01, "w_ready")
    m.move("b", F.imm(0x02), F.absl(GA + 0x03))          # DMNA: Word RAM to the sub
    m.move("b", F.imm(0x01), F.absl(GA + 0x0E))          # go
    F.wait_flag(m, GA + 0x0F, 0x02, "w_done")
    for i in range(3):
        m.move("w", F.absl(GA + 0x24 + 2 * i), F.absl(RESULT + 0x10 + 2 * i))
    m.move("b", F.imm(0xA5), F.absl(RESULT + 0xFF))
    m.stop(0x2700)
    code = m.assemble()
    assert MAIN + len(code) <= FAULT, hex(MAIN + len(code))
    return code


def plan(case):
    """(sub steps builder, disc blocks {lba: bytes}, expected ground truth for the tests) of one case. Ground truth is never an input of a build."""
    blocks = {}
    truth = {"extents": [], "markers": []}

    def put(lba, data):
        assert len(data) % 2048 == 0
        for k in range(len(data) // 2048):
            blocks[lba + k] = data[2048 * k:2048 * (k + 1)]

    sub = Sub()
    base = WORD_RAM_SUB
    L = FIRST_MODULE_LBA
    if case == "direct":
        data = module_bytes(base, MARK, 0xA001, 24, 4096, 1)
        put(L, data)
        sub.load(L, 2, 0)
        sub.run(base)
        truth["extents"].append({"processor": "sub", "offset": L * 2048, "base": base, "length": 4096})
        truth["markers"] = [0xA001, 0, 0]
    elif case == "siblings":
        p = module_bytes(base, MARK, 0xA001, 24, 4096, 2)
        q = bytearray(module_bytes(base + 0x1000, MARK2, 0xB002, 24, 4096, 2))
        q[1500:] = bytes(random.Random(99).randrange(256) for _ in range(len(q) - 1500))   # Q shares P's first 1,500 bytes (filler) and then differs
        # same filler stream: Q's code differs from P's (other base and marker), so make the shared prefix real: copy P's bytes 0..1499 into Q after Q's own code
        q[:1500] = bytes(q[:96]) + p[96:1500]
        put(L, p + bytes(q))
        sub.load(L, 4, 0)
        sub.run(base + 0x1000)
        truth["extents"].append({"processor": "sub", "offset": L * 2048, "base": base, "length": 8192})
        truth["markers"] = [0, 0xB002, 0]
    elif case == "duplicates":
        data = module_bytes(base, MARK, 0xA001, 24, 4096, 3)
        put(L, data)
        put(L + 6, data)
        sub.load(L, 2, 0)
        sub.load(L + 6, 2, 0x20000)
        sub.run(base)
        truth["markers"] = [0xA001, 0, 0]
    elif case == "overlay":
        m1 = module_bytes(base, MARK, 0xA001, 24, 4096, 4)
        m2 = module_bytes(base, MARK2, 0xB002, 40, 4096, 5)
        put(L, m1)
        put(L + 8, m2)
        sub.load(L, 2, 0)
        sub.run(base)
        sub.load(L + 8, 2, 0)
        sub.run(base)
        truth["extents"] = [{"processor": "sub", "offset": L * 2048, "base": base, "length": 4096},
                            {"processor": "sub", "offset": (L + 8) * 2048, "base": base, "length": 4096}]
        truth["markers"] = [0xA001, 0xB002, 0]
    elif case == "late_flip":
        data = module_bytes(base, MARK, 0xA001, 120, 4096, 6)
        put(L, data)
        sub.load(L, 2, 0)
        sub.flip(base + 8 + 2 * 100, 0x5A)       # inside the 101st NOP (an instruction the pass reaches later), far from the first 32 bytes
        sub.run(base)
        truth["markers"] = [0xA001, 0, 0]
        truth["flip_offset"] = 8 + 2 * 100
    elif case == "generated":
        put(L, bytes(random.Random(7).randrange(256) for _ in range(4096)))
        sub.load(L, 2, 0)          # the ledger is not empty: something was read, none of it is the generated code
        words = []
        for i in range(24):
            words += [0x303C, 0x1000 + 0x111 * i]      # move.w #imm,d0
        words += [0x33FC, 0xA001, (MARK >> 16) & 0xFFFF, MARK & 0xFFFF, 0x4E75]
        sub.generate(0x030000, words)
        sub.run(0x030000)
        truth["markers"] = [0xA001, 0, 0]
    elif case == "transformed":
        data = module_bytes(base, MARK, 0xA001, 24, 4096, 8)
        put(L, bytes(b ^ 0x5A for b in data))
        sub.load(L, 2, 0)
        sub.decode(base, 4096, 0x5A)
        sub.run(base)
        truth["markers"] = [0xA001, 0, 0]
    elif case == "undelivered":
        data = module_bytes(0x030000, MARK, 0xA001, 24, 2048, 9)
        put(L + 10, data)                         # on the disc, never read
        put(L, bytes(random.Random(11).randrange(256) for _ in range(2048)))
        sub.load(L, 1, 0)                         # something else is read
        truth["blob"] = data
        sub.copy_blob(BLOB_AT, 0x030000, 2048)    # the blob rides in the sub image at a fixed place
        sub.run(0x030000)
        truth["markers"] = [0xA001, 0, 0]
    else:
        raise SystemExit("unknown case " + case)
    return sub, blocks, truth


def build_disc(out, blocks):
    area, _ = F.system_area(bytes(64), bytes(64), "LOADERFIX")
    users = []
    rng = random.Random(0x7003)
    for lba in range(TRACK1_SECTORS):
        if lba < 16:
            users.append(area[lba * 2048:(lba + 1) * 2048])
        elif lba in blocks:
            users.append(blocks[lba])
        else:
            users.append(bytes(rng.randrange(256) for _ in range(2048)))
    out.mkdir(parents=True, exist_ok=True)
    (out / "track01.bin").write_bytes(b"".join(users))
    (out / "disc.cue").write_text('FILE "track01.bin" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n', newline="\n")


def build(case, out):
    sub, blocks, truth = plan(case)
    blob = truth.pop("blob", None)
    code = sub.finish()
    if blob is not None:                            # the undelivered case: the blob rides in the sub image at BLOB_AT
        assert len(code) <= BLOB_AT, hex(len(code))
        code = code.ljust(BLOB_AT, b"\0") + blob
    sub_len = (len(code) + 1) & ~1
    rom = bytearray(ROM_SIZE)
    struct.pack_into(">II", rom, 0, 0x00FFFE00, MAIN)
    for v in range(2, 64):
        struct.pack_into(">I", rom, 4 * v, FAULT)
    rom[0x100:0x120] = b"SEGA BIOS-IMAGE LOADER FIXTURE  "
    main = main_program(sub_len)
    rom[MAIN:MAIN + len(main)] = main
    fault = F.Asm(FAULT)
    fault.move("w", F.imm(0xDEAD), F.absl(RESULT + 0xF0))
    fault.stop(0x2700)
    rom[FAULT:FAULT + len(fault.assemble())] = fault.assemble()
    assert SUB_SRC + sub_len <= ROM_SIZE, hex(SUB_SRC + sub_len)
    rom[SUB_SRC:SUB_SRC + len(code)] = code
    out.mkdir(parents=True, exist_ok=True)
    (out / "bios.bin").write_bytes(bytes(rom))
    build_disc(out / "disc", blocks)
    truth.update({"case": case, "rom_sha256": hashlib.sha256(bytes(rom)).hexdigest(), "sub_length": sub_len, "track1_sectors": TRACK1_SECTORS})
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
