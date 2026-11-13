#!/usr/bin/env python3
"""The deterministic, redistributable fixture image of the Luau HUD example: a tiny RUNNABLE Genesis program written for this repository (not a game, no vendor code).

It configures the VDP (40-cell picture, plane A at 0xC000), loads three palette entries and two 8x8 tiles, fills the first two rows of plane A and then busy-waits for about 120 frames and stops with `reset`. The
generated-native program therefore presents real VDP frames (a solid backdrop plus two rows of tiles at the top left); the Luau overlay is drawn on top. Every block of the code ends in a
branch (the build's translator requires it), hence the `bra.w .+2` before each branch target.

usage: fixture_rom.py <out.rom>      (prints the SHA-256; `make_rom()` returns the bytes)"""
import hashlib
import struct
import sys


def make_rom() -> bytes:
    code = bytearray()

    def w(*words):
        for word in words:
            code.extend(struct.pack(">H", word))

    def l(value):
        code.extend(struct.pack(">I", value))

    w(0x41F9); l(0x00C00004)                     # lea 0xC00004,a0   (VDP control)
    w(0x43F9); l(0x00C00000)                     # lea 0xC00000,a1   (VDP data)
    for reg in (0x8004, 0x8144, 0x8230, 0x8407, 0x8578, 0x8700, 0x8B00, 0x8C81, 0x8D3F, 0x8F02, 0x9001):
        w(0x30BC, reg)                           # move.w #reg,(a0)
    w(0x20BC); l(0xC0000000)                     # move.l #CRAM write 0,(a0)
    for colour in (0x0600, 0x00E0, 0x000E):
        w(0x32BC, colour)                        # move.w #colour,(a1)
    w(0x20BC); l(0x40200000)                     # VRAM write 0x0020: tile 1 (solid colour 1)
    for _ in range(8):
        w(0x22BC); l(0x11111111)                 # move.l #pattern,(a1)
    w(0x20BC); l(0x40400000)                     # VRAM write 0x0040: tile 2 (checker of colours 1 and 2)
    for row in range(8):
        w(0x22BC); l(0x12121212 if row % 2 == 0 else 0x21212121)
    w(0x20BC); l(0x40000003)                     # VRAM write 0xC000: plane A, row 0 then row 1
    for tile in (1, 2):
        w(0x303C, 63)                            # move.w #63,d0
        w(0x6000, 0x0002)                        # bra.w .+2: the next instruction is a branch target, so the straight-line block must end in a branch
        w(0x32BC, tile)                          # L: move.w #tile,(a1)
        w(0x51C8, 0xFFFA)                        # dbra d0,L   (displacement counts from the extension word)
    w(0x323C, 23)                                # move.w #23,d1   outer count: 24 x 65536 inner iterations = about 120 frames of busy waiting
    w(0x6000, 0x0002)                            # bra.w .+2
    w(0x303C, 0xFFFF)                            # O: move.w #0xFFFF,d0
    w(0x6000, 0x0002)                            # bra.w .+2
    w(0x51C8, 0xFFFE)                            # I: dbra d0,I
    w(0x51C9, 0xFFF2)                            # dbra d1,O    (to the move.w #0xFFFF,d0 above)
    w(0x4E70)                                    # reset: a typed guest stop ends the program after the busy wait
    rom = bytearray(struct.pack(">H", 0x60FE) * 0x200)   # every byte not used below is `bra.s *`, so each aligned start of the image ends in a terminal branch
    rom[0:8] = struct.pack(">II", 0x00FF0000, 0x00000200)
    for vector in range(2, 64):
        rom[vector * 4:vector * 4 + 4] = struct.pack(">I", 0x00000200)
    header = (b"SEGA MEGA DRIVE " + b"(C)FIXT 2026.NOV" + b"SEGARECOMP LUAU HUD FIXTURE".ljust(48) + b"SEGARECOMP LUAU HUD FIXTURE".ljust(48) + b"GM 00000000-00" + b"\0\0" +
              b"J".ljust(16) + struct.pack(">IIII", 0, 0x3FF, 0xFF0000, 0xFFFFFF) + b" " * 12 + b" " * 12 + b" " * 40 + b"JUE".ljust(16))
    assert len(header) == 0x100, len(header)
    rom[0x100:0x200] = header
    rom[0x200:0x200 + len(code)] = code
    return bytes(rom)


if __name__ == "__main__":
    image = make_rom()
    if len(sys.argv) > 1:
        open(sys.argv[1], "wb").write(image)
    print(hashlib.sha256(image).hexdigest())
