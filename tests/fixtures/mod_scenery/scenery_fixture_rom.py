#!/usr/bin/env python3
"""SEG-140-T011: the deterministic, redistributable SCROLLING fixture of the ABI 2 scenery tests (derived from the gameplay fixture: same turn loop, original program, no vendor code): a tiny RUNNABLE Genesis program written for this repository (not a game, no vendor code).

It configures the VDP (40-cell picture, plane A at 0xC000, three palette entries, two tiles, two filled rows) and then runs 90 one-frame "game turns". Each turn it waits for the start of vertical blank,
reads controller port 1 and applies the game's rules to two big-endian words of work RAM:
    0xFF0010  score    +1 for every turn button B is held
    0xFF0012  position +1 for every turn the Right direction is held
The program ends with `reset` (a typed guest stop) after the 90th turn. The gameplay example mod (scripts/main.luau) adds a bonus to the score while button C is held, through typed work-RAM access.

usage: fixture_rom.py <out.rom>      (prints the SHA-256; `make_rom()` returns the bytes)"""
import hashlib
import struct
import sys

TURNS = 40
SCORE_OFFSET, POSITION_OFFSET = 0x10, 0x12


class Code:
    def __init__(self):
        self.out = bytearray()
        self.labels = {}
        self.fixups = []

    def w(self, *words):
        for word in words:
            self.out.extend(struct.pack(">H", word))

    def l(self, value):
        self.out.extend(struct.pack(">I", value))

    def label(self, name):
        # every block must end in a branch (the build's translator requires it): fall through a branch to the next instruction before each label
        self.w(0x6000, 0x0002)   # bra.w .+2
        self.labels[name] = len(self.out)

    def branch_s(self, opcode, name):   # Bcc.s / BRA.s to a label
        self.fixups.append((len(self.out), name))
        self.w(opcode << 8)

    def resolve(self):
        for at, name in self.fixups:
            displacement = self.labels[name] - (at + 2)
            assert -128 <= displacement <= 126 and displacement != 0, (name, displacement)
            self.out[at + 1] = displacement & 0xFF


def make_rom(mid_frame: bool = False, interlace: bool = False) -> bytes:
    """VDP: 40-cell picture, plane A at 0xC000 (64 x 32), horizontal scroll table at 0xFC00 (full screen). Plane A alternates tile 1 and tile 3 by column (green / red of palette line 0). Each of the 40 turns: while Right is held the
    camera word at 0xFF0012 grows by one, and the program writes the plane-A horizontal scroll -(camera * 8) into the table. Camera stays inside 0..24 in the tests (a 10-bit scroll value never wraps)."""
    c = Code()
    c.w(0x41F9); c.l(0x00C00004)                     # lea 0xC00004,a0   (VDP control)
    c.w(0x43F9); c.l(0x00C00000)                     # lea 0xC00000,a1   (VDP data)
    for reg in (0x8004, 0x8144, 0x8230, 0x8407, 0x8578, 0x8700, 0x8B00, 0x8C87 if interlace else 0x8C81, 0x8D3F, 0x8F02, 0x9001):   # SEG-152-T003: interlace=True selects interlace mode 2 (448-line output, 224 field lines)
        c.w(0x30BC, reg)                             # move.w #reg,(a0)
    c.w(0x20BC); c.l(0xC0000000)                     # move.l #CRAM write 0,(a0)
    for colour in (0x0420, 0x00E0, 0x000E):
        c.w(0x32BC, colour)                          # palette line 0: backdrop, green, red
    c.w(0x20BC); c.l(0xC0200000)                     # CRAM write 0x20 (palette line 1)
    for colour in (0x0000, 0x0E00, 0x0EE0):
        c.w(0x32BC, colour)                          # palette line 1: unused, blue, cyan
    for tile, fill in ((1, 0x11111111), (2, 0x11111111), (3, 0x22222222)):
        c.w(0x20BC); c.l(0x40000000 | (tile * 0x20 & 0x3FFF) << 16 | (tile * 0x20 >> 14))   # VRAM write tile * 32
        for _ in range(8):
            c.w(0x22BC); c.l(fill)
    c.w(0x20BC); c.l(0x40000003)                     # VRAM write 0xC000: plane A
    c.w(0x303C, 64 * 28 // 2 - 1)                     # move.w #895,d0   (two cells per pass)
    c.label("fill")
    c.w(0x32BC, 1)                                   # move.w #1,(a1)   even cells: tile 1 (green)
    c.w(0x32BC, 3)                                   # move.w #3,(a1)   odd cells: tile 3 (red): the picture itself shows the scroll's parity
    c.w(0x51C8, 0xFFF6)                              # dbra d0,fill
    c.w(0x45F9); c.l(0x00FF0000)                     # lea 0xFF0000,a2   (work RAM)
    c.w(0x13FC, 0x0040); c.l(0x00A10009)             # move.b #0x40,0xA10009
    c.w(0x13FC, 0x0040); c.l(0x00A10003)             # move.b #0x40,0xA10003
    c.w(0x426A, SCORE_OFFSET)                        # clr.w score(a2)
    c.w(0x426A, POSITION_OFFSET)                     # clr.w camera(a2)
    c.w(0x3E3C, TURNS - 1)                           # move.w #TURNS-1,d7
    c.label("turn")
    c.label("wait_blank")
    c.w(0x3010)                                      # move.w (a0),d0
    c.w(0x0800, 3)                                   # btst #3,d0
    c.branch_s(0x67, "wait_blank")                   # beq.s wait_blank
    c.w(0x1039); c.l(0x00A10003)                     # move.b 0xA10003,d0
    c.w(0x4600)                                      # not.b d0
    c.w(0x0800, 3)                                   # btst #3,d0       (Right)
    c.branch_s(0x67, "no_right")
    c.w(0x526A, POSITION_OFFSET)                     # addq.w #1,camera(a2)
    c.label("no_right")
    if mid_frame:   # SEG-152-T003: no input needed: the camera also advances by itself during the last 22 turns, so a save/load or rewind needs no input timing to reproduce the run
        c.w(0x0C47, 22)                              # cmpi.w #22,d7
        c.branch_s(0x64, "auto_skip")                # bcc.s auto_skip   (d7 >= 22)
        c.w(0x526A, POSITION_OFFSET)                 # addq.w #1,camera(a2)
        c.label("auto_skip")
    c.w(0x322A, POSITION_OFFSET)                     # move.w camera(a2),d1
    c.w(0xE749)                                      # lsl.w #3,d1
    c.w(0x4441)                                      # neg.w d1
    c.w(0x20BC); c.l(0x7C000003)                     # VRAM write 0xFC00 (horizontal scroll table)
    c.w(0x3281)                                      # move.w d1,(a1)   plane A
    c.w(0x32BC, 0)                                   # move.w #0,(a1)   plane B
    c.label("wait_active")
    c.w(0x3010)
    c.w(0x0800, 3)
    c.branch_s(0x66, "wait_active")
    if mid_frame:
        # SEG-152-T003 (ADR 0320): a raster effect for the exact-frame tests (off by default: the image is then byte-identical to the scenery fixture). Once per turn, at V counter 100, the program changes
        # (1) a CRAM entry nobody displays (palette line 3 entry 15), (2) VDP register 7 (backdrop index within palette line 3) and (3) a VRAM word nobody displays (0x0800), all from the turn counter d7, so
        # lines 0..99 and 100..223 of every frame carry different CRAM/register state and the frame-start VRAM differs from the end-of-frame VRAM. No pixel of the picture or of the authored columns changes.
        c.label("mid_wait")
        c.w(0x1039); c.l(0x00C00008)                 # move.b 0xC00008,d0   (V counter)
        c.w(0x0C00, 100)                             # cmpi.b #100,d0
        c.branch_s(0x65, "mid_wait")                 # bcs.s mid_wait
        c.w(0x20BC); c.l(0xC07E0000)                 # CRAM write 0x7E (palette line 3, entry 15)
        c.w(0x3287)                                  # move.w d7,(a1)
        c.w(0x3007)                                  # move.w d7,d0
        c.w(0x0240, 0x0003)                          # andi.w #3,d0
        c.w(0x0040, 0x8730)                          # ori.w #0x8730,d0     (register 7: palette line 3, index d7 & 3)
        c.w(0x3080)                                  # move.w d0,(a0)
        c.w(0x20BC); c.l(0x48000000)                 # VRAM write 0x0800
        c.w(0x3287)                                  # move.w d7,(a1)
    c.w(0x51CF)                                      # dbra d7,turn
    at = len(c.out)
    c.w(0x0000)
    disp = c.labels["turn"] - at
    c.out[at:at + 2] = struct.pack(">h", disp)
    c.w(0x4E70)                                      # reset: a typed guest stop
    c.resolve()
    code = c.out
    rom = bytearray(struct.pack(">H", 0x60FE) * 0x400)   # every byte not used below is `bra.s *`, so each aligned start of the image ends in a terminal branch
    rom[0:8] = struct.pack(">II", 0x00FF0000, 0x00000200)
    for vector in range(2, 64):
        rom[vector * 4:vector * 4 + 4] = struct.pack(">I", 0x00000200)
    header = (b"SEGA MEGA DRIVE " + b"(C)FIXT 2026.NOV" + b"SEGARECOMP SCENERY FIXTURE".ljust(48) + b"SEGARECOMP SCENERY FIXTURE".ljust(48) + b"GM 00000000-00" + b"\0\0" +
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
