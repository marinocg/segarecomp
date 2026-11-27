#!/usr/bin/env python3
"""The deterministic, redistributable fixture image of the Genesis sound-mod examples (SEG-145-T003 / SEG-145-T008): a tiny RUNNABLE program with its own tiny sound driver, written for this
repository (not a game, no vendor code, no commercial sound data).

The 68000 "game" uploads a project-authored Z80 driver with its data to sound RAM and then runs 90 one-frame turns. Sound requests go through the usual two-step shape: the game logic writes a command
byte into a WORK-RAM mailbox and, once per turn, a forwarder hands each non-zero mailbox to the Z80 (bus hold, one byte, release), clears it and records it in an "accepted" byte:
    0xFF0020  sfx command mailbox        0xFF0021  music command mailbox        0xFF0022  the last command forwarded to the driver
The game itself starts one piece of music on turn 3 (`GAME_MUSIC`, the "selection behaviour") and requests one effect on every press of button B (`GAME_SFX`).

The Z80 driver is data driven: a command table maps command ids to (type, data pointer, count). The commands: 1 theme and 2 alt (PSG note sequences), 3 blip (a PSG effect), 4 drum (YM2612 DAC
samples), 5 chime (a YM2612 FM instrument, then key on/off); 0x7F silences the PSG. `layout()` returns the ROM offsets of every editable item, so a mod (and the tests) name them without any literal
guesswork. The driver's one scratch store (`driver_scratch_store`) is what the negative case retargets into its own next instruction: a run-time value over compiled code, which the product cannot
prove and isolates.

usage: fixture_rom.py <out.md>      (prints the SHA-256; `make_rom()` returns the bytes)"""
import hashlib
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[5] / "tools"))
import sms_fixture_rom as z80asm  # noqa: E402  (the project's Z80 assembler only)

TURNS = 90
ROM_SIZE = 0x4000
FREE_BASE, FREE_BYTES = 0x3000, 0x100   # zero-filled space a `guest-asm` payload may claim (its declared free range)
DRIVER_ROM_BASE = 0x2000          # where the driver image (code, command table, data) lives in the cartridge
TABLE = 0x0200                    # command table inside the Z80 image: 8 entries of (type, pointer lo, pointer hi, count)
MAIL_SFX, MAIL_MUSIC, MAIL_LAST = 0xFF0020, 0xFF0021, 0xFF0022
Z80_MAIL_MUSIC, Z80_MAIL_SFX = 0xA01F00, 0xA01F01
GAME_MUSIC, GAME_SFX = 1, 3       # what the vanilla game asks for
MUSIC_TURN = 3
TYPE_PSG, TYPE_FM, TYPE_DAC = 1, 2, 3


def note(channel, period, attenuation):
    """One PSG note record: tone latch, tone data, volume latch (three bytes the driver writes as they are)."""
    return bytes([0x80 | (channel << 5) | (period & 0x0F), (period >> 4) & 0x3F, 0x90 | (channel << 5) | (attenuation & 0x0F)])


THEME = note(0, 0x0FE, 2) + note(0, 0x0D6, 2) + note(0, 0x0BE, 2) + note(0, 0x0A0, 2)
ALT = note(0, 0x1AC, 3) + note(0, 0x168, 3) + note(0, 0x140, 3) + note(0, 0x11D, 3)
BLIP = note(1, 0x055, 1) + note(1, 0x047, 4)
DRUM = bytes((0xF0 if (i // 3) % 2 == 0 else 0x10) if i < 36 else 0x80 for i in range(48))


def instrument(algorithm=7, level=0x20, block_fnum_hi=0x22, fnum_lo=0x69):
    """An FM instrument as (register, value) pairs for channel 1, ending with the pitch (key on is the driver's)."""
    pairs = [(0xB0, algorithm), (0xB4, 0xC0)]
    for off in (0x00, 0x04, 0x08, 0x0C):
        pairs += [(0x30 + off, 1), (0x40 + off, level), (0x50 + off, 0x1F), (0x60 + off, 0), (0x70 + off, 0), (0x80 + off, 0x0F)]
    pairs += [(0xA4, block_fnum_hi), (0xA0, fnum_lo)]
    return bytes(b for pair in pairs for b in pair)


CHIME = instrument()

DRIVER_SOURCE = """
.org 0x0000
        ld sp,0x1FF0
        ld a,0x9F
        ld (0x7F11),a
        ld a,0xBF
        ld (0x7F11),a
        ld a,0xDF
        ld (0x7F11),a
        ld a,0xFF
        ld (0x7F11),a
        ld a,(0x1F20)           ; a run-time value
        add a,0x3C
scratch: ld (0x1F21),a          ; the driver's scratch byte (data, never code)
patch:  nop
main:   ld a,(0x1F00)
        or a
        jr z,sfxq
        ld c,a
        xor a
        ld (0x1F00),a
        ld a,c
        call command
sfxq:   ld a,(0x1F01)
        or a
        jr z,main
        ld c,a
        xor a
        ld (0x1F01),a
        ld a,c
        call command
        jr main
command: cp 0x7F
        jr z,silence
        dec a
        add a,a
        add a,a
        ld l,a
        ld h,%(table_hi)d
        ld a,(hl)
        inc hl
        ld e,(hl)
        inc hl
        ld d,(hl)
        inc hl
        ld b,(hl)
        ld h,d
        ld l,e
        cp 1
        jr z,notes
        cp 2
        jr z,fm
        cp 3
        jr z,dac
        ret
notes:  ld a,(hl)
        inc hl
        ld (0x7F11),a
        ld a,(hl)
        inc hl
        ld (0x7F11),a
        ld a,(hl)
        inc hl
        ld (0x7F11),a
        call delay
        djnz notes
silence: ld a,0x9F
        ld (0x7F11),a
        ld a,0xBF
        ld (0x7F11),a
        ret
delay:  ld de,0x0A00
dl:     dec de
        ld a,d
        or e
        jr nz,dl
        ret
fm:     ld e,(hl)
        inc hl
        ld d,(hl)
        inc hl
        call ymwrite
        djnz fm
        ld e,0x28
        ld d,0xF0
        call ymwrite
        call delay
        call delay
        ld e,0x28
        ld d,0x00
        call ymwrite
        ret
dac:    ld e,0x2B
        ld d,0x80
        call ymwrite
dloop:  ld e,0x2A
        ld d,(hl)
        inc hl
        call ymwrite
        ld c,40
dw:     dec c
        jr nz,dw
        djnz dloop
        ld e,0x2B
        ld d,0x00
        call ymwrite
        ret
ymwrite: ld a,(0x4000)
        add a,a
        jr c,ymwrite
        ld a,e
        ld (0x4000),a
w2:     ld a,(0x4000)
        add a,a
        jr c,w2
        ld a,d
        ld (0x4001),a
        ret
""" % {"table_hi": TABLE >> 8}


def driver():
    """(image bytes, labels, {item: (offset in the image, length)}, command table order)."""
    code, labels = z80asm.Assembler(DRIVER_SOURCE).assemble()
    end = max(code) + 1
    assert end <= TABLE, "the driver code overlaps its command table"
    image = bytearray(TABLE + 8 * 4)
    for address, value in code.items():
        image[address] = value
    items = {}
    pointers = {}
    for name, payload in (("theme", THEME), ("alt", ALT), ("blip", BLIP), ("drum", DRUM), ("chime", CHIME)):
        pointers[name] = len(image)
        items[name] = (len(image), len(payload))
        image += payload
    table = (("theme", TYPE_PSG, len(THEME) // 3), ("alt", TYPE_PSG, len(ALT) // 3), ("blip", TYPE_PSG, len(BLIP) // 3),
             ("drum", TYPE_DAC, len(DRUM)), ("chime", TYPE_FM, len(CHIME) // 2))
    for index, (name, kind, count) in enumerate(table):
        at = TABLE + 4 * index
        image[at:at + 4] = bytes([kind, pointers[name] & 0xFF, pointers[name] >> 8, count])
        items["table_" + name] = (at, 4)
    if len(image) % 2:
        image.append(0)
    return bytes(image), labels, items


class Code:
    def __init__(self, base):
        self.base = base
        self.out = bytearray()
        self.labels = {}
        self.fixups = []
        self.marks = {}

    def w(self, *words):
        for word in words:
            self.out.extend(struct.pack(">H", word & 0xFFFF))

    def l(self, value):
        self.out.extend(struct.pack(">I", value))

    def mark(self, name):
        self.marks[name] = self.base + len(self.out)

    def label(self, name):
        # every block ends in a branch (the build's translator requires it): fall through a branch to the next instruction before each label
        self.w(0x6000, 0x0002)
        self.labels[name] = len(self.out)

    def branch_s(self, opcode, name):
        self.fixups.append((len(self.out), name))
        self.w(opcode << 8)

    def dbra(self, register, name):   # a backwards dbra to an existing label
        self.w(0x51C8 | register)
        self.w(self.labels[name] - len(self.out))

    def resolve(self):
        for at, name in self.fixups:
            displacement = self.labels[name] - (at + 2)
            assert -128 <= displacement <= 126 and displacement != 0, (name, displacement)
            self.out[at + 1] = displacement & 0xFF

    # -- the few forms this program needs --
    def move_w_imm(self, value, address):
        self.w(0x33FC, value)
        self.l(address)

    def acquire_bus(self, name):
        self.move_w_imm(0x0100, 0x00A11100)
        self.label(name)
        self.w(0x0839, 0x0000)                         # btst #0,0xA11100
        self.l(0x00A11100)
        self.branch_s(0x66, name)

    def forward(self, mailbox, z80_mailbox, tag):
        self.w(0x1039); self.l(mailbox)                # move.b mailbox,d0
        self.branch_s(0x67, tag + "_none")             # beq.s
        self.acquire_bus(tag + "_bus")
        self.w(0x13C0); self.l(z80_mailbox)            # move.b d0,Z80 mailbox
        self.move_w_imm(0x0000, 0x00A11100)
        self.w(0x13C0); self.l(MAIL_LAST)              # move.b d0,last
        self.w(0x4239); self.l(mailbox)                # clr.b mailbox
        self.label(tag + "_none")


def program(image_length):
    c = Code(0x200)
    c.w(0x41F9); c.l(0x00C00004)                       # lea 0xC00004,a0   (VDP control)
    for reg in (0x8004, 0x8144, 0x8230, 0x8407, 0x8578, 0x8700, 0x8B00, 0x8C81, 0x8D3F, 0x8F02, 0x9001):
        c.w(0x30BC, reg)                               # move.w #reg,(a0)
    # upload the driver: bus request, release reset, copy, assert reset, release bus, release reset (the documented sequence)
    c.move_w_imm(0x0100, 0x00A11100)
    c.move_w_imm(0x0100, 0x00A11200)
    c.label("ack")
    c.w(0x0839, 0x0000); c.l(0x00A11100)
    c.branch_s(0x66, "ack")
    c.w(0x45F9); c.l(DRIVER_ROM_BASE)                  # lea driver,a2
    c.w(0x43F9); c.l(0x00A00000)                       # lea Z80 RAM,a1
    c.w(0x303C, image_length - 1)                      # move.w #n-1,d0
    c.label("copy")
    c.w(0x12DA)                                        # move.b (a2)+,(a1)+
    c.dbra(0, "copy")
    c.move_w_imm(0x0000, 0x00A11200)
    c.move_w_imm(0x0000, 0x00A11100)
    c.move_w_imm(0x0100, 0x00A11200)
    for mailbox in (MAIL_SFX, MAIL_MUSIC, MAIL_LAST):
        c.w(0x4239); c.l(mailbox)                      # clr.b
    c.w(0x13FC, 0x0040); c.l(0x00A10009)               # port 1 control: TH output
    c.w(0x13FC, 0x0040); c.l(0x00A10003)               # TH high
    c.w(0x7A00)                                        # moveq #0,d5   (B held on the previous turn)
    c.w(0x3E3C, TURNS - 1)                             # move.w #TURNS-1,d7
    c.label("turn")
    c.label("wait_blank")
    c.w(0x3010)                                        # move.w (a0),d0
    c.w(0x0800, 3)                                     # btst #3,d0
    c.branch_s(0x67, "wait_blank")
    c.w(0x0C47, TURNS - 1 - MUSIC_TURN)                # cmpi.w #turn,d7
    c.branch_s(0x66, "no_music")
    c.mark("game_music")
    c.w(0x13FC, GAME_MUSIC); c.l(MAIL_MUSIC)           # move.b #GAME_MUSIC,music mailbox
    c.label("no_music")
    c.w(0x1039); c.l(0x00A10003)                       # move.b pad,d0 (bit clear = pressed)
    c.w(0x4600)                                        # not.b d0
    c.w(0x0800, 4)                                     # btst #4,d0 (B)
    c.branch_s(0x67, "released")
    c.w(0x4A05)                                        # tst.b d5
    c.branch_s(0x66, "pad_done")
    c.w(0x7A01)                                        # moveq #1,d5
    c.mark("game_sfx")
    c.w(0x13FC, GAME_SFX); c.l(MAIL_SFX)               # move.b #GAME_SFX,sfx mailbox
    c.branch_s(0x60, "pad_done")
    c.label("released")
    c.w(0x7A00)
    c.label("pad_done")
    c.forward(MAIL_SFX, Z80_MAIL_SFX, "sfx")
    c.forward(MAIL_MUSIC, Z80_MAIL_MUSIC, "music")
    c.label("wait_active")
    c.w(0x3010)
    c.w(0x0800, 3)
    c.branch_s(0x66, "wait_active")
    c.w(0x51CF)                                        # dbra d7,turn (a word displacement follows)
    at = len(c.out)
    c.w(c.labels["turn"] - at)
    c.w(0x4E70)                                        # reset: a typed guest stop ends the program
    c.resolve()
    return bytes(c.out), c.marks


def make_rom() -> bytes:
    image, _, _ = driver()
    code, _ = program(len(image))
    assert 0x200 + len(code) <= DRIVER_ROM_BASE
    rom = bytearray(struct.pack(">H", 0x60FE) * (ROM_SIZE // 2))   # every unused word is `bra.s *`
    rom[0:8] = struct.pack(">II", 0x00FFFE00, 0x00000200)   # the stack is inside work RAM: an injected subroutine may be called
    for vector in range(2, 64):
        rom[vector * 4:vector * 4 + 4] = struct.pack(">I", 0x00000200)
    header = (b"SEGA MEGA DRIVE " + b"(C)FIXT 2026.NOV" + b"SEGARECOMP SOUND FIXTURE".ljust(48) + b"SEGARECOMP SOUND FIXTURE".ljust(48) + b"GM 00000000-00" + b"\0\0" +
              b"J".ljust(16) + struct.pack(">IIII", 0, ROM_SIZE - 1, 0xFF0000, 0xFFFFFF) + b" " * 12 + b" " * 12 + b" " * 40 + b"JUE".ljust(16))
    assert len(header) == 0x100, len(header)
    rom[0x100:0x200] = header
    rom[0x200:0x200 + len(code)] = code
    rom[DRIVER_ROM_BASE:DRIVER_ROM_BASE + len(image)] = image
    rom[FREE_BASE:FREE_BASE + FREE_BYTES] = bytes(FREE_BYTES)
    return bytes(rom)


def layout():
    """ROM offsets of every editable item: driver data and table entries as (offset, length); the two game immediates and the driver's scratch store operand as offsets."""
    image, labels, items = driver()
    _, marks = program(len(image))
    out = {name: (DRIVER_ROM_BASE + offset, length) for name, (offset, length) in items.items()}
    out["game_music_immediate"] = (marks["game_music"] + 3, 1)       # move.b #imm: the low byte of the immediate word
    out["game_sfx_immediate"] = (marks["game_sfx"] + 3, 1)
    out["driver_scratch_store"] = (DRIVER_ROM_BASE + labels["scratch"] + 1, 2)   # ld (nn),a: the little-endian address operand
    out["game_sfx_instruction"] = (marks["game_sfx"], 8)             # the whole `move.b #GAME_SFX,mailbox` (a code-injection site)
    out["free"] = (FREE_BASE, FREE_BYTES)
    out["driver_patch_address"] = (labels["patch"], 0)              # a Z80 address, not a ROM offset
    return out


if __name__ == "__main__":
    rom = make_rom()
    if len(sys.argv) > 1:
        open(sys.argv[1], "wb").write(rom)
    print(hashlib.sha256(rom).hexdigest())
