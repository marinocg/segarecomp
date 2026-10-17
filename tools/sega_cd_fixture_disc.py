#!/usr/bin/env python3
"""Deterministic synthetic Sega CD fixture disc builder (SEG-062-T002, ADR 0129).

Stdlib only. Every byte is project-authored: hand-encoded MC68000 programs (the tiny encoder below), a system-area header in the
documented layout (docs/architecture/sega-cd-machine-contract.md section 9), seeded data sectors and an optional audio track. No
BIOS, security code or commercial byte is read or reproduced. Images are generated at test time and never committed; the committed
manifest tests/fixtures/sega-cd-fixture-discs.json records every generated file's size and SHA-256, so `--check` proves
byte-for-byte reproducibility on every host.

Fixtures (the main program is the IP, mapped at $FF0000; the sub program is the SP, mapped at PRG RAM $6000):
  comm        T003  main writes sub vectors + a parameter through the PRG-RAM window (banks 0 and 1), releases the sub, posts a
                    command through COMCMD and IFL2; the sub's INT2 handler computes a response into COMSTAT
  wordram2m   T004  sub fills 2M Word RAM after DMNA, returns it with RET; main checks the pattern
  wordram1m   T004  1M mode: both CPUs use their bank at once, the sub swaps banks with RET
  timer       T004  sub counts five timer (INT3) interrupts, then stops the timer
  subside     SEG-064-T004  the stopwatch on the CPU (hand-derived cycle counts, rollover) and the level-1 (graphics completion) interrupt:
                    disabled, accepted, masked, and simultaneous with level 2
  cdframe     T005  sub reads two data sectors through CDD/CDC and DMAs them into Word RAM; main programs the VDP from them
  modules     SEG-063 Checkpoint D: an ISO9660 volume with module files; the sub loads module A by CDC DMA into Word RAM and runs
                    it at $080000, copies it to PRG RAM $020000 and runs it there, then does the same with module B at the same
                    addresses (--module-case selects a negative loader; --iso-defect corrupts one ISO9660 structure)
  wordram_wait T004  the sub touches 2M Word RAM before main hands it over: it waits for DTACK and retries (GPGX)
  wordram_violation, neg_*   negative fixtures (typed stops or defined GPGX results)
Variants: `cooked` (MODE1/2048) and `raw` (MODE1/2352 with sync and header); options alter the cdframe sub program for
runtime tests (CDD checksum, CTRL1 MODRQ, LBA past the disc / in the audio track, unsupported DMA destination, DMA abort) and one
data byte (liveness).

Result block convention: the main program reports into work RAM at $FFC000 and writes $A5 at $FFC0FF last.

usage:
  sega_cd_fixture_disc.py build --fixture NAME --out DIR [--variant cooked|raw] [--seed N] [--data-flip OFFSET]
                                [--cd-lba LBA] [--dma-dest N] [--bad-checksum] [--abort-dma] [--bad-cdc-register] [--no-audio]
                                [--module-case CASE] [--iso-defect DEFECT]
  sega_cd_fixture_disc.py describe --fixture NAME [options]   JSON: data LBA, expected values (used by tests)
  sega_cd_fixture_disc.py --check | --write-manifest
"""
import argparse
import hashlib
import json
import pathlib
import random
import struct
import sys

HERE = pathlib.Path(__file__).resolve().parent
MANIFEST = HERE.parent / "tests" / "fixtures" / "sega-cd-fixture-discs.json"
SCHEMA = 1

IP_BASE = 0xFF0000
SP_BASE = 0x006000
RESULT = 0xFFC000
DONE = RESULT + 0xFF
SUB_SSP = 0x005F00
SUB_DATA = 0x010000
GA_MAIN = 0xA12000
GA_SUB = 0xFF8000
PRG_WINDOW = 0x020000
WORD_RAM_MAIN = 0x200000
WORD_RAM_SUB_2M = 0x080000
WORD_RAM_SUB_1M = 0x0C0000
VDP_DATA = 0xC00000
VDP_CTRL = 0xC00004

TRACK1_SECTORS = 40
DATA_LBA = 24  # two sectors: 24, 25
AUDIO_PREGAP = 150
AUDIO_SECTORS = 75

# ------------------------------------------------------------------------------------------------------------------------------
# A small MC68000 encoder: only the forms the fixtures use. EA syntax: ('d', n) Dn, ('a', n) An, ('ind', n) (An), ('inc', n) (An)+,
# ('dec', n) -(An), ('d16', disp, n) d16(An), ('absw', addr), ('absl', addr or label), ('imm', value).

SIZE_MOVE = {"b": 1, "w": 3, "l": 2}
SIZE_STD = {"b": 0, "w": 1, "l": 2}
COND = {"t": 0, "f": 1, "hi": 2, "ls": 3, "cc": 4, "cs": 5, "ne": 6, "eq": 7, "vc": 8, "vs": 9, "pl": 10, "mi": 11, "ge": 12,
        "lt": 13, "gt": 14, "le": 15}


class Asm:
    def __init__(self, base):
        self.base = base
        self.words = []
        self.labels = {}
        self.fixups = []  # (word index, label, kind)

    @property
    def pc(self):
        return self.base + 2 * len(self.words)

    def label(self, name):
        assert name not in self.labels, name
        self.labels[name] = self.pc

    def w(self, *values):
        for v in values:
            self.words.append(v & 0xFFFF)

    def lw(self, value):
        if isinstance(value, str):
            self.fixups.append((len(self.words), value, "abs32"))
            self.w(0, 0)
        else:
            self.w(value >> 16, value)

    # -- effective addresses
    @staticmethod
    def mode_reg(ea):
        kind = ea[0]
        if kind == "d": return 0, ea[1]
        if kind == "a": return 1, ea[1]
        if kind == "ind": return 2, ea[1]
        if kind == "inc": return 3, ea[1]
        if kind == "dec": return 4, ea[1]
        if kind == "d16": return 5, ea[2]
        if kind == "absw": return 7, 0
        if kind == "absl": return 7, 1
        if kind == "imm": return 7, 4
        raise ValueError(ea)

    def ext(self, ea, size):
        kind = ea[0]
        if kind == "d16":
            self.w(ea[1])
        elif kind == "absw":
            self.w(ea[1])
        elif kind == "absl":
            self.lw(ea[1])
        elif kind == "imm":
            if size == "l":
                self.lw(ea[1])
            else:
                self.w(ea[1] & (0xFF if size == "b" else 0xFFFF))

    def ea_bits(self, ea):
        m, r = self.mode_reg(ea)
        return (m << 3) | r

    # -- instructions
    def move(self, size, src, dst):
        sm, sr = self.mode_reg(src)
        dm, dr = self.mode_reg(dst)
        self.w((SIZE_MOVE[size] << 12) | (dr << 9) | (dm << 6) | (sm << 3) | sr)
        self.ext(src, size)
        self.ext(dst, size)

    def moveq(self, value, dn):
        self.w(0x7000 | (dn << 9) | (value & 0xFF))

    def lea(self, ea, an):
        self.w(0x41C0 | (an << 9) | self.ea_bits(ea))
        self.ext(ea, "l")

    def alu(self, op, size, src, dn):  # <ea>,Dn
        base = {"add": 0xD000, "sub": 0x9000, "and": 0xC000, "or": 0x8000, "cmp": 0xB000}[op]
        self.w(base | (dn << 9) | (SIZE_STD[size] << 6) | self.ea_bits(src))
        self.ext(src, size)

    def eor(self, size, dn, dst):  # Dn,<ea>
        self.w(0xB000 | (dn << 9) | ((4 + SIZE_STD[size]) << 6) | self.ea_bits(dst))
        self.ext(dst, size)

    def quick(self, op, size, value, dst):
        assert 1 <= value <= 8
        self.w(0x5000 | ((value & 7) << 9) | (0x100 if op == "sub" else 0) | (SIZE_STD[size] << 6) | self.ea_bits(dst))
        self.ext(dst, size)

    def immop(self, op, size, value, dst):
        base = {"or": 0x0000, "and": 0x0200, "sub": 0x0400, "add": 0x0600, "eor": 0x0A00, "cmp": 0x0C00}[op]
        self.w(base | (SIZE_STD[size] << 6) | self.ea_bits(dst))
        self.ext(("imm", value), size)
        self.ext(dst, size)

    def tst(self, size, ea):
        self.w(0x4A00 | (SIZE_STD[size] << 6) | self.ea_bits(ea))
        self.ext(ea, size)

    def clr(self, size, ea):
        self.w(0x4200 | (SIZE_STD[size] << 6) | self.ea_bits(ea))
        self.ext(ea, size)

    def btst(self, bit, ea):
        self.w(0x0800 | self.ea_bits(ea))
        self.w(bit)
        self.ext(ea, "b")

    def bcc(self, cond, target):
        self.w(0x6000 | (COND[cond] << 8))
        self.fixups.append((len(self.words), target, "rel16"))
        self.w(0)

    def bra(self, target):
        self.bcc("t", target)

    def bsr(self, target):
        self.w(0x6100)
        self.fixups.append((len(self.words), target, "rel16"))
        self.w(0)

    def dbra(self, dn, target):
        self.w(0x51C8 | dn)
        self.fixups.append((len(self.words), target, "rel16"))
        self.w(0)

    def lsl_w(self, count, dn):
        self.w(0xE000 | ((count & 7) << 9) | 0x100 | (1 << 6) | (1 << 3) | dn)

    def lsr_w(self, count, dn):
        self.w(0xE000 | ((count & 7) << 9) | (1 << 6) | (1 << 3) | dn)

    def mulu(self, src, dn):
        self.w(0xC0C0 | (dn << 9) | self.ea_bits(src))
        self.ext(src, "w")

    def move_sr(self, value):
        self.w(0x46FC, value)

    def stop(self, value):
        self.w(0x4E72, value)

    def rts(self):
        self.w(0x4E75)

    def rte(self):
        self.w(0x4E73)

    def nop(self):
        self.w(0x4E71)

    def jsr(self, ea):  # JSR (An) or JSR abs.L
        self.w(0x4E80 | self.ea_bits(ea))
        self.ext(ea, "l")

    def assemble(self):
        for index, label, kind in self.fixups:
            target = self.labels[label]
            if kind == "abs32":
                self.words[index] = (target >> 16) & 0xFFFF
                self.words[index + 1] = target & 0xFFFF
            else:
                disp = target - (self.base + 2 * index)
                assert -0x8000 <= disp < 0x8000, label
                self.words[index] = disp & 0xFFFF
        return b"".join(struct.pack(">H", w) for w in self.words)


def absl(addr):
    return ("absl", addr)


def imm(value):
    return ("imm", value)


def d(n):
    return ("d", n)


def a(n):
    return ("a", n)


# ------------------------------------------------------------------------------------------------------------------------------
# Shared program fragments


def main_start_sub(m, sub_entry, extra=None):
    """Write the sub reset vectors through the PRG-RAM window (sub is held in reset with the bus requested), then release it."""
    m.move("l", imm(SUB_SSP), absl(PRG_WINDOW + 0))
    m.move("l", imm(sub_entry), absl(PRG_WINDOW + 4))
    if extra:
        extra(m)
    m.move("b", imm(0x01), absl(GA_MAIN + 0x01))  # SRES = 1, SBRQ = 0


def wait_flag(asm, address, value, label):
    asm.label(label)
    asm.immop("cmp", "b", value, absl(address))
    asm.bcc("ne", label)


def main_finish(m):
    m.move("b", imm(0xA5), absl(DONE))
    m.stop(0x2700)


def cdd_words(command, lba=0):
    """TX0..TX9 as five words for $FF8042..$FF804A. Command 3 reads from `lba`; the checksum is ~(TX0 + ... + TX8) & $F."""
    tx = [command, 0, 0, 0, 0, 0, 0, 0, 0, 0]
    if command == 3:
        msf = lba + 150
        mm, ss, ff = msf // 75 // 60, msf // 75 % 60, msf % 75
        tx[2:8] = [mm // 10, mm % 10, ss // 10, ss % 10, ff // 10, ff % 10]
    tx[9] = (~sum(tx[:9])) & 0xF
    return [(tx[2 * i] << 8) | tx[2 * i + 1] for i in range(5)], tx


# ------------------------------------------------------------------------------------------------------------------------------
# Fixtures. Each returns (ip bytes, sp bytes, data sectors {lba: 2048 bytes}, expected dict).


def fixture_comm(options):
    sub = Asm(SP_BASE)
    sub.label("entry")
    sub.move("l", imm("int2"), absl(0x000068))  # autovector level 2 (vector 26)
    sub.move("b", imm(0x04), absl(GA_SUB + 0x33))  # IEN2
    sub.move_sr(0x2000)
    sub.move("b", imm(0x01), absl(GA_SUB + 0x0F))  # ready
    sub.label("idle")
    sub.stop(0x2000)
    sub.tst("b", absl(SUB_DATA + 0x10))
    sub.bcc("eq", "idle")
    sub.stop(0x2700)
    sub.label("int2")
    sub.move("w", absl(GA_SUB + 0x10), d(0))  # COMCMD0
    sub.move("w", absl(GA_SUB + 0x12), d(1))  # COMCMD1
    sub.mulu(d(1), 0)
    sub.move("l", absl(0x008000), d(2))  # parameter written by main through the window (bank 0)
    sub.alu("add", "l", d(2), 0)
    sub.move("w", absl(0x020100), d(3))  # word written by main through the window with bank 1 selected
    sub.move("l", d(0), absl(GA_SUB + 0x20))  # COMSTAT0/1
    sub.move("w", d(3), absl(GA_SUB + 0x24))  # COMSTAT2
    sub.move("b", imm(0x01), absl(SUB_DATA + 0x10))
    sub.move("b", imm(0x02), absl(GA_SUB + 0x0F))
    sub.rte()
    sp = sub.assemble()

    main = Asm(IP_BASE)
    main.label("start")

    def params(m):
        m.move("l", imm(0x00012345), absl(PRG_WINDOW + 0x8000))  # PRG $008000 (bank 0)
        m.move("b", imm(0x40), absl(GA_MAIN + 0x03))  # BK = 1
        m.move("w", imm(0x5A5A), absl(PRG_WINDOW + 0x0100))  # PRG $020100
        m.move("b", imm(0x00), absl(GA_MAIN + 0x03))  # BK = 0

    main_start_sub(main, SP_BASE, params)
    wait_flag(main, GA_MAIN + 0x0F, 1, "w_ready")
    main.move("w", imm(0x1234), absl(GA_MAIN + 0x10))
    main.move("w", imm(0x0003), absl(GA_MAIN + 0x12))
    main.move("b", imm(0x01), absl(GA_MAIN + 0x0E))
    main.move("b", imm(0x01), absl(GA_MAIN + 0x00))  # IFL2
    wait_flag(main, GA_MAIN + 0x0F, 2, "w_done")
    main.lea(absl(RESULT), 0)
    for reg in range(4):
        main.move("w", absl(GA_MAIN + 0x20 + 2 * reg), ("inc", 0))
    main_finish(main)
    ip = main.assemble()
    response = (0x1234 * 3 + 0x12345) & 0xFFFFFFFF
    expected = {"result_words": [response >> 16, response & 0xFFFF, 0x5A5A, 0x0000],
                "sub_data": {"0x010010": 1}}
    return ip, sp, {}, expected


def fixture_open_bus(options):
    """Negative: the sub writes and then reads $0C0000, unused in 2M mode (GPGX: the write is ignored, the read returns the
    prefetched instruction word - open bus, the typed stop open_bus_read). The write goes through the 1 MiB mirror at $1C0000."""
    sub = Asm(SP_BASE)
    sub.move("w", imm(0x1234), absl(0x1C0000))
    sub.move("w", absl(0x0C0000), d(0))
    sub.stop(0x2700)
    main = Asm(IP_BASE)
    main_start_sub(main, SP_BASE)
    main.label("spin")
    main.bra("spin")
    return main.assemble(), sub.assemble(), {}, {"stop": "open_bus_read", "cpu": "sub", "ignored_writes": 1}


def fixture_negative(kind):
    """SEG-062-T003 negative fixtures: each ends in one typed stop or one defined (non-faulting) hardware result."""
    def build(options):
        sub = Asm(SP_BASE)
        main = Asm(IP_BASE)
        expected = {}
        if kind == "ga_register":
            sub.move("w", imm(1), absl(GA_SUB + 0x4C))  # font data registers: not modelled
            sub.stop(0x2700)
            main_start_sub(main, SP_BASE)
            main.move("w", imm(1), absl(GA_MAIN + 0x30))  # unused main offset: the write is ignored (GPGX)
            main.stop(0x2700)
            expected = {"stop": "gate_array_register_unsupported", "cpu": "sub"}
        elif kind == "prg_window_nobus":
            sub.label("spin")
            sub.bra("spin")
            main_start_sub(main, SP_BASE)
            main.move("w", imm(0xBEEF), absl(PRG_WINDOW + 0x100))  # unmapped once the sub runs: ignored (GPGX)
            main.move("w", absl(0x060100), d(0))  # the window's mirror at $060000: open bus
            main.stop(0x2700)
            expected = {"stop": "open_bus_read", "cpu": "main", "ignored_writes": 1}
        elif kind == "ifl2_masked":
            sub.move("l", imm("handler"), absl(0x000068))
            sub.move_sr(0x2000)
            sub.move("b", imm(0x01), absl(GA_SUB + 0x0F))
            sub.stop(0x2000)
            sub.label("handler")
            sub.move("b", imm(0x02), absl(GA_SUB + 0x0F))
            sub.rte()
            main_start_sub(main, SP_BASE)
            wait_flag(main, GA_MAIN + 0x0F, 1, "w_ready")
            main.move("b", imm(0x01), absl(GA_MAIN + 0x00))  # IFL2 with IEN2 = 0: dropped (V2)
            main.move("b", absl(GA_MAIN + 0x00), absl(RESULT))
            main.move("b", absl(GA_MAIN + 0x0F), absl(RESULT + 1))
            main_finish(main)
            expected = {"result_bytes": [0x00, 0x01], "sub_interrupts_taken_2": 0}
        elif kind == "handler_not_compiled":
            sub.move("l", imm(0x7000), absl(0x000068))  # not code of the SP image
            sub.move("b", imm(0x04), absl(GA_SUB + 0x33))
            sub.move_sr(0x2000)
            sub.move("b", imm(0x01), absl(GA_SUB + 0x0F))
            sub.stop(0x2000)
            main_start_sub(main, SP_BASE)
            wait_flag(main, GA_MAIN + 0x0F, 1, "w_ready")
            main.move("b", imm(0x01), absl(GA_MAIN + 0x00))
            main.label("spin")
            main.bra("spin")
            expected = {"stop": "exception_vector_not_compiled", "cpu": "sub"}
        elif kind == "entry_not_compiled":
            sub.move("l", imm(0x12345678), d(0))
            sub.stop(0x2700)
            main_start_sub(main, SP_BASE + 2)  # the middle of the first SP instruction
            main.stop(0x2700)
            expected = {"stop": "entry_not_compiled", "cpu": "sub"}
        elif kind == "code_overwrite":
            sub.move("w", imm(0x4E71), absl("victim"))
            sub.label("victim")
            sub.moveq(1, 0)
            sub.stop(0x2700)
            main_start_sub(main, SP_BASE)
            main.stop(0x2700)
            expected = {"stop": "code_bytes_mismatch", "cpu": "sub"}
        elif kind == "write_protect":
            sub.move("w", imm(0x1234), absl(0x001000))  # inside WP = $40 * $200: ignored (V2)
            sub.move("w", absl(0x001000), absl(GA_SUB + 0x20))
            sub.move("w", imm(0x5678), absl(0x009000))
            sub.move("w", absl(0x009000), absl(GA_SUB + 0x22))
            sub.move("b", imm(0x01), absl(GA_SUB + 0x0F))
            sub.stop(0x2700)

            def protect(m):
                m.move("b", imm(0x40), absl(GA_MAIN + 0x02))
            main_start_sub(main, SP_BASE, protect)
            wait_flag(main, GA_MAIN + 0x0F, 1, "w_done")
            main.move("w", absl(GA_MAIN + 0x20), absl(RESULT))
            main.move("w", absl(GA_MAIN + 0x22), absl(RESULT + 2))
            main_finish(main)
            expected = {"result_words": [0x0000, 0x5678], "prg_write_protect_drops": 1}
        elif kind == "busreq_stopped":
            sub.move("b", imm(0x01), absl(GA_SUB + 0x0F))
            sub.stop(0x2700)
            main_start_sub(main, SP_BASE)
            wait_flag(main, GA_MAIN + 0x0F, 1, "w_ready")
            main.move("w", imm(200), d(7))
            main.label("delay")
            main.dbra(7, "delay")
            main.move("b", imm(0x03), absl(GA_MAIN + 0x01))  # SBRQ while the sub is in STOP: SBRQ reads 0 (GPGX, V1)
            main.move("b", absl(GA_MAIN + 0x01), absl(RESULT))
            main.move("w", imm(0xBEEF), absl(PRG_WINDOW + 0x100))  # the window stays unmapped: ignored
            main_finish(main)
            expected = {"result_bytes": [0x01], "ignored_writes": 1}
        return main.assemble(), sub.assemble(), {}, expected
    return build


def wordram_pattern(count, start, step):
    return [(start + i * step) & 0xFFFF for i in range(count)]


def fixture_wordram2m(options):
    sub = Asm(SP_BASE)
    sub.move("b", imm(0x01), absl(GA_SUB + 0x0F))
    wait_flag(sub, GA_SUB + 0x0E, 1, "w_go")
    sub.label("w_own")
    sub.btst(0, absl(GA_SUB + 0x03))  # RET = 0 when the sub owns 2M Word RAM
    sub.bcc("ne", "w_own")
    sub.lea(absl(WORD_RAM_SUB_2M), 0)
    sub.move("w", imm(0x5A5A), d(0))
    sub.move("w", imm(63), d(7))
    sub.label("fill")
    sub.move("w", d(0), ("inc", 0))
    sub.immop("add", "w", 0x0103, d(0))
    sub.dbra(7, "fill")
    sub.move("b", imm(0x01), absl(GA_SUB + 0x03))  # RET = 1: return Word RAM to main
    sub.move("b", imm(0x02), absl(GA_SUB + 0x0F))
    sub.stop(0x2700)

    main = Asm(IP_BASE)
    main_start_sub(main, SP_BASE)
    wait_flag(main, GA_MAIN + 0x0F, 1, "w_ready")
    main.move("b", imm(0x02), absl(GA_MAIN + 0x03))  # DMNA = 1: give Word RAM to the sub
    if options.get("violation"):
        main.move("w", imm(0x4321), absl(WORD_RAM_MAIN + 0x40002))  # Word RAM owned by the sub (mirror): ignored (GPGX)
        main.move("w", absl(WORD_RAM_MAIN), d(0))  # negative: main reads Word RAM it no longer owns: open bus
    main.move("b", imm(0x01), absl(GA_MAIN + 0x0E))
    wait_flag(main, GA_MAIN + 0x0F, 2, "w_done")
    main.label("w_ret")
    main.btst(0, absl(GA_MAIN + 0x03))
    main.bcc("eq", "w_ret")
    main.lea(absl(WORD_RAM_MAIN), 0)
    main.moveq(0, 0)
    main.moveq(0, 1)
    main.move("w", imm(63), d(7))
    main.label("sum")
    main.move("w", ("inc", 0), d(2))
    main.eor("w", 2, d(0))
    main.alu("add", "w", d(2), 1)
    main.dbra(7, "sum")
    main.move("w", d(0), absl(RESULT))
    main.move("w", d(1), absl(RESULT + 2))
    main.move("w", absl(WORD_RAM_MAIN + 126), absl(RESULT + 4))
    main_finish(main)
    words = wordram_pattern(64, 0x5A5A, 0x0103)
    xor = 0
    for w in words:
        xor ^= w
    expected = {"result_words": [xor, sum(words) & 0xFFFF, words[63]]}
    if options.get("violation"):
        expected = {"stop": "open_bus_read", "cpu": "main", "ignored_writes": 1}
    return main.assemble(), sub.assemble(), {}, expected


def fixture_wordram_wait(options):
    """The sub writes 2M Word RAM before main hands it over: the sub waits for DTACK (GPGX s68k_pulse_wait) and its instruction is
    retried once main sets DMNA. Main sees the value after the sub returns Word RAM."""
    sub = Asm(SP_BASE)
    sub.move("w", imm(0x00C0), d(4))
    sub.lea(absl(WORD_RAM_SUB_2M + 0x100), 3)
    sub.move("w", d(4), ("inc", 3))  # waits here (A3 must not advance twice)
    sub.move("w", imm(0x5AA5), ("inc", 3))
    sub.move("b", imm(0x01), absl(GA_SUB + 0x03))  # RET = 1
    sub.move("b", imm(0x01), absl(GA_SUB + 0x0F))
    sub.stop(0x2700)
    main = Asm(IP_BASE)
    main_start_sub(main, SP_BASE)
    main.move("w", imm(300), d(7))
    main.label("delay")
    main.dbra(7, "delay")
    main.move("b", imm(0x02), absl(GA_MAIN + 0x03))  # DMNA
    wait_flag(main, GA_MAIN + 0x0F, 1, "w_done")
    main.label("w_ret")
    main.btst(0, absl(GA_MAIN + 0x03))
    main.bcc("eq", "w_ret")
    main.move("w", absl(WORD_RAM_MAIN + 0x100), absl(RESULT))
    main.move("w", absl(WORD_RAM_MAIN + 0x102), absl(RESULT + 2))
    main_finish(main)
    return main.assemble(), sub.assemble(), {}, {"result_words": [0x00C0, 0x5AA5], "sub_a3": WORD_RAM_SUB_2M + 0x104}


def fixture_wordram1m(options):
    sub = Asm(SP_BASE)
    sub.move("b", imm(0x04), absl(GA_SUB + 0x03))  # MODE = 1 (1M), RET = 0
    sub.move("b", imm(0x01), absl(GA_SUB + 0x0F))
    wait_flag(sub, GA_SUB + 0x0E, 1, "w_marker")
    sub.lea(absl(WORD_RAM_SUB_1M), 0)
    sub.move("w", imm(0x1111), d(0))
    sub.move("w", imm(31), d(7))
    sub.label("fill")
    sub.move("w", d(0), ("inc", 0))
    sub.immop("add", "w", 0x0205, d(0))
    sub.dbra(7, "fill")
    sub.move("b", imm(0x05), absl(GA_SUB + 0x03))  # RET = 1: swap the banks
    sub.move("w", absl(WORD_RAM_SUB_1M), absl(GA_SUB + 0x20))  # the main CPU's marker is now in the sub's bank
    sub.move("b", imm(0x02), absl(GA_SUB + 0x0F))
    sub.stop(0x2700)

    main = Asm(IP_BASE)
    main_start_sub(main, SP_BASE)
    wait_flag(main, GA_MAIN + 0x0F, 1, "w_ready")
    main.move("w", imm(0xBEEF), absl(WORD_RAM_MAIN))  # main's own bank while the sub fills the other one
    main.move("b", imm(0x01), absl(GA_MAIN + 0x0E))
    wait_flag(main, GA_MAIN + 0x0F, 2, "w_done")
    main.lea(absl(WORD_RAM_MAIN), 0)
    main.moveq(0, 0)
    main.moveq(0, 1)
    main.move("w", imm(31), d(7))
    main.label("sum")
    main.move("w", ("inc", 0), d(2))
    main.eor("w", 2, d(0))
    main.alu("add", "w", d(2), 1)
    main.dbra(7, "sum")
    main.move("w", d(0), absl(RESULT))
    main.move("w", d(1), absl(RESULT + 2))
    main.move("w", absl(GA_MAIN + 0x20), absl(RESULT + 4))
    main_finish(main)
    words = wordram_pattern(32, 0x1111, 0x0205)
    xor = 0
    for w in words:
        xor ^= w
    return main.assemble(), sub.assemble(), {}, {"result_words": [xor, sum(words) & 0xFFFF, 0xBEEF]}


def fixture_timer(options):
    sub = Asm(SP_BASE)
    sub.move("l", imm("int3"), absl(0x00006C))  # autovector level 3 (vector 27)
    sub.clr("w", absl(SUB_DATA))
    sub.move("b", imm(9), absl(GA_SUB + 0x31))  # timer: every (9 + 1) * 384 sub cycles
    sub.move("b", imm(0x08), absl(GA_SUB + 0x33))  # IEN3
    sub.move_sr(0x2000)
    sub.label("idle")
    sub.stop(0x2000)
    sub.immop("cmp", "w", 5, absl(SUB_DATA))
    sub.bcc("ne", "idle")
    sub.move("w", absl(SUB_DATA), absl(GA_SUB + 0x20))
    sub.move("b", imm(0x01), absl(GA_SUB + 0x0F))
    sub.stop(0x2700)
    sub.label("int3")
    sub.quick("add", "w", 1, absl(SUB_DATA))
    sub.immop("cmp", "w", 5, absl(SUB_DATA))
    sub.bcc("ne", "int3_done")
    sub.move("b", imm(0x00), absl(GA_SUB + 0x31))  # stop the timer
    sub.move("b", imm(0x00), absl(GA_SUB + 0x33))  # disable INT3
    sub.label("int3_done")
    sub.rte()

    main = Asm(IP_BASE)
    main_start_sub(main, SP_BASE)
    wait_flag(main, GA_MAIN + 0x0F, 1, "w_done")
    main.move("w", absl(GA_MAIN + 0x20), absl(RESULT))
    main_finish(main)
    return main.assemble(), sub.assemble(), {}, {"result_words": [5]}


# subside (SEG-064-T004) -----------------------------------------------------------------------------------------------------
# The stopwatch and the graphics-completion interrupt (level 1) on the CPU. Expected values come from a hand-written MC68000 cycle table
# (MC68000 User's Manual, instruction execution times), not from the runtime's timing core.
CYCLES = {"move_b_imm_abs": 20, "move_w_abs_dn": 16, "move_w_imm_dn": 8, "move_w_dn_abs": 16}


def dbra_cycles(n):
    """DBRA with the counter at n: n taken branches of 10 cycles and the final fall-through of 14."""
    return 10 * n + 14


def fixture_subside(options):
    sub = Asm(SP_BASE)
    model = []  # (kind, value) in program order, for the expected stopwatch values
    # negative cases (sub:a6 = k selects one; 0 = none): each ends in one typed stop at a known instruction with a known retirement count
    negatives = {}
    sub.move("l", a(6), d(7))
    negative_cases = [
        (1, "gate_array_register_unsupported", 0xFF800D, 1, 0, "stopwatch byte read", lambda: sub.move("b", absl(GA_SUB + 0x0D), d(0))),
        (2, "gate_array_register_unsupported", 0xFF800C, 1, 1, "stopwatch byte write", lambda: sub.move("b", imm(1), absl(GA_SUB + 0x0C))),
        (3, "pcm_unsupported", 0xFF0021, 1, 0, "PCM register read", lambda: sub.move("b", absl(0xFF0021), d(0))),
        (4, "gate_array_register_unsupported", 0xFF805A, 1, 1, "graphics byte write", lambda: sub.move("b", imm(1), absl(GA_SUB + 0x5A))),
        (5, "gate_array_register_unsupported", 0xFF805A, 2, 0, "graphics parameter read", lambda: sub.move("w", absl(GA_SUB + 0x5A), d(0))),
        (6, "cdc_destination_unsupported", 0xFF8008, 0, 0, "CDC host-data byte write", lambda: sub.move("b", imm(1), absl(GA_SUB + 0x08))),
        (7, "gate_array_register_unsupported", 0xFF8066, 2, 1, "graphics start in 1M Word RAM mode", lambda: sub.move("w", imm(0xC000), absl(GA_SUB + 0x66))),
        (8, "cdd_command_unsupported", 0x105, 0, 0, "CDD TOC of track 0", lambda: sub.move("w", imm(0), absl(GA_SUB + 0x4A))),
    ]
    for k, stop_class, address, width, write, label, fault in negative_cases:
        sub.immop("cmp", "w", k, d(7))
        sub.bcc("ne", "neg%d" % k)
        pre = 0
        if k == 7:
            sub.move("b", imm(0x04), absl(GA_SUB + 0x03))   # MODE = 1M
            pre = 1
        if k == 8:
            for reg, value in ((0x42, 0x0200), (0x44, 0x0005), (0x46, 0x0000), (0x48, 0x0000)):
                sub.move("w", imm(value), absl(GA_SUB + reg))
            pre = 4
        negatives[str(k)] = {"class": stop_class, "pc": sub.pc, "address": address, "width": width, "write": write,
                             "retired": 1 + 2 * k + pre, "label": label}
        fault()
        sub.label("neg%d" % k)
    # vectors for levels 1 and 2 (autovectors 25 and 26) and the work counters
    sub.move("l", imm("int1"), absl(0x000064))
    sub.move("l", imm("int2"), absl(0x000068))
    for off in (0, 2, 4, 6):
        sub.clr("w", absl(SUB_DATA + off))
    # --- stopwatch ---
    sub.move("b", imm(0x00), absl(GA_SUB + 0x01))   # RES0 = 0: the peripheral reset restarts the stopwatch grid at this instruction
    t = CYCLES["move_b_imm_abs"]
    sub.move("w", absl(GA_SUB + 0x0C), d(0)); r0 = t // 384; t += CYCLES["move_w_abs_dn"]
    sub.move("w", imm(99), d(1)); t += CYCLES["move_w_imm_dn"]
    sub.label("a"); sub.dbra(1, "a"); t += dbra_cycles(99)
    sub.move("w", absl(GA_SUB + 0x0C), d(2)); r2 = t // 384; t += CYCLES["move_w_abs_dn"]
    sub.move("w", absl(GA_SUB + 0x0C), d(3)); r3 = t // 384; t += CYCLES["move_w_abs_dn"]
    sub.move("w", d(2), absl(GA_SUB + 0x0C)); base = t // 384; t += CYCLES["move_w_dn_abs"]   # any word write clears the count
    sub.move("w", absl(GA_SUB + 0x0C), d(4)); r4 = t // 384 - base; t += CYCLES["move_w_abs_dn"]
    sub.move("w", imm(5), d(1)); t += CYCLES["move_w_imm_dn"]
    sub.label("b"); sub.dbra(1, "b"); t += dbra_cycles(5)
    sub.move("w", absl(GA_SUB + 0x0C), d(6)); r6 = t // 384 - base; t += CYCLES["move_w_abs_dn"]
    # three passes of 65535 iterations: more than 4096 stopwatch units, so the 12-bit count has rolled over
    sub.move("w", imm(2), d(7)); t += CYCLES["move_w_imm_dn"]
    sub.label("outer")
    sub.move("w", imm(65534), d(1)); t += 3 * CYCLES["move_w_imm_dn"]
    sub.label("inner"); sub.dbra(1, "inner")
    sub.dbra(7, "outer")
    t += 3 * dbra_cycles(65534) + 2 * 10 + 14 - 2 * CYCLES["move_w_imm_dn"] * 0
    sub.move("w", absl(GA_SUB + 0x0C), d(5)); r5 = ((t // 384) - base) & 0xFFF
    # --- level 1 (graphics completion) on the CPU ---
    sub.bra("after_start")
    sub.label("gfx_start")                            # a tiny operation: 8 dots x 2 lines = 2 * 66 sub cycles
    for reg, value in ((0x58, 0), (0x5A, 0), (0x5C, 0), (0x5E, 0x1000), (0x60, 0), (0x62, 8), (0x64, 2), (0x66, 0xC000)):
        sub.move("w", imm(value), absl(GA_SUB + reg))
    sub.rts()
    sub.label("after_start")
    sub.move("b", imm(0x00), absl(GA_SUB + 0x33))     # phase 1: IEN1 = 0
    sub.bsr("gfx_start")
    sub.move("w", imm(40), d(1)); sub.label("p1"); sub.dbra(1, "p1")
    sub.move("w", absl(SUB_DATA), absl(GA_SUB + 0x20))      # COMSTAT0: interrupts taken (0)
    sub.move("w", absl(GA_SUB + 0x58), absl(GA_SUB + 0x22))  # COMSTAT1: busy/control read (0: finished)
    sub.move("b", imm(0x02), absl(GA_SUB + 0x33))     # phase 2: IEN1, mask 0: taken at the end of the operation
    sub.move_sr(0x2000)
    sub.bsr("gfx_start")
    sub.stop(0x2000)
    sub.move("w", absl(SUB_DATA), absl(GA_SUB + 0x24))      # COMSTAT2: 1
    sub.move_sr(0x2100)                               # phase 3: mask 1 hides level 1
    sub.bsr("gfx_start")
    sub.move("w", imm(40), d(1)); sub.label("p3"); sub.dbra(1, "p3")
    sub.move("w", absl(SUB_DATA), absl(GA_SUB + 0x26))      # COMSTAT3: still 1 (pending, masked)
    sub.move_sr(0x2000)
    sub.nop()
    sub.move("w", absl(SUB_DATA), absl(GA_SUB + 0x28))      # COMSTAT4: 2 (taken at the first boundary after the unmask)
    sub.move("b", imm(0x06), absl(GA_SUB + 0x33))     # phase 4: IEN1 and IEN2, all levels masked
    sub.move_sr(0x2700)
    sub.bsr("gfx_start")
    sub.move("w", imm(40), d(1)); sub.label("p4"); sub.dbra(1, "p4")
    sub.move("b", imm(0x01), absl(GA_SUB + 0x0F))     # main: raise IFL2 now
    sub.label("wait_main"); sub.tst("b", absl(GA_SUB + 0x0E)); sub.bcc("eq", "wait_main")
    sub.move_sr(0x2000)                               # unmask: level 2 (higher) first, then level 1
    sub.nop()
    sub.nop()
    sub.move("w", absl(SUB_DATA + 4), absl(GA_SUB + 0x2A))  # COMSTAT5: order slot of the level-1 handler
    sub.move("w", absl(SUB_DATA + 6), absl(GA_SUB + 0x2C))  # COMSTAT6: order slot of the level-2 handler
    sub.move("w", absl(SUB_DATA), absl(GA_SUB + 0x2E))      # COMSTAT7: level-1 interrupts in all
    sub.move("b", imm(0x02), absl(GA_SUB + 0x0F))
    sub.stop(0x2700)
    sub.label("int1")
    sub.quick("add", "w", 1, absl(SUB_DATA))
    sub.quick("add", "w", 1, absl(SUB_DATA + 2))
    sub.move("w", absl(SUB_DATA + 2), absl(SUB_DATA + 4))
    sub.rte()
    sub.label("int2")
    sub.quick("add", "w", 1, absl(SUB_DATA + 2))
    sub.move("w", absl(SUB_DATA + 2), absl(SUB_DATA + 6))
    sub.rte()

    main = Asm(IP_BASE)
    main.move("b", imm(0x02), absl(GA_MAIN + 0x03))   # DMNA: Word RAM to the sub (the ASIC works in 2M Word RAM the sub owns)
    main_start_sub(main, SP_BASE)
    wait_flag(main, GA_MAIN + 0x0F, 1, "w_ready")
    main.move("b", imm(0x01), absl(GA_MAIN + 0x00))   # IFL2
    main.move("b", imm(0x01), absl(GA_MAIN + 0x0E))   # main flag: the sub may unmask
    wait_flag(main, GA_MAIN + 0x0F, 2, "w_done")
    main_finish(main)
    expected = {"stopwatch_d": {"d0": r0, "d2": r2, "d3": r3, "d4": r4, "d5": r5, "d6": r6},
                "comstat": [0, 0, 1, 1, 2, 4, 3, 3], "interrupts": {"1": 3, "2": 1}, "negatives": negatives}
    return main.assemble(), sub.assemble(), {}, expected


# cdframe ---------------------------------------------------------------------------------------------------------------------
PALETTE_BYTES = 32
TILE_COUNT = 16
TILE_BYTES = TILE_COUNT * 32
NAME_ROWS, NAME_COLS = 28, 40
DATA_BYTES = 2 * 2048


def frame_data(seed):
    rng = random.Random(seed)
    palette = [0] + [rng.randrange(0, 0x1000) & 0x0EEE for _ in range(15)]
    tiles = bytearray(32)  # tile 0 is transparent
    for _ in range(1, TILE_COUNT):
        tiles += bytes(rng.randrange(256) for _ in range(32))
    names = [rng.randrange(1, TILE_COUNT) | (rng.randrange(2) << 11) | (rng.randrange(2) << 12) for _ in range(NAME_ROWS * NAME_COLS)]
    blob = bytearray(DATA_BYTES)
    offset = 0
    for c in palette:
        struct.pack_into(">H", blob, offset, c)
        offset += 2
    blob[offset:offset + TILE_BYTES] = tiles
    offset += TILE_BYTES
    for n in names:
        struct.pack_into(">H", blob, offset, n)
        offset += 2
    tail = rng.randbytes(DATA_BYTES - offset)
    blob[offset:] = tail
    return bytes(blob)


def fixture_cdframe(options):
    lba = options.get("cd_lba", DATA_LBA)
    dma_dest = options.get("dma_dest", 7)
    play_words, play_tx = cdd_words(3, lba)
    if options.get("bad_checksum"):
        play_words[4] ^= 0x0001
    stop_words, _ = cdd_words(1)

    sub = Asm(SP_BASE)
    def cdc_write(reg, value):
        sub.move("b", imm(reg), absl(GA_SUB + 0x05))
        sub.move("b", imm(value), absl(GA_SUB + 0x07))
    cdc_write(0x0F, 0x00)  # CDC reset
    cdc_write(0x01, 0x02)  # IFCTRL: DOUTEN
    cdc_write(0x0A, 0x84)  # CTRL0: DECEN | WRRQ
    sub.move("b", imm(0x08 if options.get("bad_cdc_register") else 0x00), absl(GA_SUB + 0x07))  # CTRL1 (address 11); MODRQ variant
    sub.move("b", imm(0x01), absl(GA_SUB + 0x0F))
    wait_flag(sub, GA_SUB + 0x0E, 1, "w_go")
    sub.label("w_own")
    sub.btst(0, absl(GA_SUB + 0x03))
    sub.bcc("ne", "w_own")
    for i, word in enumerate(play_words):
        sub.move("w", imm(word), absl(GA_SUB + 0x42 + 2 * i))  # the word write to $FF804A sends the command
    sub.lea(absl(GA_SUB + 0x20), 2)
    for k in range(2):
        # The decoder also runs while the drive seeks (GPGX): blocks whose header is not the requested MSF are skipped.
        msf = lba + k + 150
        want = [(bcd(msf // 75 // 60) << 8) | bcd(msf // 75 % 60), (bcd(msf % 75) << 8) | 0x01]
        sub.label("w_valid%d" % k)
        sub.move("b", imm(0x0F), absl(GA_SUB + 0x05))  # STAT3
        sub.move("b", absl(GA_SUB + 0x07), d(0))
        sub.btst(7, d(0))
        sub.bcc("ne", "w_valid%d" % k)
        sub.move("b", imm(0x04), absl(GA_SUB + 0x05))  # HEAD0..HEAD3, then PTL, PTH
        for reg in (0, 5):
            sub.move("b", absl(GA_SUB + 0x07), d(reg))
            sub.lsl_w(8, reg)
            sub.move("b", absl(GA_SUB + 0x07), d(reg))
        sub.immop("cmp", "w", want[0], d(0))
        sub.bcc("ne", "w_valid%d" % k)
        sub.immop("cmp", "w", want[1], d(5))
        sub.bcc("ne", "w_valid%d" % k)
        sub.move("w", d(0), ("inc", 2))
        sub.move("w", d(5), ("inc", 2))
        sub.moveq(0, 1)
        sub.move("b", absl(GA_SUB + 0x07), d(1))
        sub.moveq(0, 2)
        sub.move("b", absl(GA_SUB + 0x07), d(2))
        sub.lsl_w(8, 2)
        sub.alu("or", "w", d(1), 2)
        sub.quick("add", "w", 4, d(2))  # DAC = PT + 4 (skip the block header)
        sub.move("b", imm(dma_dest), absl(GA_SUB + 0x04))  # destination (also resets $FF800A)
        sub.move("w", imm(k << 8), absl(GA_SUB + 0x0A))  # Word RAM offset k * 2048 (in 8-byte units)
        sub.move("b", imm(0x02), absl(GA_SUB + 0x05))  # DBCL, DBCH, DACL, DACH, DTRG
        sub.move("b", imm(0xFF), absl(GA_SUB + 0x07))
        sub.move("b", imm(0x07), absl(GA_SUB + 0x07))
        sub.move("b", d(2), absl(GA_SUB + 0x07))
        sub.move("w", d(2), d(3))
        sub.lsr_w(8, 3)
        sub.move("b", d(3), absl(GA_SUB + 0x07))
        sub.move("b", imm(0x00), absl(GA_SUB + 0x07))  # DTRG
        if options.get("abort_dma") and k == 0:
            # Partial-progress variant: let the DMA run for a few instructions, then clear DOUTEN (abort) and report EDT.
            for _ in range(4):
                sub.nop()
            cdc_write(0x01, 0x00)
            sub.move("b", absl(GA_SUB + 0x04), absl(GA_SUB + 0x2E))  # EDT/DSR/destination into COMSTAT7 (high byte)
            sub.move("b", imm(0x01), absl(GA_SUB + 0x03))
            sub.move("b", imm(0x02), absl(GA_SUB + 0x0F))
            sub.stop(0x2700)
        sub.label("w_edt%d" % k)
        sub.btst(7, absl(GA_SUB + 0x04))
        sub.bcc("eq", "w_edt%d" % k)
    for i, word in enumerate(stop_words):
        sub.move("w", imm(word), absl(GA_SUB + 0x42 + 2 * i))
    sub.move("b", imm(0x01), absl(GA_SUB + 0x03))  # RET = 1: Word RAM back to main
    sub.move("b", imm(0x02), absl(GA_SUB + 0x0F))
    sub.stop(0x2700)

    main = Asm(IP_BASE)
    main_start_sub(main, SP_BASE)
    wait_flag(main, GA_MAIN + 0x0F, 1, "w_ready")
    main.move("b", imm(0x02), absl(GA_MAIN + 0x03))  # DMNA: Word RAM to the sub
    main.move("b", imm(0x01), absl(GA_MAIN + 0x0E))
    wait_flag(main, GA_MAIN + 0x0F, 2, "w_done")
    main.label("w_ret")
    main.btst(0, absl(GA_MAIN + 0x03))
    main.bcc("eq", "w_ret")
    main.lea(absl(VDP_CTRL), 1)
    main.lea(absl(VDP_DATA), 2)
    for reg in (0x8004, 0x8144, 0x8230, 0x8407, 0x856C, 0x8700, 0x8B00, 0x8C81, 0x8D3F, 0x8F02, 0x9001, 0x9100, 0x9200):
        main.move("w", imm(reg), ("ind", 1))
    main.move("l", imm(0xC0000000), ("ind", 1))  # CRAM write at 0
    main.lea(absl(WORD_RAM_MAIN), 0)
    main.move("w", imm(PALETTE_BYTES // 2 - 1), d(7))
    main.label("cram")
    main.move("w", ("inc", 0), ("ind", 2))
    main.dbra(7, "cram")
    main.move("l", imm(0x40000000), ("ind", 1))  # VRAM write at $0000
    main.move("w", imm(TILE_BYTES // 2 - 1), d(7))
    main.label("tiles")
    main.move("w", ("inc", 0), ("ind", 2))
    main.dbra(7, "tiles")
    main.move("l", imm(0x40000003), d(4))  # VRAM write at $C000 (plane A)
    main.move("w", imm(NAME_ROWS - 1), d(6))
    main.label("row")
    main.move("l", d(4), ("ind", 1))
    main.move("w", imm(NAME_COLS - 1), d(7))
    main.label("col")
    main.move("w", ("inc", 0), ("ind", 2))
    main.dbra(7, "col")
    main.immop("add", "l", 0x00800000, d(4))  # next row: +128 bytes
    main.dbra(6, "row")
    main.lea(absl(RESULT), 3)
    for reg in range(4):
        main.move("w", absl(GA_MAIN + 0x20 + 2 * reg), ("inc", 3))
    main_finish(main)

    data = frame_data(options.get("seed", 62))
    if options.get("data_flip") is not None:
        flipped = bytearray(data)
        flipped[options["data_flip"]] ^= 0xFF
        data = bytes(flipped)
    sectors = {DATA_LBA: data[:2048], DATA_LBA + 1: data[2048:]}
    headers = []
    for k in range(2):
        msf = DATA_LBA + k + 150
        headers += [(bcd(msf // 75 // 60) << 8) | bcd(msf // 75 % 60), (bcd(msf % 75) << 8) | 0x01]
    expected = {"result_words": headers, "data_sha256": hashlib.sha256(data).hexdigest(), "data_lba": DATA_LBA,
                "data_sectors": 2, "palette_bytes": PALETTE_BYTES, "tile_bytes": TILE_BYTES, "name_rows": NAME_ROWS,
                "name_cols": NAME_COLS, "cdd_play_tx": play_tx}
    return main.assemble(), sub.assemble(), sectors, expected

# modules (SEG-063-T001, Checkpoint D) --------------------------------------------------------------------------------------------
# Track 1 carries a minimal ISO9660 volume after the system area: PVD (16), terminator (17), root directory (18), /OVL directory (19)
# and one file per block. The sub program (the loader) reads module files by CDD/CDC into 2M Word RAM (CDC DMA) and runs them at
# $080000, copies them into PRG RAM $020000 with CPU moves and runs them there. Every module executes at both addresses.
ISO_PVD, ISO_TERMINATOR, ISO_ROOT, ISO_OVL = 16, 17, 18, 19
MODULE_BLOCKS = {"README.TXT;1": 20, "MODA.BIN;1": 21, "MODB.BIN;1": 22, "MODC.BIN;1": 23, "MODA2.BIN;1": 24, "APFX.BIN;1": 25}
MODULE_BYTES = 192            # three 64-byte granules
MODULE_WORD_RAM = 0x080000    # sub 2M view (CDC DMA destination offset 0)
MODULE_PRG = 0x020000         # PRG RAM copy destination
MARK_A, MARK_B, MARK_A_AFTER, COUNT_A, COUNT_B = SUB_DATA + 0x20, SUB_DATA + 0x22, SUB_DATA + 0x24, SUB_DATA + 0x30, SUB_DATA + 0x32
MODULE_CASES = ("checkpoint", "unknown", "mismatch", "partial", "missing_entry", "stale")
ISO_DEFECTS = ("bad_identifier", "no_terminator", "duplicate_primary", "endian", "block_size", "volume_space", "xattr", "multi_extent",
               "interleave", "associated", "cross_sector", "duplicate_name", "extent_range", "dot_record")


def module_code(kind):
    """Position-independent module images (no absolute self-reference): the same bytes run at $080000 and at $020000."""
    m = Asm(0)
    if kind != "c":
        m.moveq(1, 0)                                 # identical first instruction in A, A' and B
    if kind in ("a", "a2", "apfx"):
        m.move("w", imm(0xAAAB if kind == "a2" else 0xAAAA), absl(MARK_A))
        m.quick("add", "w", 1, absl(COUNT_A))
        m.move("l", a(5), d(7))                       # loader service hook (0 = none)
        m.bcc("eq", "skip")
        m.jsr(("ind", 5))                             # stale case: the service loads B over this image, then returns here
        m.label("skip")
        m.move("w", imm(0xA1A1), absl(MARK_A_AFTER))
        m.rts()
        fill = 0xA0
    elif kind == "b":
        m.moveq(2, 1)
        m.move("w", imm(0xBBBB), absl(MARK_B))
        m.quick("add", "w", 1, absl(COUNT_B))
        m.rts()
        fill = 0xB0
    else:  # "c": an unlisted module with its own first instruction
        m.moveq(3, 0)
        m.move("w", imm(0xCCCC), absl(MARK_A))
        m.rts()
        fill = 0xC0
    code = m.assemble()
    size = 64 if kind == "apfx" else MODULE_BYTES
    body = bytearray(code + bytes(((fill + i) & 0xFF) for i in range(MODULE_BYTES - len(code))))
    if kind == "apfx":
        return bytes(module_code("a")[:64])
    return bytes(body[:size])


def iso_both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def iso_both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def iso_record(name, block, size, directory=False, flags_extra=0, xattr=0, unit=0):
    n = len(name)
    length = 33 + n + (1 if n % 2 == 0 else 0)
    rec = bytearray(length)
    rec[0] = length
    rec[1] = xattr
    rec[2:10] = iso_both32(block)
    rec[10:18] = iso_both32(size)
    rec[18:25] = bytes([126, 10, 16, 12, 0, 0, 0])  # 2026-10-16 12:00:00 GMT
    rec[25] = (0x02 if directory else 0x00) | flags_extra
    rec[26] = unit
    rec[28:32] = iso_both16(1)
    rec[32] = n
    rec[33:33 + n] = name
    return bytes(rec)


def iso_volume(files, defect=None):
    """{block: 2048 bytes} for the descriptor set and the two directories. `files` maps identifier -> (block, bytes)."""
    sectors = {}
    pvd = bytearray(2048)
    pvd[0] = 1
    pvd[1:6] = b"CD002" if defect == "bad_identifier" else b"CD001"
    pvd[6] = 1
    pvd[8:40] = b"SEGARECOMP".ljust(32)
    pvd[40:72] = b"SEGARECOMP_MODULES".ljust(32)
    space = 1000 if defect == "volume_space" else TRACK1_SECTORS
    pvd[80:88] = iso_both32(space)
    if defect == "endian":
        pvd[84:88] = struct.pack(">I", space + 1)
    pvd[120:124] = iso_both16(1)
    pvd[124:128] = iso_both16(1)
    pvd[128:132] = iso_both16(1024 if defect == "block_size" else 2048)
    pvd[156:190] = iso_record(b"\x00", ISO_ROOT, 2048, directory=True)
    pvd[190:318] = b" " * 128  # volume set identifier
    pvd[881] = 1
    sectors[ISO_PVD] = bytes(pvd)
    term = bytearray(2048)
    term[0] = 255
    term[1:6] = b"CD001"
    term[6] = 1
    sectors[ISO_TERMINATOR] = bytes(pvd) if defect == "duplicate_primary" else bytes(term)
    if defect == "no_terminator":
        boot = bytearray(2048)
        boot[0] = 0
        boot[1:6] = b"CD001"
        boot[6] = 1
        for blk in range(ISO_TERMINATOR, ISO_PVD + 16):
            sectors[blk] = bytes(boot)
    root = bytearray()
    root += iso_record(b"\x00", ISO_ROOT, 2048, directory=True)
    root += iso_record(b"\x01", ISO_ROOT, 2048, directory=True)
    root += iso_record(b"OVL", ISO_OVL, 2048, directory=True)
    readme = files["README.TXT;1"]
    root += iso_record(b"README.TXT;1", readme[0], len(readme[1]))
    ovl = bytearray()
    ovl += iso_record(b"\x00", ISO_ROOT if defect == "dot_record" else ISO_OVL, 2048, directory=True)
    ovl += iso_record(b"\x01", ISO_ROOT, 2048, directory=True)
    if defect == "cross_sector":  # empty filler files (odd-length names: record = 33 + n) so MODA.BIN;1 starts 20 bytes before the end
        k = 0
        while 2048 - 20 - len(ovl) > 0:
            left = 2048 - 20 - len(ovl)
            n = 199 if left - 33 > 199 + 34 else left - 33
            assert n % 2 == 1 and n >= 5, n
            ovl += iso_record(("F%03d" % k).encode() + b"X" * (n - 4), readme[0], 0)
            k += 1
        assert len(ovl) == 2048 - 20
    for name in ("MODA.BIN;1", "MODB.BIN;1", "MODC.BIN;1", "MODA2.BIN;1", "APFX.BIN;1"):
        block, data = files[name]
        extra = dict()
        if name == "MODA.BIN;1":
            if defect == "xattr":
                extra["xattr"] = 1
            if defect == "multi_extent":
                extra["flags_extra"] = 0x80
            if defect == "associated":
                extra["flags_extra"] = 0x04
            if defect == "interleave":
                extra["unit"] = 1
            if defect == "extent_range":
                block = 1000
        ovl += iso_record(name.encode(), block, len(data), **extra)
        if defect == "duplicate_name" and name == "MODA.BIN;1":
            ovl += iso_record(name.encode(), files["MODB.BIN;1"][0], len(files["MODB.BIN;1"][1]))
    if defect == "cross_sector":
        ovl = ovl[:2048]
    assert len(root) <= 2048 and len(ovl) <= 2048
    sectors[ISO_ROOT] = bytes(root.ljust(2048, b"\x00"))
    sectors[ISO_OVL] = bytes(ovl.ljust(2048, b"\x00"))
    return sectors


def module_files():
    files = {"README.TXT;1": (MODULE_BLOCKS["README.TXT;1"], b"segarecomp synthetic Sega CD module disc (SEG-063)\n")}
    for name, kind in (("MODA.BIN;1", "a"), ("MODB.BIN;1", "b"), ("MODC.BIN;1", "c"), ("MODA2.BIN;1", "a2"), ("APFX.BIN;1", "apfx")):
        files[name] = (MODULE_BLOCKS[name], module_code(kind))
    return files


def fixture_modules(options):
    case = options.get("module_case", "checkpoint")
    assert case in MODULE_CASES, case
    files = module_files()
    stop_words, _ = cdd_words(1)
    sub = Asm(SP_BASE)

    def cdc_write(reg, value):
        sub.move("b", imm(reg), absl(GA_SUB + 0x05))
        sub.move("b", imm(value), absl(GA_SUB + 0x07))

    sub.label("entry")
    cdc_write(0x0F, 0x00)  # CDC reset
    cdc_write(0x01, 0x02)  # IFCTRL: DOUTEN
    cdc_write(0x0A, 0x84)  # CTRL0: DECEN | WRRQ
    sub.move("b", imm(0x00), absl(GA_SUB + 0x07))  # CTRL1
    sub.move("l", imm(0), a(5))  # no loader service for modules (MOVEA.L #0,A5)
    sub.move("b", imm(0x01), absl(GA_SUB + 0x0F))
    wait_flag(sub, GA_SUB + 0x0E, 1, "w_go")
    sub.label("w_own")
    sub.btst(0, absl(GA_SUB + 0x03))  # RET = 0: the sub owns 2M Word RAM
    sub.bcc("ne", "w_own")

    def load(name):
        sub.bsr("load_" + name.split(".")[0].lower())

    def run(address):
        sub.jsr(absl(address))

    def copy(count):
        sub.move("w", imm(count // 2 - 1), d(7))
        sub.bsr("copy")

    if case == "checkpoint":
        load("MODA.BIN;1"); run(MODULE_WORD_RAM); copy(MODULE_BYTES); run(MODULE_PRG)
        load("MODB.BIN;1"); run(MODULE_WORD_RAM); copy(MODULE_BYTES); run(MODULE_PRG)
    elif case == "unknown":
        load("MODC.BIN;1"); copy(MODULE_BYTES); run(MODULE_PRG)
    elif case == "mismatch":
        load("MODA2.BIN;1"); run(MODULE_WORD_RAM)
    elif case == "partial":
        load("MODA.BIN;1"); copy(MODULE_BYTES); load("MODB.BIN;1"); copy(64); run(MODULE_PRG)
    elif case == "missing_entry":
        load("MODB.BIN;1")
        sub.lea(absl(MODULE_WORD_RAM + 10), 0)  # an instruction boundary of A, inside B's second instruction
        sub.jsr(("ind", 0))
    elif case == "stale":
        load("MODB.BIN;1")  # (also makes the service routine statically reachable)
        load("MODA.BIN;1")
        sub.lea(absl("load_modb"), 5)  # A calls the service, which loads B over A, and returns into A
        run(MODULE_WORD_RAM)
    sub.move("w", absl(COUNT_A), absl(GA_SUB + 0x20))
    sub.move("w", absl(COUNT_B), absl(GA_SUB + 0x22))
    sub.move("w", absl(MARK_A), absl(GA_SUB + 0x24))
    sub.move("w", absl(MARK_B), absl(GA_SUB + 0x26))
    sub.move("w", absl(MARK_A_AFTER), absl(GA_SUB + 0x28))
    for i, word in enumerate(stop_words):
        sub.move("w", imm(word), absl(GA_SUB + 0x42 + 2 * i))
    sub.move("b", imm(0x01), absl(GA_SUB + 0x03))  # RET = 1: Word RAM back to main
    sub.move("b", imm(0x02), absl(GA_SUB + 0x0F))
    sub.stop(0x2700)

    # copy: D7 + 1 words from Word RAM $080000 to PRG RAM $020000 (ordinary CPU stores)
    sub.label("copy")
    sub.lea(absl(MODULE_WORD_RAM), 0)
    sub.lea(absl(MODULE_PRG), 1)
    sub.label("copy_loop")
    sub.move("w", ("inc", 0), ("inc", 1))
    sub.dbra(7, "copy_loop")
    sub.rts()

    # one loader per module file: CDD play at the file's block, wait for its block header, CDC DMA 2048 bytes to Word RAM offset 0
    used = {"checkpoint": ["MODA.BIN;1", "MODB.BIN;1"], "unknown": ["MODC.BIN;1"], "mismatch": ["MODA2.BIN;1"],
            "partial": ["MODA.BIN;1", "MODB.BIN;1"], "missing_entry": ["MODB.BIN;1"], "stale": ["MODB.BIN;1", "MODA.BIN;1"]}[case]
    for name in used:
        lba = files[name][0]
        tag = name.split(".")[0].lower()
        play_words, _ = cdd_words(3, lba)
        sub.label("load_" + tag)
        for i, word in enumerate(play_words):
            sub.move("w", imm(word), absl(GA_SUB + 0x42 + 2 * i))
        msf = lba + 150
        want = [(bcd(msf // 75 // 60) << 8) | bcd(msf // 75 % 60), (bcd(msf % 75) << 8) | 0x01]
        sub.label("w_valid_" + tag)
        sub.move("b", imm(0x0F), absl(GA_SUB + 0x05))  # STAT3
        sub.move("b", absl(GA_SUB + 0x07), d(0))
        sub.btst(7, d(0))
        sub.bcc("ne", "w_valid_" + tag)
        sub.move("b", imm(0x04), absl(GA_SUB + 0x05))  # HEAD0..HEAD3, PTL, PTH
        for reg in (0, 5):
            sub.move("b", absl(GA_SUB + 0x07), d(reg))
            sub.lsl_w(8, reg)
            sub.move("b", absl(GA_SUB + 0x07), d(reg))
        sub.immop("cmp", "w", want[0], d(0))
        sub.bcc("ne", "w_valid_" + tag)
        sub.immop("cmp", "w", want[1], d(5))
        sub.bcc("ne", "w_valid_" + tag)
        sub.moveq(0, 1)
        sub.move("b", absl(GA_SUB + 0x07), d(1))
        sub.moveq(0, 2)
        sub.move("b", absl(GA_SUB + 0x07), d(2))
        sub.lsl_w(8, 2)
        sub.alu("or", "w", d(1), 2)
        sub.quick("add", "w", 4, d(2))  # DAC = PT + 4
        sub.move("b", imm(7), absl(GA_SUB + 0x04))  # destination Word RAM
        sub.move("w", imm(0), absl(GA_SUB + 0x0A))  # Word RAM offset 0
        sub.move("b", imm(0x02), absl(GA_SUB + 0x05))  # DBCL, DBCH, DACL, DACH, DTRG
        sub.move("b", imm(0xFF), absl(GA_SUB + 0x07))
        sub.move("b", imm(0x07), absl(GA_SUB + 0x07))
        sub.move("b", d(2), absl(GA_SUB + 0x07))
        sub.move("w", d(2), d(3))
        sub.lsr_w(8, 3)
        sub.move("b", d(3), absl(GA_SUB + 0x07))
        sub.move("b", imm(0x00), absl(GA_SUB + 0x07))  # DTRG
        sub.label("w_edt_" + tag)
        sub.btst(7, absl(GA_SUB + 0x04))
        sub.bcc("eq", "w_edt_" + tag)
        sub.rts()
    sp = sub.assemble()

    main = Asm(IP_BASE)
    main_start_sub(main, SP_BASE)
    wait_flag(main, GA_MAIN + 0x0F, 1, "w_ready")
    main.move("b", imm(0x02), absl(GA_MAIN + 0x03))  # DMNA: Word RAM to the sub
    main.move("b", imm(0x01), absl(GA_MAIN + 0x0E))
    wait_flag(main, GA_MAIN + 0x0F, 2, "w_done")
    main.lea(absl(RESULT), 3)
    for reg in range(5):
        main.move("w", absl(GA_MAIN + 0x20 + 2 * reg), ("inc", 3))
    main_finish(main)

    sectors = {blk: data for blk, data in iso_volume(files, options.get("iso_defect")).items()}
    for name, (block, data) in files.items():
        sectors[block] = data.ljust(2048, b"\x00")
    modules = {name: {"block": block, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()} for name, (block, data) in files.items()}
    expected = {"case": case, "files": modules, "module_bytes": MODULE_BYTES, "word_ram_base": MODULE_WORD_RAM, "prg_base": MODULE_PRG,
                "missing_entry_offset": 10}
    if case == "checkpoint":
        expected["result_words"] = [2, 2, 0xAAAA, 0xBBBB, 0xA1A1]
    else:
        expected["stop"] = {"unknown": "image_unknown", "mismatch": "image_bytes_mismatch", "partial": "image_partially_overwritten",
                            "missing_entry": "image_entry_not_compiled", "stale": "stale_image_selection"}[case]
    return main.assemble(), sp, sectors, expected


FIXTURES = {
    "comm": fixture_comm,
    "neg_open_bus": fixture_open_bus,
    "wordram2m": fixture_wordram2m,
    "wordram_violation": lambda o: fixture_wordram2m(dict(o, violation=True)),
    "wordram1m": fixture_wordram1m,
    "wordram_wait": fixture_wordram_wait,
    "timer": fixture_timer,
    "subside": fixture_subside,
    "cdframe": fixture_cdframe,
    "modules": fixture_modules,
}
for _kind in ("ga_register", "prg_window_nobus", "ifl2_masked", "handler_not_compiled", "entry_not_compiled", "code_overwrite",
              "write_protect", "busreq_stopped"):
    FIXTURES["neg_" + _kind] = fixture_negative(_kind)

# ------------------------------------------------------------------------------------------------------------------------------
# Disc image


def bcd(v):
    return ((v // 10) << 4) | (v % 10)


def system_area(ip, sp, volume):
    area = bytearray(16 * 2048)
    area[0x000:0x010] = b"SEGADISCSYSTEM  "
    area[0x010:0x01B] = volume.ljust(11)[:11].encode("ascii")
    area[0x020:0x02B] = b"SEGARECOMP "
    ip_addr = 0x200
    ip_size = (len(ip) + 1) & ~1
    sp_addr = 0x800 if ip_addr + ip_size <= 0x800 else 0x1000  # the US-BIOS variant: IP spills into sector 1
    sp_size = (len(sp) + 1) & ~1
    assert ip_addr + ip_size <= sp_addr and sp_addr + sp_size <= len(area)
    struct.pack_into(">IIII", area, 0x030, ip_addr, ip_size, 0, 0x1000)
    struct.pack_into(">IIII", area, 0x040, sp_addr, sp_size, 0, 0x1000)
    area[0x100:0x110] = b"SEGA MEGA DRIVE "
    area[0x180:0x18E] = b"GM SEGARECOMP "
    area[0x1F0:0x1F3] = b"U  "
    area[ip_addr:ip_addr + len(ip)] = ip
    area[sp_addr:sp_addr + len(sp)] = sp
    return bytes(area), {"ip": [ip_addr, ip_size, 0], "sp": [sp_addr, sp_size, 0]}


def raw_sector(lba, user):
    msf = lba + 150
    header = bytes([0x00] + [0xFF] * 10 + [0x00, bcd(msf // 75 // 60), bcd(msf // 75 % 60), bcd(msf % 75), 0x01])
    return header + user + bytes(288)  # EDC/ECC are not generated (and not checked: contract section 13)


def build(fixture, out, variant="cooked", audio=True, **options):
    ip, sp, data, expected = FIXTURES[fixture](options)
    area, extents = system_area(ip, sp, fixture.upper()[:11])
    rng = random.Random(0x5E6062)
    users = []
    for lba in range(TRACK1_SECTORS):
        if lba < 16:
            users.append(area[lba * 2048:(lba + 1) * 2048])
        elif lba in data:
            users.append(data[lba])
        else:
            users.append(bytes(rng.randrange(256) for _ in range(2048)))
    out.mkdir(parents=True, exist_ok=True)
    track1 = b"".join(raw_sector(lba, u) if variant == "raw" else u for lba, u in enumerate(users))
    (out / "track01.bin").write_bytes(track1)
    mode = "MODE1/2352" if variant == "raw" else "MODE1/2048"
    cue = 'FILE "track01.bin" BINARY\n  TRACK 01 %s\n    INDEX 01 00:00:00\n' % mode
    if audio:
        audio_bytes = bytes((i * 13 + 7) & 0xFF for i in range(AUDIO_SECTORS * 2352))
        (out / "track02.bin").write_bytes(audio_bytes)
        cue += 'FILE "track02.bin" BINARY\n  TRACK 02 AUDIO\n    PREGAP 00:02:00\n    INDEX 01 00:00:00\n'
    (out / "disc.cue").write_text(cue, newline="\n")  # byte-identical on every host (no CRLF translation)
    expected = dict(expected, extents=extents, ip_size=len(ip), sp_size=len(sp), audio_first_lba=TRACK1_SECTORS + AUDIO_PREGAP,
                    track1_sectors=TRACK1_SECTORS)
    return expected


def files_digest(directory):
    return {p.name: {"size": p.stat().st_size, "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
            for p in sorted(directory.iterdir())}


MANIFEST_CASES = [(name, variant) for name in sorted(FIXTURES) for variant in ("cooked", "raw")]


def manifest_now(tmp):
    entries = {}
    for name, variant in MANIFEST_CASES:
        directory = tmp / (name + "-" + variant)
        build(name, directory, variant)
        entries[name + "/" + variant] = files_digest(directory)
    return {"schema": SCHEMA, "fixtures": entries}


def options_from(args):
    options = {}
    if args.seed is not None:
        options["seed"] = args.seed
    if args.data_flip is not None:
        options["data_flip"] = args.data_flip
    if args.cd_lba is not None:
        options["cd_lba"] = args.cd_lba
    if args.dma_dest is not None:
        options["dma_dest"] = args.dma_dest
    if args.bad_checksum:
        options["bad_checksum"] = True
    if args.abort_dma:
        options["abort_dma"] = True
    if args.bad_cdc_register:
        options["bad_cdc_register"] = True
    if args.module_case is not None:
        options["module_case"] = args.module_case
    if args.iso_defect is not None:
        options["iso_defect"] = args.iso_defect
    return options


def main(argv=None):
    import tempfile
    parser = argparse.ArgumentParser()
    parser.add_argument("command", nargs="?", choices=["build", "describe"])
    parser.add_argument("--fixture", choices=sorted(FIXTURES))
    parser.add_argument("--out", type=pathlib.Path)
    parser.add_argument("--variant", choices=["cooked", "raw"], default="cooked")
    parser.add_argument("--seed", type=int)
    parser.add_argument("--data-flip", type=int)
    parser.add_argument("--cd-lba", type=int)
    parser.add_argument("--dma-dest", type=int)
    parser.add_argument("--bad-checksum", action="store_true")
    parser.add_argument("--abort-dma", action="store_true")
    parser.add_argument("--bad-cdc-register", action="store_true")
    parser.add_argument("--no-audio", action="store_true")
    parser.add_argument("--module-case", choices=MODULE_CASES)
    parser.add_argument("--iso-defect", choices=ISO_DEFECTS)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--write-manifest", action="store_true")
    args = parser.parse_args(argv)
    if args.check or args.write_manifest:
        with tempfile.TemporaryDirectory() as tmp:
            now = manifest_now(pathlib.Path(tmp))
        if args.write_manifest:
            MANIFEST.write_text(json.dumps(now, indent=1, sort_keys=True) + "\n", newline="\n")
            print("wrote", MANIFEST)
            return 0
        committed = json.loads(MANIFEST.read_text())
        if committed != now:
            print("FAIL: generated fixture discs differ from the committed manifest")
            return 1
        print("sega-cd fixture discs reproduce the committed manifest (%d images)" % len(now["fixtures"]))
        return 0
    if args.command is None or args.fixture is None:
        parser.error("build/describe needs --fixture")
    options = options_from(args)
    if args.command == "describe":
        with tempfile.TemporaryDirectory() as tmp:
            expected = build(args.fixture, pathlib.Path(tmp), args.variant, not args.no_audio, **options)
        print(json.dumps(expected, sort_keys=True))
        return 0
    if args.out is None:
        parser.error("build needs --out")
    expected = build(args.fixture, args.out, args.variant, not args.no_audio, **options)
    print(json.dumps(expected, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
