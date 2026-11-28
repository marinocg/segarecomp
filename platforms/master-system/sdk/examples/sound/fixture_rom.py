#!/usr/bin/env python3
"""The synthetic PSG cartridge of the Master System sound-mod example (SEG-145-T004). Project-authored bytes only: a tiny INVENTED sound driver, not a game and not any real game's format.

Once per frame, in its VBlank interrupt, the driver plays one step of an 8-step pattern (a new step every 4 frames) on the SN76489 PSG (port $7F):
  COMMANDS  8 bytes   the note index (0-3) of each step: the command table
  PERIODS   4 pairs   the 10-bit tone period of each note, as the two bytes the PSG takes: its low 4 bits, then its high 6 bits
  VOLUMES   8 bytes   the attenuation (0 loudest, 15 silent) of each step, passed through the routine `volume_hook` (it returns its input; a mod may redirect its call)
  NOISE     2 bytes   the noise control (0-7) and the noise attenuation
At start it also probes the FM unit of the Japanese console: it writes 1 then 0 to port $F2 and keeps both reads at $C020 / $C021. With an FM unit bit 0 reads back what was written; this machine models
no YM2413, so the two reads never echo the writes.

usage: fixture_rom.py <output.sms>"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[5] / "tools"))
import sms_fixture_rom as builder  # noqa: E402

COMMANDS, PERIODS, VOLUMES, NOISE = 0x1000, 0x1010, 0x1020, 0x1030
ORIGINAL = {COMMANDS: bytes([0, 1, 2, 3, 2, 1, 0, 3]), PERIODS: b"".join(bytes([v & 0x0F, v >> 4]) for v in (0x1AC, 0x17D, 0x153, 0x11D)),
            VOLUMES: bytes([2, 3, 4, 5, 6, 7, 8, 9]), NOISE: bytes([0x04, 0x0C])}


def source():
    def reg(number, value):
        return ["        ld a,0x%02X" % value, "        out (0xBF),a", "        ld a,0x%02X" % (0x80 | number), "        out (0xBF),a"]

    lines = [".org 0x0000", "        di", "        im 1", "        ld sp,0xDFF0", "        jp main",
             ".org 0x0038", "        jp vblank",
             ".org 0x0066", "        retn",
             ".org 0x0100", "main:", "        ld hl,0xC000", "        ld de,0xC001", "        ld bc,0x00FF", "        ld (hl),0", "        ldir",
             "        ld a,1", "        out (0xF2),a", "        in a,(0xF2)", "        ld (0xC020),a",
             "        xor a", "        out (0xF2),a", "        in a,(0xF2)", "        ld (0xC021),a",
             "        ld a,0x9F", "        out (0x7F),a", "        ld a,0xBF", "        out (0x7F),a", "        ld a,0xDF", "        out (0x7F),a", "        ld a,0xFF", "        out (0x7F),a"]
    for number, value in {0: 0x06, 1: 0x80, 2: 0x0E, 5: 0x7E, 6: 0x00, 7: 0x00, 8: 0x00, 9: 0x00, 10: 0xFF}.items():
        lines += reg(number, value)
    lines += ["        in a,(0xBF)"] + reg(1, 0xE0)
    lines += ["        ei", "idle:   halt", "        jr idle",
              "vblank:", "        push af", "        push bc", "        push de", "        push hl", "        in a,(0xBF)",
              # $C000 = frame counter, $C001 = frames spent in this step, $C002 = the step (0-7): a new step every 4 frames
              "        ld hl,0xC000", "        inc (hl)", "        inc hl", "        inc (hl)", "        ld a,(hl)", "        cp 4", "        jr nz,same_step", "        ld (hl),0",
              "        ld a,(0xC002)", "        inc a", "        and 7", "        ld (0xC002),a", "same_step:", "        ld a,(0xC002)", "        ld e,a", "        ld d,0",
              "        ld hl,0x%04X" % COMMANDS, "        add hl,de", "        ld a,(hl)", "        and 3", "        add a,a", "        ld c,a", "        ld b,0",
              "        ld hl,0x%04X" % PERIODS, "        add hl,bc",
              # tone channel 0: the latch byte takes the period's low 4 bits, the data byte its high 6 bits
              "        ld a,(hl)", "        and 0x0F", "        or 0x80", "        out (0x7F),a", "        inc hl", "        ld a,(hl)", "        and 0x3F", "        out (0x7F),a",
              "        ld hl,0x%04X" % VOLUMES, "        add hl,de", "        ld a,(hl)",
              "hook_site: call volume_hook", "        and 0x0F", "        or 0x90", "        out (0x7F),a",
              "        ld a,(0x%04X)" % NOISE, "        and 0x07", "        or 0xE0", "        out (0x7F),a",
              "        ld a,(0x%04X)" % (NOISE + 1), "        and 0x0F", "        or 0xF0", "        out (0x7F),a",
              "        pop hl", "        pop de", "        pop bc", "        pop af", "        ei", "        reti",
              "volume_hook:", "        ret"]
    return "\n".join(lines) + "\n"


def assemble():
    return builder.Assembler(source()).assemble()


def build():
    image, _ = assemble()
    markers = {}
    for offset, blob in ORIGINAL.items():
        for i, byte in enumerate(blob):
            markers[offset + i] = byte
    free = {0x2000 + i: 0xFF for i in range(0x100)}   # declared free space for an injected driver routine
    return builder.build_rom(0x8000, image, {**free, **markers})


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    pathlib.Path(sys.argv[1]).write_bytes(build())
