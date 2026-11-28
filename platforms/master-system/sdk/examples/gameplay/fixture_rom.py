#!/usr/bin/env python3
"""The synthetic runner cartridge of the Master System gameplay example (SEG-147-T011, ADR 0325). Project-authored bytes only: it is not a game and holds no third-party data.

Once per frame, in its VBlank interrupt, the program reads controller 1 and
  * adds 1 to the SCORE word ($C010, little-endian) while button 1 is held;
  * adds the SPEED byte ($C016; the program sets 1 at start) to the POSITION word ($C012) while Right is held;
  * stores the pad it read at $C014 and counts the frame at $C015.
The picture is a flat sky (CRAM entry 0, value 0x30). Everything else the example shows (running, the HUD, the tint, the marker) comes from the mod.

usage: fixture_rom.py <output.sms>"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[5] / "tools"))
import sms_fixture_rom as builder  # noqa: E402

SKY = 0x30


def source():
    def reg(number, value):
        return ["        ld a,0x%02X" % value, "        out (0xBF),a", "        ld a,0x%02X" % (0x80 | number), "        out (0xBF),a"]

    lines = [".org 0x0000", "        di", "        im 1", "        ld sp,0xDFF0", "        jp main",
             ".org 0x0038", "        jp vblank",
             ".org 0x0066", "        retn",
             ".org 0x0100", "main:", "        ld hl,0xC000", "        ld de,0xC001", "        ld bc,0x00FF", "        ld (hl),0", "        ldir",
             "        ld a,1", "        ld (0xC016),a"]
    for number, value in {0: 0x06, 1: 0x80, 2: 0x0E, 5: 0x7E, 6: 0x00, 7: 0x00, 8: 0x00, 9: 0x00, 10: 0xFF}.items():
        lines += reg(number, value)
    # CRAM 0 (the cleared picture's colour) = SKY
    lines += ["        ld a,0x00", "        out (0xBF),a", "        ld a,0xC0", "        out (0xBF),a", "        ld a,0x%02X" % SKY, "        out (0xBE),a"]
    # clear VRAM so the picture is flat, then end the sprite list
    lines += ["        xor a", "        out (0xBF),a", "        ld a,0x40", "        out (0xBF),a", "        ld bc,0x4000", "wipe:   xor a", "        out (0xBE),a", "        dec bc",
              "        ld a,b", "        or c", "        jr nz,wipe",
              "        xor a", "        out (0xBF),a", "        ld a,0x7F", "        out (0xBF),a", "        ld a,0xD0", "        out (0xBE),a"]
    lines += ["        in a,(0xBF)"] + reg(1, 0xE0)   # display on, frame interrupt enabled
    lines += ["        ei", "idle:   halt", "        jr idle",
              "vblank:", "        push af", "        push bc", "        push hl", "        in a,(0xBF)",
              "        in a,(0xDC)", "        cpl", "        and 0x3F", "        ld (0xC014),a", "        ld b,a",
              "        ld a,b", "        and 0x10", "        jr z,no_score", "        ld hl,(0xC010)", "        inc hl", "        ld (0xC010),hl", "no_score:",
              "        ld a,b", "        and 0x08", "        jr z,no_move", "        ld a,(0xC016)", "        ld c,a", "        ld b,0", "        ld hl,(0xC012)", "        add hl,bc", "        ld (0xC012),hl", "no_move:",
              "        ld hl,0xC015", "        inc (hl)", "        pop hl", "        pop bc", "        pop af", "        ei", "        reti"]
    return "\n".join(lines) + "\n"


def build():
    image, _ = builder.Assembler(source()).assemble()
    return builder.build_rom(0x8000, image, {})


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    pathlib.Path(sys.argv[1]).write_bytes(build())
