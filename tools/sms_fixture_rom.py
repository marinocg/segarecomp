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


# --- SEG-009-T003 scheduler fixtures ------------------------------------------------------------------------------
# rom_only 32 KiB programs (small generated code) for the generated-native scheduler tests. The INT handler acknowledges
# with a status read (port $BF) and counts in RAM $C200; the NMI handler counts in $C201.
SCHED_HANDLERS = """
.org 0x0038
        in a,(0xBF)             ; acknowledge: a status read deasserts /INT
        ld hl,0xC200
        inc (hl)
        ei
        reti
.org 0x0066
        ld hl,0xC201
        inc (hl)
        retn
"""
SCHED_IRQ_SOURCE = """
.org 0x0000
        im 1
        ld sp,0xDFF0
        ei
main:   halt                    ; woken by the frame interrupt (INT) or the pause NMI
        jr main
""" + SCHED_HANDLERS

STRADDLE_SOURCE = (
    """
.org 0x0000
        im 1
        ld sp,0xDFF0
        ei
        jp main
""" + SCHED_HANDLERS + """
.org 0x0100
main:   halt                    ; sync: the frame interrupt (line 192) wakes the probe
        ld a,0x5A
"""
    + "        out (0x7F),a\n" * 60      # 60 back-to-back 11-T PSG writes: their bus cycles cross three line starts
    + "        .db 0xDD,0xDD,0xDD,0xDD,0xDD,0xDD,0xD3,0x7F\n" * 20  # index-prefix chains (6 superseded prefixes + OUT)
    + "        in a,(0x7E)\n" * 30        # V counter reads
    + "        jp main\n"
)

PORT_OPEN_SOURCE = """
.org 0x0000
        in a,(0x00)             ; reads of $00-$3F return $FF (no device)
        ld (0xC210),a
        ld a,0xAF
        out (0x3E),a            ; cartridge + RAM enabled, BIOS off, I/O chip disabled (bit 2)
        in a,(0xC0)             ; $C0-$FF read $FF while the I/O chip is disabled, without consulting a pad device
        ld (0xC211),a
        in a,(0xDD)
        ld (0xC212),a
        out (0xC1),a            ; writes to $C0-$FF have no effect
        in a,(0x3E)
        ld (0xC213),a
done:   halt
        jr done
"""
PORT_VDP_SOURCE = """
.org 0x0000
        ld a,0x55
        ld (0xC220),a
        out (0xBF),a            ; VDP control write: no VDP device is attached -> typed stop
        ld a,0x66
        ld (0xC221),a
done:   halt
        jr done
"""
MEMCTL_BAD_SOURCE = """
.org 0x0000
        ld a,0xE3               ; cartridge disabled (bit 6 set): outside the baseline
        out (0x3E),a
done:   halt
        jr done
"""


def _rom_only(source):
    image, _ = Assembler(source).assemble()
    return build_rom(0x8000, image, {}), "rom_only"


def fixture_sched_irq():
    return _rom_only(SCHED_IRQ_SOURCE)


def fixture_sched_straddle():
    return _rom_only(STRADDLE_SOURCE)


def fixture_port_open():
    return _rom_only(PORT_OPEN_SOURCE)


def fixture_port_vdp():
    return _rom_only(PORT_VDP_SOURCE)


def fixture_memctl_bad():
    return _rom_only(MEMCTL_BAD_SOURCE)


# U11 probe (SEG-009-T003): the same V-counter read instruction is issued at instruction-start offsets 4n T after a line
# interrupt with k superseded DD prefixes in front of it. Under instruction-start ordering the observed flip position n*
# does not depend on k; a bus-cycle-accurate machine moves it by k (each prefix delays the real bus read by 4 T). Every
# trial is one line interrupt (R0 bit 4, R10 = 1: every second line), dispatched through a constant-length handler so the sync phase is
# identical for every trial. Results: RAM $C100 + index (index = 4 n + position of k in U11_PREFIXES).
U11_PREFIXES = (0, 2, 4, 6)
U11_STEPS = 48


def u11_probe_source():
    lines = [
        ".org 0x0000",
        "        di",
        "        im 1",
        "        ld sp,0xDFF0",
        "        jp main",
        ".org 0x0038",
        "        in a,(0xBF)             ; acknowledge (clears the line-pending flag)",
        "        ld hl,(0xC0F0)",
        "        jp (hl)                 ; constant-length dispatch to this trial's block",
        ".org 0x0066",
        "        retn",
        ".org 0x0080",
        "main:   ld hl,vdp_regs",
        "        ld b,vdp_regs_end-vdp_regs",
        "        ld c,0xBF",
        "        otir                    ; R0 = M4 + line IRQ, R1 = display on (frame IRQ off), R10 = 1 (an interrupt every 2nd line)",
        "        ld hl,blk_0",
        "        ld (0xC0F0),hl",
        "        ei",
        "idle:   halt",
        "        jr idle",
        "vdp_regs:",
        "        .db 0x14,0x80, 0x40,0x81, 0x01,0x8A",
        "vdp_regs_end:",
        ".org 0x0100",
    ]
    total = U11_STEPS * len(U11_PREFIXES)
    index = 0
    for n in range(U11_STEPS):
        for k in U11_PREFIXES:
            lines.append("blk_%d:" % index)
            lines += ["        nop"] * n
            lines.append("        .db " + ",".join(["0xDD"] * k + ["0xDB", "0x7E"]))
            lines.append("        ld (0x%04X),a" % (0xC100 + index))
            index += 1
            lines.append("        ld hl,blk_%d" % index)
            lines += ["        ld (0xC0F0),hl", "        ei", "        reti"]
    lines += ["blk_%d:" % total, "        ld a,4", "        ld (0xC0F5),a", "        ei", "        reti"]
    return "\n".join(lines) + "\n"


U11_PROBE_SOURCE = u11_probe_source()


def fixture_u11_probe():
    return _rom_only(U11_PROBE_SOURCE)


T003_SOURCES = [SCHED_IRQ_SOURCE, STRADDLE_SOURCE, PORT_OPEN_SOURCE, PORT_VDP_SOURCE, MEMCTL_BAD_SOURCE,
                U11_PROBE_SOURCE]

# --- SEG-009-T004 VDP fixtures ---------------------------------------------------------------------------------------
# All project-authored. `vdp_seq_*`: a deterministic generated port-operation sequence (table in ROM, interpreter below)
# whose read results land at $C200.., followed by a VRAM Fletcher-style checksum; `vdp_irq`, `vdp_straddle` and
# `vdp_reset_probe`: interrupt/race, straddle and reset-state observations. Result blocks are documented where consumed
# (tests/sms_vdp_test.py, tests/sms_vdp_oracle_test.py).
VDP_SEQ_OPS = 700
VDP_SEQ_TABLE = 0x1000
VDP_SEQ_SEEDS = {"vdp_seq_a": 0x5EED0001, "vdp_seq_b": 0xC0FFEE17}

# op codes of the generated sequence
SEQ_CTRL, SEQ_DATA_W, SEQ_STATUS, SEQ_DATA_R, SEQ_END = 0, 1, 2, 3, 0xFF


def vdp_sequence(seed, count=VDP_SEQ_OPS):
    """Deterministic (LCG) mix of control/data port operations: [(op, arg)] ending with no terminator. The generator
    tracks the first/second-byte latch only to keep register writes away from R0/R1 (the mode stays supported, display and
    interrupts off) - every other effect is left to the devices under test."""
    state = [seed & 0xFFFFFFFF]

    def rnd(n):
        state[0] = (state[0] * 1664525 + 1013904223) & 0xFFFFFFFF
        return (state[0] >> 8) % n

    ops = []
    latch = [False]

    def ctrl(value):
        if latch[0] and value >> 6 == 2 and (value & 15) in (0, 1):
            value ^= 0x02  # never write R0/R1 through the second byte
        latch[0] = not latch[0]
        ops.append((SEQ_CTRL, value))

    def data_w():
        latch[0] = False
        ops.append((SEQ_DATA_W, rnd(256)))

    def data_r():
        latch[0] = False
        ops.append((SEQ_DATA_R, 0))

    def status():
        latch[0] = False
        ops.append((SEQ_STATUS, 0))

    def set_address(code, address):
        if latch[0]:
            status()  # resynchronise the latch
        ctrl(address & 0xFF)
        ctrl((code << 6) | ((address >> 8) & 0x3F))

    while len(ops) < count:
        kind = rnd(100)
        if kind < 14:
            set_address(rnd(4) if rnd(4) else 1, rnd(0x4000))
        elif kind < 20:
            set_address(rnd(4), 0x3FF0 + rnd(16))  # near the address wrap
        elif kind < 26:
            set_address(3, rnd(0x40))  # CRAM, including the $20-$3F alias
        elif kind < 34:
            if latch[0]:
                status()
            reg = rnd(16)
            if reg < 2:
                reg += 2
            ctrl(rnd(256) if reg != 10 else rnd(256))
            ctrl(0x80 | reg)
        elif kind < 46:
            ctrl(rnd(256))  # raw control byte: exercises half-set latch states
        elif kind < 62:
            data_w()
        elif kind < 82:
            data_r()
        elif kind < 90:
            status()
        else:
            for _ in range(1 + rnd(6)):
                (data_r if rnd(2) else data_w)()
    return ops[:count]


def vdp_seq_source(seed):
    lines = [
        ".org 0x0000", "        di", "        im 1", "        ld sp,0xDFF0", "        jp main",
        ".org 0x0038", "        ei", "        reti",
        ".org 0x0066", "        retn",
        ".org 0x0080",
        "main:   ld hl,seq", "        ld de,0xC200",
        "loop:   ld a,(hl)", "        cp 0xFF", "        jr z,done", "        inc hl", "        ld b,(hl)", "        inc hl",
        "        cp 1", "        jr c,op0", "        jr z,op1", "        cp 3", "        jr c,op2",
        "        in a,(0xBE)", "        jr store",
        "op0:    ld c,0xBF", "        out (c),b", "        jr loop",
        "op1:    ld c,0xBE", "        out (c),b", "        jr loop",
        "op2:    in a,(0xBF)",
        "store:  ld (de),a", "        inc de", "        jr loop",
        "done:   ld a,e", "        ld (0xC1F4),a", "        ld a,d", "        ld (0xC1F5),a",
        "        ld a,0x5A", "        out (0xBE),a",          # marks the final address (CRAM when code 3)
        "        xor a", "        out (0xBF),a", "        out (0xBF),a",  # address 0, code 0: read set-up
        "        ld hl,0", "        ld b,h", "        ld c,l", "        ld de,0x4000",
        "csum:   in a,(0xBE)", "        add a,c", "        ld c,a", "        jr nc,cs1", "        inc b",
        "cs1:    add hl,bc", "        dec de", "        ld a,d", "        or e", "        jr nz,csum",
        "        ld (0xC1F0),hl", "        ld a,c", "        ld (0xC1F2),a", "        ld a,b", "        ld (0xC1F3),a",
        "        ld a,0xA5", "        ld (0xC1FF),a", "        ld a,4", "        ld (0xC0F5),a",
        "fin:    halt", "        jr fin",
        ".org 0x%04X" % VDP_SEQ_TABLE, "seq:",
    ]
    ops = vdp_sequence(seed)
    flat = []
    for op, arg in ops:
        flat += [op, arg]
    flat += [SEQ_END, 0]
    for i in range(0, len(flat), 16):
        lines.append("        .db " + ",".join("0x%02X" % b for b in flat[i:i + 16]))
    return "\n".join(lines) + "\n"


def _vdp_setregs(r0, r1, r10):
    return ["        ld a,0x%02X" % r0, "        out (0xBF),a", "        ld a,0x80", "        out (0xBF),a",
            "        ld a,0x%02X" % r1, "        out (0xBF),a", "        ld a,0x81", "        out (0xBF),a",
            "        ld a,0x%02X" % r10, "        out (0xBF),a", "        ld a,0x8A", "        out (0xBF),a"]


def _wait_n(target):
    return ["        ei", "w%d:     halt" % target, "        ld a,(0xC0F2)", "        cp %d" % target,
            "        jr c,w%d" % target, "        di"]


VDP_IRQ_PHASES = []  # (name, first index, count) of the recorded interrupt entries, filled by vdp_irq_source


def vdp_irq_source():
    """Phases (each records V counter at $C100+n and the status byte read in the handler at $C200+n; n at $C0F2;
    the n reached after each phase is stored at $C0E0+phase):
    0 frame IRQ 192-line (2), 1 frame IRQ 224-line (2), 2 line IRQ R10=15 (12, lines 15..191), 3 line IRQ R10=0 from the start of a frame (100), 4 frame flag pending
    while disabled then enabled (1), 5 line flag pending while disabled then enabled (1), 6 status read clears a pending
    line flag before enable (1, the next line IRQ), 7 line IRQ R10=15 across the frame boundary (14: lines 15..191 then 15, 31 of the next frame), 8 R10 written at line 191-192 with the line IRQ enabled there (30: the new value applies from the next reload, lines 0, 1, 2, ...)."""
    del VDP_IRQ_PHASES[:]
    lines = [
        ".org 0x0000", "        di", "        im 1", "        ld sp,0xDFF0", "        jp main",
        ".org 0x0038",
        "        push af", "        push hl", "        ld a,(0xC0F2)", "        ld l,a", "        ld h,0xC1",
        "        in a,(0x7E)", "        ld (hl),a", "        inc h", "        in a,(0xBF)", "        ld (hl),a",
        "        ld a,l", "        inc a", "        ld (0xC0F2),a", "        pop hl", "        pop af", "        ei", "        reti",
        ".org 0x0066", "        retn",
        ".org 0x0080", "main:   xor a", "        ld (0xC0F2),a",
    ]
    n = 0
    phase = [0]

    def boundary():
        lines.extend(["        ld a,(0xC0F2)", "        ld (0x%04X),a" % (0xC0E0 + phase[0])])
        phase[0] += 1

    def prepare(r0, r1, r10):
        lines.append("        di")
        lines.extend(_vdp_setregs(r0, r1, r10))
        lines.append("        in a,(0xBF)")

    def waitv(value, tag):
        lines.extend(["wv%s:    in a,(0x7E)" % tag, "        cp 0x%02X" % value, "        jr nz,wv%s" % tag])

    prepare(0x06, 0x80, 0xFF)
    lines.extend(_vdp_setregs(0x06, 0xA0, 0xFF)); n += 2; lines.extend(_wait_n(n)); boundary()               # phase 0
    prepare(0x06, 0x80, 0xFF)
    lines.extend(_vdp_setregs(0x06, 0xB0, 0xFF)); n += 2; lines.extend(_wait_n(n)); boundary()               # phase 1
    prepare(0x06, 0x80, 0xFF)
    lines.extend(_vdp_setregs(0x16, 0x80, 15)); n += 12; lines.extend(_wait_n(n)); boundary()                # phase 2
    prepare(0x06, 0x80, 0)
    waitv(0xE0, "3")                                # vblank: the counter reloads from R10 every line
    lines.extend(["        in a,(0xBF)"])
    lines.extend(_vdp_reg(0, 0x16))
    n += 100; lines.extend(_wait_n(n)); boundary()                                                          # phase 3
    prepare(0x06, 0x80, 0xFF)
    waitv(0xD0, "4")
    lines.extend(["        ld a,0xA0", "        out (0xBF),a", "        ld a,0x81", "        out (0xBF),a"])
    n += 1; lines.extend(_wait_n(n)); boundary()                                                            # phase 4
    prepare(0x06, 0x80, 15)
    waitv(0xE0, "5a"); waitv(0x08, "5b"); waitv(0x40, "5c")
    lines.extend(["        ld a,0x16", "        out (0xBF),a", "        ld a,0x80", "        out (0xBF),a"])
    n += 1; lines.extend(_wait_n(n)); boundary()                                                            # phase 5
    prepare(0x16, 0x80, 15)
    waitv(0xE0, "6a"); waitv(0x20, "6b")
    lines.extend(["        in a,(0xBF)"])
    n += 1; lines.extend(_wait_n(n)); boundary()                                                            # phase 6
    prepare(0x06, 0x80, 15)
    waitv(0xE0, "7")                                # vblank: the counter reloads from R10 every line
    lines.extend(["        in a,(0xBF)"])           # clear the line flag raised while the interrupt was disabled
    lines.extend(_vdp_reg(0, 0x16))
    n += 14; lines.extend(_wait_n(n)); boundary()                                                            # phase 7
    prepare(0x06, 0x80, 15)
    waitv(0xBF, "8")                                # line 191: the counter has just been reloaded with 15 (R10 = 15)
    lines.extend(["        in a,(0xBF)"])
    lines.extend(_vdp_reg(0, 0x16))                 # line IRQ enabled, then R10 changes while the counter is mid-flight
    lines.extend(_vdp_reg(10, 0))
    n += 30; lines.extend(_wait_n(n)); boundary()                                                            # phase 8
    lines.extend(["        ld a,0xA5", "        ld (0xC1FF),a", "        ld a,4", "        ld (0xC0F5),a",
                  "fin:    halt", "        jr fin"])
    return "\n".join(lines) + "\n"


VDP_STRADDLE_STEPS = 64


def vdp_straddle_source():
    """One trial per frame: a line interrupt at line 192 (R10 = 192, line IRQ only) is dispatched through a constant-length
    handler to trial block i: n NOPs (n = i mod 64), then a status read (series S, result $C100+n: bit 7 = frame flag seen)
    or a V counter read followed by an acknowledging status read (series V, result $C140+n); series S first reads the status once at
    the handler entry (acknowledge; clears the frame flag set by the previous frame). The frame flag and the V
    counter step at line 193, 228 T after the interrupt's line starts."""
    lines = [
        ".org 0x0000", "        di", "        im 1", "        ld sp,0xDFF0", "        jp main",
        ".org 0x0038", "        ld hl,(0xC0F0)", "        jp (hl)",
        ".org 0x0066", "        retn",
        ".org 0x0080", "main:   ld hl,blk_0", "        ld (0xC0F0),hl",
    ]
    lines.extend(_vdp_setregs(0x16, 0x80, 192))
    lines.extend(["        in a,(0xBF)", "        ei", "idle:   halt", "        jr idle", ".org 0x0100"])
    total = 2 * VDP_STRADDLE_STEPS
    for i in range(total):
        series, n = divmod(i, VDP_STRADDLE_STEPS)
        lines.append("blk_%d:" % i)
        if series == 0:
            lines.append("        in a,(0xBF)             ; acknowledge and clear the frame flag left by the previous frame")
        lines += ["        nop"] * n
        if series == 0:
            lines += ["        in a,(0xBF)", "        ld (0x%04X),a" % (0xC100 + n)]
        else:
            lines += ["        in a,(0x7E)", "        ld (0x%04X),a" % (0xC140 + n), "        in a,(0xBF)"]
        lines += ["        ld hl,blk_%d" % (i + 1), "        ld (0xC0F0),hl", "        ei", "        reti"]
    lines += ["blk_%d:" % total, "        in a,(0xBF)", "        ld a,4", "        ld (0xC0F5),a", "        ld a,0xA5",
              "        ld (0xC1FF),a", "        ei", "        reti"]
    return "\n".join(lines) + "\n"


def vdp_straddle_write_source():
    """U11 for an IRQ-sensitive register write. R10 = 0 with the line IRQ enabled raises an interrupt on every line; trial n
    (handlers A_n, B_n, C_n chained through consecutive interrupts) writes R10 = 3 in A_n after n NOPs, at an instruction-start
    offset that straddles the next line's event. The reload at that line's underflow uses the R10 of that instant: a write
    before the event makes the following interrupt 4 lines later, after it 1 line later. B_n and C_n log the V counter of
    the next two interrupts ($C100 + n, $C140 + n); C_n restores R10 = 0."""
    lines = [
        ".org 0x0000", "        di", "        im 1", "        ld sp,0xDFF0", "        jp main",
        ".org 0x0038", "        ld hl,(0xC0F0)", "        jp (hl)",
        ".org 0x0066", "        retn",
        ".org 0x0080", "main:   ld hl,a_0", "        ld (0xC0F0),hl",
    ]
    lines.extend(_vdp_setregs(0x16, 0x80, 0))
    lines.extend(["        in a,(0xBF)", "        ei", "idle:   halt", "        jr idle", ".org 0x0100"])
    for n in range(VDP_STRADDLE_STEPS):
        lines += ["a_%d:" % n, "        in a,(0xBF)"] + ["        nop"] * n
        lines += ["        ld a,3", "        out (0xBF),a", "        ld a,0x8A", "        out (0xBF),a",
                  "        ld hl,b_%d" % n, "        ld (0xC0F0),hl", "        ei", "        reti"]
        lines += ["b_%d:" % n, "        in a,(0x7E)", "        ld (0x%04X),a" % (0xC100 + n), "        in a,(0xBF)",
                  "        ld hl,c_%d" % n, "        ld (0xC0F0),hl", "        ei", "        reti"]
        lines += ["c_%d:" % n, "        in a,(0x7E)", "        ld (0x%04X),a" % (0xC140 + n), "        in a,(0xBF)",
                  "        xor a", "        out (0xBF),a", "        ld a,0x8A", "        out (0xBF),a",
                  "        ld hl,a_%d" % (n + 1), "        ld (0xC0F0),hl", "        ei", "        reti"]
    lines += ["a_%d:" % VDP_STRADDLE_STEPS, "        in a,(0xBF)", "        ld a,4", "        ld (0xC0F5),a", "        ld a,0xA5",
              "        ld (0xC1FF),a", "        ei", "        reti"]
    return "\n".join(lines) + "\n"


def vdp_reset_source():
    """Observes the post-reset VDP with no register written: $C100 first data read (buffer), $C101 first status byte,
    $C102-$C109 VRAM[0..7] reads, $C10A count of interrupts taken with only the reset registers (R1 bit 5 or an R10-gated
    line interrupt: none expected for R10 = $FF), $C10B count taken after only R10 = 0 was written (R0 bit 4 at reset)."""
    lines = [
        ".org 0x0000", "        di", "        im 1", "        ld sp,0xDFF0", "        jp main",
        ".org 0x0038", "        push af", "        in a,(0xBF)", "        ld a,(0xC0F2)", "        inc a", "        ld (0xC0F2),a",
        "        pop af", "        ei", "        reti",
        ".org 0x0066", "        retn",
        ".org 0x0080", "main:   in a,(0xBE)", "        ld (0xC100),a", "        in a,(0xBF)", "        ld (0xC101),a",
        "        xor a", "        out (0xBF),a", "        out (0xBF),a",
    ]
    for i in range(8):
        lines += ["        in a,(0xBE)", "        ld (0x%04X),a" % (0xC102 + i)]
    lines += ["        xor a", "        ld (0xC0F2),a", "        ei", "        ld bc,0",
              "spin1:  dec bc", "        ld a,b", "        or c", "        jr nz,spin1", "        di",
              "        ld a,(0xC0F2)", "        ld (0xC10A),a", "        in a,(0xBF)", "        xor a", "        ld (0xC0F2),a",
              "        out (0xBF),a", "        ld a,0x8A", "        out (0xBF),a", "        ei", "        ld bc,0",
              "spin2:  dec bc", "        ld a,b", "        or c", "        jr nz,spin2", "        di",
              "        ld a,(0xC0F2)", "        ld (0xC10B),a", "        ld a,0xA5", "        ld (0xC1FF),a", "        ld a,4",
              "        ld (0xC0F5),a", "fin:    halt", "        jr fin"]
    return "\n".join(lines) + "\n"


def fixture_vdp_seq_a():
    return _rom_only(vdp_seq_source(VDP_SEQ_SEEDS["vdp_seq_a"]))


def fixture_vdp_seq_b():
    return _rom_only(vdp_seq_source(VDP_SEQ_SEEDS["vdp_seq_b"]))


def fixture_vdp_straddle_write():
    return _rom_only(vdp_straddle_write_source())


def fixture_vdp_irq():
    return _rom_only(vdp_irq_source())


def fixture_vdp_straddle():
    return _rom_only(vdp_straddle_source())


def fixture_vdp_reset_probe():
    return _rom_only(vdp_reset_source())


def vdp_mode_source(body):
    """Mode-check fixtures (contract section 9.5): `body` runs after the VDP register writes of the case; $C100 = $5A is
    written before it and $C101 = $A5 after it, so a typed stop leaves $C101 = 0."""
    lines = [
        ".org 0x0000", "        di", "        im 1", "        ld sp,0xDFF0", "        jp main",
        ".org 0x0066", "        retn",
        ".org 0x0080", "main:   ld a,0x5A", "        ld (0xC100),a",
    ]
    lines += body
    lines += ["        ld a,0xA5", "        ld (0xC101),a", "fin:    halt", "        jr fin"]
    return "\n".join(lines) + "\n"


def _vdp_reg(register, value):
    return ["        ld a,0x%02X" % value, "        out (0xBF),a", "        ld a,0x%02X" % (0x80 | register), "        out (0xBF),a"]


VDP_MODE_CASES = {
    # the invalid text mode (R0 $04: M4 set, M2 clear; R1 bit 4: M1; M4 M3 M2 M1 = 1 0 0 1) set while the display is blanked: no stop, the mode never reaches output
    "vdp_mode_blank_ok": _vdp_reg(0, 0x04) + _vdp_reg(1, 0x90) + _vdp_reg(1, 0x80) + _vdp_reg(0, 0x36) + ["        ld bc,0x6000", "sp0:     dec bc", "        ld a,b",
                                                                   "        or c", "        jr nz,sp0"],
    # the same mode with the display enabled: stops at the first active line of the next frame
    "vdp_mode_display_stop": _vdp_reg(0, 0x04) + _vdp_reg(1, 0xD0) + ["        ld bc,0xFFFF", "sp1:     dec bc", "        ld a,b", "        or c",
                                                   "        jr nz,sp1"],
    # a status read in an unsupported mode (the flags it would return are not modelled): stops at the read
    "vdp_mode_status_stop": _vdp_reg(0, 0x04) + _vdp_reg(1, 0x90) + ["        in a,(0xBF)"],
    # the H counter (U3) stops typed until the TH latch trigger exists
    "vdp_hcounter_stop": ["        in a,(0x7F)"],
}


def _vdp_mode_fixture(name):
    return lambda: _rom_only(vdp_mode_source(VDP_MODE_CASES[name]))


VDP_FIXTURES = {"vdp_seq_a": fixture_vdp_seq_a, "vdp_seq_b": fixture_vdp_seq_b, "vdp_irq": fixture_vdp_irq,
                "vdp_straddle": fixture_vdp_straddle, "vdp_straddle_write": fixture_vdp_straddle_write, "vdp_reset_probe": fixture_vdp_reset_probe,
                **{name: _vdp_mode_fixture(name) for name in VDP_MODE_CASES}}


# --- SEG-009-T005: rendering fixtures --------------------------------------------------------------------------------------
# Project-authored scenes (LCG-generated tile, name table, sprite and palette data) loaded through the VDP data port with OTIR,
# then the display is enabled. The static scenes differ in register configuration (scroll, locks, sprite size/zoom/shift, left
# blank, 224-line mode, sprite pattern base); `render_raster` additionally changes R8/R9 from a line interrupt every 8 lines so the
# per-line latching and the R9 frame latch are exercised through the real interrupt route.
RENDER_DATA_ORG = 0x1000
RENDER_SCENES = {
    # name: (seed, tall, regs R0..R10 as finally programmed, raster, ywrap)
    "render_scene_a": (0x5CE11A01, False, [0x06, 0xC0, 0x0E, 0xFF, 0xFF, 0x7F, 0xFB, 0x03, 0x1D, 0x25, 0xFF], False, False),
    "render_scene_b": (0x5CE11B02, False, [0xEE, 0xC3, 0x0A, 0xFF, 0xFF, 0x7F, 0xFB, 0x0C, 0x2B, 0x91, 0xFF], False, False),
    "render_scene_c": (0x5CE11C03, True, [0x06, 0xD0, 0x0C, 0xFF, 0xFF, 0x7F, 0xFF, 0x05, 0x46, 0x7A, 0xFF], False, False),
    "render_raster": (0x5CE11D04, False, [0x76, 0xC0, 0x0E, 0xFF, 0xFF, 0x7F, 0xFB, 0x01, 0x00, 0x00, 0x07], True, False),
    # SEG-009-T013 / U12: sprites with Y near the bottom of the 256-line space (they wrap to the top), zoomed 8x16 in both heights
    "render_ywrap_192": (0x5CE11E05, False, [0x06, 0xC3, 0x0E, 0xFF, 0xFF, 0x7F, 0xFB, 0x00, 0x00, 0x00, 0xFF], False, True),
    "render_ywrap_224": (0x5CE11F06, True, [0x06, 0xD3, 0x0C, 0xFF, 0xFF, 0x7F, 0xFF, 0x05, 0x00, 0x00, 0xFF], False, True),
}


def render_scene_data(seed, tall, ywrap=False):
    """(tiles 0-95 at VRAM 0 [3072 B], tiles 256-263 at $2000 [256 B], name table [2048 B], sprite attribute area $3F00 [256 B],
    CRAM [32 B]) of a deterministic scene that exercises overflow, collision, priority, flips and both palettes."""
    state = [seed & 0xFFFFFFFF]

    def rnd(n):
        state[0] = (state[0] * 1664525 + 1013904223) & 0xFFFFFFFF
        return (state[0] >> 8) % n

    def tile(density):
        out = []
        for _ in range(8):
            planes = [0, 0, 0, 0]
            for col in range(8):
                v = rnd(16) if rnd(100) < density else 0
                for p in range(4):
                    planes[p] |= ((v >> p) & 1) << (7 - col)
            out += planes
        return out

    tiles = []
    for t in range(96):
        tiles += tile((20, 50, 90)[t % 3])
    upper = []
    for _ in range(8):
        upper += tile(70)
    nt = []
    for _ in range(32 * 32):
        entry = rnd(96) | (rnd(2) << 9) | (rnd(2) << 10) | (rnd(2) << 11) | (rnd(2) << 12)
        nt += [entry & 255, entry >> 8]
    sat = [0xD0] * 64 + [0] * 64 + [0] * 128
    count = 26
    for i in range(count):
        crowd = i < 12  # twelve sprites share a band of lines: per-line overflow, overlaps: collision
        y = (70 + rnd(6)) if crowd else rnd(180)
        if y == 0xD0:
            y = 0xCF
        sat[i] = y
        sat[0x80 + 2 * i] = (60 + rnd(90)) if crowd else rnd(256)
        sat[0x81 + 2 * i] = rnd(96) if i % 5 else rnd(8)
    sat[count] = 0xD0
    if not tall:  # entries after the terminator are dead in 192-line mode
        for i in range(count + 1, count + 6):
            sat[i] = 30 + 20 * (i - count)
            sat[0x80 + 2 * i], sat[0x81 + 2 * i] = 40 + rnd(150), 1 + rnd(90)
    if tall:
        sat[count] = 200  # 224-line mode has no terminator: the entries beyond are live
        for i in range(count + 1, 40):
            sat[i] = rnd(224)
            sat[0x80 + 2 * i], sat[0x81 + 2 * i] = rnd(256), rnd(96)
        sat[40] = 0xD0
    cram = [rnd(64) for _ in range(32)]
    if ywrap:  # applied after every random draw, so the other scenes are unchanged
        for k, y in enumerate((0xF0, 0xF6, 0xFA, 0xFF)):
            sat[12 + k] = y
    return bytes(tiles), bytes(upper), bytes(nt), bytes(sat), bytes(cram)


def _render_segment(label, address, code, length):
    lo, hi = address & 255, (address >> 8) | (code << 6)
    lines = ["        ld a,0x%02X" % lo, "        out (0xBF),a", "        ld a,0x%02X" % hi, "        out (0xBF),a",
             "        ld hl,%s" % label]
    if length >= 256:
        lines += ["        ld d,%d" % (length // 256), "%s_l:  ld b,0" % label, "        ld c,0xBE", "        otir",
                  "        dec d", "        jr nz,%s_l" % label]
    else:
        lines += ["        ld b,%d" % length, "        ld c,0xBE", "        otir"]
    return lines


def render_source(name):
    seed, tall, regs, raster, ywrap = RENDER_SCENES[name]
    tiles, upper, nt, sat, cram = render_scene_data(seed, tall, ywrap)
    nt_base = ((regs[2] & 0x0C) << 10) | 0x700 if tall else (regs[2] & 0x0E) << 10
    lines = [".org 0x0000", "        di", "        im 1", "        ld sp,0xDFF0", "        jp main",
             ".org 0x0038"]
    if raster:
        lines += ["        push af", "        in a,(0xBF)", "        ld a,(0xC010)", "        add a,13", "        ld (0xC010),a",
                  "        out (0xBF),a", "        ld a,0x88", "        out (0xBF),a", "        ld a,(0xC010)",
                  "        add a,a", "        out (0xBF),a", "        ld a,0x89", "        out (0xBF),a", "        pop af", "        ei",
                  "        reti"]
    else:
        lines += ["        ei", "        reti"]
    lines += [".org 0x0066", "        retn", ".org 0x0080", "main:   xor a", "        ld (0xC010),a"]
    lines += _render_segment("d_tiles", 0x0000, 1, len(tiles))
    lines += _render_segment("d_upper", 0x2000, 1, len(upper))
    lines += _render_segment("d_nt", nt_base, 1, len(nt))
    lines += _render_segment("d_sat", 0x3F00, 1, len(sat))
    lines += ["        ld a,0x00", "        out (0xBF),a", "        ld a,0xC0", "        out (0xBF),a", "        ld hl,d_cram",
              "        ld b,32", "        ld c,0xBE", "        otir"]
    for reg in (0, 2, 5, 6, 7, 8, 9, 10):
        lines += _vdp_reg(reg, regs[reg])
    lines += _vdp_reg(1, regs[1])  # display enable last: VRAM, CRAM and every register are final
    if raster:
        lines += ["        in a,(0xBF)", "        ei", "fin:    halt", "        jr fin"]
    else:
        lines += ["        ld bc,0x1800", "sp:     dec bc", "        ld a,b", "        or c", "        jr nz,sp", "        in a,(0xBF)",
                  "        ld (0xC100),a", "        ld a,0xA5", "        ld (0xC1FF),a", "fin:    halt", "        jr fin"]
    lines.append(".org 0x%04X" % RENDER_DATA_ORG)
    for label, blob in (("d_tiles", tiles), ("d_upper", upper), ("d_nt", nt), ("d_sat", sat), ("d_cram", cram)):
        lines.append("%s:" % label)
        for i in range(0, len(blob), 16):
            lines.append("        .db " + ",".join("0x%02X" % b for b in blob[i:i + 16]))
    return "\n".join(lines) + "\n"


def _render_fixture(name):
    return lambda: _rom_only(render_source(name))


RENDER_FIXTURES = {name: _render_fixture(name) for name in RENDER_SCENES}


# --- SEG-009-T008: end-to-end machine fixture ---------------------------------------------------------------------------
# One project-authored program on the Sega mapper (declared by the builder, never inferred) that exercises every baseline
# machine area through generated-native Z80 owners: reset entry, RAM and its mirror, slot-1/slot-2 bank switches (including
# a mapper value above the bank count), the same logical PC 0x4100 dispatched under three banks, VRAM/name-table/sprite/CRAM
# uploads read from banked tables, mode 4 background scrolling (R8 per frame and from the line interrupt, R9 per frame),
# sprites with per-line overflow and collision, the IM1 frame and line interrupts (status acknowledged by a read, V counter
# sampled at entry), both controller ports under scripted input plus the I/O control readback, the pause NMI, and PSG tone and
# noise writes spread over the frames. Guest-visible results (the reference comparison surface) are ordinary RAM:
#   $C100 magic $5A; $C101-$C102 RAM mirror reads; $C103-$C108 bank markers; $C109-$C10C the routine at $4100 under banks
#   1, 3, 6, 1; $C10D the mapper RAM copy; $C10E-$C10F undecoded port reads; $C110-$C111 nationalization; $C112-$C113 the sum
#   and xor of a VRAM read-back; $C1FF = $A5 when the program finished.
#   Frame n log at $C200 + 16 n (n = guest frame counter, increments in the frame interrupt): +0 n, +1 V counter at entry,
#   +2 status flags accumulated over the frame (bit 7 frame, bit 6 overflow, bit 5 collision), +3 port $DC, +4 port $DD,
#   +5 line interrupts taken in the frame, +6..+9 V counter at entry of each line interrupt, +10 pause NMIs so far,
#   +11 slot-2 bank marker for bank 4 + (n & 3), +12 slot-1 bank marker for bank 2 + (n & 1).
E2E_FRAMES = 52            # headless --frames bound (the program finishes its 40 logged frames inside it)
E2E_LOGGED_FRAMES = 40
E2E_SEED = 0x5E2E0001
E2E_R10 = 39
E2E_SCRIPT = """# scripted input of the machine_e2e fixture: <frame> <p1 UDLR12> <p2> <pause P|->
0  ------ ------ -
9  U----- ------ -
12 U---1- ---R-- -
15 -D--1- ---R-- P
18 -D--1- ---R-- -
21 ---R-2 ------ -
24 ---R-2 -D---- P
25 ---R-2 -D---- P
27 ------ -D---- -
30 U-L-12 ------ -
33 ------ ------ P
34 ------ ------ -
"""
# (latch/data byte pair per guest frame, 0 = no write); every PSG write of the program is one of these
E2E_PSG = {1: (0x8E, 0x0F), 2: (0x90, 0), 8: (0x9F, 0),                      # tone 0 alone
           9: (0xA3, 0x1F), 10: (0xB0, 0), 15: (0xBF, 0),                    # tone 1 alone
           16: (0xC7, 0x08), 17: (0xD0, 0), 22: (0xDF, 0),                   # tone 2 alone
           23: (0xE6, 0), 24: (0xF0, 0), 29: (0xFF, 0),                      # white noise alone (rate 2)
           30: (0xE1, 0), 31: (0xF4, 0), 35: (0xFF, 0)}                      # periodic noise alone (rate 1), then silence
E2E_BANK_MARKER_OFFSET = 0x10
E2E_TABLES = {"tiles": (2, 0x200), "nt": (3, 0x200), "sat": (4, 0x200), "cram": (4, 0x300)}
E2E_ROUTINES = {1: 0x11, 3: 0x33, 6: 0x66}   # bank -> value returned by the routine at logical 0x4100


def e2e_data():
    tiles, upper, nt, sat, cram = render_scene_data(E2E_SEED, False)
    return {"tiles": tiles + upper, "nt": nt, "sat": sat, "cram": cram}


def e2e_source():
    data = e2e_data()
    lines = [
        ".org 0x0000", "        di", "        im 1", "        ld sp,0xDFF0", "        jp main",
        ".org 0x0038", "        jp irq",
        ".org 0x0066",                                   # pause NMI: count in $C013
        "        push af", "        ld a,(0xC013)", "        inc a", "        ld (0xC013),a", "        pop af", "        retn",
        ".org 0x0100",
        "irq:    push af", "        push bc", "        push de", "        push hl",
        "        in a,(0xBF)",                          # status read acknowledges the source
        "        ld b,a",
        "        in a,(0x7E)",                          # V counter at entry
        "        ld c,a",
        "        ld a,b", "        and 0x80", "        jp z,line_irq",
        # ---- frame interrupt: PSG writes first (their T-state offsets from the acknowledge are fixed) ----
        "psg_begin:",
        "        ld a,(0xC010)", "        add a,a", "        ld e,a", "        ld d,0", "        ld hl,psg_table", "        add hl,de",
        "        ld a,(hl)", "        or a", "        jr z,psg_s1", "        out (0x7F),a",
        "psg_s1: inc hl", "        ld a,(hl)", "        or a", "        jr z,psg_s2", "        out (0x7F),a",
        "psg_s2:",
        # ---- frame log record ----
        "        ld a,(0xC010)", "        ld l,a", "        ld h,0", "        add hl,hl", "        add hl,hl", "        add hl,hl",
        "        add hl,hl", "        ld de,0xC200", "        add hl,de",
        "        ld a,(0xC010)", "        ld (hl),a", "        inc hl",               # +0
        "        ld (hl),c", "        inc hl",                                          # +1 V counter at entry
        "        ld a,b", "        and 0x60", "        ld e,a", "        ld a,(0xC014)", "        or e", "        or 0x80",
        "        ld (hl),a", "        inc hl",                                          # +2 flags
        "        xor a", "        ld (0xC014),a",
        "        in a,(0xDC)", "        ld (hl),a", "        inc hl",                  # +3
        "        in a,(0xDD)", "        ld (hl),a", "        inc hl",                  # +4
        "        ld a,(0xC011)", "        ld (hl),a", "        inc hl",                # +5
        "        ld a,(0xC020)", "        ld (hl),a", "        inc hl",
        "        ld a,(0xC021)", "        ld (hl),a", "        inc hl",
        "        ld a,(0xC022)", "        ld (hl),a", "        inc hl",
        "        ld a,(0xC023)", "        ld (hl),a", "        inc hl",                # +6..+9
        "        ld a,(0xC013)", "        ld (hl),a", "        inc hl",                # +10
        "        ld a,(0xC010)", "        and 3", "        add a,4", "        ld (0xFFFF),a",   # slot 2 <- bank 4 + (n & 3)
        "        ld a,(0x8010)", "        ld (hl),a", "        inc hl",                # +11
        "        ld a,(0xC010)", "        and 1", "        add a,2", "        ld (0xFFFE),a",   # slot 1 <- bank 2 + (n & 1)
        "        ld a,(0x4010)", "        ld (hl),a",                                 # +12
        # ---- per-frame scroll registers ----
        "        ld a,(0xC010)", "        ld e,a", "        add a,a", "        add a,e", "        add a,3",
        "        out (0xBF),a", "        ld a,0x88", "        out (0xBF),a",           # R8 = 3 (n + 1)
        "        ld a,(0xC010)", "        add a,a", "        add a,2",
        "        out (0xBF),a", "        ld a,0x89", "        out (0xBF),a",           # R9 = 2 (n + 1), latched at the next line 0
        "        xor a", "        ld (0xC011),a",
        "        ld a,(0xC010)", "        inc a", "        ld (0xC010),a",
        "        cp %d" % E2E_LOGGED_FRAMES, "        jr nz,irq_exit", "        ld a,0xA5", "        ld (0xC1FF),a",
        "irq_exit:", "        pop hl", "        pop de", "        pop bc", "        pop af", "        ei", "        reti",
        # ---- line interrupt: log the V counter, accumulate the sprite flags, raster scroll ----
        "line_irq:",
        "        ld a,b", "        and 0x60", "        ld e,a", "        ld a,(0xC014)", "        or e", "        ld (0xC014),a",
        "        ld a,(0xC011)", "        ld e,a", "        ld d,0", "        ld hl,0xC020", "        add hl,de", "        ld (hl),c",
        "        inc a", "        ld (0xC011),a", "        ld e,a", "        add a,a", "        add a,a", "        add a,a", "        add a,e",
        "        out (0xBF),a", "        ld a,0x88", "        out (0xBF),a",            # R8 = 9 x interrupt number
        "        jp irq_exit",
        ".org 0x0400",
        "main:   ld hl,0xC000", "        ld de,0xC001", "        ld bc,0x07FF", "        ld (hl),0", "        ldir",
        "        ld a,0x5A", "        ld (0xC100),a",
        "        ld a,0x3C", "        ld (0xC0F0),a", "        ld a,(0xE0F0)", "        ld (0xC101),a",          # RAM mirror
        "        ld a,0xA7", "        ld (0xE0F1),a", "        ld a,(0xC0F1)", "        ld (0xC102),a",
        "        ld a,(0x4010)", "        ld (0xC103),a", "        ld a,(0x8010)", "        ld (0xC104),a",          # reset banks
        "        ld a,3", "        ld (0xFFFE),a", "        ld a,(0x4010)", "        ld (0xC105),a",
        "        ld a,5", "        ld (0xFFFF),a", "        ld a,(0x8010)", "        ld (0xC106),a",
        "        ld a,10", "        ld (0xFFFF),a", "        ld a,(0x8010)", "        ld (0xC107),a",              # masked to bank 2
        "        ld a,13", "        ld (0xFFFE),a", "        ld a,(0x4010)", "        ld (0xC108),a",              # masked to bank 5
    ]
    for slot_bank, cell in ((1, 0xC109), (3, 0xC10A), (6, 0xC10B), (1, 0xC10C)):
        lines += ["        ld a,%d" % slot_bank, "        ld (0xFFFE),a", "        call 0x4100", "        ld (0x%04X),a" % cell]
    lines += [
        "        ld a,(0xFFFE)", "        ld (0xC10D),a",
        "        in a,(0x00)", "        ld (0xC10E),a", "        in a,(0x3E)", "        ld (0xC10F),a",
        "        ld a,0xF5", "        out (0x3F),a", "        in a,(0xDD)", "        and 0xC0", "        ld (0xC110),a",
        "        ld a,0x55", "        out (0x3F),a", "        in a,(0xDD)", "        and 0xC0", "        ld (0xC111),a",
        "        ld a,0xFF", "        out (0x3F),a",
    ]
    lines += ["        ld a,0x12", "        out (0xBF),a", "        in a,(0xBF)"]   # a dangling first control byte: the status read resets the latch
    regs = {0: 0x16, 2: 0x0E, 5: 0xFF, 6: 0xFB, 7: 0x03, 8: 0, 9: 0, 10: E2E_R10}
    for reg, value in regs.items():
        lines += _vdp_reg(reg, value)

    def upload(table, vdp_address, code, length):
        bank, offset = E2E_TABLES[table]
        window = 0x8000 if table in ("sat", "cram") else 0x4000
        slot_reg = "0xFFFF" if window == 0x8000 else "0xFFFE"
        out = ["        ld a,%d" % bank, "        ld (%s),a" % slot_reg,
               "        ld a,0x%02X" % (vdp_address & 255), "        out (0xBF),a",
               "        ld a,0x%02X" % ((vdp_address >> 8) | (code << 6)), "        out (0xBF),a",
               "        ld hl,0x%04X" % (window + offset)]
        if length >= 256:
            out += ["        ld d,%d" % (length // 256), "up_%s: ld b,0" % table, "        ld c,0xBE", "        otir",
                    "        dec d", "        jr nz,up_%s" % table]
        else:
            out += ["        ld b,%d" % length, "        ld c,0xBE", "        otir"]
        return out
    lines += upload("tiles", 0x0000, 1, len(data["tiles"]))
    lines += upload("nt", (0x0E & 0x0E) << 10, 1, len(data["nt"]))
    lines += upload("sat", 0x3F00, 1, len(data["sat"]))
    lines += upload("cram", 0x0000, 3, len(data["cram"]))
    lines += [  # read back the first 2 KiB of VRAM: sum and xor into $C112/$C113 (the VDP state checked by the guest itself)
        "        xor a", "        out (0xBF),a", "        out (0xBF),a",
        "        ld hl,0x0800", "        ld b,0", "        ld c,0", "rb:     in a,(0xBE)", "        ld d,a", "        add a,b",
        "        ld b,a", "        ld a,d", "        xor c", "        ld c,a", "        dec hl", "        ld a,h", "        or l",
        "        jr nz,rb", "        ld a,b", "        ld (0xC112),a", "        ld a,c", "        ld (0xC113),a",
    ]
    lines += _vdp_reg(1, 0xE0)        # display on, frame interrupt enabled
    lines += ["        in a,(0xBF)", "        ei", "idle:   halt", "        jr idle"]
    lines += [".org 0x0800", "psg_table:"]
    for n in range(0, 64, 4):
        row = []
        for k in range(n, n + 4):
            row += list(E2E_PSG.get(k, (0, 0)))
        lines.append("        .db " + ",".join("0x%02X" % b for b in row))
    return "\n".join(lines) + "\n"


def fixture_machine_e2e():
    image, _ = Assembler(e2e_source()).assemble()
    if max(image) >= 0x2000:
        raise AsmError("machine_e2e code must stay below 0x2000")
    markers = {}
    for bank in range(1, 8):
        markers[bank * BANK_SIZE + E2E_BANK_MARKER_OFFSET] = 0xB0 + bank
    for bank, value in E2E_ROUTINES.items():   # the routine at logical 0x4100 (slot 1) of each bank
        routine, _ = Assembler(".org 0x4100\n        ld a,0x%02X\n        ret\n" % value).assemble()
        for address, byte in routine.items():
            markers[bank * BANK_SIZE + (address - 0x4000)] = byte
    for table, blob in e2e_data().items():
        bank, offset = E2E_TABLES[table]
        for i, byte in enumerate(blob):
            markers[bank * BANK_SIZE + offset + i] = byte
    markers[0x0010] = 0xB0     # bank 0 marker (slot 0 is never remapped by this program)
    return build_rom(0x20000, image, markers), "sega"


FIXTURES = {"trivial": fixture_trivial, "oracle_smoke": fixture_oracle_smoke, "bank_crossing": fixture_bank_crossing,
            "sched_irq": fixture_sched_irq, "sched_straddle": fixture_sched_straddle, "port_open": fixture_port_open,
            "port_vdp": fixture_port_vdp, "memctl_bad": fixture_memctl_bad, "u11_probe": fixture_u11_probe, **VDP_FIXTURES,
            **RENDER_FIXTURES, "machine_e2e": fixture_machine_e2e}


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
