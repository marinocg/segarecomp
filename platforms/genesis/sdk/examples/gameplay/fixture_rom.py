#!/usr/bin/env python3
"""The deterministic, redistributable fixture image of the gameplay example (SEG-138-T009): a tiny RUNNABLE Genesis program written for this repository (not a game, no vendor code).

It configures the VDP (40-cell picture, plane A at 0xC000, three palette entries, two tiles, two filled rows) and then runs 90 one-frame "game turns". Each turn it waits for the start of vertical blank,
reads controller port 1 and applies the game's rules to two big-endian words of work RAM:
    0xFF0010  score    +1 for every turn button B is held
    0xFF0012  position +1 for every turn the Right direction is held
The program ends with `reset` (a typed guest stop) after the 90th turn. The gameplay example mod (scripts/main.luau) adds a bonus to the score while button C is held, through typed work-RAM access.

usage: fixture_rom.py <out.rom>      (prints the SHA-256; `make_rom()` returns the bytes)"""
import hashlib
import struct
import sys

TURNS = 90
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


def make_rom() -> bytes:
    c = Code()
    c.w(0x41F9); c.l(0x00C00004)                     # lea 0xC00004,a0   (VDP control)
    c.w(0x43F9); c.l(0x00C00000)                     # lea 0xC00000,a1   (VDP data)
    for reg in (0x8004, 0x8144, 0x8230, 0x8407, 0x8578, 0x8700, 0x8B00, 0x8C81, 0x8D3F, 0x8F02, 0x9001):
        c.w(0x30BC, reg)                             # move.w #reg,(a0)
    c.w(0x20BC); c.l(0xC0000000)                     # move.l #CRAM write 0,(a0)
    for colour in (0x0420, 0x00E0, 0x000E):
        c.w(0x32BC, colour)                          # move.w #colour,(a1)
    c.w(0x20BC); c.l(0x40200000)                     # VRAM write 0x0020: tile 1 (solid colour 1)
    for _ in range(8):
        c.w(0x22BC); c.l(0x11111111)
    c.w(0x20BC); c.l(0x40400000)                     # VRAM write 0x0040: tile 2 (checker of colours 1 and 2)
    for row in range(8):
        c.w(0x22BC); c.l(0x12121212 if row % 2 == 0 else 0x21212121)
    c.w(0x20BC); c.l(0x40000003)                     # VRAM write 0xC000: plane A, row 0 then row 1
    for tile in (1, 2):
        c.w(0x303C, 63)                              # move.w #63,d0
        c.label(f"fill{tile}")
        c.w(0x32BC, tile)                            # move.w #tile,(a1)
        c.w(0x51C8, 0xFFFA)                          # dbra d0,fill
    c.w(0x45F9); c.l(0x00FF0000)                     # lea 0xFF0000,a2   (work RAM)
    c.w(0x13FC, 0x0040); c.l(0x00A10009)             # move.b #0x40,0xA10009   (port 1 control: TH is an output)
    c.w(0x13FC, 0x0040); c.l(0x00A10003)             # move.b #0x40,0xA10003   (TH high: the six-bit direction/B/C read)
    c.w(0x426A, SCORE_OFFSET)                        # clr.w score(a2)
    c.w(0x426A, POSITION_OFFSET)                     # clr.w position(a2)
    c.w(0x3E3C, TURNS - 1)                           # move.w #TURNS-1,d7
    c.label("turn")
    c.label("wait_blank")
    c.w(0x3010)                                      # move.w (a0),d0   (VDP status)
    c.w(0x0800, 3)                                   # btst #3,d0       (vertical blank)
    c.branch_s(0x67, "wait_blank")                   # beq.s wait_blank
    c.w(0x1039); c.l(0x00A10003)                     # move.b 0xA10003,d0   (pad: bit clear = pressed)
    c.w(0x4600)                                      # not.b d0
    c.w(0x0800, 4)                                   # btst #4,d0       (B)
    c.branch_s(0x67, "no_b")                         # beq.s no_b
    c.w(0x526A, SCORE_OFFSET)                        # addq.w #1,score(a2)
    c.label("no_b")
    c.w(0x0800, 3)                                   # btst #3,d0       (Right)
    c.branch_s(0x67, "no_right")                     # beq.s no_right
    c.w(0x526A, POSITION_OFFSET)                     # addq.w #1,position(a2)
    c.label("no_right")
    c.label("wait_active")
    c.w(0x3010)                                      # move.w (a0),d0
    c.w(0x0800, 3)                                   # btst #3,d0
    c.branch_s(0x66, "wait_active")                  # bne.s wait_active   (until vertical blank ends)
    c.w(0x51CF)                                      # dbra d7,turn   (displacement word follows)
    at = len(c.out)
    c.w(0x0000)
    c.fixups.append((None, None))
    c.fixups.pop()
    disp = c.labels["turn"] - at
    c.out[at:at + 2] = struct.pack(">h", disp)
    c.w(0x4E70)                                      # reset: a typed guest stop ends the program
    c.resolve()
    code = c.out
    rom = bytearray(struct.pack(">H", 0x60FE) * 0x400)   # every byte not used below is `bra.s *`, so each aligned start of the image ends in a terminal branch
    rom[0:8] = struct.pack(">II", 0x00FF0000, 0x00000200)
    for vector in range(2, 64):
        rom[vector * 4:vector * 4 + 4] = struct.pack(">I", 0x00000200)
    header = (b"SEGA MEGA DRIVE " + b"(C)FIXT 2026.NOV" + b"SEGARECOMP GAMEPLAY FIXTURE".ljust(48) + b"SEGARECOMP GAMEPLAY FIXTURE".ljust(48) + b"GM 00000000-00" + b"\0\0" +
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
