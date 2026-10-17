#!/usr/bin/env python3
"""Project-authored BIOS-like image for the SEG-064-T003 main-side hardware of the BIOS-image profile (ADR 0142). Stdlib only.

Every byte is written here with the fixture encoder of tools/sega_cd_fixture_disc.py; no BIOS or commercial byte is read or reproduced.
The main program exercises, in order, the main-side devices an authentic BIOS reaches before the sub reset release, stores what it
observes in a result block, then builds a small sub program in PRG RAM and releases the sub (the runnable epoch):

  +$00 version byte $A10001 (USA NTSC, Mega-CD attached: MODE 1, VMOD 0, DISK 0, VER 0 = $80)    +$02 version word $A10000 ($8080: the byte on both lanes)
  +$04 CTRL1 read back after writing $40                    +$06 DATA1 with TH = 1 (3-button pad, nothing pressed: $7F)
  +$08 DATA1 with TH = 0 ($33: Start/A and Down/Up released, bits 3-2 low)
  +$0A BUSACK byte while the bus is not requested (D0 = 1, D7-D1 = the prefetch: the high byte of the next instruction word)
  +$0C BUSACK byte with the bus requested and the Z80 out of reset (D0 = 0, same prefetch rule)
  +$0E the Z80 RAM bytes read back while the 68000 holds the bus (the upload), before the Z80 ever ran
  +$10 VRAM word at $0100 after a VRAM fill of $5Axx over $0000-$03FF ($5A5A)    +$12 the status DMA bit after the fill (0)
  +$14 VRAM words $1000-$1006 after a 68K->VRAM DMA from work RAM (checksum)      +$16 same from the BIOS image at $0F00 (checksum)
  +$18 vertical-blank flag observed by polling the status register (1)
  +$1A frames counted by the level-6 handler, entered through a work-RAM JMP (xxx).L thunk at $FFF900 (3)
  +$1C H-INT vector register read back ($A12006, $1234)     +$1E BIOS $72 read through the main bus ($1234)
  +$20 BIOS $70 read through the main bus ($FFFF: the H-INT default vector's high word)
  +$22..+$25 MOVE.L $400100,D0 on the empty cartridge slot: the next instruction word twice (GPGX open bus)
  +$26 TST.B-style byte read $400001: the low byte of the next instruction word
  +$FF $A5 when main reached the epoch
The sub program writes COMSTAT0 = $C0DE and stops.

Negatives (main d7): 1 an open-bus read by a MOVE whose destination still has extension words (prefetch not established); 2 a Z80
RAM read after the Z80 has been runnable; 3 a 68K->VDP DMA from Word RAM; 4 the H-interrupt enabled (line counter not modelled: stops at the write); 8 the same by a long write that also clears it again;
5 a VDP copy DMA (not modelled by the device); 6 one byte of the sub image corrupted before the release; 7 the image copied into
PRG-RAM bank 1 (the wrong mapping) - both stop at the epoch before the sub retires anything.
usage: sega_cd_bios_hw_fixture.py build --out DIR   ->   DIR/bios.bin and DIR/expected.json (stdout: the expected JSON)
"""
import argparse
import hashlib
import json
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sega_cd_fixture_disc as F  # noqa: E402  (the shared project-authored MC68000 encoder)

ROM_SIZE = 0x1000
MAIN = 0x0200
FAULT = 0x0C00
VINT_HANDLER = 0x0C40
SUB_SRC = 0x0D00
DMA_ROM = 0x0F00
RESULT = 0xFFC000
THUNK = 0xFFF900
FRAMES = 0xFFC080
DMA_RAM = 0xFF8000
GA = 0xA12000
WINDOW = 0x020000
VDP_DATA = 0xC00000
VDP_CTRL = 0xC00004
SUB_SSP = 0x2000
SUB_ENTRY = 0x0100
GA_SUB = 0xFF8000
DMA_WORDS = (0x1357, 0x2468, 0x9ABC, 0xDEF0)
ROM_DMA_WORDS = (0x0F1E, 0x2D3C, 0x4B5A, 0x6978)


def vdp_reg(m, reg, value):
    m.move("w", F.imm(0x8000 | (reg << 8) | value), F.absl(VDP_CTRL))


def vdp_cmd(m, value):
    m.move("l", F.imm(value), F.absl(VDP_CTRL))


def vram_checksum(m, address, words, slot):
    """Sum of `words` VRAM words from `address` (VDP VRAM read command) into the result slot."""
    vdp_cmd(m, ((address & 0x3FFF) << 16) | (address >> 14))
    m.moveq(0, 2)
    m.move("w", F.imm(words - 1), F.d(0))
    m.label("sum%x" % slot)
    m.alu("add", "w", F.absl(VDP_DATA), 2)
    m.dbra(0, "sum%x" % slot)
    m.move("w", F.d(2), F.absl(RESULT + slot))


def dma_from(m, source, words, vram):
    vdp_reg(m, 19, words & 0xFF)
    vdp_reg(m, 20, words >> 8)
    vdp_reg(m, 21, (source >> 1) & 0xFF)
    vdp_reg(m, 22, (source >> 9) & 0xFF)
    vdp_reg(m, 23, (source >> 17) & 0x7F)
    vdp_cmd(m, 0x40000080 | ((vram & 0x3FFF) << 16) | (vram >> 14))


def negative(m, case, body):
    m.immop("cmp", "w", case, F.d(7))
    m.bcc("ne", "neg%d" % case)
    body(m)
    m.label("neg%d" % case)


def main_program(sub_len, layout):
    m = F.Asm(MAIN)
    # ---- I/O: version, controller port 1 ----
    m.move("b", F.absl(0xA10001), F.d(0))
    m.move("b", F.d(0), F.absl(RESULT + 0x01))
    m.move("w", F.absl(0xA10000), F.absl(RESULT + 0x02))
    m.move("b", F.imm(0x40), F.absl(0xA10009))
    m.move("b", F.absl(0xA10009), F.absl(RESULT + 0x05))
    m.move("b", F.imm(0x40), F.absl(0xA10003))
    m.move("b", F.absl(0xA10003), F.absl(RESULT + 0x07))
    m.move("b", F.imm(0x00), F.absl(0xA10003))
    m.move("b", F.absl(0xA10003), F.absl(RESULT + 0x09))
    # ---- Z80: BUSACK before the request; request the bus, release the reset, upload, read back ----
    layout["busack_free"] = m.pc
    m.move("b", F.absl(0xA11100), F.d(1))                 # 6 bytes: the prefetch is the next instruction word
    layout["busack_free_next"] = m.pc
    m.move("b", F.d(1), F.absl(RESULT + 0x0A))
    m.move("w", F.imm(0x0100), F.absl(0xA11100))          # BUSREQ
    m.move("w", F.imm(0x0100), F.absl(0xA11200))          # /RESET released: the bus is granted
    layout["busack_held"] = m.pc
    m.move("b", F.absl(0xA11100), F.d(1))
    layout["busack_held_next"] = m.pc
    m.move("b", F.d(1), F.absl(RESULT + 0x0C))
    m.move("b", F.imm(0xC3), F.absl(0xA00000))            # write-only upload (JP $xxxx and a byte)
    m.move("b", F.imm(0x5A), F.absl(0xA00001))
    m.move("b", F.absl(0xA00000), F.absl(RESULT + 0x0E))
    m.move("b", F.absl(0xA00001), F.absl(RESULT + 0x0F))
    m.move("b", F.imm(0x9F), F.absl(0xC00011))            # PSG: silence channel 0
    # the Z80 becomes runnable (bus released while out of reset), then the bus is requested again
    m.move("w", F.imm(0x0000), F.absl(0xA11100))
    m.move("w", F.imm(0x0100), F.absl(0xA11100))
    negative(m, 2, lambda n: n.move("b", F.absl(0xA00000), F.d(0)))
    m.move("w", F.imm(0x0000), F.absl(0xA11200))          # hold the Z80 in reset again
    m.move("w", F.imm(0x0000), F.absl(0xA11100))
    # ---- VDP: Mega Drive mode, DMA enabled, VRAM fill, status DMA bit ----
    vdp_reg(m, 0, 0x04)
    vdp_reg(m, 1, 0x14)                                   # M5 | M1 (DMA), display off, IE0 off
    vdp_reg(m, 15, 0x01)                                  # auto-increment 1 (fill)
    vdp_reg(m, 19, 0xFF)
    vdp_reg(m, 20, 0x03)
    vdp_reg(m, 23, 0x80)                                  # fill
    vdp_cmd(m, 0x40000080)                                # VRAM write $0000, DMA
    m.move("w", F.imm(0x5A00), F.absl(VDP_DATA))
    m.label("fill_wait")
    m.move("w", F.absl(VDP_CTRL), F.d(3))
    m.btst(1, F.d(3))
    m.bcc("ne", "fill_wait")
    m.move("w", F.d(3), F.d(4))
    m.immop("and", "w", 0x0002, F.d(4))
    m.move("w", F.d(4), F.absl(RESULT + 0x12))
    vdp_reg(m, 15, 0x02)
    vdp_cmd(m, 0x01000000)                                # VRAM read $0100
    m.move("w", F.absl(VDP_DATA), F.absl(RESULT + 0x10))
    # ---- 68K->VRAM DMA from work RAM and from the BIOS image ----
    for i, word in enumerate(DMA_WORDS):
        m.move("w", F.imm(word), F.absl(DMA_RAM + 2 * i))
    dma_from(m, DMA_RAM, len(DMA_WORDS), 0x1000)
    vram_checksum(m, 0x1000, len(DMA_WORDS), 0x14)
    dma_from(m, DMA_ROM, len(ROM_DMA_WORDS), 0x1100)
    vram_checksum(m, 0x1100, len(ROM_DMA_WORDS), 0x16)
    negative(m, 3, lambda n: dma_from(n, 0x200000, 4, 0x1200))
    negative(m, 5, lambda n: (vdp_reg(n, 23, 0xC0), vdp_cmd(n, 0x000000C0)))  # VRAM copy
    # ---- vertical blank: poll the status flag ----
    m.label("vb_wait")
    m.btst(3, F.absl(VDP_CTRL + 1))
    m.bcc("eq", "vb_wait")
    m.move("w", F.imm(1), F.absl(RESULT + 0x18))
    # ---- level 6 through a RAM thunk: JMP (VINT_HANDLER).L at $FFF900 (the vector names the thunk) ----
    m.move("w", F.imm(0x4EF9), F.absl(THUNK))
    m.move("l", F.imm(VINT_HANDLER), F.absl(THUNK + 2))
    m.clr("w", F.absl(FRAMES))
    negative(m, 4, lambda n: vdp_reg(n, 0, 0x14))         # IE1: the H-interrupt
    negative(m, 8, lambda n: vdp_cmd(n, 0x80148004))      # IE1 set and cleared again by one long write (two register writes)
    vdp_reg(m, 1, 0x34)                                   # IE0
    m.move_sr(0x2000)
    m.label("frames")
    m.immop("cmp", "w", 3, F.absl(FRAMES))
    m.bcc("lt", "frames")
    m.move_sr(0x2700)
    vdp_reg(m, 1, 0x14)
    m.move("w", F.absl(FRAMES), F.absl(RESULT + 0x1A))
    # ---- H-INT vector register ----
    m.move("w", F.imm(0x1234), F.absl(GA + 0x06))
    m.move("w", F.absl(GA + 0x06), F.absl(RESULT + 0x1C))
    m.move("w", F.absl(0x000072), F.absl(RESULT + 0x1E))
    m.move("w", F.absl(0x000070), F.absl(RESULT + 0x20))
    # ---- the empty cartridge slot: open bus ----
    layout["cart_long"] = m.pc
    m.move("l", F.absl(0x400100), F.d(0))                 # 6 bytes; the prefetch is the next instruction word
    layout["cart_long_next"] = m.pc
    m.move("l", F.d(0), F.absl(RESULT + 0x22))
    layout["cart_byte"] = m.pc
    m.move("b", F.absl(0x400001), F.d(0))
    layout["cart_byte_next"] = m.pc
    m.move("b", F.d(0), F.absl(RESULT + 0x26))
    negative(m, 1, lambda n: n.move("l", F.absl(0x400100), F.absl(RESULT + 0x40)))
    # ---- the sub program into PRG RAM (bank 0) through the window, then the runnable epoch ----
    negative(m, 7, lambda n: n.move("b", F.imm(0x40), F.absl(GA + 0x03)))  # BK = 1: the copy lands in the wrong bank
    m.move("l", F.imm(SUB_SRC), F.a(0))
    m.move("l", F.imm(WINDOW), F.a(1))
    m.move("w", F.imm(sub_len // 2 - 1), F.d(0))
    m.label("copy")
    m.move("w", ("inc", 0), ("inc", 1))
    m.dbra(0, "copy")
    negative(m, 6, lambda n: n.move("b", F.imm(0xFF), F.absl(WINDOW + SUB_ENTRY + 1)))  # one corrupted byte
    m.move("b", F.imm(0xA5), F.absl(RESULT + 0xFF))
    m.move("b", F.imm(0x01), F.absl(GA + 0x01))           # SRES 1, SBRQ 0
    F.wait_flag(m, GA + 0x0F, 0x01, "wait_sub")
    m.stop(0x2700)
    code = m.assemble()
    assert MAIN + len(code) <= FAULT, hex(MAIN + len(code))
    return code


def sub_program():
    s = F.Asm(0)
    s.lw(SUB_SSP)
    s.lw(SUB_ENTRY)
    while s.pc < SUB_ENTRY:
        s.w(0)
    s.move("w", F.imm(0xC0DE), F.absl(GA_SUB + 0x20))
    s.move("b", F.imm(0x01), F.absl(GA_SUB + 0x0F))
    s.stop(0x2700)
    return s.assemble()


def build():
    sub = sub_program()
    sub_len = (len(sub) + 1) & ~1
    layout = {}
    rom = bytearray(ROM_SIZE)
    struct.pack_into(">II", rom, 0, 0x00FFFE00, MAIN)
    for v in range(2, 64):
        struct.pack_into(">I", rom, 4 * v, FAULT)
    struct.pack_into(">I", rom, 30 * 4, THUNK)              # level 6 -> the work-RAM thunk
    rom[0x100:0x120] = b"SEGA BIOS-IMAGE HW FIXTURE T003 "
    main = main_program(sub_len, layout)
    rom[MAIN:MAIN + len(main)] = main
    fault = F.Asm(FAULT)
    fault.move("w", F.imm(0xDEAD), F.absl(RESULT + 0xF0))
    fault.stop(0x2700)
    rom[FAULT:FAULT + len(fault.assemble())] = fault.assemble()
    handler = F.Asm(VINT_HANDLER)
    handler.quick("add", "w", 1, F.absl(FRAMES))
    handler.rte()
    rom[VINT_HANDLER:VINT_HANDLER + len(handler.assemble())] = handler.assemble()
    assert SUB_SRC + len(sub) <= DMA_ROM
    rom[SUB_SRC:SUB_SRC + len(sub)] = sub
    for i, word in enumerate(ROM_DMA_WORDS):
        struct.pack_into(">H", rom, DMA_ROM + 2 * i, word)
    return bytes(rom), sub, layout


def word_at(rom, address):
    return struct.unpack_from(">H", rom, address)[0]


def expected(rom, sub, layout):
    def be(value, n):
        return value & ((1 << (8 * n)) - 1)
    free_next = word_at(rom, layout["busack_free_next"])
    held_next = word_at(rom, layout["busack_held_next"])
    cart_next = word_at(rom, layout["cart_long_next"])
    byte_next = word_at(rom, layout["cart_byte_next"])
    result = {
        "0": 0x80, "2": 0x8080, "4": 0x40, "6": 0x7F, "8": 0x33,
        "10": ((free_next >> 8) | 0x01) << 8, "12": ((held_next >> 8) & 0xFE) << 8,
        "14": 0xC35A, "16": 0x5A5A, "18": 0, "20": be(sum(DMA_WORDS), 2), "22": be(sum(ROM_DMA_WORDS), 2), "24": 1, "26": 3,
        "28": 0x1234, "30": 0x1234, "32": 0xFFFF, "34": cart_next, "36": cart_next, "38": (byte_next & 0xFF) << 8,
    }
    padded = sub.ljust((len(sub) + 0xFF) & ~0xFF, b"\0")
    return {
        "rom_size": len(rom), "rom_sha256": hashlib.sha256(rom).hexdigest(),
        "result_words": result,
        "sub_image_length": len(padded), "sub_image_sha256": hashlib.sha256(padded).hexdigest(),
        "sub_comstat0": 0xC0DE, "thunk": THUNK,
        "negatives": {
            "main:d7=1": ["open_bus_read", 0], "main:d7=2": ["main_device_unsupported", 0], "main:d7=3": ["vdp_dma_unsupported", 0],
            "main:d7=4": ["interrupt_source_unsupported", 0], "main:d7=8": ["interrupt_source_unsupported", 0], "main:d7=5": ["vdp_access_unsupported", 0],
            "main:d7=6": ["materialized_image_unverified", 1], "main:d7=7": ["materialized_image_unverified", 1],
        },
    }


def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=["build"])
    parser.add_argument("--out", type=pathlib.Path, required=True)
    args = parser.parse_args(argv)
    rom, sub, layout = build()
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "bios.bin").write_bytes(rom)
    exp = expected(rom, sub, layout)
    (args.out / "expected.json").write_text(json.dumps(exp, indent=1, sort_keys=True) + "\n")
    print(json.dumps(exp, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
