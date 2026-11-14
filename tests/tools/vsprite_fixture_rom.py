#!/usr/bin/env python3
"""A deterministic, FABRICATED, runnable Genesis image for the virtual-sprite viewer tests (SEG-140-T008/T009, F28): not a game, no vendor code. Built with the code builder of the gameplay example
(platforms/genesis/sdk/examples/gameplay/fixture_rom.py). It configures a 40-cell picture and then idles for 90 frames before a typed guest stop (`reset`):

  palette 0   colour 0 backdrop 0x0420, colour 1 green 0x00E0, colour 2 red 0x000E, colour 3 blue 0x0E00 (CRAM words 0BGR)
  patterns    1: solid colour 1   2: checker of colours 1 and 2   3: solid colour 3          (VRAM 0x0020, 0x0040, 0x0060)
  plane A     cell row 0: pattern 1, LOW priority (picture rows 0-7); cell row 1: pattern 1, HIGH priority (picture rows 8-15); every other row is empty (backdrop)
  sprites     24 hardware sprites (1x1 cell, pattern 3, x = 8 * n) linked on picture rows 96-103: the first 20 are drawn (the original per-line limit of a 40-cell picture), 4 are not

usage: vsprite_fixture_rom.py <out.rom>"""
import hashlib
import importlib.util
import pathlib
import struct
import sys

_spec = importlib.util.spec_from_file_location("gameplay_fixture", pathlib.Path(__file__).resolve().parents[2] / "platforms/genesis/sdk/examples/gameplay/fixture_rom.py")
_base = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_base)

FRAMES = 90
HW_SPRITES, HW_Y, HW_X_STEP = 24, 96, 8
PALETTE = (0x0420, 0x00E0, 0x000E, 0x0E00)


def make_rom() -> bytes:
    c = _base.Code()
    c.w(0x41F9); c.l(0x00C00004)                     # lea 0xC00004,a0   (VDP control)
    c.w(0x43F9); c.l(0x00C00000)                     # lea 0xC00000,a1   (VDP data)
    for reg in (0x8004, 0x8144, 0x8230, 0x8407, 0x8578, 0x8700, 0x8B00, 0x8C81, 0x8D3F, 0x8F02, 0x9001):
        c.w(0x30BC, reg)                             # move.w #reg,(a0)
    c.w(0x20BC); c.l(0xC0000000)                     # move.l #CRAM write 0,(a0)
    for colour in PALETTE:
        c.w(0x32BC, colour)                          # move.w #colour,(a1)
    c.w(0x20BC); c.l(0x40200000)                     # VRAM write 0x0020: pattern 1 (solid colour 1)
    for _ in range(8):
        c.w(0x22BC); c.l(0x11111111)
    c.w(0x20BC); c.l(0x40400000)                     # VRAM write 0x0040: pattern 2 (checker of colours 1 and 2)
    for row in range(8):
        c.w(0x22BC); c.l(0x12121212 if row % 2 == 0 else 0x21212121)
    c.w(0x20BC); c.l(0x40600000)                     # VRAM write 0x0060: pattern 3 (solid colour 3)
    for _ in range(8):
        c.w(0x22BC); c.l(0x33333333)
    c.w(0x20BC); c.l(0x40000003)                     # VRAM write 0xC000: plane A, cell row 0 (low priority) then cell row 1 (high priority)
    for index, word in enumerate((0x0001, 0x8001)):
        c.w(0x303C, 63)                              # move.w #63,d0
        c.label(f"fill{index}")
        c.w(0x32BC, word)                            # move.w #word,(a1)
        c.w(0x51C8, 0xFFFA)                          # dbra d0,fill
    c.w(0x20BC); c.l(0x70000003)                     # VRAM write 0xF000: the sprite attribute table
    for n in range(HW_SPRITES):
        link = n + 1 if n + 1 < HW_SPRITES else 0
        c.w(0x32BC, 128 + HW_Y)                      # y + 128
        c.w(0x32BC, link)                            # size 1x1 cell, link
        c.w(0x32BC, 0x0003)                          # low priority, palette 0, pattern 3
        c.w(0x32BC, 128 + HW_X_STEP * n)             # x + 128
    c.w(0x3E3C, FRAMES - 1)                          # move.w #FRAMES-1,d7
    c.label("turn")
    c.label("wait_blank")
    c.w(0x3010)                                      # move.w (a0),d0   (VDP status)
    c.w(0x0800, 3)                                   # btst #3,d0       (vertical blank)
    c.branch_s(0x67, "wait_blank")                   # beq.s wait_blank
    c.label("wait_active")
    c.w(0x3010)                                      # move.w (a0),d0
    c.w(0x0800, 3)                                   # btst #3,d0
    c.branch_s(0x66, "wait_active")                  # bne.s wait_active   (until vertical blank ends)
    c.w(0x51CF)                                      # dbra d7,turn   (displacement word follows)
    at = len(c.out)
    c.w(0x0000)
    disp = c.labels["turn"] - at
    c.out[at:at + 2] = struct.pack(">h", disp)
    c.w(0x4E70)                                      # reset: a typed guest stop ends the program
    c.resolve()
    code = c.out
    rom = bytearray(struct.pack(">H", 0x60FE) * 0x400)
    rom[0:8] = struct.pack(">II", 0x00FF0000, 0x00000200)
    for vector in range(2, 64):
        rom[vector * 4:vector * 4 + 4] = struct.pack(">I", 0x00000200)
    header = (b"SEGA MEGA DRIVE " + b"(C)FIXT 2026.NOV" + b"SEGARECOMP VSPRITE FIXTURE".ljust(48) + b"SEGARECOMP VSPRITE FIXTURE".ljust(48) + b"GM 00000001-00" + b"\0\0" +
              b"J".ljust(16) + struct.pack(">IIII", 0, 0x7FF, 0xFF0000, 0xFFFFFF) + b" " * 12 + b" " * 12 + b" " * 40 + b"JUE".ljust(16))
    assert len(header) == 0x100, len(header)
    rom[0x100:0x200] = header
    assert 0x200 + len(code) <= len(rom)
    rom[0x200:0x200 + len(code)] = code
    return bytes(rom)


if __name__ == "__main__":
    image = make_rom()
    if len(sys.argv) > 1:
        open(sys.argv[1], "wb").write(image)
    print(hashlib.sha256(image).hexdigest())
