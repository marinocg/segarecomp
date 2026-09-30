#!/usr/bin/env python3
"""Deterministic Master System fixture ROM builder (SEG-009-T001 skeleton, ADR 0064).

Stdlib only. Every byte this tool emits is project-authored: Z80 code assembled from the sources
below, fill bytes, per-bank marker bytes and a 16-byte `TMR SEGA` header. No commercial,
BIOS or oracle-derived byte is read or reproduced.

Contract (docs/architecture/master-system-machine-contract.md, "Fixture builder"):
  * a fixture is (name, rom_size, declared mapper identity, assembly source, marker bytes);
  * the ROM bytes are regenerated at test time and are never committed; only the committed
    manifest `tests/fixtures/sms-fixture-roms.json` records each fixture's size, declared mapper
    identity and SHA-256, so `--check` proves byte-for-byte reproducibility on every host;
  * the declared mapper identity travels with the ROM in the manifest (and in `--declaration`
    output). It is one of the explicit mapper declaration sources of the mapper identity contract:
    the header written here identifies platform/region only and never the mapper.

Header (SMS Power! "ROM header", https://www.smspower.org/Development/ROMHeader):
  $7FF0 "TMR SEGA"; $7FF8-$7FF9 reserved ($00 $00 here); $7FFA-$7FFB little-endian checksum;
  $7FFC-$7FFE product code/version (fixture: 0); $7FFF high nibble region ($4 = SMS export),
  low nibble ROM size code ($C 32 KiB, $E 64 KiB, $F 128 KiB, $0 256 KiB, $1 512 KiB).
  The checksum is informational (the baseline never accepts or rejects on it): it is the 16-bit
  sum of every byte of the size-code range excluding the header bytes $7FF0-$7FFF.

The embedded assembler is a small two-pass NMOS Z80 assembler for the documented forms used by
fixtures (Zilog UM0080 encodings). It exists so T008 can extend fixtures without an external
toolchain; unknown mnemonics or operand forms are hard errors, never guesses.

Usage:
  sms_fixture_rom.py --out DIR [--fixture NAME]...   write <name>.sms and <name>.mapper.json
  sms_fixture_rom.py --check                         verify every fixture against the manifest
  sms_fixture_rom.py --write-manifest                regenerate the committed manifest
"""
import argparse
import hashlib
import json
import pathlib
import re
import sys

SCHEMA = 1
REPO = pathlib.Path(__file__).resolve().parent.parent
MANIFEST = REPO / "tests" / "fixtures" / "sms-fixture-roms.json"

BANK_SIZE = 0x4000
HEADER_OFFSET = 0x7FF0
SIZE_CODES = {0x8000: 0xC, 0x10000: 0xE, 0x20000: 0xF, 0x40000: 0x0, 0x80000: 0x1}
REGION_SMS_EXPORT = 0x4
MAPPER_IDENTITIES = ("sega", "rom_only")


class AsmError(Exception):
    pass


R8 = {"b": 0, "c": 1, "d": 2, "e": 3, "h": 4, "l": 5, "(hl)": 6, "a": 7}
RP = {"bc": 0, "de": 1, "hl": 2, "sp": 3}
RP2 = {"bc": 0, "de": 1, "hl": 2, "af": 3}
CC = {"nz": 0, "z": 1, "nc": 2, "c": 3, "po": 4, "pe": 5, "p": 6, "m": 7}
JR_CC = {"nz": 0x20, "z": 0x28, "nc": 0x30, "c": 0x38}
ALU = {"add": 0, "adc": 1, "sub": 2, "sbc": 3, "and": 4, "xor": 5, "or": 6, "cp": 7}
FIXED = {"nop": [0x00], "halt": [0x76], "di": [0xF3], "ei": [0xFB], "ret": [0xC9], "exx": [0xD9],
         "reti": [0xED, 0x4D], "retn": [0xED, 0x45], "ldir": [0xED, 0xB0], "otir": [0xED, 0xB3],
         "inir": [0xED, 0xB2], "neg": [0xED, 0x44], "cpl": [0x2F], "scf": [0x37], "ccf": [0x3F]}
EXPR_OK = re.compile(r"^[0-9A-Za-z_+\-*()<>&| ~$]+$")


class Assembler:
    """Two-pass assembler producing a sparse {address: byte} image for one 16-bit window."""

    def __init__(self, source):
        self.lines = []
        for number, raw in enumerate(source.splitlines(), 1):
            text = raw.split(";", 1)[0].strip()
            if text:
                self.lines.append((number, text))

    def value(self, text, labels, final):
        text = text.strip()
        if not EXPR_OK.match(text):
            raise AsmError("bad expression %r" % text)
        names = [t for t in re.findall(r"0[xX][0-9A-Fa-f]+|\d+|[A-Za-z_]\w*", text) if t[0].isalpha() or t[0] == "_"]
        env = {}
        for name in names:
            if name in ("lo", "hi"):
                continue
            if name in labels:
                env[name] = labels[name]
            elif final:
                raise AsmError("undefined symbol %r" % name)
            else:
                env[name] = 0
        env["lo"] = lambda v: v & 0xFF
        env["hi"] = lambda v: (v >> 8) & 0xFF
        return int(eval(text.replace("$", "_pc_"), {"__builtins__": {}}, dict(env, _pc_=self.pc)))  # noqa: S307

    def encode(self, mnem, ops, labels, final):
        v = lambda t: self.value(t, labels, final)  # noqa: E731
        n8 = lambda t: v(t) & 0xFF  # noqa: E731

        def n16(t):
            x = v(t) & 0xFFFF
            return [x & 0xFF, x >> 8]

        def rel(t):
            d = v(t) - (self.pc + 2)
            if final and not -128 <= d <= 127:
                raise AsmError("relative jump out of range")
            return d & 0xFF

        mem = lambda t: t.startswith("(") and t.endswith(")")  # noqa: E731
        o = ops
        if mnem in FIXED and not o:
            return list(FIXED[mnem])
        if mnem == "im" and len(o) == 1:
            return [0xED, {"0": 0x46, "1": 0x56, "2": 0x5E}[o[0]]]
        if mnem == "rst" and len(o) == 1:
            t = v(o[0])
            if t not in range(0, 0x40, 8):
                raise AsmError("bad rst target")
            return [0xC7 | t]
        if mnem == "ld" and len(o) == 2:
            d, s = o
            if d in R8 and s in R8 and not (d == s == "(hl)"):
                return [0x40 | (R8[d] << 3) | R8[s]]
            if d == "a" and s in ("(bc)", "(de)"):
                return [0x0A if s == "(bc)" else 0x1A]
            if s == "a" and d in ("(bc)", "(de)"):
                return [0x02 if d == "(bc)" else 0x12]
            if d == "i" and s == "a":
                return [0xED, 0x47]
            if d == "a" and s == "i":
                return [0xED, 0x57]
            if d == "sp" and s == "hl":
                return [0xF9]
            if d == "a" and mem(s):
                return [0x3A] + n16(s[1:-1])
            if s == "a" and mem(d):
                return [0x32] + n16(d[1:-1])
            if d == "hl" and mem(s):
                return [0x2A] + n16(s[1:-1])
            if s == "hl" and mem(d):
                return [0x22] + n16(d[1:-1])
            if d in RP and not mem(s):
                return [0x01 | (RP[d] << 4)] + n16(s)
            if d in R8 and not mem(s):
                return [0x06 | (R8[d] << 3), n8(s)]
        if mnem in ("push", "pop") and len(o) == 1 and o[0] in RP2:
            return [(0xC5 if mnem == "push" else 0xC1) | (RP2[o[0]] << 4)]
        if mnem in ("inc", "dec") and len(o) == 1:
            if o[0] in R8:
                return [(0x04 if mnem == "inc" else 0x05) | (R8[o[0]] << 3)]
            if o[0] in RP:
                return [(0x03 if mnem == "inc" else 0x0B) | (RP[o[0]] << 4)]
        if mnem == "add" and len(o) == 2 and o[0] == "hl" and o[1] in RP:
            return [0x09 | (RP[o[1]] << 4)]
        if mnem in ALU:
            src = o[1] if len(o) == 2 and o[0] == "a" else (o[0] if len(o) == 1 else None)
            if src is not None:
                if src in R8:
                    return [0x80 | (ALU[mnem] << 3) | R8[src]]
                return [0xC6 | (ALU[mnem] << 3), n8(src)]
        if mnem == "jp":
            if o == ["(hl)"]:
                return [0xE9]
            if len(o) == 1:
                return [0xC3] + n16(o[0])
            if len(o) == 2 and o[0] in CC:
                return [0xC2 | (CC[o[0]] << 3)] + n16(o[1])
        if mnem == "call":
            if len(o) == 1:
                return [0xCD] + n16(o[0])
            if len(o) == 2 and o[0] in CC:
                return [0xC4 | (CC[o[0]] << 3)] + n16(o[1])
        if mnem == "ret" and len(o) == 1 and o[0] in CC:
            return [0xC0 | (CC[o[0]] << 3)]
        if mnem == "jr":
            if len(o) == 1:
                return [0x18, rel(o[0])]
            if len(o) == 2 and o[0] in JR_CC:
                return [JR_CC[o[0]], rel(o[1])]
        if mnem == "djnz" and len(o) == 1:
            return [0x10, rel(o[0])]
        if mnem == "out" and len(o) == 2:
            if o[0] == "(c)" and o[1] in R8 and o[1] != "(hl)":
                return [0xED, 0x41 | (R8[o[1]] << 3)]
            if o[1] == "a" and mem(o[0]):
                return [0xD3, n8(o[0][1:-1])]
        if mnem == "in" and len(o) == 2:
            if o[1] == "(c)" and o[0] in R8 and o[0] != "(hl)":
                return [0xED, 0x40 | (R8[o[0]] << 3)]
            if o[0] == "a" and mem(o[1]):
                return [0xDB, n8(o[1][1:-1])]
        raise AsmError("unsupported instruction %s %s" % (mnem, ",".join(o)))

    def run(self, final, labels):
        image = {}
        self.listing = []  # (address, mnemonic, bytes) for every encoded instruction (not data)
        self.pc = 0
        for number, text in self.lines:
            try:
                while True:
                    m = re.match(r"^([A-Za-z_][A-Za-z_0-9]*):\s*(.*)$", text)
                    if not m:
                        break
                    if not final:
                        if m.group(1) in labels and labels[m.group(1)] != self.pc:
                            raise AsmError("duplicate label %r" % m.group(1))
                        labels[m.group(1)] = self.pc
                    text = m.group(2)
                if not text:
                    continue
                mnem, _, rest = text.partition(" ")
                mnem = mnem.lower()
                if mnem == ".org":
                    self.pc = self.value(rest, labels, True)
                    continue
                if mnem in (".db", ".dw"):
                    out = []
                    for item in split_ops(rest):
                        x = self.value(item, labels, final)
                        out += [x & 0xFF] if mnem == ".db" else [x & 0xFF, (x >> 8) & 0xFF]
                elif mnem == ".fill":
                    count, fill = [self.value(x, labels, True) for x in split_ops(rest)]
                    out = [fill & 0xFF] * count
                else:
                    ops = [x.lower() for x in split_ops(rest)]
                    out = self.encode(mnem, ops, labels, final)
                    self.listing.append((self.pc, mnem, list(out)))
                for byte in out:
                    if not 0 <= self.pc <= 0xFFFF:
                        raise AsmError("address out of range")
                    if final and self.pc in image:
                        raise AsmError("overlapping output at 0x%04X" % self.pc)
                    image[self.pc] = byte & 0xFF
                    self.pc += 1
            except AsmError as error:
                raise AsmError("line %d: %s (%s)" % (number, error, text)) from None
        return image

    def assemble(self):
        labels = {}
        self.run(False, labels)
        return self.run(True, labels), labels


def split_ops(text):
    parts, depth, cur = [], 0, ""
    for ch in text:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        parts.append(cur.strip())
    return parts


def write_header(rom, size):
    code = SIZE_CODES[size]
    rom[HEADER_OFFSET:HEADER_OFFSET + 8] = b"TMR SEGA"
    rom[0x7FF8:0x7FFA] = b"\x00\x00"
    rom[0x7FFC:0x7FFF] = b"\x00\x00\x00"
    rom[0x7FFF] = (REGION_SMS_EXPORT << 4) | code
    total = (sum(rom[:HEADER_OFFSET]) + sum(rom[0x8000:size])) & 0xFFFF
    rom[0x7FFA] = total & 0xFF
    rom[0x7FFB] = total >> 8


def build_rom(size, code_image, markers, fill=0xFF):
    """code_image: {logical address in 0x0000-0x7FEF: byte} placed at the same ROM offset.
    markers: {rom offset: byte}. Header bytes are always owned by the builder."""
    if size not in SIZE_CODES:
        raise AsmError("unsupported fixture ROM size 0x%X" % size)
    rom = bytearray([fill]) * size
    for address, byte in sorted(code_image.items()):
        if address >= HEADER_OFFSET:
            raise AsmError("code at 0x%04X collides with the header area" % address)
        rom[address] = byte
    for offset, byte in sorted(markers.items()):
        if HEADER_OFFSET <= offset < 0x8000 or offset >= size:
            raise AsmError("marker offset 0x%X is invalid" % offset)
        if offset < HEADER_OFFSET and offset in code_image and code_image[offset] != byte:
            raise AsmError("marker at 0x%X collides with code" % offset)
        rom[offset] = byte
    write_header(rom, size)
    return bytes(rom)


# --- fixtures -----------------------------------------------------------------------------------

TRIVIAL_SOURCE = """
; Trivial deterministic fixture: IM1 setup, mode-4 VDP init, backdrop colour, display on, idle.
.org 0x0000
        di
        im 1
        ld sp,0xDFF0
        jp main
.org 0x0038
        push af
        in a,(0xBF)             ; acknowledge: status read deasserts /INT
        pop af
        ei
        reti
.org 0x0066
        retn
.org 0x0080
main:   ld hl,vdp_regs
        ld b,vdp_regs_end-vdp_regs
        ld c,0xBF
        otir                    ; registers 0,1,2,5,6,7,10 as (value, 0x80|reg) pairs
        ld a,0x10
        out (0xBF),a
        ld a,0xC0
        out (0xBF),a            ; CRAM write address 0x10 (sprite palette entry 0 = backdrop)
        ld a,0x30
        out (0xBE),a            ; backdrop = blue (%110000)
        ld a,0xE0
        out (0xBF),a
        ld a,0x81
        out (0xBF),a            ; register 1 = display on, frame interrupt enabled
        ei
idle:   halt
        jr idle
vdp_regs:
        .db 0x06,0x80, 0x80,0x81, 0xFF,0x82, 0xFF,0x85, 0xFB,0x86, 0x00,0x87, 0xFF,0x8A
vdp_regs_end:
"""

# Oracle smoke program (ADR 0062). Results are written to RAM 0xC100-0xC1FF; the libretro host
# (tests/sms_oracle/libretro_host.c) reads them and drives the handshake at 0xC0F5 (phase, written
# by the program) / 0xC0F6 (acknowledge, written by the host). Every expectation lives in
# tests/sms_oracle_smoke_test.py next to its public citation, never here.
ORACLE_SMOKE_SOURCE = """
.org 0x0000
        di
        im 1
        ld sp,0xDFF0
        jp main
.org 0x0038
        jp isr
.org 0x0066
        push af
        ld a,(0xC0F2)
        inc a
        ld (0xC0F2),a
        pop af
        retn
; ---- slot-0 mapper probe: must live in the fixed first 1 KiB (0x0000-0x03FF) -----------------
.org 0x0100
slot0_probe:
        ld a,3
        ld (0xFFFD),a           ; slot 0 <- bank 3
        ld a,(0x0200)
        ld (0xC121),a           ; fixed first 1 KiB: still bank 0 byte
        ld a,(0x0400)
        ld (0xC122),a           ; 0x0400 now bank 3
        ld a,(0x2000)
        ld (0xC123),a           ; bank 3 marker
        xor a
        ld (0xFFFD),a           ; restore slot 0 <- bank 0
        ret
.org 0x0200
        .db 0xB0                ; bank-0 byte inside the fixed 1 KiB
.org 0x0400
        .db 0xC0                ; bank-0 byte at 0x0400 (other banks carry 0xC0+bank)
; ---- main program -------------------------------------------------------------------------------
.org 0x0410
main:
        ld hl,0xC000
        ld de,0xC001
        ld bc,0x1EFF
        ld (hl),0
        ldir                    ; clear RAM 0xC000-0xDEFF (leave stack page and mapper mirror)
        ld a,0x5A
        ld (0xC100),a           ; result block magic
        ; -- initial mapper state (before any mapper write)
        ld a,(0x6000)
        ld (0xC11D),a           ; slot 1 bank marker
        ld a,(0xA000)
        ld (0xC11E),a           ; slot 2 bank marker
        ; -- undecoded / write-only port reads (A=0 so the full port is 0x00nn)
        xor a
        in a,(0x00)
        ld (0xC101),a
        xor a
        in a,(0x3E)
        ld (0xC102),a
        ; -- nationalization readback
        ld a,0xF5
        out (0x3F),a
        in a,(0xDD)
        and 0xC0
        ld (0xC103),a
        ld a,0x55
        out (0x3F),a
        in a,(0xDD)
        and 0xC0
        ld (0xC104),a
        ld a,0xFF
        out (0x3F),a
        ; -- idle controller reads and mirrors
        in a,(0xDC)
        ld (0xC105),a
        in a,(0xC0)
        ld (0xC106),a
        in a,(0xDD)
        ld (0xC107),a
        ; -- VDP init (display blanked, no interrupts)
        call vdp_init
        call vram_clear
        ; -- control latch reset by a status read
        in a,(0xBF)
        ld a,0x12
        out (0xBF),a            ; first byte only
        in a,(0xBF)             ; resets the first/second byte flag
        ld a,0x34
        out (0xBF),a
        ld a,0x40
        out (0xBF),a            ; VRAM write 0x0034
        ld a,0xA5
        out (0xBE),a
        ld hl,0x0034
        call vram_read_setup
        in a,(0xBE)
        ld (0xC108),a
        ; -- first control byte updates the address low byte immediately
        ld a,0x40
        out (0xBF),a
        ld a,0x40
        out (0xBF),a            ; VRAM write 0x0040
        ld a,0x50
        out (0xBF),a            ; first byte only: address low <- 0x50
        in a,(0xBF)             ; reset the flag; code stays VRAM write
        ld a,0x3C
        out (0xBE),a
        ld hl,0x0050
        call vram_read_setup
        in a,(0xBE)
        ld (0xC109),a
        ld hl,0x0040
        call vram_read_setup
        in a,(0xBE)
        ld (0xC10A),a
        ; -- read buffer loaded by data-port writes
        ld hl,0x0100
        call vram_write_setup
        ld a,0x11
        out (0xBE),a
        ld a,0x22
        out (0xBE),a
        ld a,0x33
        out (0xBE),a
        ld hl,0x0100
        call vram_write_setup
        ld a,0x44
        out (0xBE),a
        in a,(0xBE)
        ld (0xC10B),a
        in a,(0xBE)
        ld (0xC10C),a
        ; -- address register wraps past 0x3FFF
        ld hl,0x3FFF
        call vram_write_setup
        ld a,0x77
        out (0xBE),a
        ld a,0x88
        out (0xBE),a
        ld hl,0x0000
        call vram_read_setup
        in a,(0xBE)
        ld (0xC10D),a
        ld hl,0x3FFF
        call vram_read_setup
        in a,(0xBE)
        ld (0xC10E),a
        ld hl,0x0000
        call vram_write_setup
        xor a
        out (0xBE),a
        ; -- mirrors: control at 0xBD, data at 0x80, V counter at 0x40
        ld a,0x00
        out (0xBD),a
        ld a,0x41
        out (0xBD),a            ; VRAM write 0x0100 via mirror
        ld a,0x9C
        out (0x80),a            ; data via mirror
        ld hl,0x0100
        call vram_read_setup
        in a,(0xBE)
        ld (0xC10F),a
        ; -- frame interrupt flag (polled, interrupts disabled)
        ld a,0xC0
        call set_reg1           ; display on, frame IRQ disabled
        call wait_line0
        in a,(0xBF)             ; clear pending flags
poll_int:
        in a,(0xBF)
        and 0x80
        jr z,poll_int
        in a,(0x7E)
        ld (0xC110),a           ; V counter when INT first observed
        in a,(0xBF)
        ld (0xC111),a           ; second read: flags cleared by the first
        ; -- frame interrupt via IM1
        ld a,1
        ld (0xC0F0),a
        call wait_line0
        in a,(0xBF)
        ld a,0xE0
        call set_reg1
        ei
        halt
        di
        ld a,0xC0
        call set_reg1
        ; -- line interrupts: register 10 = 15, counted over one frame
        ld a,0x0F
        ld b,0x8A
        call set_reg
        ld a,0x16
        ld b,0x80
        call set_reg            ; mode 4 + line interrupt enable
        ld a,2
        ld (0xC0F0),a
        call wait_line_c8
        in a,(0xBF)
        xor a
        ld (0xC115),a
        ld (0xC116),a
        ld (0xC117),a
        ei
        call wait_line0
        call wait_line_c8
        di
        ld a,0x06
        ld b,0x80
        call set_reg
        ld a,0xFF
        ld b,0x8A
        call set_reg
        in a,(0xBF)
        ; -- IM2 with the SMS 2 data bus (vector low byte 0xFF)
        ld a,lo(isr_im2)
        ld (0xC2FF),a
        ld a,hi(isr_im2)
        ld (0xC300),a
        ld a,0xC2
        ld i,a
        im 2
        call wait_line0
        in a,(0xBF)
        ld a,0xE0
        call set_reg1
        ei
        halt
        di
        im 1
        ld a,0xC0
        call set_reg1
        in a,(0xBF)
        ; -- V counter jump: first backward step other than 0xFF -> 0x00
        call wait_line0
        in a,(0x7E)
        ld b,a
vloop:  in a,(0x7E)
        cp b
        jr z,vloop
        jr nc,vnext
        ld c,a
        ld a,b
        cp 0xFF
        ld a,c
        jr nz,vfound
vnext:  ld b,a
        jr vloop
vfound: ld a,b
        ld (0xC119),a
        ld a,c
        ld (0xC11A),a
        ; -- sprite overflow: nine transparent sprites on one line
        ld hl,0x3F00
        call vram_write_setup
        ld b,9
spry:   ld a,0x3F
        out (0xBE),a
        djnz spry
        ld a,0xD0
        out (0xBE),a
        ld hl,0x3F80
        call vram_write_setup
        ld b,9
        ld c,0
sprx:   ld a,c
        out (0xBE),a
        add a,16
        ld c,a
        xor a
        out (0xBE),a            ; tile 0 (all transparent)
        djnz sprx
        call wait_line_c8
        in a,(0xBF)
        call wait_line0
        call wait_line_c8
        in a,(0xBF)
        ld (0xC11B),a
        ; -- sprite collision: two overlapping opaque sprites
        ld hl,0x0020
        call vram_write_setup
        ld b,8
patt:   ld a,0xFF
        out (0xBE),a
        xor a
        out (0xBE),a
        out (0xBE),a
        out (0xBE),a
        djnz patt               ; pattern 1: colour 1 everywhere
        ld hl,0x3F00
        call vram_write_setup
        ld a,0x3F
        out (0xBE),a
        out (0xBE),a
        ld a,0xD0
        out (0xBE),a
        ld hl,0x3F80
        call vram_write_setup
        ld a,0x20
        out (0xBE),a
        ld a,1
        out (0xBE),a
        ld a,0x24
        out (0xBE),a
        ld a,1
        out (0xBE),a
        call wait_line_c8
        in a,(0xBF)
        call wait_line0
        call wait_line_c8
        in a,(0xBF)
        ld (0xC11C),a
        ld hl,0x3F00
        call vram_write_setup
        ld a,0xD0
        out (0xBE),a            ; sprites off for the render phase
        ; -- mapper: masking, write-through, fixed first 1 KiB
        ld a,0x0A
        ld (0xFFFF),a           ; bank 10 of an 8-bank ROM
        ld a,(0xA000)
        ld (0xC11F),a
        ld a,(0xDFFF)
        ld (0xC120),a
        ld a,5
        ld (0xFFFE),a
        ld a,(0x6000)
        ld (0xC124),a
        ld a,(0xFFFE)
        ld (0xC125),a
        call slot0_probe
        ld a,1
        ld (0xFFFE),a
        ld a,2
        ld (0xFFFF),a
        ; -- render phase A: CRAM via the 0x20-0x3F mirror, two opaque tiles at row 0
        ld hl,0x0040
        call vram_write_setup
        ld b,8
patt2:  ld a,0xFF
        out (0xBE),a
        xor a
        out (0xBE),a
        out (0xBE),a
        out (0xBE),a
        djnz patt2              ; pattern 2: colour 1
        ld hl,0x3800
        call vram_write_setup
        ld a,2
        out (0xBE),a
        xor a
        out (0xBE),a
        ld a,2
        out (0xBE),a
        xor a
        out (0xBE),a            ; name table (0,0) and (1,0) -> pattern 2
        ld a,0x20
        out (0xBF),a
        ld a,0xC0
        out (0xBF),a            ; CRAM address 0x20 (mirror of 0x00)
        xor a
        out (0xBE),a            ; background colour 0 = black
        ld a,0x3F
        out (0xBE),a            ; background colour 1 = white (address 0x21 -> 0x01)
        ld a,0x10
        out (0xBF),a
        ld a,0xC0
        out (0xBF),a            ; CRAM 0x10: sprite palette colour 0
        ld a,0x03
        out (0xBE),a            ; red
        xor a
        ld b,0x87
        call set_reg            ; backdrop = sprite palette entry 0
        ld a,0xC0
        call set_reg1
        ld a,1
        call handshake
        ; -- render phase B: left column blank
        ld a,0x26
        ld b,0x80
        call set_reg
        ld a,2
        call handshake
        ; -- pause button: count NMIs while the host presses/holds/releases
        xor a
        ld (0xC0F2),a
        ld a,3
        call handshake
        ld a,(0xC0F2)
        ld (0xC126),a
        ; -- controller: host holds UP
        ld a,0xA5
        ld (0xC1FF),a
        ld a,4
        ld (0xC0F5),a
final:  in a,(0xDC)
        ld (0xC127),a
        in a,(0xDD)
        ld (0xC128),a
        jr final

; ---- subroutines --------------------------------------------------------------------------------
handshake:                       ; phase in A; wait for the host acknowledge
        ld (0xC0F5),a
        ld b,a
hs_wait:
        ld a,(0xC0F6)
        cp b
        jr nz,hs_wait
        ret
set_reg:                         ; A = value, B = 0x80|register
        out (0xBF),a
        ld a,b
        out (0xBF),a
        ret
set_reg1:
        ld b,0x81
        jr set_reg
vram_write_setup:                ; HL = address
        ld a,l
        out (0xBF),a
        ld a,h
        or 0x40
        out (0xBF),a
        ret
vram_read_setup:
        ld a,l
        out (0xBF),a
        ld a,h
        out (0xBF),a
        ret
wait_line0:
        in a,(0x7E)
        or a
        jr nz,wait_line0
        ret
wait_line_c8:
        in a,(0x7E)
        cp 0xC8
        jr nz,wait_line_c8
        ret
vdp_init:
        ld hl,init_regs
        ld b,init_regs_end-init_regs
        ld c,0xBF
        otir
        ret
init_regs:
        .db 0x06,0x80, 0x80,0x81, 0xFF,0x82, 0xFF,0x83, 0xFF,0x84, 0xFF,0x85, 0xFB,0x86
        .db 0x00,0x87, 0x00,0x88, 0x00,0x89, 0xFF,0x8A
init_regs_end:
vram_clear:
        ld hl,0x0000
        call vram_write_setup
        ld bc,0x4000
vc_loop:
        xor a
        out (0xBE),a
        dec bc
        ld a,b
        or c
        jr nz,vc_loop
        ret
isr:
        push af
        ld a,(0xC0F0)
        cp 1
        jr z,isr_frame
        cp 2
        jr z,isr_line
        in a,(0xBF)
        pop af
        ei
        reti
isr_frame:
        in a,(0xBF)
        ld (0xC112),a           ; status read inside the IM1 handler
        in a,(0x7E)
        ld (0xC113),a           ; V counter inside the handler
        xor a
        ld (0xC0F0),a
        pop af
        ei
        reti
isr_line:
        in a,(0xBF)
        ld a,(0xC115)
        or a
        jr nz,isr_line_n
        in a,(0x7E)
        ld (0xC116),a           ; V counter at the first line interrupt
isr_line_n:
        ld a,(0xC115)
        cp 1
        jr nz,isr_line_c
        in a,(0x7E)
        ld (0xC117),a           ; V counter at the second line interrupt
isr_line_c:
        ld a,(0xC115)
        inc a
        ld (0xC115),a
        pop af
        ei
        reti
isr_im2:
        push af
        in a,(0xBF)
        ld a,0x2A
        ld (0xC118),a
        pop af
        ei
        reti
"""


def fixture_trivial():
    image, _ = Assembler(TRIVIAL_SOURCE).assemble()
    return build_rom(0x8000, image, {}), "rom_only"


def fixture_oracle_smoke():
    image, _ = Assembler(ORACLE_SMOKE_SOURCE).assemble()
    if max(image) >= 0x2000:
        raise AsmError("oracle smoke code must stay below the bank marker at 0x2000")
    size = 0x20000
    markers = {}
    for bank in range(size // BANK_SIZE):
        markers[bank * BANK_SIZE + 0x2000] = bank
        if bank:
            markers[bank * BANK_SIZE + 0x0200] = 0xB0 + bank
            markers[bank * BANK_SIZE + 0x0400] = 0xC0 + bank
    return build_rom(size, image, markers), "sega"


# Bank-crossing fixture (SEG-009-T002). Distinct code lives at the SAME logical address under different banks so a
# generated-native run must dispatch one logical PC under two image identities after a mapper write. Result bytes go to
# RAM 0xC200-0xC2FF. `BANK_CROSSING_MAIN` is the normal entry (PC 0); test hosts may start at other labelled entries
# (0x0200 jumps into RAM; 0x7FFF is a slot-boundary-straddling instruction when bank 2 is mapped into slot 1).
BANK_CROSSING_MAIN = """
.org 0x0000
        di
        im 1
        ld sp,0xDFF0
        jp main
.org 0x0038
        reti
.org 0x0066
        retn
.org 0x0100
main:   call 0x0500             ; slot 0 still bank 0
        call 0x4100             ; slot 1 = bank 1
        ld a,3
        ld (0xFFFE),a           ; slot 1 <- bank 3
        call 0x4100             ; the same logical PC, now bank 3 code
        ld a,2
        ld (0xFFFD),a           ; slot 0 <- bank 2
        call 0x0500             ; slot 0 now bank 2 code
        call 0x0210             ; fixed first 1 KiB is unaffected by slot 0
        ld a,0x5A
        ld (0xC2FF),a
done:   halt
        jr done
.org 0x0200
ram_jump:
        ld hl,0xC300
        jp (hl)                 ; code in work RAM fails closed
.org 0x0210
        ld a,0x44
        ld (0xC204),a
        ret
"""
BANK_CROSSING_ROUTINES = [  # (bank, logical origin of the routine as seen from `slot_base`, slot base, source)
    (0, 0x0500, 0x0000, "        ld a,0x0F\n        ld (0xC203),a\n        ret\n"),
    (1, 0x4100, 0x4000, "        ld a,0x11\n        ld (0xC200),a\n        ret\n"),
    (2, 0x0500, 0x0000, "        ld a,0x22\n        ld (0xC202),a\n        ret\n"),
    (3, 0x4100, 0x4000, "        ld a,0x33\n        ld (0xC201),a\n        ret\n"),
]


def fixture_bank_crossing():
    image, _ = Assembler(BANK_CROSSING_MAIN).assemble()
    size = 0x10000
    markers = {}
    for bank, origin, slot_base, source in BANK_CROSSING_ROUTINES:
        routine, _ = Assembler(".org 0x%04X\n%s" % (origin, source)).assemble()
        for address, byte in routine.items():
            markers[bank * BANK_SIZE + (address - slot_base)] = byte
    markers[2 * BANK_SIZE + 0x3FFF] = 0x3E  # last byte of bank 2: a two-byte instruction straddling the slot edge
    return build_rom(size, image, markers), "sega"


FIXTURES = {"trivial": fixture_trivial, "oracle_smoke": fixture_oracle_smoke, "bank_crossing": fixture_bank_crossing}


def build(name):
    rom, mapper = FIXTURES[name]()
    if mapper not in MAPPER_IDENTITIES:
        raise AsmError("fixture %s declares unknown mapper identity %r" % (name, mapper))
    return rom, {"fixture": name, "size": len(rom), "mapper": mapper,
                 "sha256": hashlib.sha256(rom).hexdigest(), "declaration_source": "fixture_builder"}


def manifest_text():
    rows = [build(name)[1] for name in sorted(FIXTURES)]
    return json.dumps({"schema": SCHEMA, "fixtures": rows}, indent=2, sort_keys=True) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=pathlib.Path, help="directory for <name>.sms and <name>.mapper.json")
    ap.add_argument("--fixture", action="append", choices=sorted(FIXTURES), help="fixture to write (default all)")
    ap.add_argument("--check", action="store_true", help="fail unless fresh builds match the committed manifest")
    ap.add_argument("--write-manifest", action="store_true", help="regenerate the committed manifest")
    args = ap.parse_args(argv)
    if args.write_manifest:
        MANIFEST.write_text(manifest_text(), encoding="utf-8", newline="\n")
        return 0
    if args.check:
        fresh = manifest_text()
        again = manifest_text()
        committed = MANIFEST.read_text(encoding="utf-8") if MANIFEST.exists() else ""
        if fresh != again:
            print("sms fixture builder is not deterministic", file=sys.stderr)
            return 1
        if fresh != committed:
            print("sms fixture manifest differs from a fresh build; run --write-manifest", file=sys.stderr)
            return 1
        print("ok: %d sms fixtures reproduce the committed manifest" % len(FIXTURES))
        return 0
    if args.out is None:
        ap.error("--out, --check or --write-manifest is required")
    args.out.mkdir(parents=True, exist_ok=True)
    for name in args.fixture or sorted(FIXTURES):
        rom, meta = build(name)
        (args.out / (name + ".sms")).write_bytes(rom)
        (args.out / (name + ".mapper.json")).write_text(json.dumps(meta, indent=2, sort_keys=True) + "\n",
                                                        encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
